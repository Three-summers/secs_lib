#!/usr/bin/env bash
set -euo pipefail

# 构建“内置 sysroot + secs_lib + glibc_compat”的 SDK 镜像（一次构建，多项目复用）。
#
# 依赖：
# - docker（需要宿主机具备网络以安装 apt 依赖）
# - SYSROOT_SRC：PetaLinux/Yocto 导出的 recipe-sysroot（glibc 2.26）
# - SECS_LIB_SRC：secs_lib 源码根目录（包含 CMakeLists.txt）
#
# 示例：
#   SYSROOT_SRC=/path/to/recipe-sysroot \
#   SECS_LIB_SRC=/path/to/secs_lib \
#   IMAGE=iib-secs-sdk:glibc226 \
#   bash docker/build-sdk-image.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 兼容两种目录结构：
# - 仓库根目录直接放脚本（本目录）
# - 仓库根目录下有 docker/ 目录（脚本位于 docker/）
if [[ -f "${SCRIPT_DIR}/Dockerfile.sdk" && -d "${SCRIPT_DIR}/sdk-assets" ]]; then
  PROJECT_ROOT="${SCRIPT_DIR}"
elif [[ -f "${SCRIPT_DIR}/../Dockerfile.sdk" && -d "${SCRIPT_DIR}/../sdk-assets" ]]; then
  PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
else
  echo "ERROR: Dockerfile.sdk / sdk-assets not found near script: ${SCRIPT_DIR}" >&2
  exit 1
fi

SYSROOT_SRC="${SYSROOT_SRC:-}"
SECS_LIB_SRC="${SECS_LIB_SRC:-}"
IMAGE="${IMAGE:-secs-sdk:glibc226}"

if [[ -z "${SYSROOT_SRC}" ]]; then
  echo "ERROR: SYSROOT_SRC is required (path to recipe-sysroot)" >&2
  exit 1
fi
if [[ -z "${SECS_LIB_SRC}" ]]; then
  echo "ERROR: SECS_LIB_SRC is required (path to secs_lib source root)" >&2
  exit 1
fi
if [[ ! -f "${SYSROOT_SRC}/lib/libc.so.6" ]]; then
  echo "ERROR: SYSROOT_SRC does not look like a glibc sysroot: ${SYSROOT_SRC}" >&2
  exit 1
fi
if [[ ! -f "${SECS_LIB_SRC}/CMakeLists.txt" ]]; then
  echo "ERROR: SECS_LIB_SRC does not look like secs_lib source root: ${SECS_LIB_SRC}" >&2
  exit 1
fi

tmp_ctx="$(mktemp -d)"
cleanup() { rm -rf "${tmp_ctx}"; }
trap cleanup EXIT

cp -f "${PROJECT_ROOT}/Dockerfile.sdk" "${tmp_ctx}/Dockerfile"
mkdir -p "${tmp_ctx}/sdk-assets"
cp -R "${PROJECT_ROOT}/sdk-assets/." "${tmp_ctx}/sdk-assets/"

echo "== packaging sysroot (may take a while) =="
tar -C "${SYSROOT_SRC}" -czf "${tmp_ctx}/sysroot.tar.gz" .

echo "== packaging secs_lib =="
tar -C "${SECS_LIB_SRC}" -czf "${tmp_ctx}/secs_lib.tar.gz" \
  --exclude='./.git' \
  --exclude='./build' \
  --exclude='./build_vendor' \
  --exclude='./.cache' \
  .

echo "== docker build: ${IMAGE} =="
docker build -t "${IMAGE}" "${tmp_ctx}"
