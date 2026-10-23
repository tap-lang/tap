#ifndef TAP_RUNTIME_H
#define TAP_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

// tap 的内建 String ABI，字段顺序必须与编译器生成的 `{ i8*, i64 }` 完全一致。
// data 指向 UTF-8 字节缓冲区；length 是有效字节数，不包含为兼容 C API 保留的尾部 NUL。
// String 可以保存内嵌 NUL，因此普通字符串操作必须使用 length，不能改用 strlen。
// data 在 length == 0 时也必须指向可读的零结尾缓冲区。
// 注意：c_string 在 C ABI 中仍是裸 `const char *`，不是下面这个结构体。
typedef struct String {
    const char *data;
    uint64_t length;
} String;

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
TAP_RUNTIME_API String __tap_arg(int32_t index);

TAP_RUNTIME_API String __tap_env_var(String name);
TAP_RUNTIME_API int32_t __tap_envc(void);
TAP_RUNTIME_API String __tap_env(int32_t index);

// 从借用的 C 零结尾字符串构造 String；通过 strlen 计算长度，NULL 转为空字符串。
// 返回值不复制缓冲区，调用方必须保证原始指针在 String 使用期间保持有效。
TAP_RUNTIME_API String __tap_string_from_c(const char *value);

// 按 UTF-8 原始字节读取字符串；越界时终止程序。
TAP_RUNTIME_API uint8_t __tap_string_byte_at(String value, int64_t index);
// 返回 UTF-8 字符数；合法 UTF-8 按 Unicode 码点计数，非法字节按一个字符计数。
TAP_RUNTIME_API uint64_t __tap_string_char_count(String value);
// 返回 [start, end) 字节区间的新字符串；非法边界时终止程序。
TAP_RUNTIME_API String __tap_string_slice(
    String value, int64_t start, int64_t end);
// 按 UTF-8 编码字节序比较两个字符串。
TAP_RUNTIME_API int32_t __tap_string_compare(String left, String right);
// 拼接两个字符串；结果由 Runtime 持有到进程退出。
TAP_RUNTIME_API String __tap_string_concat(String left, String right);
// 判断 value 是否包含 needle（按字节匹配）。needle 为空串时总是返回 1。
TAP_RUNTIME_API int32_t __tap_string_contains(String value, String needle);
// 判断 value 是否以 prefix 开头。prefix 长度超过 value 时返回 0。
TAP_RUNTIME_API int32_t __tap_string_starts_with(String value, String prefix);
// 判断 value 是否以 suffix 结尾。suffix 长度超过 value 时返回 0。
TAP_RUNTIME_API int32_t __tap_string_ends_with(String value, String suffix);
// 把 value 中所有 from 的出现替换为 to；结果由 Runtime 持有到进程退出。
// from 为空串时返回 value 的副本（不无限插入）。
TAP_RUNTIME_API String __tap_string_replace(
    String value, String from, String to);
// 把标量格式化为 Runtime 管理的字符串。
TAP_RUNTIME_API String __tap_format_i64(int64_t value);
TAP_RUNTIME_API String __tap_format_u64(uint64_t value);
// 128 位整数通过高、低两个 64 位无符号半部传递，保持 Windows/Cygwin ABI 可移植。
TAP_RUNTIME_API String __tap_format_i128(uint64_t low, uint64_t high);
TAP_RUNTIME_API String __tap_format_u128(uint64_t low, uint64_t high);
TAP_RUNTIME_API String __tap_format_bool(int32_t value);
// 把 Unicode 码点编码为 UTF-8 字符串；非法码点会终止程序。
TAP_RUNTIME_API String __tap_format_char(uint32_t value);
// 把字符串的指定字节数复制到调用方提供的缓冲区；调用方保证两端空间足够。
TAP_RUNTIME_API int32_t __tap_string_copy_bytes(
    String value, uint8_t *destination, size_t length);
// 把指定长度的字节复制为 Runtime 管理的字符串并补尾部 NUL；允许内容包含 NUL。
TAP_RUNTIME_API String __tap_bytes_to_string(
    const uint8_t *data, size_t length);
// 从十进制字符串解析标量；输入非法或超出范围时终止程序。
TAP_RUNTIME_API int64_t __tap_parse_i64(String value);
TAP_RUNTIME_API uint64_t __tap_parse_u64(String value);
// 解析结果写入 low/high；输入非法或超出对应范围时终止程序。
TAP_RUNTIME_API void __tap_parse_i128(
    String value, uint64_t *low, uint64_t *high);
TAP_RUNTIME_API void __tap_parse_u128(
    String value, uint64_t *low, uint64_t *high);
TAP_RUNTIME_API int32_t __tap_parse_bool(String value);

// 把 double 渲染成最短的、能往返的十进制字符串；结果由 Runtime 管理。
// 非有限值输出 "inf" / "-inf" / "nan"。
TAP_RUNTIME_API String __tap_format_f64(double value);
// 把 float 渲染成最短的、能往返的十进制字符串。
TAP_RUNTIME_API String __tap_format_f32(float value);

