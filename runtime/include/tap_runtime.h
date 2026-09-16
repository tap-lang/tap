#ifndef TAP_RUNTIME_H
#define TAP_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

// Shared-library builds export the stable C ABI on Windows.
#if defined(_WIN32) && defined(TAP_RUNTIME_SHARED)
#define TAP_RUNTIME_API __declspec(dllexport)
#else
#define TAP_RUNTIME_API
#endif

TAP_RUNTIME_API int32_t __tap_read_key(void);

TAP_RUNTIME_API int32_t __tap_sleep_ms(int32_t milliseconds);

TAP_RUNTIME_API int32_t __tap_clear_screen(void);

TAP_RUNTIME_API int32_t __tap_random(int32_t maximum);

TAP_RUNTIME_API void __tap_init_args(int32_t argc, char **argv);
TAP_RUNTIME_API int32_t __tap_argc(void);
TAP_RUNTIME_API const char *__tap_arg(int32_t index);

TAP_RUNTIME_API const char *__tap_env_var(const char *name);
TAP_RUNTIME_API int32_t __tap_envc(void);
TAP_RUNTIME_API const char *__tap_env(int32_t index);

// 按 UTF-8 原始字节读取字符串；越界时终止程序。
TAP_RUNTIME_API uint8_t __tap_string_byte_at(const char *value, int64_t index);
// 返回 [start, end) 字节区间的新字符串；非法边界时终止程序。
TAP_RUNTIME_API const char *__tap_string_slice(
    const char *value, int64_t start, int64_t end);
// 按 UTF-8 编码字节序比较两个字符串。
TAP_RUNTIME_API int32_t __tap_string_compare(const char *left, const char *right);

// 分配堆内存；size 为 0 时 Runtime 会按 1 字节处理。
TAP_RUNTIME_API void *__tap_malloc(size_t size);
// 调整堆内存大小；size 为 0 时 Runtime 会按 1 字节处理。
TAP_RUNTIME_API void *__tap_realloc(void *pointer, size_t size);
// 释放 Runtime 分配的堆内存；NULL 指针安全无操作，返回 0。
TAP_RUNTIME_API int32_t __tap_free(void *pointer);

#endif
