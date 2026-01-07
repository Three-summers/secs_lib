# 在纯 C 工程（tvoc_code 类项目）中集成 secs_lib（C ABI）

> 文档生成日期：2026-01-07  
> 执行者：Codex  
> 参考工程（仅只读分析，不修改）：`/home/say/code_bak/tvoc_code`  
> 目标：让“纯 C 工程”可以稳定使用 `secs_lib` 的 C ABI（`#include <secs/c_api.h>`），并给出从 Makefile 迁移到 CMake 的可落地做法。

---

## 0. 你要的结论（推荐路径）

1. **C 工程迁移到 CMake**（tvoc_code 的 Makefile → CMakeLists.txt）
2. 在 CMake 里用 `add_subdirectory()` 把 `secs_lib` 作为子项目引入
3. C 工程最终可执行文件 **强制使用 C++ 链接器**（关键点：`LINKER_LANGUAGE CXX`）

这样做的好处是：`secs_lib` 的依赖（Asio/Threads/spdlog 头文件等）和库之间的依赖关系由 CMake 自动处理，你不需要在 Makefile 里手工维护一长串 `*.a` 的链接顺序。

---

## 1. secs_lib 侧需要怎么“配置”（给 C 工程用）

### 1.1 你真正要给 C 工程暴露的东西

- 头文件：`include/secs/c_api.h`
- CMake target：`secs::c_api`（内部实现为 C++20，但对外提供 C ABI）

### 1.2 典型构建选项（建议）

如果 `secs_lib` 作为子项目引入（`add_subdirectory(secs_lib)`），建议在主工程里预先设置这些选项（避免把 tests/examples/bench 拉进产品固件）：

```cmake
set(SECS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SECS_ENABLE_INTEGRATION_TESTS OFF CACHE BOOL "" FORCE)
set(SECS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SECS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SECS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
```

### 1.3 无网络/内网环境（重点）

`secs_lib` 的 Asio/spdlog 依赖有三种来源：vendored / 外部指定 / FetchContent（需要网络）。

tvoc_code 这类交叉编译环境通常不允许联网，建议：

- 直接把 `secs_lib` 仓库完整带入（包含 `third_party/asio` 与 `third_party/spdlog`）
- **不要**启用 `SECS_FETCH_ASIO` / `SECS_FETCH_SPDLOG`
- 或者由主工程显式指定外部路径：
  - `-DSECS_ASIO_ROOT=/path/to/asio/include`
  - `-DSECS_SPDLOG_ROOT=/path/to/spdlog/include`

### 1.4 旧运行库/嵌入式环境：静态链接 C++ 运行库（可选但常用）

如果目标系统的 `libstdc++/libgcc` 版本偏旧，运行时可能加载失败；可在配置 `secs_lib` 时开启：

- `-DSECS_STATIC_CPP_RUNTIME=ON`：仅静态链接 C++ 运行库（推荐）
- `-DSECS_FULLY_STATIC=ON`：尽可能全静态（`-static`，对 glibc 环境可能不可用）

---

## 2. tvoc_code 现状扫描（为什么需要 CXX 链接）

基于 `tvoc_code/Makefile`（只读）可见：

- 编译器：`CC = arm-linux-gnueabihf-gcc`（纯 C）
- 链接库：`-lrt -lpthread -lm`
- 源文件：由大量 `src/**` 目录 `wildcard` 收集

而 `secs_lib` 的实现是 **C++20**（协程 + Asio）。即使你只 include 了 `secs/c_api.h`（纯 C），链接阶段也必须引入 C++ 运行库，因此必须使用 `arm-linux-gnueabihf-g++`（或在 CMake 里强制用 CXX 链接）。

---

## 3. 方案 A：把 tvoc_code 的 Makefile 改成 CMake（推荐）

下面给出“尽量贴近原 Makefile”的 CMake 写法，目标是先让工程跑起来，再逐步做结构化改造（例如拆分成多个 `add_library()` 模块）。

### 3.1 交叉编译 toolchain 文件（示例）

建议在 tvoc_code 工程里新增 `toolchains/arm-linux-gnueabihf.cmake`（示例）：

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# 如有 SDK/sysroot，可按你的环境补齐（示例）：
# set(CMAKE_SYSROOT /path/to/sysroot)
# set(CMAKE_FIND_ROOT_PATH /path/to/sysroot)
# set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
# set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

### 3.2 顶层 CMakeLists.txt（示例：复刻 Makefile 的收集逻辑）

