# secs_sdkConfig.cmake
#
# 提供一个“胶水包”，把 secs_lib（C API）与 glibc 兼容库组合为单一目标：
#   - secs_sdk::c_api = secs::c_api + secs_sdk::glibc_compat
#
# 这样业务工程只需：
#   find_package(secs_sdk CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE secs_sdk::c_api)

include(CMakeFindDependencyMacro)
find_dependency(secs CONFIG REQUIRED)

get_filename_component(_SECS_SDK_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_SECS_GLIBC_COMPAT_A "${_SECS_SDK_PREFIX}/lib/libsecs_glibc_compat.a")

if(NOT EXISTS "${_SECS_GLIBC_COMPAT_A}")
  message(FATAL_ERROR "secs glibc compat library not found: ${_SECS_GLIBC_COMPAT_A}")
endif()

if(NOT TARGET secs_sdk::glibc_compat)
  add_library(secs_sdk::glibc_compat STATIC IMPORTED GLOBAL)
  set_target_properties(secs_sdk::glibc_compat PROPERTIES
    IMPORTED_LOCATION "${_SECS_GLIBC_COMPAT_A}"
  )
endif()

if(NOT TARGET secs_sdk::c_api)
  add_library(secs_sdk::c_api INTERFACE IMPORTED GLOBAL)
  set_target_properties(secs_sdk::c_api PROPERTIES
    INTERFACE_LINK_LIBRARIES "secs::c_api;secs_sdk::glibc_compat"
  )
endif()
