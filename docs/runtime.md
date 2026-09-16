# Runtime

4yue Runtime 使用稳定的 C ABI 提供需要操作系统的能力。编译器支持无函数体的外部声明：

```text
extern fn __4yue_sleep_ms(milliseconds: i32): i32;
```

普通程序不应直接使用 `__4yue_` 前缀符号。[`std/prelude.tp`](../std/prelude.tp)
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
- 可以用 `4YUE_RUNTIME_PATH` 指定同时包含静态库和共享库的目录。

Runtime 的公开 ABI 声明位于 [`runtime/include/4yue_runtime.h`](../runtime/include/4yue_runtime.h)，
跨平台实现位于 [`runtime/src/runtime.c`](../runtime/src/runtime.c)。

## 底层内存 ABI

Runtime 还提供堆内存管理函数，供后续指针类型、可变数组或容器标准库使用：

| C ABI | 行为 |
|---|---|
| `__4yue_malloc(size): *T` | 分配堆内存；`size == 0` 时按 1 字节处理 |
| `__4yue_realloc(pointer, size): *T` | 调整堆内存大小；`size == 0` 时按 1 字节处理 |
| `__4yue_free(pointer): i32` | 释放堆内存；`NULL` 指针安全无操作；返回 `0` |

这些函数由 `std.memory` 封装，底层 ABI 声明不需要出现在普通程序或其他容器模块中。
需要直接管理堆内存时使用 `memory.malloc/realloc/free`；业务代码仍应优先使用
`std.vec_string` 这类容器，避免直接管理裸指针和容量。
