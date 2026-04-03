#!/usr/bin/env bash
set -euo pipefail

# 更新 SDK 镜像内置的 secs_lib（不重建 sysroot 层）。
#
# 适用：sysroot 稳定，但 secs_lib 经常更新。
#
# 用法：
#   SECS_LIB_SRC=/path/to/secs_lib bash docker/update-secs-sdk.sh
# 或：
#   bash docker/update-secs-sdk.sh /path/to/secs_lib
#
# 可选：
#   BASE_IMAGE=secs-sdk:glibc226   # 作为底座的现有 SDK 镜像
#   IMAGE=secs-sdk:glibc226        # 输出镜像 tag（可与 BASE_IMAGE 相同，表示“原地更新”）

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 兼容两种目录结构：
# - 仓库根目录直接放脚本（本目录）
# - 仓库根目录下有 docker/ 目录（脚本位于 docker/）
if [[ -d "${SCRIPT_DIR}/sdk-assets" ]]; then
  PROJECT_ROOT="${SCRIPT_DIR}"
elif [[ -d "${SCRIPT_DIR}/../sdk-assets" ]]; then
  PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
else
  echo "ERROR: sdk-assets not found near script: ${SCRIPT_DIR}" >&2
  exit 1
fi

BASE_IMAGE="${BASE_IMAGE:-secs-sdk:glibc226}"
IMAGE="${IMAGE:-secs-sdk:glibc226}"

