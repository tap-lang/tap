# Runtime

4yue Runtime 使用稳定的 C ABI 提供需要操作系统的能力。编译器支持无函数体的外部声明：

```text
extern fn yue_sleep_ms(milliseconds: i32): i32;
```

普通程序不应直接使用 `yue_` 前缀符号。[`std/prelude.tp`](../std/prelude.tp)
将它们封装为以下公共函数：

| 函数 | 行为 |
|---|---|
| `read_key(): i32` | 非阻塞读取一个按键；方向键归一化为 `w/s/a/d`，无按键或 stdin 不是终端时返回 `-1` |
| `sleep_ms(milliseconds: i32): i32` | 休眠指定毫秒；成功返回 `0`，负数或系统错误返回 `-1` |
| `clear_screen(): i32` | 清空终端并将光标移到左上角；成功返回 `0` |
| `random(maximum: i32): i32` | `maximum > 0` 时返回 `[0, maximum)`，否则返回 `0` |

## 构建与加载

- 静态 Runtime 用于链接原生可执行文件。
- 共享 Runtime 由 `-run-lli` 通过 `lli --load` 加载。
- 开发构建会在编译器同目录生成两种库。
- 安装后编译器会在相邻的 `../lib` 目录查找 Runtime。
- 可以用 `4YUE_RUNTIME_PATH` 指定同时包含静态库和共享库的目录。

Runtime 的公开 ABI 声明位于 [`runtime/include/4yue_runtime.h`](../runtime/include/4yue_runtime.h)，
跨平台实现位于 [`runtime/src/runtime.c`](../runtime/src/runtime.c)。
