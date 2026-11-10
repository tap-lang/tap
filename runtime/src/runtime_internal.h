#ifndef TAP_RUNTIME_INTERNAL_H
#define TAP_RUNTIME_INTERNAL_H

#include "tap_runtime.h"

// Runtime 内部共用的辅助函数（runtime.c / process.c 之间使用）。
//
// 这些符号**不是**对外 ABI，不导出、不安装，只用来避免在两个 .c 之间重复实现
// 「分配 Runtime 字符串」和「UTF-8 ↔ UTF-16 转码」这两件小事。

// 复制 length 个字节并补尾部 NUL，返回 Runtime 持有的字符串。
// 允许内容包含内嵌 NUL；length 为 0 时 data 可以为 NULL。
String tap_allocate_string_copy(const void *data, size_t length);

#ifdef _WIN32

// UTF-8 → UTF-16，返回 malloc 的缓冲区（调用方 free）。失败返回 NULL。
// 含内嵌 NUL 的输入会被拒绝：Win32 的 W 版 API 只认零结尾宽字符串，
// 静默截断会让路径和实际文件不一致。
wchar_t *tap_wide_from_utf8(const char *data, size_t length);

// UTF-16 → UTF-8，返回 Runtime 持有的字符串；非法代理对按 U+FFFD 处理。
String tap_string_from_wide(const wchar_t *data, size_t length);

#endif // _WIN32

#endif // TAP_RUNTIME_INTERNAL_H
