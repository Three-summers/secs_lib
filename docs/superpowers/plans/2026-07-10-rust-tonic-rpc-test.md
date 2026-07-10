# Rust tonic RPC Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a permanent Rust `tonic + prost` black-box integration suite that validates the RPC server across contract, lifecycle, messaging, failure, timeout, and concurrency paths.

**Architecture:** A standalone Cargo binary generates clients from the repository proto, starts the C++ RPC server and HSMS peer as child processes, and drives all three RPC services over HTTP/2. CMake registers the binary through CTest only for RPC integration builds; deterministic peer delay/drop options make timeout and pending-limit tests reproducible.

**Tech Stack:** C++20, CMake/CTest, Rust 1.91, Tokio 1.48, tonic 0.14.5, tonic-prost 0.14.5, prost 0.14.3, proto2, brpc, HSMS.

## Global Constraints

- Keep the production RPC contract unchanged unless a failing Rust test proves a server defect.
- Every application success asserts `status.is_some()` and `status.ok == Some(true)`.
- Every application error asserts status presence, `ok == Some(false)`, error presence, and the intended failure reason.
- Validation tests use an existing session whenever lookup precedes validation.
- All child processes and waits have bounded timeouts and deterministic cleanup.
- The current sandbox exposes `.git` read-only, so implementation verification is required but commits cannot be created in this session.

---

### Task 1: Re-establish the RPC baseline

**Files:**
- No source changes.

**Interfaces:**
- Consumes: existing CMake RPC targets and tests.
- Produces: a clean, freshly configured baseline without stale removed test targets.

- [ ] **Step 1: Reconfigure the RPC build**

```bash
cmake -S . -B build_rpc \
  -DSECS_ENABLE_RPC=ON \
  -DSECS_ENABLE_TESTS=ON \
  -DSECS_ENABLE_INTEGRATION_TESTS=ON \
  -DSECS_BUILD_TOOLS=ON
```

Expected: configure succeeds and stale targets from the reverted test batch disappear from `ctest -N`.

- [ ] **Step 2: Build the existing RPC executables**

```bash
cmake --build build_rpc --target secs-rpc-server test_rpc_hsms_peer test_rpc_internal test_rpc_smoke -j2
```

Expected: all four targets build successfully.

- [ ] **Step 3: Run the existing RPC baseline**

```bash
ctest --test-dir build_rpc --output-on-failure -R 'rpc_(internal|smoke|python_interop)'
```

Expected: existing tests pass or Python interop is explicitly skipped with return code 77.

---

### Task 2: Create the Rust proto client

**Files:**
- Create: `integration_tests/rpc/rust_tonic_client/Cargo.toml`
- Create: `integration_tests/rpc/rust_tonic_client/build.rs`
- Create: `integration_tests/rpc/rust_tonic_client/src/main.rs`
- Create: `integration_tests/rpc/rust_tonic_client/Cargo.lock` via Cargo.

**Interfaces:**
- Consumes: `src/rpc/proto/secs/rpc/v1/secs_rpc.proto`.
- Produces: generated modules `library_service_client`, `session_service_client`, and `messaging_service_client` under `secs::rpc::v1`.

- [ ] **Step 1: Add a compile-only Rust client that references all three generated clients**

`Cargo.toml`:

```toml
[package]
name = "secs-rpc-tonic-integration"
version = "0.1.0"
edition = "2024"
publish = false

[dependencies]
anyhow = "1.0.100"
prost = "0.14.3"
tokio = { version = "1.48.0", features = ["io-util", "macros", "net", "process", "rt-multi-thread", "sync", "time"] }
tonic = "0.14.5"
tonic-prost = "0.14.5"

[build-dependencies]
tonic-prost-build = "0.14.5"
```

`build.rs`:

```rust
use std::{env, path::PathBuf};

fn main() {
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap())
        .join("../../..");
    let proto_root = root.join("src/rpc/proto");
    let proto = proto_root.join("secs/rpc/v1/secs_rpc.proto");
    println!("cargo:rerun-if-changed={}", proto.display());
    tonic_prost_build::configure()
        .build_server(false)
        .compile_protos(&[proto], &[proto_root])
        .expect("compile secs RPC proto");
}
```

Initial `src/main.rs`:

```rust
mod rpc {
    tonic::include_proto!("secs.rpc.v1");
}

fn main() {
    let _ = std::any::type_name::<rpc::library_service_client::LibraryServiceClient<tonic::transport::Channel>>();
    let _ = std::any::type_name::<rpc::session_service_client::SessionServiceClient<tonic::transport::Channel>>();
    let _ = std::any::type_name::<rpc::messaging_service_client::MessagingServiceClient<tonic::transport::Channel>>();
}
```

- [ ] **Step 2: Run Cargo and verify code generation**

```bash
cargo check --manifest-path integration_tests/rpc/rust_tonic_client/Cargo.toml
```