```cmake
cmake_minimum_required(VERSION 3.20)
project(tvoc_code LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 1) 引入 secs_lib（建议作为子模块或源码拷贝放到 third_party/）
set(SECS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SECS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SECS_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SECS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/secs_lib)

# 2) 收集 tvoc_code 的 C 源文件（按原 Makefile 的 SRC_DIRS）
set(TVOC_SRC_DIRS
  src/Algorithm_App/Common_Algorithm_APP
  src/Algorithm_App/Filtering_Algorithm_APP
  src/Algorithm_App/Statis_Data_App
  src/BSP_App/Linux_BSP
  src/BSP_App/Linux_BSP/AXI_bsp/AXI_Intr_APP
  src/BSP_App/Linux_BSP/AXI_bsp/AXI_Param_APP
  src/BSP_App/Linux_BSP/ddr_bsp
  src/BSP_App/Linux_BSP/emmc_bsp
  src/BSP_App/Linux_BSP/gpio_bsp
  src/BSP_App/Linux_BSP/Timer_bsp
  src/BSP_App/Linux_BSP/serial_bsp
  src/BSP_App/FPGA_BSP
  src/BSP_App/FPGA_BSP/ps_out_ctrl
  src/BSP_App/FPGA_BSP/raw_data
  src/BSP_App/FPGA_BSP/signal_gen
  src/Watchdog_App
  src/Secs_App
  src/Log_App
  src/Conf_App
  src/HMI_App
  src/TVOC_App
  src/TVOC_App/TVOC_conf
  src/TVOC_App/Driver_IP_APP
  src/TVOC_App/Driver_IP_APP/AD76xx_driver_API
  src/TVOC_App/Driver_IP_APP/aq7pid_app
  src/TVOC_App/Driver_IP_APP/ps_out_ctrl_ex
  src/TVOC_App/Driver_IP_APP/PWM_driver_API
  src/TVOC_App/ADC_deal_app
  src/TVOC_App/SingalCtrl_app
  src/TVOC_App/TVOC_Timer_app
  src/TVOC_App/Valve_Ctrl_app
  src/TVOC_App/TVOC_Secs_App
  src/TVOC_App/TVOC_hmi_app
  src/TVOC_App/tvoc_func_app
)

set(TVOC_SOURCES main.c)
foreach(dir IN LISTS TVOC_SRC_DIRS)
  file(GLOB _dir_src CONFIGURE_DEPENDS "${dir}/*.c")
  list(APPEND TVOC_SOURCES ${_dir_src})
endforeach()

add_executable(run ${TVOC_SOURCES})

target_compile_options(run PRIVATE -Wall -Wextra -Wpedantic)

# 3) 链接：保留原工程的 -lrt -lpthread -lm，并加上 secs::c_api
find_package(Threads REQUIRED)
target_link_libraries(run PRIVATE rt m Threads::Threads secs::c_api)

# 4) 关键：纯 C 可执行程序，强制用 C++ 链接器（否则会缺 libstdc++/协程支持符号）
set_property(TARGET run PROPERTY LINKER_LANGUAGE CXX)

# 5) （可选）某些 32-bit ARM 工具链对 64-bit atomic 需要额外链接 -latomic
# 如遇到 __atomic_* undefined reference，再打开这一行：
# target_link_libraries(run PRIVATE atomic)
```

### 3.3 构建命令（示例）

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 3.4 常见报错与定位

- **链接期 undefined reference（大量 C++ 符号）**  
  - 现象：用 `gcc` 链接导致缺 `libstdc++`/协程/异常运行库符号  
  - 处理：确保 `LINKER_LANGUAGE CXX` 生效，或确保最终链接器是 `arm-linux-gnueabihf-g++`

- **Asio/spdlog not found**  
  - 处理：保证 `third_party/secs_lib/third_party/asio` 与 `third_party/spdlog` 随仓库带入；或通过 `SECS_ASIO_ROOT/SECS_SPDLOG_ROOT` 指向外部头文件目录  
  - 避免：不要依赖 FetchContent（无网络环境会失败）

- **`__atomic_*` undefined reference（常见于 32-bit ARM）**  
  - 处理：在最终可执行文件上加 `-latomic`（CMake：`target_link_libraries(run PRIVATE atomic)`）

---

## 4. 方案 B：先安装 secs_lib，再用 find_package(secs CONFIG)（可选）

适用于：你不想把 `secs_lib` 作为源码塞进 tvoc_code，而是把它当作“预编译依赖”。

### 4.1 在 secs_lib 侧交叉编译并安装到 staging 前缀

```bash
cmake -S /path/to/secs_lib -B build-secs \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DSECS_ENABLE_INSTALL=ON \
  -DSECS_ENABLE_TESTS=OFF \
  -DSECS_BUILD_EXAMPLES=OFF
cmake --build build-secs -j
cmake --install build-secs --prefix /abs/path/to/stage/secs
```

### 4.2 在 tvoc_code 的 CMake 里消费

```cmake
list(APPEND CMAKE_PREFIX_PATH "/abs/path/to/stage/secs")
find_package(secs CONFIG REQUIRED)

add_executable(run ${TVOC_SOURCES})
target_link_libraries(run PRIVATE rt m Threads::Threads secs::c_api)
set_property(TARGET run PROPERTY LINKER_LANGUAGE CXX)
```

---

## 5. 方案 C：保持 Makefile（不推荐，但可作为过渡）

核心原则不变：**最终链接必须用 `g++`**，并处理好库依赖顺序。

建议过渡做法：

1. 用 CMake 把 `secs_lib` 交叉编译出来（`build-secs/`），得到一组 `libsecs_*.a` 或 `libsecs_*.so`
2. 把 `include/`（至少 `include/secs/c_api.h`）和库文件拷贝进 tvoc_code 的第三方目录
3. 修改 tvoc_code 的链接命令：
   - 把 `$(CC)` 链接改成 `$(CXX)=arm-linux-gnueabihf-g++`
   - 增加 `-L...` 指向 secs_lib 产物目录
   - 增加 `-lsecs_c_api ...`（静态库场景推荐用 `--start-group/--end-group` 包住，避免顺序问题）

> 说明：静态库依赖顺序维护成本很高，因此仍然推荐尽快迁移到 CMake 的“方案 A/B”。