SECS_LIB_SRC="${SECS_LIB_SRC:-}"
if [[ $# -ge 1 ]]; then
  SECS_LIB_SRC="$1"
fi

if [[ -z "${SECS_LIB_SRC}" ]]; then
  echo "ERROR: SECS_LIB_SRC is required (path to secs_lib source root)" >&2
  exit 1
fi
if [[ ! -f "${SECS_LIB_SRC}/CMakeLists.txt" ]]; then
  echo "ERROR: SECS_LIB_SRC does not look like secs_lib source root: ${SECS_LIB_SRC}" >&2
  exit 1
fi

tmp_ctx="$(mktemp -d)"
cleanup() { rm -rf "${tmp_ctx}"; }
trap cleanup EXIT

cat > "${tmp_ctx}/Dockerfile" <<EOF
FROM ${BASE_IMAGE}

# SDK 基本路径由底座镜像提供；这里仅用于保证脚本可读性。
ENV SECS_SDK_ROOT=/opt/secs-sdk
ENV SECS_SDK_SYSROOT=/opt/secs-sdk/sysroot
ENV SECS_SDK_PREFIX=/opt/secs-sdk/arm-linux-gnueabihf
ENV SECS_SDK_TOOLCHAIN=/opt/secs-sdk/toolchains/arm-linux-gnueabihf-glibc226.cmake

COPY secs_lib.tar.gz /tmp/secs_lib.tar.gz
COPY sdk-assets/ /tmp/sdk-assets/

RUN set -eu; \\
  rm -rf "\${SECS_SDK_ROOT}/src/secs_lib"; \\
  mkdir -p "\${SECS_SDK_ROOT}/src/secs_lib"; \\
  tar -xzf /tmp/secs_lib.tar.gz -C "\${SECS_SDK_ROOT}/src/secs_lib"; \\
  test -f "\${SECS_SDK_ROOT}/src/secs_lib/CMakeLists.txt"; \\
  \\
  # 清理旧安装（仅清 secs_lib 相关，不动 glibc_compat / secs_sdk）\\
  find "\${SECS_SDK_PREFIX}/lib" -maxdepth 1 -type f -name 'libsecs_*.a' ! -name 'libsecs_glibc_compat.a' -delete 2>/dev/null || true; \\
  rm -rf "\${SECS_SDK_PREFIX}/include/secs" "\${SECS_SDK_PREFIX}/lib/cmake/secs" || true; \\
  \\
  # 同步 toolchain（保持与仓库版本一致）\\
  cp -f /tmp/sdk-assets/toolchains/arm-linux-gnueabihf-glibc226.cmake "\${SECS_SDK_TOOLCHAIN}"; \\
  \\
  # 生成/刷新 glibc_compat（后续编译 tools/ 可执行文件会用到）\\
  test -f /tmp/sdk-assets/src/glibc_compat.c; \\
  mkdir -p "\${SECS_SDK_PREFIX}/lib"; \\
  arm-linux-gnueabihf-gcc -c -O2 -fno-pic -o /tmp/glibc_compat.o /tmp/sdk-assets/src/glibc_compat.c; \\
  arm-linux-gnueabihf-ar rcs "\${SECS_SDK_PREFIX}/lib/libsecs_glibc_compat.a" /tmp/glibc_compat.o; \\
  rm -f /tmp/glibc_compat.o; \\
  test -f "\${SECS_SDK_PREFIX}/lib/libsecs_glibc_compat.a"; \\
  \\
  # 兼容不同 third_party/asio 布局\\
  ASIO1="\${SECS_SDK_ROOT}/src/secs_lib/third_party/asio/asio/include"; \\
  ASIO2="\${SECS_SDK_ROOT}/src/secs_lib/third_party/asio/asio/asio/include"; \\
  if [ -f "\${ASIO1}/asio.hpp" ] && [ -f "\${ASIO1}/asio/awaitable.hpp" ]; then \\
    SECS_ASIO_ROOT="\${ASIO1}"; \\
  elif [ -f "\${ASIO2}/asio.hpp" ] && [ -f "\${ASIO2}/asio/awaitable.hpp" ]; then \\
    SECS_ASIO_ROOT="\${ASIO2}"; \\
  else \\
    echo "ERROR: Asio headers not found under secs_lib/third_party/asio" >&2; \\
    exit 1; \\
  fi; \\
  \\
  cmake -S "\${SECS_SDK_ROOT}/src/secs_lib" -B /tmp/build-secs -G Ninja \\
    -DCMAKE_TOOLCHAIN_FILE="\${SECS_SDK_TOOLCHAIN}" \\
    -DCMAKE_SYSROOT="\${SECS_SDK_SYSROOT}" \\
    -DCMAKE_BUILD_TYPE=Release \\
    -DCMAKE_INSTALL_PREFIX="\${SECS_SDK_PREFIX}" \\
    -DBUILD_SHARED_LIBS=OFF \\
    -DSECS_ENABLE_INSTALL=ON \\
    -DSECS_ENABLE_TESTS=OFF \\
    -DSECS_ENABLE_INTEGRATION_TESTS=OFF \\
    -DSECS_BUILD_EXAMPLES=OFF \\
    -DSECS_BUILD_BENCHMARKS=OFF \\
    -DSECS_ENABLE_WERROR=OFF \\
    -DSECS_FETCH_ASIO=OFF \\
    -DSECS_ASIO_ROOT="\${SECS_ASIO_ROOT}" \\
    -DSECS_STATIC_CPP_RUNTIME=ON; \\
  cmake --build /tmp/build-secs --target install -v; \\
  rm -rf /tmp/build-secs; \\
  \\
  # 同步 SDK 胶水包与入口脚本（保持与仓库版本一致）\\
  mkdir -p "\${SECS_SDK_PREFIX}/lib/cmake/secs_sdk"; \\
  cp -f /tmp/sdk-assets/cmake/secs_sdkConfig.cmake "\${SECS_SDK_PREFIX}/lib/cmake/secs_sdk/secs_sdkConfig.cmake"; \\
  install -m 0755 /tmp/sdk-assets/bin/secs-build /usr/local/bin/secs-build; \\
  \\
  rm -rf /tmp/secs_lib.tar.gz /tmp/sdk-assets
EOF

mkdir -p "${tmp_ctx}/sdk-assets"
cp -R "${PROJECT_ROOT}/sdk-assets/." "${tmp_ctx}/sdk-assets/"

echo "== packaging secs_lib from: ${SECS_LIB_SRC} =="
tar -C "${SECS_LIB_SRC}" -czf "${tmp_ctx}/secs_lib.tar.gz" \
  --exclude='./.git' \
  --exclude='./build' \
  --exclude='./build_vendor' \
  --exclude='./.cache' \
  .

echo "== docker build: ${IMAGE} (base: ${BASE_IMAGE}) =="
docker build -t "${IMAGE}" "${tmp_ctx}"
