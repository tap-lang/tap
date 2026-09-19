# Standard Library

tap 标准库由两部分组成：

- `std/*.tp`：用 tap 编写的标准库源码。
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
| `std.memory` | `malloc<T>(size: uint): *T` | 分配指定字节数的堆内存 |
| `std.memory` | `realloc<T>(pointer: *T, size: uint): *T` | 调整已有堆内存大小 |
| `std.memory` | `free<T>(pointer: *T): i32` | 释放堆内存，成功返回 `0` |
| `std.vec` | `Vec<T>` | 可扩容的泛型动态数组 |
| `std.vec` | `new<T>()/with_capacity<T>(capacity)` | 创建空动态数组 |
| `std.vec` | `len/capacity/reserve/push/get/set/clear` | 容量管理、追加、读写和清空元素 |
| `std.vec` | `free<T>(vec): i32` | 释放内部缓冲区，不递归释放元素资源 |
| `std.byte_vec` | `ByteVec` | 可扩容的 `u8` 字节容器 |
| `std.byte_vec` | `new()/with_capacity(capacity)` | 创建字节容器 |
| `std.byte_vec` | `push/extend/get/set/clear` | 追加、读写和清空字节 |
| `std.byte_vec` | `to_string(): string` | 复制有效字节为 Runtime 管理的字符串 |
| `std.string_builder` | `StringBuilder` | 基于 `ByteVec` 的可增量字符串构建器 |
| `std.string_builder` | `append/append_byte/clear` | 追加字符串或字节并复用容量 |
| `std.string_builder` | `to_string(): string` | 获取与后续 Builder 修改无关的字符串快照 |
| `std.vec_string` | `StringVec` | 可变长度字符串数组结构体 |
| `std.vec_string` | `new(): StringVec` | 创建空字符串动态数组，初始容量为 8 |
| `std.vec_string` | `len(vec: StringVec): i32` | 返回当前元素数量 |
| `std.vec_string` | `push(vec: StringVec, value: string): StringVec` | 追加字符串，容量不足时自动扩容 |
| `std.vec_string` | `get(vec: StringVec, index: i32): string` | 读取指定元素；越界时输出错误并返回空字符串 |
| `std.vec_string` | `set(vec: StringVec, index: i32, value: string): StringVec` | 替换指定元素；越界时输出错误并保持原值 |
| `std.vec_string` | `free(vec: StringVec): i32` | 释放动态数组内部缓冲区，成功返回 `0` |
| `std.parse` | `parse_i64(text: string): ParsedInt` | 解析十进制整数，返回 `{ value: i64, ok: bool }` |
| `std.parse` | `parse_f64(text: string): ParsedFloat` | 解析十进制浮点，返回 `{ value: f64, ok: bool }` |
| `std.parse` | `format_i64(value: i64): string` | 把整数格式化为十进制字符串 |
| `std.parse` | `format_f64(value: f64): string` | 把浮点数格式化为最短可往返的十进制字符串 |
| `std.json` | `escape(text: string): string` | 转义为 JSON 字符串内容，不含两侧引号 |
| `std.json` | `escape_quoted(text: string): string` | 转义并补上两侧引号，结果可直接放进 JSON |
| `std.json` | `append_escaped(builder, text): StringBuilder` | 把转义结果追加进构建器，不含两侧引号 |
| `std.json` | `append_quoted(builder, text): StringBuilder` | 追加转义结果并补上两侧引号 |
| `std.json` | `unescape(text: string): Unescaped` | 反转义 JSON 字符串内容，返回 `{ text: string, ok: bool }` |

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
tap run app.tp -- alpha beta
```

`std.memory` 示例：

```text
import std.memory as memory;

fn main(): i32 {
    // 显式指定 T 为 string。
    let values: *string = memory.malloc<string>(16);
    values[0] = "hello";
    values = memory.realloc(values, 32);
    values[1] = "tap";

    // 根据变量的 *i32 类型推断 T。
    let numbers: *i32 = memory.malloc(8);

    memory.free(values);
    return memory.free(numbers);
}
```

泛型类型实参直接写在函数名后的尖括号中，不使用 `::`。`malloc` 可以通过
`memory.malloc<i32>(8)` 显式指定元素类型，也可以根据变量声明中的 `*i32` 推断类型。
`realloc` 和 `free` 会从传入的指针推断类型。分配大小和扩容大小的单位都是字节，调用方需要
按元素大小正确计算容量；释放后不能继续访问原指针。

`std.vec` 示例：

```text
import std.vec as vec;

fn main(): i32 {
    let values: Vec<i32> = vec.new();
    values.push(10);
    values.push(20);
    values.set(1, 22);
    print("%llu %d\n", values.len(), values.get(1));
    return values.free();
}
```

`Vec<T>` 的 `len` 和 `capacity` 使用 `uint`，索引也使用 `uint`。`get()` 和 `set()` 会在
索引越界时终止程序。`clear()` 只把长度归零并保留容量；`free()`
只释放 Vec 自己的元素缓冲区，不会递归释放元素持有的资源。`push()`、`set()`、`clear()`
和 `reserve()` 作为方法语句调用时会自动把返回的新结构体写回接收者。

`std.vec_string` 示例：

```text
import std.vec_string as vec;

