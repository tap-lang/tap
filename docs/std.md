# Standard Library

4yue 标准库由两部分组成：

- `std/*.tp`：用 4yue 编写的标准库源码。
- `runtime/`：用 C 实现的 Runtime ABI，提供需要操作系统支持的能力。

编译器会自动加载 `std/prelude.tp`。其他标准库模块通过 `import std.xxx;`
显式导入。

## Prelude

`std/prelude.tp` 会在普通编译、`run`、`-ir`、`-emit-obj`、`-run-lli`
流程中自动加载，用户源码无需导入即可调用其中的公共函数。

当前 Prelude 导出：

| 函数 | 说明 |
| --- | --- |
| `min(a: i32, b: i32): i32` | 返回两个 `i32` 中较小的值 |
| `max(a: i32, b: i32): i32` | 返回两个 `i32` 中较大的值 |
| `read_key(): i32` | 非阻塞读取真实键值；无按键时返回 `-1` |
| `sleep_ms(milliseconds: i32): i32` | 休眠指定毫秒；成功返回 `0`，失败返回 `-1` |
| `clear_screen(): i32` | 清空终端并将光标移动到左上角；成功返回 `0` |
| `random(maximum: i32): i32` | 当 `maximum > 0` 时返回 `[0, maximum)`，否则返回 `0` |

说明：`read_key()` 返回 Runtime 读到的真实键值，不会把方向键映射为 WASD。POSIX 方向键会在一次调用中消费 ANSI 序列，并返回末尾方向字节 `65/66/67/68`；Windows 方向键通常返回 `72/80/77/75`。

`-lex` 和 `-parse` 只处理传入的源文件，不加载 Prelude。

Prelude 中的函数名会进入全局函数集合。用户源码不能重复定义同名函数，
否则编译器会报重复定义错误。

## Modules

非 Prelude 标准库模块需要显式导入。模块导入使用点分隔路径，默认名称空间为
最后一段模块名：

```text
import std.math;

fn main(): i32 {
    return math.square(5);
}
```

当前标准库模块：

| 模块 | 函数 | 说明 |
| --- | --- | --- |
| `std.env` | `argc(): i32` | 返回当前程序的命令行参数数量，包含程序路径自身 |
| `std.env` | `arg(index: i32): string` | 返回指定位置的命令行参数；越界时返回空字符串 |
| `std.env` | `args(): [string; 64]` | 返回最多 64 个命令行参数，不足的位置为空字符串 |
| `std.env` | `var(name: string): string` | 返回指定环境变量的值；不存在时返回空字符串 |
| `std.env` | `varc(): i32` | 返回当前进程环境变量数量 |
| `std.env` | `vars(): [string; 256]` | 返回最多 256 个环境变量条目，格式为 `NAME=VALUE`，不足的位置为空字符串 |
| `std.math` | `PI: f64` | 圆周率常量，值为 `3.141592653589793` |
| `std.math` | `square(value: i32): i32` | 返回平方值 |
| `std.math` | `abs(value: i32): i32` | 返回绝对值 |
| `std.memory` | `malloc(size: uint): ptr<string>` | 分配指定字节数的堆内存 |
| `std.memory` | `realloc(pointer: ptr<string>, size: uint): ptr<string>` | 调整已有堆内存大小 |
| `std.memory` | `free(pointer: ptr<string>): i32` | 释放堆内存，成功返回 `0` |
| `std.vec_string` | `StringVec` | 可变长度字符串数组结构体 |
| `std.vec_string` | `new(): StringVec` | 创建空字符串动态数组，初始容量为 8 |
| `std.vec_string` | `len(vec: StringVec): i32` | 返回当前元素数量 |
| `std.vec_string` | `push(vec: StringVec, value: string): StringVec` | 追加字符串，容量不足时自动扩容 |
| `std.vec_string` | `get(vec: StringVec, index: i32): string` | 读取指定元素；越界时输出错误并返回空字符串 |
| `std.vec_string` | `set(vec: StringVec, index: i32, value: string): StringVec` | 替换指定元素；越界时输出错误并保持原值 |
| `std.vec_string` | `free(vec: StringVec): i32` | 释放动态数组内部缓冲区，成功返回 `0` |

`std.env` 示例：

```text
import std.env as env;

fn main(): i32 {
    let values: [string; 64] = env.args();
    print("%d\n", env.argc());
    print("%s\n", values[1]);
    print("%s\n", env.var("PATH"));
    print("%s\n", env.vars()[0]);
    return 0;
}
```

使用 `run` 传递被编译程序的参数时，用 `--` 分隔编译器参数和程序参数：

