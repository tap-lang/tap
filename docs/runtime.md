# Runtime

tap Runtime 使用稳定的 C ABI 提供需要操作系统的能力。编译器支持无函数体的外部声明：

```text
extern fn __tap_sleep_ms(milliseconds: i32): i32;
```

普通程序不应直接使用 `__tap_` 前缀符号。[`std/prelude.tp`](../std/prelude.tp)
和 [`std/env.tp`](../std/env.tp) 将它们封装为以下公共函数：

| 函数 | 行为 |
|---|---|
| `read_key(): i32` | 非阻塞读取一个真实键值；无按键或 stdin 不是终端时返回 `-1` |
| `sleep_ms(milliseconds: i32): i32` | 休眠指定毫秒；成功返回 `0`，负数或系统错误返回 `-1` |
| `clear_screen(): i32` | 清空终端并将光标移到左上角；成功返回 `0` |
| `random(maximum: i32): i32` | `maximum > 0` 时返回 `[0, maximum)`，否则返回 `0` |
| `argc(): i32` | 返回命令行参数数量，包含程序路径自身 |
| `arg(index: i32): string` | 返回指定命令行参数；越界时返回空字符串 |
| `args(): [string; 64]` | 返回最多 64 个命令行参数组成的固定长度字符串数组 |
| `var(name: string): string` | 返回指定环境变量的值；不存在时返回空字符串 |
| `varc(): i32` | 返回当前进程环境变量数量 |
| `vars(): [string; 256]` | 返回最多 256 个环境变量条目，格式为 `NAME=VALUE` |

说明：POSIX 终端方向键会在一次 read_key 调用中消费 ANSI 序列，并返回末尾方向字节 `65/66/67/68`；Windows 扩展方向键返回第二个 scan code，通常是 `72/80/77/75`。

## 构建与加载

- 静态 Runtime 用于链接原生可执行文件。
- 共享 Runtime 由 `-run-lli` 通过 `lli --load` 加载。
- 开发构建会在编译器同目录生成两种库。
- 安装后编译器会在相邻的 `../lib` 目录查找 Runtime。
- 可以用 `TAP_RUNTIME_PATH` 指定同时包含静态库和共享库的目录。

Runtime 的公开 ABI 声明位于 [`runtime/include/tap_runtime.h`](../runtime/include/tap_runtime.h)，
跨平台实现位于 [`runtime/src/runtime.c`](../runtime/src/runtime.c)。

## 底层字符串 ABI

内建字符串方法和比较运算符由编译器生成对以下 Runtime ABI 的调用：

| C ABI | 行为 |
|---|---|
| `__tap_string_byte_at(value, index): u8` | 读取 UTF-8 原始字节；越界时终止程序 |
| `__tap_string_slice(value, start, end): string` | 复制 `[start, end)` 字节区间；无效边界时终止程序 |
| `__tap_string_compare(left, right): i32` | 按 UTF-8 无符号字节序返回 `-1`、`0` 或 `1` |
| `__tap_string_copy_bytes(value, destination, length): i32` | 把字符串字节批量复制到已有缓冲区 |
| `__tap_bytes_to_string(data, length): string` | 把字节复制为零结尾字符串；内部 NUL 会终止程序 |

`slice` 和 `bytes_to_string` 结果由 Runtime 跟踪，进程退出时统一释放。
普通程序不应直接调用这些符号。

## 数值格式化 ABI

`std.parse` 的 `format_f64` 下沉到 C 库的十进制转换：

| C ABI | 行为 |
|---|---|
| `__tap_format_f64(value: f64): string` | 渲染成最短的、能往返的十进制字符串；结果由 Runtime 跟踪 |

精确的十进制展开需要大整数算法，纯 tap 实现会引入难以察觉的精度偏差，因此这里选择调用
C 库：先用 `%.15g`，再逐步提高精度直到结果能往返，这样 `0.1` 输出 `0.1` 而不是
`0.10000000000000001`。非有限值输出 `inf` / `-inf` / `nan`。

NaN 的符号位被丢弃，正负一律输出 `nan`：各平台默认 NaN 的符号并不一致（x86-64 给 `-nan`，
ARM64 给 `nan`），而 C 的 `printf` 会原样暴露这个差异。归一化之后同一个程序在不同平台的
输出才相同。`inf` 的符号是有意义的，予以保留。

这是 `std.parse` 里唯一不纯 tap 的函数；其余解析和整数格式化都由 tap 自己实现。
返回值与其他 Runtime 字符串一样在进程退出时统一释放。

## 文件 ABI

`std.file` 的流式读写底层是以下 Runtime ABI：

| C ABI | 行为 |
|---|---|
| `__tap_file_open(path, mode): *i8` | 打开文件；失败返回 `NULL`。`mode` 与 `fopen` 一致 |
| `__tap_file_is_open(handle): i32` | 句柄是否在打开登记表里；`NULL` 或已关闭返回 `0` |
| `__tap_file_read(handle, count): string` | 读取最多 `count` 字节；末尾或失败返回空串 |
| `__tap_file_write(handle, data): i64` | 写入全部字节并返回写入数；失败返回 `-1` |
| `__tap_file_close(handle): i32` | 关闭并从登记表移除；成功返回 `0` |
| `__tap_file_eof(handle): i32` | 是否已到末尾；句柄无效返回 `0` |
| `__tap_file_remove(path): i32` | 删除文件；成功返回 `0` |

**登记表是必需的，不是可选的优化**：tap 的 struct 是值类型，`close()` 拿到的是副本，无法把
调用方手里的句柄置空；而 `fclose` 之后指针值不变，再读写就是 use-after-close。Runtime 记录
当前打开的句柄（上限 64，超出时 `open` 失败），把已关闭的句柄判定为无效，于是误用只会得到
空结果或 `-1`，而不是崩溃。

## 底层内存 ABI

Runtime 还提供堆内存管理函数，供后续指针类型、可变数组或容器标准库使用：

| C ABI | 行为 |
|---|---|
| `__tap_malloc(size): *T` | 分配堆内存；`size == 0` 时按 1 字节处理 |
| `__tap_realloc(pointer, size): *T` | 调整堆内存大小；`size == 0` 时按 1 字节处理 |
| `__tap_free(pointer): i32` | 释放堆内存；`NULL` 指针安全无操作；返回 `0` |

这些函数由 `std.memory` 封装，底层 ABI 声明不需要出现在普通程序或其他容器模块中。
需要直接管理堆内存时使用 `memory.malloc/realloc/free`；业务代码仍应优先使用
`std.vec_string` 这类容器，避免直接管理裸指针和容量。