fn main(): i32 {
    let values: StringVec = vec.new();
    values.push("hello");
    values.push("tap");

    print("%d\n", values.len());
    print("%s %s\n", values.get(0), values.get(1));

    values.free();
    return 0;
}
```

`StringVec` 是值类型。直接写 `values.push("hello");` 或 `values.set(0, "HELLO");`
时，编译器会把返回的 `StringVec` 自动写回 `values`。`free` 只释放内部缓冲区，
不会清空原结构体字段；释放后不要继续访问同一个 `StringVec`。

`std.byte_vec` 示例：

```text
import std.byte_vec as bytes;

fn main(): i32 {
    let buffer: ByteVec = bytes.with_capacity(4);
    buffer.extend("hello");
    buffer.push(33);
    print("%s\n", buffer.to_string());
    return buffer.free();
}
```

`ByteVec` 容量不足时按倍数扩容。`clear()` 只将长度置零并保留容量；
`free()` 后不能继续访问容器。`to_string()` 会复制当前字节，因此之后清空或修改
`ByteVec` 不会改变已返回的字符串。由于 tap `string` 使用结尾 NUL，包含 `0` 字节的
`ByteVec` 不能转换为字符串。

`std.string_builder` 示例：

```text
import std.string_builder as builder;

fn main(): i32 {
    let output: StringBuilder = builder.new();
    output.append("{\"name\":\"");
    output.append("tap");
    output.append("\"}");
    print("%s\n", output.to_string());
    return output.free();
}
```

`StringBuilder` 内部使用 `ByteVec`。`append()` 追加字符串的 UTF-8 字节，
`append_byte()` 追加单个字节，`to_string()` 返回独立快照。

`std.parse` 示例：

```text
import std.parse;

fn main(): i32 {
    let parsed: ParsedInt = parse.parse_i64("-456");
    if (parsed.ok) {
        print("value=%lld text=%s\n", parsed.value, parse.format_i64(parsed.value));
    } else {
        print("not a number\n");
    }

    let ratio: ParsedFloat = parse.parse_f64("1.5e-3");
    print("ratio=%f\n", ratio.value);

    // format_f64 取最短的、能往返的表示，不补齐到固定小数位。
    let tenth: ParsedFloat = parse.parse_f64("0.1");
    print("text=%s\n", parse.format_f64(tenth.value));
    return 0;
}
```

`parse_i64` 接受可选正负号，`parse_f64` 额外接受小数部分和 `e` / `E` 指数。两者都要求整个
输入被完整消费：空串、只有符号、含有非数字字符或末尾有多余字符时 `ok` 为 `false`，此时
`value` 没有意义。浮点的数字要求与 C 的 `strtod` 一致，整数部分和小数部分至少一侧要有数字，
因此 `.5` 和 `1.` 都合法。

结果结构体是值类型，字段访问需要先绑定到变量：

```text
let result: ParsedInt = parse.parse_i64(text);
if (result.ok) {
    print("%lld\n", result.value);
}
```

`format_f64` 取最短的、能往返的表示：`0.1` 输出 `0.1`，圆周率输出 `3.141592653589793`，
极大或极小的值用指数形式。它不提供定点位数、宽度或对齐控制。`inf` / `-inf` / `nan` 直接
输出字面文本——JSON 没有这三种写法，需要写出 JSON 的调用方要自行处理。NaN 的符号位会被
丢弃（各平台默认 NaN 的符号不一致），正负都输出 `nan`。

注意 tap 的浮点字面量默认是 `f32`（见[数据类型](data-types.md#标量类型)），所以
`format_f64(0.1)` 会先把 `0.1` 按 `f32` 取近似、再放宽到 `f64`，输出
`0.10000000149011612`。要拿到 `f64` 精度的输入，用 `f64` 类型的变量、运算结果，或者
`parse.parse_f64("0.1")` 的 `value`。

`std.json` 示例：

```text
import std.json;
import std.string_builder as sb;

fn main(): i32 {
    // 输出：转义并补上两侧引号，结果可以直接放进 JSON
    print("%s\n", json.escape_quoted("say \"hi\"\n"));

    // 增量拼接时用 append_quoted，省掉每段字符串的临时对象
    let out: StringBuilder = sb.new();
    out = json.append_quoted(out, "key");
    out.append(":");
    out = json.append_quoted(out, "va\\lue");
    print("%s\n", sb.to_string(out));

    // 输入：反转义，非法转义序列通过 ok 报告
    let decoded: Unescaped = json.unescape("a\\u0041\\uD83D\\uDE00b");
    if (decoded.ok) {
        // 字段访问要先绑定到变量，不能在字段链上直接调方法。
        let text: string = decoded.text;
        print("len=%llu\n", text.len());
    }
    return 0;
}
```

输出侧转义 `"`、`\` 以及所有小于 `0x20` 的字符：`\b` `\f` `\n` `\r` `\t` 用短形式，其余控制
字符写成 `\u00XX`。`0x7F` 及以上不是控制字符，按 UTF-8 字节原样透传。

