# Docker SDK 镜像（内置 sysroot + secs_lib + glibc_compat）

日期：2026-01-15（Codex）

## 适用场景
- 你有多个“类似项目”需要交叉编译到同一目标板（ARM + glibc 2.26）
- 不希望每个项目都重复指定 `SYSROOT`、`SECS_LIB_ROOT`，也不希望每个项目都拷贝 `glibc_compat.c`

## 镜像内容（固定路径）
- sysroot（glibc 2.26）：`/opt/secs-sdk/sysroot`
- SDK 安装前缀（安装 secs_lib 与胶水包）：`/opt/secs-sdk/arm-linux-gnueabihf`
- toolchain file：`/opt/secs-sdk/toolchains/arm-linux-gnueabihf-glibc226.cmake`
- 构建入口：`secs-build`（容器内命令）

## 1) 构建 SDK 镜像（只需做一次）

宿主机执行（需要 Docker，且构建镜像阶段需要网络访问 apt 源）：

```bash
export SYSROOT_SRC=/path/to/recipe-sysroot
export SECS_LIB_SRC=/path/to/secs_lib
export IMAGE=secs-sdk:glibc226

bash ./build-sdk-image.sh
```

说明：
- 该脚本会把 `SYSROOT_SRC` 与 `SECS_LIB_SRC` 打包为 tar.gz 并在镜像构建时解压到固定路径。
- 镜像会比较大（sysroot 很大），但后续多个项目可直接复用。

## 2) 在任意项目中使用

### 2.1 根 CMakeLists.txt 骨架
- 直接参考：`templates/cmake-root/CMakeLists.txt`
- 关键点：
  - `find_package(secs_sdk CONFIG REQUIRED)`
  - `target_link_libraries(app PRIVATE secs_sdk::c_api)`
  - 纯 C 项目需要 `set_property(TARGET app PROPERTY LINKER_LANGUAGE CXX)`

### 2.2 编译（不再需要指定 SYSROOT/SECS_LIB）

在项目根目录执行：

```bash
docker run --rm -t \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/work/src" \
  -w /work/src \
  secs-sdk:glibc226 \
  secs-build
```

默认输出：`build-arm/bin/<你的目标>`

可选环境变量：
- `BUILD_DIR=/work/src/build-arm-release`
- `BUILD_TYPE=Release`
- `TARGET=run`

## 3) 更新 secs_lib（经常更新时推荐）

sysroot 通常很稳定，但 `secs_lib` 可能经常更新；此时不建议每次都重建包含 sysroot 的整张镜像。

你可以基于现有 SDK 镜像“原地更新” secs_lib：

```bash
export SECS_LIB_SRC=/path/to/secs_lib
export BASE_IMAGE=secs-sdk:glibc226
export IMAGE=secs-sdk:glibc226

bash ./update-secs-sdk.sh
```

说明：
- `SECS_LIB_SRC` 可以指向任意一个 `secs_lib` 工作目录（经常切换/更新都没问题）。
- `BASE_IMAGE` 与 `IMAGE` 可以相同（覆盖 tag），也可以输出新 tag（例如带日期/commit）。