// 打开文件；失败返回 NULL。mode 与 C 的 fopen 一致，调用方应带 b，
// 否则 Windows 上会把 \n 翻译成 \r\n。
TAP_RUNTIME_API void *__tap_file_open(String path, String mode);
// 句柄是否有效；句柄为 NULL 时返回 0。
TAP_RUNTIME_API int32_t __tap_file_is_open(void *handle);
// 读取最多 count 个字节；返回 Runtime 管理的字符串，末尾或失败时为空串。
TAP_RUNTIME_API String __tap_file_read(void *handle, uint64_t count);
// 写入字符串的全部字节；返回实际写入的字节数，失败返回 -1。
TAP_RUNTIME_API int64_t __tap_file_write(void *handle, String data);
// 关闭文件；成功返回 0。
TAP_RUNTIME_API int32_t __tap_file_close(void *handle);
// 是否已读到文件末尾；句柄无效时返回 0。
TAP_RUNTIME_API int32_t __tap_file_eof(void *handle);
// 删除文件；成功返回 0。
TAP_RUNTIME_API int32_t __tap_file_remove(String path);

// 格式化输出到标准输出 / 标准错误（内部是 vfprintf(stdout/stderr, ...)）；
// 返回写出的字符数，出错为负。变参 ABI 与 printf 一致，格式串和实参的对应关系由调用方保证。
// 流固定在 Runtime 侧：tap 拿不到 libc 的 FILE*，而变参转发又插不进取流句柄的调用。
TAP_RUNTIME_API int32_t __tap_printf(const char *format, ...);
TAP_RUNTIME_API int32_t __tap_eprintf(const char *format, ...);

// 目录遍历：打开时读入全部条目名并排序（不含 "." 和 ".."）。
// 名字由 Runtime 持有，__tap_dir_close 之后失效。
TAP_RUNTIME_API void *__tap_dir_open(String path);
TAP_RUNTIME_API String __tap_dir_next(void *handle);
TAP_RUNTIME_API int32_t __tap_dir_is_open(void *handle);
TAP_RUNTIME_API int32_t __tap_dir_close(void *handle);

// path 是不是目录；不存在或不是目录都返回 0。
TAP_RUNTIME_API int32_t __tap_path_is_directory(String path);

// 建符号链接 link_path -> target；成功返回 0。
// Windows 需要管理员权限或开发者模式，失败时调用方应回退到复制。
TAP_RUNTIME_API int32_t __tap_symlink(String target, String link_path);

// 建目录（已存在算成功）；成功返回 0。
TAP_RUNTIME_API int32_t __tap_make_directory(String path);

// 删除文件或空目录；成功返回 0。
TAP_RUNTIME_API int32_t __tap_remove_entry(String path);

// 打印 `panic: <message>` 并以状态 1 终止；不会返回。
TAP_RUNTIME_API int32_t __tap_panic(String message);
// 以指定状态码终止程序；不会返回。走 libc 的 exit，标准库缓冲区会被刷新。
TAP_RUNTIME_API int32_t __tap_exit(int32_t code);

// ---- 进程 / 子命令 ----
//
// 薄包装 libc 的 system / popen / pclose。句柄是 FILE*（跨模块用 void* 表示）。
// 注意：popen 返回的 FILE* **没有**登记进 open_files 表，所以 __tap_file_read
// 读不到内容——它按 slot 表校验句柄。读管道输出用下面的 __tap_stream_read，
// 它不做登记、直接对传入的 FILE* 做 fread。

// 运行一条 shell 命令，返回 wait status：0 表示命令成功退出，非 0 表示命令不存在、
// 非零退出或被信号打断。判断「成不成功」用 `status != 0`。
// （POSIX 下是被 << 8 编码过的 wait status，Windows 下是直接退出码。）
TAP_RUNTIME_API int32_t __tap_system(String command);
// 打开管道，返回不透明句柄（只读 "r" / 只写 "w"）；失败返回 NULL。
TAP_RUNTIME_API void *__tap_popen(String command, String mode);
// 关闭管道并等待子进程，返回 wait status（语义同 __tap_system）。
TAP_RUNTIME_API int32_t __tap_pclose(void *stream);
// 从一个已打开的流（popen 返回的管道）读取最多 count 字节；不做 slot 表登记。
// POSIX/Cygwin 直接对底层描述符做一次 read，Windows 使用 fread；空句柄、读失败
// 或到末尾时返回空串。
TAP_RUNTIME_API String __tap_stream_read(void *stream, uint64_t count);
// 句柄是否为空指针：NULL 返回 1，否则 0。popen 失败时用来判断。
TAP_RUNTIME_API int32_t __tap_ptr_is_null(void *pointer);
// 把 system / pclose 的状态码转换为进程退出码；进程被信号终止或状态无效时返回 -1。
TAP_RUNTIME_API int32_t __tap_process_exit_code(int32_t status);
// 判断进程是否被信号终止；Windows 或状态无效时返回 0。
TAP_RUNTIME_API int32_t __tap_process_was_signaled(int32_t status);

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
TAP_RUNTIME_API int64_t __tap_socket_send_text(int64_t handle, String data);
TAP_RUNTIME_API int32_t __tap_socket_close(int64_t handle);

#endif
