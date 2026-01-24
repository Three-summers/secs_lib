#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-"${root_dir}/build"}"
out_dir="${OUT_DIR:-"${root_dir}/.codex"}"
timestamp="$(date +%Y%m%d_%H%M%S)"
out_file="${OUT_FILE:-"${out_dir}/secs_bench_${timestamp}.txt"}"

mkdir -p "${build_dir}"
mkdir -p "${out_dir}"

need_configure="0"
if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
  need_configure="1"
else
  if ! grep -q "SECS_BUILD_BENCHMARKS:BOOL=ON" "${build_dir}/CMakeCache.txt"; then
    need_configure="1"
  fi
fi

if [[ "${need_configure}" == "1" ]]; then
  spdlog_root=""
  if [[ -f "${root_dir}/third_party/spdlog/include/spdlog/spdlog.h" ]]; then
    spdlog_root="${root_dir}/third_party/spdlog/include"
  elif [[ -f "${root_dir}/build/_deps/secs_spdlog_fc-src/include/spdlog/spdlog.h" ]]; then
    spdlog_root="${root_dir}/build/_deps/secs_spdlog_fc-src/include"
  elif [[ -f "/usr/include/spdlog/spdlog.h" ]]; then
    spdlog_root="/usr/include"
  fi

  cmake_args=(
    -S "${root_dir}"
    -B "${build_dir}"
    -DCMAKE_BUILD_TYPE=Release
    -DSECS_BUILD_BENCHMARKS=ON
    -DSECS_FETCH_SPDLOG=OFF
  )
  if [[ -n "${spdlog_root}" ]]; then
    cmake_args+=("-DSECS_SPDLOG_ROOT=${spdlog_root}")
  fi

  cmake "${cmake_args[@]}"
fi

cmake --build "${build_dir}" --target benchmarks -j

{
  for exe in \
    bench_core_buffer \
    bench_secs2_codec \
    bench_hsms_message \
    bench_secs1_block \
    bench_sml_runtime \
    bench_tools_recording \
    bench_c_api_render_context \
    bench_protocol_router \
    bench_protocol_system_bytes; do
    echo
    echo "===== ${exe} ====="
    "${build_dir}/benchmarks/${exe}"
    echo "[done] ${exe}"
  done
} | tee "${out_file}"

echo
echo "Saved benchmark output to: ${out_file}"
