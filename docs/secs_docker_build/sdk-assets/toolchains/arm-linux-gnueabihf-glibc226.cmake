set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# SDK toolchain file（用于“sysroot 已烘焙进镜像”的场景）。
# - 默认 sysroot: /opt/secs-sdk/sysroot（glibc 2.26）
# - 默认 prefix: /opt/secs-sdk/arm-linux-gnueabihf（安装 secs_lib + secs_sdk）

if(NOT DEFINED CMAKE_C_COMPILER OR "${CMAKE_C_COMPILER}" STREQUAL "")
  set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER OR "${CMAKE_CXX_COMPILER}" STREQUAL "")
  set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
endif()

set(_SECS_SDK_SYSROOT_DEFAULT "/opt/secs-sdk/sysroot")
if(DEFINED ENV{SECS_SDK_SYSROOT} AND NOT "$ENV{SECS_SDK_SYSROOT}" STREQUAL "")
  set(_SECS_SDK_SYSROOT_DEFAULT "$ENV{SECS_SDK_SYSROOT}")
endif()

if(NOT DEFINED CMAKE_SYSROOT OR "${CMAKE_SYSROOT}" STREQUAL "")
  set(CMAKE_SYSROOT "${_SECS_SDK_SYSROOT_DEFAULT}" CACHE PATH "glibc sysroot" FORCE)
endif()
if(NOT EXISTS "${CMAKE_SYSROOT}/lib/libc.so.6")
  message(FATAL_ERROR "CMAKE_SYSROOT does not look like a glibc sysroot: ${CMAKE_SYSROOT}")
endif()

set(_SECS_SDK_PREFIX_DEFAULT "/opt/secs-sdk/arm-linux-gnueabihf")
if(DEFINED ENV{SECS_SDK_PREFIX} AND NOT "$ENV{SECS_SDK_PREFIX}" STREQUAL "")
  set(_SECS_SDK_PREFIX_DEFAULT "$ENV{SECS_SDK_PREFIX}")
endif()
if(NOT EXISTS "${_SECS_SDK_PREFIX_DEFAULT}")
  message(FATAL_ERROR "SECS SDK prefix not found: ${_SECS_SDK_PREFIX_DEFAULT}")
endif()

# 限制 CMake 查找库/头文件的根目录，避免误命中宿主机（x86_64）环境。
# 同时把 SDK prefix 纳入查找根目录，支持 find_package(secs CONFIG REQUIRED) / find_package(secs_sdk CONFIG REQUIRED)。
set(CMAKE_FIND_ROOT_PATH
  "${_SECS_SDK_PREFIX_DEFAULT}"
  "${CMAKE_SYSROOT}"
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(_SECS_SYSROOT_LIB_DIR "${CMAKE_SYSROOT}/lib")
set(_SECS_SYSROOT_USR_LIB_DIR "${CMAKE_SYSROOT}/usr/lib")

foreach(_p
  "${_SECS_SYSROOT_USR_LIB_DIR}/Scrt1.o"
  "${_SECS_SYSROOT_USR_LIB_DIR}/crti.o"
  "${_SECS_SYSROOT_USR_LIB_DIR}/crtn.o"
)
  if(NOT EXISTS "${_p}")
    message(FATAL_ERROR "Missing required startup file in sysroot: ${_p}")
  endif()
endforeach()

# 交叉场景下，sysroot 可能自带一份较旧的 libstdc++（例如 GCC 8），而编译器本身是较新版本（例如 GCC 11）。
# 如果把 sysroot 的 -L 放在最前面，则 -static-libstdc++ 会优先选中旧 libstdc++.a，进而出现缺符号的问题。
#
# 解决策略：把“编译器自带的 libstdc++.a 所在目录”放到 -L 的最前面；同时仍保持 sysroot 的 -L
# 在宿主机交叉 sysroot（/usr/arm-linux-gnueabihf/lib 等）之前，以约束 glibc 版本。
execute_process(
  COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.a
  OUTPUT_VARIABLE _SECS_LIBSTDCXX_A
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if("${_SECS_LIBSTDCXX_A}" STREQUAL "" OR "${_SECS_LIBSTDCXX_A}" STREQUAL "libstdc++.a" OR NOT EXISTS "${_SECS_LIBSTDCXX_A}")
  message(FATAL_ERROR "Failed to locate libstdc++.a via ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a (got: '${_SECS_LIBSTDCXX_A}')")
endif()
get_filename_component(_SECS_GCC_LIB_DIR "${_SECS_LIBSTDCXX_A}" DIRECTORY)

execute_process(
  COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=crtbeginS.o
  OUTPUT_VARIABLE _SECS_CRTBEGIN_S
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=crtendS.o
  OUTPUT_VARIABLE _SECS_CRTEND_S
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if("${_SECS_CRTBEGIN_S}" STREQUAL "" OR NOT EXISTS "${_SECS_CRTBEGIN_S}")
  message(FATAL_ERROR "Failed to locate crtbeginS.o via ${CMAKE_CXX_COMPILER}")
endif()
if("${_SECS_CRTEND_S}" STREQUAL "" OR NOT EXISTS "${_SECS_CRTEND_S}")
  message(FATAL_ERROR "Failed to locate crtendS.o via ${CMAKE_CXX_COMPILER}")
endif()

set(_SECS_LINK_FLAGS_COMMON
  "-Wl,--sysroot=${CMAKE_SYSROOT}"
  "-Wl,-dynamic-linker,/lib/ld-linux-armhf.so.3"
  "-L${_SECS_GCC_LIB_DIR}"
  "-L${_SECS_SYSROOT_LIB_DIR}"
  "-L${_SECS_SYSROOT_USR_LIB_DIR}"
)

string(JOIN " " CMAKE_EXE_LINKER_FLAGS_INIT
  ${_SECS_LINK_FLAGS_COMMON}
  "-nostartfiles"
  "${_SECS_SYSROOT_USR_LIB_DIR}/Scrt1.o"
  "${_SECS_SYSROOT_USR_LIB_DIR}/crti.o"
  "${_SECS_CRTBEGIN_S}"
)

string(JOIN " " _SECS_TAIL_OBJS
  "${_SECS_CRTEND_S}"
  "${_SECS_SYSROOT_USR_LIB_DIR}/crtn.o"
)

set(_SECS_GLIBC_COMPAT_A "${_SECS_SDK_PREFIX_DEFAULT}/lib/libsecs_glibc_compat.a")
if(NOT EXISTS "${_SECS_GLIBC_COMPAT_A}")
  message(FATAL_ERROR "secs glibc compat library not found: ${_SECS_GLIBC_COMPAT_A}")
endif()
set(_SECS_GLIBC_COMPAT_WHOLE_ARCHIVE "-Wl,--whole-archive ${_SECS_GLIBC_COMPAT_A} -Wl,--no-whole-archive")

set(CMAKE_C_LINK_EXECUTABLE
  "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${_SECS_GLIBC_COMPAT_WHOLE_ARCHIVE} -lpthread ${_SECS_TAIL_OBJS}"
)
set(CMAKE_CXX_LINK_EXECUTABLE
  "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${_SECS_GLIBC_COMPAT_WHOLE_ARCHIVE} -lpthread ${_SECS_TAIL_OBJS}"
)

# 交叉编译下，try_compile 默认会尝试链接可执行文件；有些工程在配置阶段会失败/卡住。
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