Expected: first run creates `Cargo.lock`; compile succeeds with all three generated clients.

---

### Task 3: Add process orchestration and contract/lifecycle assertions

**Files:**
- Modify: `integration_tests/rpc/rust_tonic_client/src/main.rs`

**Interfaces:**
- Consumes CLI: `--server <path> --peer <path>`.
- Produces helpers `TestProcesses`, `expect_ok`, `expect_error`, `create_session`, and `wait_for_selected`.

- [ ] **Step 1: Write a failing invocation test**

```rust
#[derive(Debug)]
struct Args { server: PathBuf, peer: PathBuf }

fn parse_args() -> Result<Args> {
    // Parse exactly --server <path> and --peer <path>; reject unknown or missing arguments.
}
```

Run the binary without arguments. Expected: non-zero exit naming both required options.

- [ ] **Step 2: Implement bounded child-process management**

```rust
struct TestProcesses {
    server: tokio::process::Child,
    peer: Option<tokio::process::Child>,
}

impl TestProcesses {
    async fn start(server: &Path, peer: Option<(&Path, &[&str])>)
        -> Result<(Self, String, Option<u16>)>;
    async fn stop(mut self) -> Result<()>;
}

fn pick_port() -> Result<u16>;
async fn connect_with_retry(endpoint: String, timeout: Duration) -> Result<Channel>;
```

`Drop` calls `start_kill()` for remaining children. `stop()` waits at most five seconds per child.

- [ ] **Step 3: Implement strict status helpers**

```rust
fn expect_ok(status: Option<RpcStatus>, what: &str) -> Result<()> {
    let status = status.ok_or_else(|| anyhow!("{what}: missing RpcStatus"))?;
    ensure!(status.ok == Some(true), "{what}: non-OK status: {status:?}");
    Ok(())
}

fn expect_error(status: Option<RpcStatus>, what: &str, message: &str) -> Result<RpcError> {
    let status = status.ok_or_else(|| anyhow!("{what}: missing RpcStatus"))?;
    ensure!(status.ok == Some(false), "{what}: unexpectedly succeeded: {status:?}");
    let error = status.error.ok_or_else(|| anyhow!("{what}: missing RpcError"))?;
    ensure!(error.message.as_deref().unwrap_or_default().contains(message),
            "{what}: wrong error: {error:?}");
    Ok(error)
}
```

- [ ] **Step 4: Add contract and lifecycle scenarios**

```rust
async fn test_library_contract(channel: Channel) -> Result<()>;
async fn test_validation_errors(channel: Channel) -> Result<()>;
async fn test_session_lifecycle(channel: Channel, peer_port: u16) -> Result<String>;
```

Cover library features, invalid transport values, not-found operations, create/get/list/start/start-again/stop/stop-again/delete/get-after-delete, and `selected_generation > 0`.

- [ ] **Step 5: Run the Rust suite**

```bash
cargo run --manifest-path integration_tests/rpc/rust_tonic_client/Cargo.toml -- \
  --server build_rpc/tools/secs-rpc-server \
  --peer build_rpc/tests/test_rpc_hsms_peer
```

Expected: contract and lifecycle scenarios pass; failures name the exact RPC and response status.

---

### Task 4: Add real messaging and ItemNode coverage

**Files:**
- Modify: `integration_tests/rpc/rust_tonic_client/src/main.rs`

**Interfaces:**
- Consumes: an active selected session.
- Produces: `test_send_and_request`, `test_all_item_types`, and ItemNode constructors/assertions.

- [ ] **Step 1: Add message helpers and assertions**

```rust
fn item(item_type: ItemType) -> ItemNode;
fn ping_item(sequence: u32) -> ItemNode;
fn all_types_item() -> ItemNode;
fn assert_echo_reply(reply: &MessageEnvelope, sequence: u32) -> Result<()>;
```

The S1F1 request is `[ASCII "PING", U4 sequence]`. The expected S1F2 is `[ASCII "ACK", original-list]`.

- [ ] **Step 2: Prove the reply assertion detects bad content**

Temporarily require `ACK-INVALID`, run the suite, and confirm the decoded reply assertion fails. Restore `ACK`.

- [ ] **Step 3: Cover every ItemNode type**

`all_types_item()` includes binary, boolean, in-range min/max I1/I2/I4/I8 and U1/U2/U4/U8, representative F4/F8, ASCII, and nested list values. Echo it through S1F1 and compare every field.

- [ ] **Step 4: Run the messaging suite**

Expected: `Send` returns a present accepted envelope; `Request` returns S1F2 with non-empty body and exact decoded content.

---

### Task 5: Make timeout and concurrency scenarios deterministic

**Files:**
- Modify: `tests/test_rpc_hsms_peer.cpp`
- Modify: `integration_tests/rpc/rust_tonic_client/src/main.rs`

