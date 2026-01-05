#!/usr/bin/env bash
set -euo pipefail

DOTNET_BIN="${1:-}"
DOTNET_PROJECT="${2:-}"
CPP_CLIENT="${3:-}"
CPP_SERVER="${4:-}"

if [[ -z "${DOTNET_BIN}" || -z "${DOTNET_PROJECT}" || -z "${CPP_CLIENT}" || -z "${CPP_SERVER}" ]]; then
  echo "用法: $0 <dotnet> <dotnet_csproj> <cpp_client_bin> <cpp_server_bin>" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

DOTNET_CLI_HOME="${REPO_ROOT}/.dotnet_cache/dotnet_home"
NUGET_HTTP_CACHE_PATH="${REPO_ROOT}/.dotnet_cache/nuget_http"
NUGET_PLUGINS_CACHE_PATH="${REPO_ROOT}/.dotnet_cache/nuget_plugins"
NUGET_SCRATCH="${REPO_ROOT}/.dotnet_cache/nuget_scratch"

mkdir -p "${DOTNET_CLI_HOME}" "${NUGET_HTTP_CACHE_PATH}" "${NUGET_PLUGINS_CACHE_PATH}" "${NUGET_SCRATCH}"

export MSBuildEnableWorkloadResolver=false
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_CLI_HOME
export NUGET_PACKAGES="/home/say/.nuget/packages"
export NUGET_HTTP_CACHE_PATH
export NUGET_PLUGINS_CACHE_PATH
export NUGET_SCRATCH

echo "[HSMS] dotnet restore/build: ${DOTNET_PROJECT}"
"${DOTNET_BIN}" restore "${DOTNET_PROJECT}" --ignore-failed-sources -v minimal -p:NuGetAudit=false
"${DOTNET_BIN}" build "${DOTNET_PROJECT}" -c Release --no-restore -v minimal -p:NuGetAudit=false

DOTNET_DLL_DIR="$(cd "$(dirname "${DOTNET_PROJECT}")/bin/Release/net8.0" && pwd)"
DOTNET_DLL="${DOTNET_DLL_DIR}/HsmsPeer.dll"

if [[ ! -f "${DOTNET_DLL}" ]]; then
  echo "[HSMS] 未找到构建产物: ${DOTNET_DLL}" >&2
  exit 3
fi

find_free_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

wait_port_open() {
  local port="$1"
  python3 - <<PY
import socket, time, sys
port = int("${port}")
deadline = time.time() + 5.0
while time.time() < deadline:
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=0.2)
        s.close()
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY
}

cleanup_pids=()
cleanup() {
  for pid in "${cleanup_pids[@]:-}"; do
    kill "${pid}" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

DEVICE_ID=1

echo "[HSMS] 用例1：C++ 主动 -> secs4net 被动（回显应答）"
PORT1="$(find_free_port)"
LOG1="${REPO_ROOT}/.dotnet_cache/hsms_case1_dotnet.log"
LOG2="${REPO_ROOT}/.dotnet_cache/hsms_case1_cpp.log"

"${DOTNET_BIN}" "${DOTNET_DLL}" --mode echo-server --ip 127.0.0.1 --port "${PORT1}" --device-id "${DEVICE_ID}" >"${LOG1}" 2>&1 &
PID_DOTNET_1=$!
cleanup_pids+=("${PID_DOTNET_1}")

wait_port_open "${PORT1}"

set +e
"${CPP_CLIENT}" 127.0.0.1 "${PORT1}" "${DEVICE_ID}" >"${LOG2}" 2>&1
RC_CPP_1=$?
set -e

wait "${PID_DOTNET_1}" || true

if [[ "${RC_CPP_1}" -ne 0 ]]; then
  echo "[HSMS] 用例1失败：C++ 返回码 ${RC_CPP_1}" >&2
  echo "--- dotnet log ---" >&2
  tail -n 200 "${LOG1}" >&2 || true
  echo "--- cpp log ---" >&2
  tail -n 200 "${LOG2}" >&2 || true
  exit 10
fi

echo "[HSMS] 用例2：secs4net 主动 -> C++ 被动（回显应答）"
PORT2="$(find_free_port)"
LOG3="${REPO_ROOT}/.dotnet_cache/hsms_case2_cpp.log"
LOG4="${REPO_ROOT}/.dotnet_cache/hsms_case2_dotnet.log"

"${CPP_SERVER}" "${PORT2}" "${DEVICE_ID}" >"${LOG3}" 2>&1 &
PID_CPP_2=$!
cleanup_pids+=("${PID_CPP_2}")

wait_port_open "${PORT2}"

set +e
"${DOTNET_BIN}" "${DOTNET_DLL}" --mode request-client --ip 127.0.0.1 --port "${PORT2}" --device-id "${DEVICE_ID}" >"${LOG4}" 2>&1
RC_DOTNET_2=$?
set -e

wait "${PID_CPP_2}" || true

if [[ "${RC_DOTNET_2}" -ne 0 ]]; then
  echo "[HSMS] 用例2失败：dotnet 返回码 ${RC_DOTNET_2}" >&2
  echo "--- cpp log ---" >&2
  tail -n 200 "${LOG3}" >&2 || true
  echo "--- dotnet log ---" >&2
  tail -n 200 "${LOG4}" >&2 || true
  exit 11
fi

echo "[HSMS] PASS"
