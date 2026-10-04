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

// 把 Windows 控制台切到 UTF-8 代码页，避免 UTF-8 输出被按 GBK 解释成乱码。
// 其他平台是空操作。由生成的入口 main 在最开始调用。
// TAP_RUNTIME_API void __tap_init_console(void);

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
// 把字符串的指定字节数复制到调用方提供的缓冲区；调用方保证两端空间足够。
TAP_RUNTIME_API int32_t __tap_string_copy_bytes(
    const char *value, uint8_t *destination, size_t length);
// 把指定长度的字节复制为 Runtime 管理的零结尾字符串；内容不能包含 NUL。
TAP_RUNTIME_API const char *__tap_bytes_to_string(
    const uint8_t *data, size_t length);

// 把 double 渲染成最短的、能往返的十进制字符串；结果由 Runtime 管理。
// 非有限值输出 "inf" / "-inf" / "nan"。
TAP_RUNTIME_API const char *__tap_format_f64(double value);

// 打开文件；失败返回 NULL。mode 与 C 的 fopen 一致，调用方应带 b，
// 否则 Windows 上会把 \n 翻译成 \r\n。
TAP_RUNTIME_API void *__tap_file_open(const char *path, const char *mode);
// 句柄是否有效；句柄为 NULL 时返回 0。
TAP_RUNTIME_API int32_t __tap_file_is_open(void *handle);
// 读取最多 count 个字节；返回 Runtime 管理的字符串，末尾或失败时为空串。
TAP_RUNTIME_API const char *__tap_file_read(void *handle, uint64_t count);
// 写入字符串的全部字节；返回实际写入的字节数，失败返回 -1。
TAP_RUNTIME_API int64_t __tap_file_write(void *handle, const char *data);
// 关闭文件；成功返回 0。
TAP_RUNTIME_API int32_t __tap_file_close(void *handle);
// 是否已读到文件末尾；句柄无效时返回 0。
TAP_RUNTIME_API int32_t __tap_file_eof(void *handle);
// 删除文件；成功返回 0。
TAP_RUNTIME_API int32_t __tap_file_remove(const char *path);

// 打印 `panic: <message>` 并以状态 1 终止；不会返回。
TAP_RUNTIME_API int32_t __tap_panic(const char *message);
// 以指定状态码终止程序；不会返回。走 libc 的 exit，标准库缓冲区会被刷新。
TAP_RUNTIME_API int32_t __tap_exit(int32_t code);

// 浮点幂，指数可为小数或负数；底层是 C 库的 pow（需要 libm）。
TAP_RUNTIME_API double __tap_pow_f64(double base, double exponent);

// 分配堆内存；size 为 0 时 Runtime 会按 1 字节处理。
TAP_RUNTIME_API void *__tap_malloc(size_t size);
// 调整堆内存大小；size 为 0 时 Runtime 会按 1 字节处理。
TAP_RUNTIME_API void *__tap_realloc(void *pointer, size_t size);
// 释放 Runtime 分配的堆内存；NULL 指针安全无操作，返回 0。
TAP_RUNTIME_API int32_t __tap_free(void *pointer);

// ---- TCP 套接字（IPv4） ----
//
// 平台差异（POSIX 的 BSD socket / Windows 的 winsock）全部收在这一层：winsock 要先
// WSAStartup、句柄是 64 位 SOCKET、关闭要用 closesocket，而 tap 没有条件编译。
//
// 句柄统一是 int64：POSIX 下是 int fd，Windows 下是 SOCKET；**负值表示无效**。
// 地址缓冲区是调用方提供的 16 字节 sockaddr_in（两平台的 family / port / addr
// 字节布局一致），Runtime 不分配内存。
// 约定：0 表示成功、-1 表示失败；recv / send 返回字节数（0 表示对端已关闭），-1 表示失败。

TAP_RUNTIME_API int64_t __tap_socket_create(void);
TAP_RUNTIME_API int32_t __tap_socket_bind(
    int64_t handle, const uint8_t *address, uint32_t length);
TAP_RUNTIME_API int32_t __tap_socket_listen(int64_t handle, int32_t backlog);
TAP_RUNTIME_API int64_t __tap_socket_accept(int64_t handle);
TAP_RUNTIME_API int32_t __tap_socket_connect(
    int64_t handle, const uint8_t *address, uint32_t length);
TAP_RUNTIME_API int32_t __tap_socket_getsockname(
    int64_t handle, uint8_t *address, uint32_t *length);
TAP_RUNTIME_API int64_t __tap_socket_recv(
    int64_t handle, uint8_t *buffer, uint64_t length);
TAP_RUNTIME_API int64_t __tap_socket_send(
    int64_t handle, const uint8_t *buffer, uint64_t length);
// 发送字符串的全部字节（不含结尾 NUL）。
TAP_RUNTIME_API int64_t __tap_socket_send_text(int64_t handle, const char *data);
TAP_RUNTIME_API int32_t __tap_socket_close(int64_t handle);

#endif