```sh
4yue run app.tp -- alpha beta
```

`std.memory` 示例：

```text
import std.memory as memory;

fn main(): i32 {
    let values: ptr<string> = memory.malloc(16);
    values[0] = "hello";
    values = memory.realloc(values, 32);
    values[1] = "4yue";
    return memory.free(values);
}
```

当前语言还没有泛型或 `void` 指针，因此 `std.memory` 暂时使用 `ptr<string>` 作为裸内存指针
类型。分配大小和扩容大小的单位都是字节，释放后不能继续访问原指针。

`std.vec_string` 示例：

```text
import std.vec_string as vec;

fn main(): i32 {
    let values: StringVec = vec.new();
    values.push("hello");
    values.push("4yue");

    print("%d\n", values.len());
    print("%s %s\n", values.get(0), values.get(1));

    values.free();
    return 0;
}
```

`StringVec` 是值类型。直接写 `values.push("hello");` 或 `values.set(0, "HELLO");`
时，编译器会把返回的 `StringVec` 自动写回 `values`。`free` 只释放内部缓冲区，
不会清空原结构体字段；释放后不要继续访问同一个 `StringVec`。

模块导入、别名、递归加载和错误规则见 [module.md](module.md)。

## Runtime ABI

Runtime 提供 Prelude 无法用纯 4yue 表达的能力。Runtime 函数使用 `__4yue_`
前缀，避免和用户函数或 libc 符号冲突：

```text
extern fn __4yue_read_key(): i32;
extern fn __4yue_sleep_ms(milliseconds: i32): i32;
extern fn __4yue_clear_screen(): i32;
extern fn __4yue_random(maximum: i32): i32;
extern fn __4yue_argc(): i32;
extern fn __4yue_arg(index: i32): string;
extern fn __4yue_env_var(name: string): string;
extern fn __4yue_envc(): i32;
extern fn __4yue_env(index: i32): string;
extern fn __4yue_malloc(size: uint): ptr<string>;
extern fn __4yue_realloc(pointer: ptr<string>, size: uint): ptr<string>;
extern fn __4yue_free(pointer: ptr<string>): i32;
// 内存 ABI 由 std.memory 统一声明和封装。
```

普通程序应调用 Prelude 封装后的 `read_key`、`sleep_ms`、`clear_screen`
和 `random`，以及 `std.env`、`std.memory` 封装后的函数，不要直接调用 `__4yue_` 前缀函数。

Runtime 的 C ABI 声明位于
[`runtime/include/4yue_runtime.h`](../runtime/include/4yue_runtime.h)，实现位于
[`runtime/src/runtime.c`](../runtime/src/runtime.c)。构建与加载细节见
[runtime.md](runtime.md)。

## Lookup Paths

编译器查找标准库时按以下顺序定位 `std` 目录：

1. 环境变量 `4YUE_STD_PATH`。
2. 当前工作目录下的 `std`。
3. 编译器可执行文件相邻的源码目录或安装目录。

原生可执行文件链接 Runtime 静态库；`-run-lli` 运行临时 IR 时加载 Runtime
共享库。Runtime 库目录可以通过 `4YUE_RUNTIME_PATH` 指定。

## Adding APIs

新增标准库 API 时优先使用 4yue 源码实现：

1. 如果函数可以用现有语言能力表达，放入 `std/*.tp`。
2. 如果函数应默认可用，放入 `std/prelude.tp`。
3. 如果函数需要操作系统、终端、时间、随机数、文件、内存等底层能力，先在
   Runtime 中增加 `__4yue_` 前缀 C ABI，再在 Prelude 或模块中提供公共包装。

新增或修改标准库函数后，需要同步：

- 相关 `.tp` 源文件。
- `docs/std.md` 和必要时的 `docs/runtime.md`。
- `tests/run-pass/stdlib/` 或对应失败测试。

## Current Limits

- 标准库函数目前只覆盖 `i32` 相关能力。
- `string` 已是可用的语言类型并提供内建 `.len()` 方法，但还没有稳定的字符串标准库
  API；语言层能力见[字符串文档](data-type-string.md)。
- 内建数组是固定长度类型；`std.env.args()` 当前返回 `[string; 64]`，
  `std.env.vars()` 当前返回 `[string; 256]`，不是动态数组。动态字符串数组请使用
  `std.vec_string`。
- 模块会导出顶层常量、结构体、枚举和函数，但没有可见性控制；导入的类型名当前进入全局类型命名空间。
- Prelude 是自动注入的全局函数集合，不支持按需选择导入。