**Interfaces:**
- Peer CLI produces: `--reply-delay-ms <u32>` and `--drop-s1f1`.
- Rust consumes those modes in isolated process groups.

- [ ] **Step 1: Add Rust launches that fail with the current peer CLI**

Launch with `--reply-delay-ms 500` and `--drop-s1f1`. Expected before peer changes: peer exits with `unknown option`.

- [ ] **Step 2: Extend peer option parsing**

```cpp
std::uint32_t reply_delay_ms{0};
bool drop_s1f1{false};
```

Parse the delay with full `std::uint32_t` validation. Pass options into the S1F1 handler, delay with an Asio steady timer, and suppress a reply in drop mode using the protocol's supported handler result. Default invocation remains the current immediate echo.

- [ ] **Step 3: Add fault and timeout scenarios**

```rust
async fn test_application_timeout(args: &Args) -> Result<()>;
async fn test_tonic_deadline(args: &Args) -> Result<()>;
async fn test_unavailable_server(args: &Args) -> Result<()>;
async fn test_peer_disconnect(args: &Args) -> Result<()>;
```

Application timeout requires present non-OK `RpcStatus`. With tonic 0.14, the client-local deadline requires `tonic::Code::Cancelled` and message `Timeout expired`. Server exit and unused-port cases require transport errors.

- [ ] **Step 4: Add concurrency and stop-race scenarios**

```rust
async fn test_parallel_reads(channel: Channel) -> Result<()>;
async fn test_parallel_requests(args: &Args) -> Result<()>;
async fn test_pending_limit(args: &Args) -> Result<()>;
async fn test_stop_with_pending_request(args: &Args) -> Result<()>;
```

Run 32 read RPCs, at least 8 tagged requests, a `max_pending_requests = 1` case with delayed replies, and a stop while a dropped request is pending. Bound every future with `tokio::time::timeout`.

- [ ] **Step 5: Verify default and controlled peer behavior**

```bash
cmake --build build_rpc --target test_rpc_hsms_peer -j2
ctest --test-dir build_rpc --output-on-failure -R rpc_python_interop
cargo run --manifest-path integration_tests/rpc/rust_tonic_client/Cargo.toml -- \
  --server build_rpc/tools/secs-rpc-server \
  --peer build_rpc/tests/test_rpc_hsms_peer
```

Expected: Python interop still passes; Rust fault and concurrency scenarios pass.

---

### Task 6: Register CTest and complete verification

**Files:**
- Modify: `integration_tests/CMakeLists.txt`
- Modify: `integration_tests/README.md`
- Modify only if a proven defect exists: `src/rpc/server.cpp` plus its Rust regression scenario.

**Interfaces:**
- Produces CTest name: `rpc_rust_tonic_interop`.

- [ ] **Step 1: Register Cargo with CTest**

```cmake
find_program(SECS_CARGO_EXECUTABLE cargo)
if(SECS_ENABLE_RPC AND SECS_CARGO_EXECUTABLE
   AND TARGET secs-rpc-server AND TARGET test_rpc_hsms_peer)
  add_test(NAME rpc_rust_tonic_interop
    COMMAND ${SECS_CARGO_EXECUTABLE} run --locked --quiet
      --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/rpc/rust_tonic_client/Cargo.toml
      --
      --server $<TARGET_FILE:secs-rpc-server>
      --peer $<TARGET_FILE:test_rpc_hsms_peer>
  )
  set_tests_properties(rpc_rust_tonic_interop PROPERTIES TIMEOUT 120)
endif()
```

- [ ] **Step 2: Document the Rust integration command**

Update `integration_tests/README.md` with prerequisites, CMake options, CTest regex, and direct Cargo invocation.

- [ ] **Step 3: Run the complete RPC matrix**

```bash
cmake -S . -B build_rpc \
  -DSECS_ENABLE_RPC=ON \
  -DSECS_ENABLE_TESTS=ON \
  -DSECS_ENABLE_INTEGRATION_TESTS=ON \
  -DSECS_BUILD_TOOLS=ON
cmake --build build_rpc -j2
ctest --test-dir build_rpc --output-on-failure \
  -R 'rpc_(internal|smoke|python_interop|rust_tonic_interop)'
```

Expected: zero failed RPC tests.

- [ ] **Step 4: Repeat the Rust black-box suite**

```bash
ctest --test-dir build_rpc --output-on-failure \
  -R rpc_rust_tonic_interop --repeat until-fail:10
```

Expected: ten consecutive passes with no timeout or leftover child processes.

- [ ] **Step 5: Run source and workspace checks**

```bash
cargo fmt --manifest-path integration_tests/rpc/rust_tonic_client/Cargo.toml -- --check
cargo clippy --manifest-path integration_tests/rpc/rust_tonic_client/Cargo.toml -- -D warnings
git diff --check
git status --short
```

Expected: format, Clippy, and diff checks succeed; status lists only intended files.
