/*
 * glibc_compat.c
 *
 * 目的：在旧版 glibc（例如 2.26）环境下使用较新 GCC/libstdc++ 时，补齐少量兼容符号。
 *
 * 背景：
 * - libstdc++ 可能引用 glibc 2.32 引入的全局变量 `__libc_single_threaded`；
 * - 当目标 sysroot 的 glibc 版本较低（如 2.26）时，该符号不存在，会导致链接失败或运行时解析失败。
 *
 * 策略：
 * - 面向目标架构提供该符号，并标记为 weak，避免与未来 glibc 自带实现冲突。
 */

#if defined(__linux__) && (defined(__arm__) || defined(__thumb__) || defined(__aarch64__))
__attribute__((visibility("default"), weak)) char __libc_single_threaded = 0;
#else
static const int k_secs_glibc_compat_dummy = 0;
#endif
