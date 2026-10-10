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
| `random(maximum: i32): i32` | `maximum > 0` 时返回 `[0, maximum)`，否则返回 `0`；现由 `std.random` 提供，需显式导入 |
| `argc(): i32` | 返回命令行参数数量，包含程序路径自身 |
| `arg(index: i32): string` | 返回指定命令行参数；越界时返回空字符串 |
| `args(): Vec<string>` | 返回全部命令行参数；`Vec` 来自 `std.vec` |
| `var(name: string): string` | 返回指定环境变量的值；不存在时返回空字符串 |
| `varc(): i32` | 返回当前进程环境变量数量 |
| `vars(): Vec<string>` | 返回全部环境变量条目，格式为 `NAME=VALUE` |

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

## 数学 ABI

| C ABI | 行为 |
|---|---|
| `__tap_pow_f64(base, exponent): f64` | 浮点幂，底层是 C 库的 `pow` |

指数是小数或负数时没法用快速幂算，必须走 libm。**这是 Runtime 里唯一需要 `-lm` 的符号**：
macOS 的 `ld -lSystem` 里含 libm，不用额外标志；Linux 和 Cygwin 的链接命令显式带了 `-lm`
（放在目标文件之后），因为 glibc 2.34 之前 libm 是独立的。

## 标准流输出 ABI

| C ABI | 行为 |
|---|---|
| `__tap_printf(format, ...): i32` | 格式化输出到标准输出（内部是 `vfprintf(stdout, ...)`）；返回写出的字符数 |
| `__tap_eprintf(format, ...): i32` | 格式化输出到标准错误（内部是 `vfprintf(stderr, ...)`）；返回写出的字符数 |

`__tap_printf` 由 Prelude 的 `print` 和 `std.io` 的 `printf` 共用，`__tap_eprintf` 由 `std.io`
的 `eprintf` 封装。**流固定在 Runtime 侧是有原因的**：`stdout` / `stderr` 是 libc 的 `FILE *`
全局量，tap 里拿不到；而 tap 的变参转发要求「一句纯转发、实参正好是具名形参」，中间插不进一个
取流句柄的调用。把流放在 Runtime 里，tap 侧就只剩一句干净的转发。

`__tap_printf` 的 extern 声明放在 Prelude —— `print` 是自动注入的，必须总能转发到它；`std.io`
的 `printf` 直接引用那份声明而不重复声明（全局函数名唯一，同一个 extern 符号只能有一处声明）。

`stderr` 按 C 标准默认不带全缓冲，所以这里不需要额外的 flush。

## 终止 ABI

| C ABI | 行为 |
|---|---|
| `__tap_panic(message): i32` | 打印 `panic: <message>` 到标准错误并 `exit(1)`；不会返回 |
| `__tap_exit(code): i32` | 以状态码 `code` 终止程序，不打印任何东西；不会返回 |

Prelude 的 `panic()` / `exit()` 就是它们的一层包装，`std.result` 的 `unwrap` / `expect`
也建在 `panic` 之上。前缀用 `panic: ` 和编译期诊断的 `error: ` 区分开；输出到标准错误，
和标准库的越界诊断（走标准输出）不同。

`__tap_exit` 除了给 Prelude 用，Codegen 的越界和 `assert` 失败路径也直接调它——**刻意不用
libc 的 `exit`**，因为那会占住 LLVM 模块里的 `exit` 符号，和 Prelude 的同名函数 `exit`
撞车（类型不同，LLVM 会把后者改名成 `exit.1`，函数体还会生成到错误的位置）。

## 文件 ABI

`std.fs` 的流式读写底层是以下 Runtime ABI：

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

## 套接字 ABI

`std.net` 的 TCP 套接字底层是以下 Runtime ABI：

| C ABI | 行为 |
|---|---|
| `__tap_socket_create(): i64` | 创建 TCP 套接字；返回句柄，失败返回 `-1` |
| `__tap_socket_bind(handle, address, length): i32` | 绑定 16 字节 `sockaddr_in`；成功返回 `0` |
| `__tap_socket_listen(handle, backlog): i32` | 开始监听；成功返回 `0` |
| `__tap_socket_accept(handle): i64` | 取一个已完成握手的连接；失败返回 `-1` |
| `__tap_socket_connect(handle, address, length): i32` | 阻塞连接；成功返回 `0` |
| `__tap_socket_getsockname(handle, address, length): i32` | 查询本地地址；成功返回 `0` |
| `__tap_socket_recv(handle, buffer, length): i64` | 收数据；返回字节数（`0` 表示对端关闭），失败返回 `-1` |
| `__tap_socket_send(handle, buffer, length): i64` | 发裸字节；返回字节数，失败返回 `-1` |
| `__tap_socket_send_text(handle, data): i64` | 发字符串全部字节；返回字节数，失败返回 `-1` |
| `__tap_socket_close(handle): i32` | 关闭套接字；成功返回 `0` |

**平台差异全部收在这一层**：POSIX 直接把 `int fd` 交给 libc 的 BSD socket；Windows 的
winsock 必须先 `WSAStartup`（惰性初始化一次）、句柄是 64 位 `SOCKET`、关闭要用
`closesocket`。tap 没有条件编译，所以这层适配只能写在 C 侧 —— 这也是 `std.net` 唯一需要
Runtime 的地方。

句柄统一是 `int64`：POSIX 的 `fd` 原样放进去，Windows 的 `SOCKET` 也是小整数，而
`INVALID_SOCKET` 恰好等于 `-1`，于是「负值 = 无效」在两平台都成立。地址缓冲区是调用方给的
16 字节 `sockaddr_in`（两平台的 family / port / addr 字节布局一致），Runtime 不分配内存。

Windows 下 `ws2_32.lib` 必须参与链接：CMake 给静态 Runtime 声明了 `PUBLIC ws2_32`（依赖会
传给最终链接方）、给共享 Runtime 直接链进去；编译器 `run` 模式调用的 `clang-cl` 也会显式
带上 `ws2_32.lib`。

## 底层内存 ABI

Runtime 还提供堆内存管理函数，供后续指针类型、可变数组或容器标准库使用：

| C ABI | 行为 |
|---|---|
| `__tap_malloc(size): *T` | 分配堆内存；`size == 0` 时按 1 字节处理 |
| `__tap_realloc(pointer, size): *T` | 调整堆内存大小；`size == 0` 时按 1 字节处理 |
| `__tap_free(pointer): i32` | 释放堆内存；`NULL` 指针安全无操作；返回 `0` |

这些函数由 `std.memory` 封装，底层 ABI 声明不需要出现在普通程序或其他容器模块中。
需要直接管理堆内存时使用 `memory.malloc/realloc/free`；业务代码仍应优先使用
`std.vec` 这类容器，避免直接管理裸指针和容量。