输入侧识别 `\"` `\\` `\/` `\b` `\f` `\n` `\r` `\t` 和 `\uXXXX`。相邻的高/低代理对会合成一个
码点再按 UTF-8 编码；孤立代理、被截断的 `\uXXXX`、未知转义和结尾的裸反斜杠都让 `ok` 为
`false`，此时 `text` 是出错前已经解出的部分。

`escape` 和 `unescape` 都不含两侧引号——引号由 `escape_quoted` / `append_quoted` 补上。

模块导入、别名、递归加载和错误规则见 [module.md](module.md)。

## Runtime ABI

Runtime 提供 Prelude 无法用纯 tap 表达的能力。Runtime 函数使用 `__tap_`
前缀，避免和用户函数或 libc 符号冲突：

```text
extern fn __tap_read_key(): i32;
extern fn __tap_sleep_ms(milliseconds: i32): i32;
extern fn __tap_clear_screen(): i32;
extern fn __tap_random(maximum: i32): i32;
extern fn __tap_argc(): i32;
extern fn __tap_arg(index: i32): string;
extern fn __tap_env_var(name: string): string;
extern fn __tap_envc(): i32;
extern fn __tap_env(index: i32): string;
// 字符串 ABI 由内建方法和比较运算符调用。
extern fn __tap_string_byte_at(value: string, index: i64): u8;
extern fn __tap_string_slice(value: string, start: i64, end: i64): string;
extern fn __tap_string_compare(left: string, right: string): i32;
extern fn __tap_string_copy_bytes(value: string, destination: *u8, length: uint): i32;
extern fn __tap_bytes_to_string(data: *u8, length: uint): string;
extern fn __tap_malloc(size: uint): *i8;
extern fn __tap_realloc(pointer: *i8, size: uint): *i8;
extern fn __tap_free(pointer: *i8): i32;
// 内存 ABI 由 std.memory 统一声明和封装。
```

普通程序应使用内建字符串操作、Prelude 封装后的 `read_key`、`sleep_ms`、
`clear_screen` 和 `random`，以及 `std.env`、`std.memory` 封装后的函数，不要直接调用
`__tap_` 前缀函数。

Runtime 的 C ABI 声明位于
[`runtime/include/tap_runtime.h`](../runtime/include/tap_runtime.h)，实现位于
[`runtime/src/runtime.c`](../runtime/src/runtime.c)。构建与加载细节见
[runtime.md](runtime.md)。

## Lookup Paths

编译器查找标准库时按以下顺序定位 `std` 目录：

1. 环境变量 `TAP_STD_PATH`。
2. 当前工作目录下的 `std`。
3. 编译器可执行文件相邻的源码目录或安装目录。

原生可执行文件链接 Runtime 静态库；`-run-lli` 运行临时 IR 时加载 Runtime
共享库。Runtime 库目录可以通过 `TAP_RUNTIME_PATH` 指定。

## Adding APIs

新增标准库 API 时优先使用 tap 源码实现：

1. 如果函数可以用现有语言能力表达，放入 `std/*.tp`。
2. 如果函数应默认可用，放入 `std/prelude.tp`。
3. 如果函数需要操作系统、终端、时间、随机数、文件、内存等底层能力，先在
   Runtime 中增加 `__tap_` 前缀 C ABI，再在 Prelude 或模块中提供公共包装。

新增或修改标准库函数后，需要同步：

- 相关 `.tp` 源文件。
- `docs/std.md` 和必要时的 `docs/runtime.md`。
- `tests/run-pass/stdlib/` 或对应失败测试。

## Current Limits

- 大部分标准库函数仍只覆盖 `i32` 相关能力；`std.parse` 使用 `i64` 和 `f64`。
- `string` 已提供内建 `.len()`、`.byte_at()`、`.slice()` 和内容比较，动态构建可使用
  `std.string_builder`，字符串与数值互转可使用 `std.parse`；尚无 `+` 拼接、查找和
  Unicode 码点级 API。
- `std.parse` 只解析十进制，不识别十六进制、下划线分隔符或 `inf` / `nan`。
- `format_f64` 通过 Runtime 调用 C 库的十进制转换（`__tap_format_f64`），是 `std.parse`
  里唯一不纯 tap 的函数：精确的十进制展开需要大整数算法。
- 内建数组是固定长度类型；`std.env.args()` 当前返回 `[string; 64]`，
  `std.env.vars()` 当前返回 `[string; 256]`，不是动态数组。动态字符串数组请使用
  `std.vec_string`。
- 模块会导出顶层常量、结构体、枚举和函数，但没有可见性控制；导入的类型名当前进入全局类型命名空间。
- Prelude 是自动注入的全局函数集合，不支持按需选择导入。
