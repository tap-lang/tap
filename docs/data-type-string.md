# 字符串

本文档描述 tap 当前已经实现的 `String` 类型，包括字符串字面量、变量、函数传递、
字节访问、切片、内容比较、拼接、输出和字符串数组。尚未实现的能力集中列在
[当前限制](#当前限制)中。

`String` 是 Tap 的文本类型。原来的小写 `string` 已更名为 `c_string`，只用于 C ABI
边界；它不是 `String` 的别名。

## 字符串字面量

字符串字面量使用双引号包围：

```text
let message: String = "Hello, tap!";
```

当前支持以下转义序列：

| 转义序列 | 含义 |
| --- | --- |
| `\n` | 换行 |
| `\t` | 制表符 |
| `\\` | 反斜杠 |
| `\"` | 双引号 |

例如：

```text
print("line 1\nline 2\n");
print("tab:\tquote:\" slash:\\\n");
```

源文件中的 UTF-8 文本会按原始字节保存在字符串中，因此可以直接书写中文等 Unicode
文本：

```text
print("你好，世界！\n");
```

未列出的转义序列没有稳定语义，不应依赖。

## 变量与常量

字符串可以显式声明为 `String`，也可以从字符串表达式推断类型：

```text
let greeting: String = "Hello";
let name = "tap";

greeting = "Hi";
print("%s, %s!\n", greeting, name);
```

字符串常量必须显式标注类型，初始化后不能重新赋值：

```text
const LANGUAGE: String = "tap";
```

## 函数参数与返回值

`String` 可以用作函数参数和返回值：

```text
fn echo(value: String): String {
    return value;
}

fn main(): i32 {
    let value = echo("Hello");
    print("%s\n", value);
    return 0;
}
```

参数传递、返回和赋值会复制 `{data, length}` 两个机器字，不会复制字符串内容。

## 获取长度

使用 `.len()` 获取字符串在 UTF-8 编码下的字节长度。该方法不接收参数，返回类型为
架构宽度的无符号整数 `uint`：

```text
let s: String = "hello";
let length = s.len();
print(length);
```

空字符串的长度为 `0`，结尾的空字节不计入长度：

```text
let empty: String = "";
let length: uint = empty.len();
```

`.len()` 可以用于字符串变量、字符串字面量、返回字符串的函数调用和字符串数组元素：

```text
let values: [String; 2] = ["one", "four"];
print(values[1].len());
print("hello".len());
```

长度按字节计算，不按 Unicode 字符或用户感知字符计算。例如 UTF-8 源文件中的
`"你好".len()` 返回 `6`，而不是 `2`。长度直接保存在值中，因此时间复杂度为 O(1)。

## 字节访问

`byte_at(index)` 按 UTF-8 原始字节读取字符串，返回 `u8`：

```text
let value: String = "hello";
let first: u8 = value.byte_at(0); // 104
```

下标从 `0` 开始，必须是最多 64 位的整数。负数或大于等于 `.len()` 的下标会输出
越界错误并以状态 `1` 终止程序。多字节 Unicode 字符需要读取多个字节；例如
`"你".byte_at(0)` 返回 `228` (`0xE4`)。

## 字节切片

`slice(start, end)` 返回左闭右开区间 `[start, end)` 的新字符串：

```text
let value: String = "hello";
let part: String = value.slice(1, 4); // "ell"
```

`start` 和 `end` 都是 UTF-8 字节偏移，必须满足 `0 <= start <= end <= value.len()`。
边界非法时程序会输出错误并以状态 `1` 终止。为了保持有效 UTF-8，调用者应确保
两个偏移都位于字符边界。切片结果由 Runtime 管理，在进程退出时统一释放。

## 内容比较

`==`、`!=`、`<`、`<=`、`>` 和 `>=` 均按字符串内容比较，不比较指针地址。
顺序比较使用 UTF-8 编码的无符号字节序：

```text
let copied: String = "hello".slice(0, 5);
assert(copied == "hello");
assert("apple" < "banana");
```

## 输出字符串

`print` 的第一个参数永远是格式字符串（它转发到 Runtime 的标准输出）。用 `%s` 输出
字符串变量、函数返回值或字符串数组元素：

```text
let value: String = "Hello";
print("%s\n", value);
```

格式占位符必须与后续参数类型和数量匹配，编译器不会校验这种匹配关系。单实参
`print(value)` 会自动为 `String` 选择 `%s`；和其他内容一起输出时显式给出格式串：

```text
print("%s", value);
```

需要精确控制格式（宽度、精度、进制）时改用 `std.io` 的 `printf`（要 `import std.io;`）。
两者行为一致，只是 `print` 多一个单实参自动挑格式的便利，而且不需要导入：

```text
import std.io;

io.printf("name=%-8s hex=%06x\n", value, 255);
```

分工详见[标准库文档](std.md#printf-与-print-的分工)。

`assert` 的可选消息也支持字符串，但当前必须是字符串字面量：

```text
assert(result == 0, "result must be zero");
```

## 与标准库的关系

`String` 是内建语言类型，无需导入模块即可使用。`.len()`、`.byte_at()` 和 `.slice()`
都是内建方法，不属于标准库函数。字符串 `+` 同样是语言内建运算，不需要导入标准库；
当前标准库尚未提供字符串查找 API，数值转换由 `std.parse` 提供。标准库的现状见
[标准库文档](std.md#current-limits)。

## 字符串拼接

两个 `String` 可以使用 `+` 按 UTF-8 字节拼接，结果仍然是 `String`：

```text
let greeting: String = "Hello, " + "tap!";
let name: String = "tap";
let message: String = "Hello, " + name + "!";
```

`+` 要求两边都是 `String`，不负责数值转字符串；需要把数字格式化后再拼接时，先使用
`std.parse.format_i64` / `format_f64`，或者使用 `std.string_builder`。拼接结果由 Runtime
统一管理，程序退出时释放。

## 字符串数组

固定长度数组可以使用 `String` 作为元素类型。数组长度必须写在类型中，初始化元素数量
必须与声明长度一致：

```text
let names: [String; 3] = ["Alice", "Bob", "Carol"];
print("%s %s %s\n", names[0], names[1], names[2]);
```

可以通过下标读取或替换单个元素：

```text
names[1] = "David";
print("%s\n", names[1]);
```

重复初始化语法同样适用于字符串数组：

```text
let values: [String; 3] = ["empty"; 3];
```

字符串数组遵循普通固定长度数组的规则：

- 下标必须是整数表达式，并在运行时检查上下界。
- `const` 字符串数组不能替换其中的元素。
- 可以声明多维固定长度字符串数组，并逐层索引到字符串元素。
- 当前不能对整个数组或子数组赋值。
- 当前不能把内建数组作为函数参数或返回值；需要动态长度字符串数组时，用 `Vec<String>`。

更多通用数组规则见[数据类型文档](data-types.md#固定长度数组)。

## 动态字符串数组

动态长度字符串数组直接用泛型容器 `std.vec` 的 `Vec<String>`：

```text
import std.vec as vec;

fn main(): i32 {
    let values: Vec<String> = vec.new();
    values.push("Alice");
    values.push("Bob");
    print("%s\n", values.get(1));
    values.free();
    return 0;
}
```

`Vec<T>` 内部用 `*T` 和 Runtime 内存函数实现，元素大小取 `sizeof(T)`。`push` 和 `set`
作为语句调用时会自动把返回的结构体写回接收者；使用完后调用 `values.free()` 释放内部缓冲区。

## 实现模型

字符串字面量的数据在 LLVM IR 中生成为以空字节结尾的全局字节数组，`String` 值表示为
`{ i8* data, i64 length }`。尾部 NUL 只用于兼容 C API，不计入 `length`；字符串可以通过
`ByteVec.to_string()` 保存内嵌 NUL。字符串变量和数组元素保存这个二字段值，因此：

- 字符串字面量的生命周期覆盖整个程序运行期。
- 给字符串变量或数组元素赋值会替换数据指针和长度，不会修改原字符串内容。
- 当前没有修改字符串内部字节或字符的语法。
- `print` 的 `%s` 仍使用 C 字符串约定，遇到内嵌 NUL 会提前停止；`.len()`、比较、切片、
  拼接和文件写入使用保存的长度。

## 与 C 字符串转换

`c_string` 是兼容 C 的零结尾 `i8*`。`String` 的缓冲区保留尾部 NUL，因此传给声明为
`c_string` 的 C 函数时可以隐式借出 `data` 指针：

```text
extern fn strlen(value: c_string): uint;
let text: String = "hello";
let length: uint = strlen(text);
```

反方向必须显式写 `raw as String`，转换时扫描到第一个 NUL 来计算长度。该转换借用原指针，
不会复制数据；调用方必须保证指针非空、所指缓冲区以 NUL 结尾，并且在 `String` 使用期间有效。
由于 C 字符串没有独立长度，内嵌 NUL 后的内容无法通过这种转换保留。

## 当前限制

当前语言和标准库尚未提供以下字符串能力：

- 字符串插值；逐步构建字符串可使用 `std.string_builder`。
- 码点下标和码点切片（字符数量可用 `.char_count()`）。
- 可变字符串及字符替换。
- 显式释放单个动态字符串的 API。
- 字符与 Unicode 码点级别的处理 API。

## 测试

字符串行为由以下运行测试覆盖：

- [`tests/run-pass/types/strings.tp`](../tests/run-pass/types/strings.tp)：变量、类型推断、
  赋值、函数参数、返回值和转义序列。
- [`tests/run-pass/types/string_arrays.tp`](../tests/run-pass/types/string_arrays.tp)：数组
  初始化、下标读取、元素替换和重复初始化。
- [`tests/run-pass/types/string_length.tp`](../tests/run-pass/types/string_length.tp)：字节长度、
  返回类型、空字符串、UTF-8 字符串和不同形式的接收者。
- [`tests/run-pass/types/string_methods.tp`](../tests/run-pass/types/string_methods.tp)：字节访问、
  切片、UTF-8 偏移和六种内容比较运算符。
- [`tests/run-pass/types/string_concat.tp`](../tests/run-pass/types/string_concat.tp)：字符串 `+`
  拼接、链式拼接、空字符串、Unicode 字符串和函数参数/返回值。
- [`tests/run-pass/types/c_string.tp`](../tests/run-pass/types/c_string.tp)：C ABI 调用、双向转换、
  全局 `c_string` 常量以及两种类型的大小。
- [`tests/run-pass/stdlib/string_embedded_nul.tp`](../tests/run-pass/stdlib/string_embedded_nul.tp)：
  内嵌 NUL 的长度、字节访问、切片和拼接。

可以只运行这些测试：

```bash
TEST_FILTER=types/strings.tp sh tests/run-tests.sh build/tap
TEST_FILTER=types/string_arrays.tp sh tests/run-tests.sh build/tap
TEST_FILTER=types/string_length.tp sh tests/run-tests.sh build/tap
TEST_FILTER=types/string_methods.tp sh tests/run-tests.sh build/tap
TEST_FILTER=types/string_concat.tp sh tests/run-tests.sh build/tap
```
