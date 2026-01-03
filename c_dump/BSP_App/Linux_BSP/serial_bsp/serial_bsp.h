#ifndef SERIAL_BSP_H
#define SERIAL_BSP_H

#include <stddef.h>

/*
 * c_dump 参考代码的最小依赖 stub（仅用于本仓库单元测试编译）。
 *
 * 说明：
 * - 原始工程在 Linux BSP 层提供 serial_listener_t 及其相关 API。
 * - 本仓库只需要编译/调用 c_dump/Secs_App/secs_II.c（SECS-II 组包），
 *   该模块只依赖类型声明即可，不依赖具体实现。
 *
 * 注意：不要在这里实现任何业务逻辑；该文件不是库对外 API。
 */
typedef struct serial_listener_t serial_listener_t;

#endif

