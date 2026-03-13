#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

SKIP_RETURN_CODE = 77


def load_grpc_modules():
    try:
        import grpc  # type: ignore
        from grpc_tools import protoc  # type: ignore
    except ImportError as exc:
        print(f"skip: missing python grpc dependency: {exc}", file=sys.stderr)
        raise SystemExit(SKIP_RETURN_CODE) from exc
    return grpc, protoc


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="验证 secs RPC 与 Python grpc 的完整 V1 互通"
    )
    parser.add_argument("--server", required=True, help="secs-rpc-server 可执行文件路径")
    parser.add_argument("--peer", required=True, help="HSMS 测试 peer 可执行文件路径")
    parser.add_argument("--proto", required=True, help="secs_rpc.proto 路径")
    parser.add_argument("--host", default="127.0.0.1", help="监听主机（默认 127.0.0.1）")
    parser.add_argument(
        "--timeout-sec", type=float, default=15.0, help="等待服务启动和调用完成的总超时"
    )
    return parser.parse_args()


def pick_unused_port(host: str) -> int:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind((host, 0))
            return int(sock.getsockname()[1])
    except PermissionError as exc:
        print(f"skip: socket bind is not permitted in current environment: {exc}", file=sys.stderr)
        raise SystemExit(SKIP_RETURN_CODE) from exc


def generate_python_stubs(proto_path: pathlib.Path, out_dir: pathlib.Path, protoc) -> None:
    proto_include_root = proto_path.parents[3]
    rc = protoc.main(
        [
            "grpc_tools.protoc",
            f"-I{proto_include_root}",
            f"--python_out={out_dir}",
            f"--grpc_python_out={out_dir}",
            str(proto_path),
        ]
    )
    if rc != 0:
        raise RuntimeError(f"grpc_tools.protoc failed with exit code {rc}")


def import_generated_modules():
    try:
        pb2 = importlib.import_module("secs.rpc.v1.secs_rpc_pb2")
        pb2_grpc = importlib.import_module("secs.rpc.v1.secs_rpc_pb2_grpc")
        return pb2, pb2_grpc
    except ModuleNotFoundError:
        pb2 = importlib.import_module("secs_rpc_pb2")
        pb2_grpc = importlib.import_module("secs_rpc_pb2_grpc")
        return pb2, pb2_grpc


def terminate_process(proc: subprocess.Popen[str]) -> str:
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                stdout, _ = proc.communicate(timeout=5.0)
                return stdout
            except subprocess.TimeoutExpired:
                proc.kill()
        stdout, _ = proc.communicate()
        return stdout
    except ValueError:
        return ""


def wait_for_server(channel, proc: subprocess.Popen[str], grpc, timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            output = terminate_process(proc)
            raise RuntimeError(f"server exited early with code {proc.returncode}\n{output}")
        try:
            grpc.channel_ready_future(channel).result(timeout=0.25)
            return
        except grpc.FutureTimeoutError:
            time.sleep(0.1)
    raise RuntimeError("timed out waiting for gRPC channel readiness")


def wait_for_process_ready(proc: subprocess.Popen[str], timeout_sec: float) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            output = terminate_process(proc)
            raise RuntimeError(f"peer exited early with code {proc.returncode}\n{output}")
        time.sleep(0.1)
        return
    raise RuntimeError("timed out waiting for peer start")


def assert_status_ok(message, what: str) -> None:
    if not message.HasField("status") or not message.status.ok:
        raise RuntimeError(f"{what} failed: {message}")


def wait_for_selected(session_stub, pb2, session_id: str, timeout_sec: float):
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        response = session_stub.GetSession(pb2.GetSessionRequest(session_id=session_id), timeout=2.0)
        assert_status_ok(response, "GetSession")
        session = response.session
        if session.selected_generation > 0:
            return session
        if (
            session.state == pb2.SESSION_STATE_STOPPED
            and session.HasField("last_error")
            and session.last_error.message
        ):
            raise RuntimeError(f"session stopped before selection: {session.last_error.message}")
        time.sleep(0.1)
    raise RuntimeError("timed out waiting for HSMS session selection")


def build_request_item(pb2):
    root = pb2.ItemNode(type=pb2.ITEM_TYPE_LIST)
    root.items.add(type=pb2.ITEM_TYPE_ASCII, ascii_value="PING")
    number = root.items.add(type=pb2.ITEM_TYPE_U4)
    number.u4_values.append(7)
    return root


def build_send_item(pb2):
    return pb2.ItemNode(type=pb2.ITEM_TYPE_ASCII, ascii_value="ONEWAY")


def assert_reply_item(pb2, reply_item) -> None:
    if reply_item.type != pb2.ITEM_TYPE_LIST or len(reply_item.items) != 2:
        raise RuntimeError(f"unexpected reply item shape: {reply_item}")
    if reply_item.items[0].type != pb2.ITEM_TYPE_ASCII or reply_item.items[0].ascii_value != "ACK":
        raise RuntimeError(f"unexpected ACK prefix: {reply_item}")
    echoed = reply_item.items[1]
    if echoed.type != pb2.ITEM_TYPE_LIST or len(echoed.items) != 2:
        raise RuntimeError(f"unexpected echoed payload: {reply_item}")
    if echoed.items[0].type != pb2.ITEM_TYPE_ASCII or echoed.items[0].ascii_value != "PING":
        raise RuntimeError(f"unexpected echoed ascii: {reply_item}")
    if echoed.items[1].type != pb2.ITEM_TYPE_U4 or list(echoed.items[1].u4_values) != [7]:
        raise RuntimeError(f"unexpected echoed integer: {reply_item}")


def main() -> int:
    args = parse_args()
    grpc, protoc = load_grpc_modules()

    server_path = pathlib.Path(args.server).resolve()
    peer_path = pathlib.Path(args.peer).resolve()
    proto_path = pathlib.Path(args.proto).resolve()
    if not server_path.exists():
        print(f"server executable not found: {server_path}", file=sys.stderr)
        return 1
    if not peer_path.exists():
        print(f"peer executable not found: {peer_path}", file=sys.stderr)
        return 1
    if not proto_path.exists():
        print(f"proto file not found: {proto_path}", file=sys.stderr)
        return 1

    rpc_port = pick_unused_port(args.host)
    peer_port = pick_unused_port(args.host)
    rpc_address = f"{args.host}:{rpc_port}"
    peer_address = f"{args.host}:{peer_port}"

    with tempfile.TemporaryDirectory(prefix="secs_rpc_py_") as tmp:
        tmp_dir = pathlib.Path(tmp)
        generate_python_stubs(proto_path, tmp_dir, protoc)

        sys.path.insert(0, str(tmp_dir))
        pb2, pb2_grpc = import_generated_modules()

        peer_proc = subprocess.Popen(
            [
                str(peer_path),
                "--listen",
                peer_address,
                "--session-id",
                "1",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        server_proc = subprocess.Popen(
            [
                str(server_path),
                "--listen",
                rpc_address,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        try:
            wait_for_process_ready(peer_proc, 1.0)

            channel = grpc.insecure_channel(rpc_address)
            wait_for_server(channel, server_proc, grpc, args.timeout_sec)

            library_stub = pb2_grpc.LibraryServiceStub(channel)
            session_stub = pb2_grpc.SessionServiceStub(channel)
            messaging_stub = pb2_grpc.MessagingServiceStub(channel)

            info = library_stub.GetLibraryInfo(pb2.GetLibraryInfoRequest(), timeout=3.0)
            assert_status_ok(info, "GetLibraryInfo")
            transports = set(info.supported_transports)
            features = set(info.supported_features)
            if "HSMS" not in transports or "SECS-I" not in transports:
                raise RuntimeError(f"unexpected transports: {sorted(transports)}")
            if "messaging-service-v1" not in features:
                raise RuntimeError(f"unexpected features: {sorted(features)}")

            create_response = session_stub.CreateSession(
                pb2.CreateSessionRequest(
                    name="python-hsms-client",
                    transport=pb2.TransportConfig(
                        kind=pb2.TRANSPORT_KIND_HSMS,
                        hsms=pb2.HsmsConfig(
                            ip=args.host,
                            port=peer_port,
                            session_id=1,
                            passive=False,
                            auto_reconnect=True,
                            t3_ms=1500,
                            t5_ms=200,
                            t6_ms=1500,
                            t7_ms=1500,
                            t8_ms=1500,
                        ),
                    ),
                    runtime=pb2.SessionRuntimeConfig(
                        request_timeout_ms=1500,
                        poll_interval_ms=10,
                        max_pending_requests=16,
                    ),
                ),
                timeout=3.0,
            )
            assert_status_ok(create_response, "CreateSession")
            session_id = create_response.session.session_id
            if not session_id:
                raise RuntimeError("CreateSession returned empty session_id")

            start_response = session_stub.StartSession(
                pb2.StartSessionRequest(session_id=session_id), timeout=3.0
            )
            assert_status_ok(start_response, "StartSession")

            selected_session = wait_for_selected(session_stub, pb2, session_id, args.timeout_sec)

            list_response = session_stub.ListSessions(pb2.ListSessionsRequest(), timeout=3.0)
            assert_status_ok(list_response, "ListSessions")
            if len(list_response.sessions) != 1 or list_response.sessions[0].session_id != session_id:
                raise RuntimeError(f"unexpected session list: {list_response}")

            send_response = messaging_stub.Send(
                pb2.SendRequest(
                    session_id=session_id,
                    message=pb2.MessageEnvelope(
                        stream=1,
                        function=3,
                        decoded_item=build_send_item(pb2),
                    ),
                ),
                timeout=3.0,
            )
            assert_status_ok(send_response, "Send")
            if send_response.accepted.stream != 1 or send_response.accepted.function != 3:
                raise RuntimeError(f"unexpected Send accepted envelope: {send_response}")

            request_response = messaging_stub.Request(
                pb2.RequestRequest(
                    session_id=session_id,
                    request=pb2.MessageEnvelope(
                        stream=1,
                        function=1,
                        decoded_item=build_request_item(pb2),
                    ),
                    timeout_ms=1500,
                ),
                timeout=5.0,
            )
            assert_status_ok(request_response, "Request")
            if request_response.reply.stream != 1 or request_response.reply.function != 2:
                raise RuntimeError(f"unexpected reply header: {request_response}")
            if not request_response.reply.body:
                raise RuntimeError("empty reply body")
            if not request_response.reply.HasField("decoded_item"):
                raise RuntimeError(f"missing decoded_item in reply: {request_response}")
            assert_reply_item(pb2, request_response.reply.decoded_item)

            stop_response = session_stub.StopSession(
                pb2.StopSessionRequest(session_id=session_id, reason="python-test"),
                timeout=3.0,
            )
            assert_status_ok(stop_response, "StopSession")
            if stop_response.session.state != pb2.SESSION_STATE_STOPPED:
                raise RuntimeError(f"unexpected stopped state: {stop_response}")

            delete_response = session_stub.DeleteSession(
                pb2.DeleteSessionRequest(session_id=session_id), timeout=3.0
            )
            assert_status_ok(delete_response, "DeleteSession")

            list_after_delete = session_stub.ListSessions(pb2.ListSessionsRequest(), timeout=3.0)
            assert_status_ok(list_after_delete, "ListSessions(after delete)")
            if list_after_delete.sessions:
                raise RuntimeError(f"sessions should be empty after delete: {list_after_delete}")

            channel.close()
            print(
                "python grpc interop ok:",
                selected_session.session_id,
                selected_session.selected_generation,
            )
            return 0
        except Exception as exc:
            server_output = terminate_process(server_proc)
            peer_output = terminate_process(peer_proc)
            print(f"python grpc interop failed: {exc}", file=sys.stderr)
            if server_output:
                print("[rpc-server output]", file=sys.stderr)
                print(server_output, file=sys.stderr)
            if peer_output:
                print("[hsms-peer output]", file=sys.stderr)
                print(peer_output, file=sys.stderr)
            return 1
        finally:
            if server_proc.poll() is None:
                terminate_process(server_proc)
            if peer_proc.poll() is None:
                terminate_process(peer_proc)


if __name__ == "__main__":
    raise SystemExit(main())
