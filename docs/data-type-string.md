# 字符串

本文档描述 4yue 当前已经实现的 `string` 类型，包括字符串字面量、变量、函数传递、
输出、固定长度字符串数组和标准库动态字符串数组。尚未实现的能力集中列在[当前限制](#当前限制)中。

## 字符串字面量

字符串字面量使用双引号包围：

```text
let message: string = "Hello, 4yue!";
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
print("你好，4月！\n");
```

未列出的转义序列没有稳定语义，不应依赖。

## 变量与常量

字符串可以显式声明为 `string`，也可以从字符串表达式推断类型：

```text
let greeting: string = "Hello";
let name = "4yue";

greeting = "Hi";
print("%s, %s!\n", greeting, name);
```

字符串常量必须显式标注类型，初始化后不能重新赋值：

```text
const LANGUAGE: string = "4yue";
```

## 函数参数与返回值

`string` 可以用作函数参数和返回值：

```text
fn echo(value: string): string {
    return value;
}

fn main(): i32 {
    let value = echo("Hello");
    print("%s\n", value);
    return 0;
}
```

参数传递、返回和赋值传递的是字符串引用，不会复制字符串内容。

## 获取长度

使用 `.len()` 获取字符串在 UTF-8 编码下的字节长度。该方法不接收参数，返回类型为
架构宽度的无符号整数 `uint`：

```text
let s: string = "hello";
let length = s.len();
print(length);
```

空字符串的长度为 `0`，结尾的空字节不计入长度：

```text
let empty: string = "";
let length: uint = empty.len();
```

`.len()` 可以用于字符串变量、字符串字面量、返回字符串的函数调用和字符串数组元素：

```text
let values: [string; 2] = ["one", "four"];
print(values[1].len());
print("hello".len());
```

长度按字节计算，不按 Unicode 字符或用户感知字符计算。例如 UTF-8 源文件中的
`"你好".len()` 返回 `6`，而不是 `2`。当前实现从字符串开头扫描到结尾空字节，因此
时间复杂度为 O(n)。

## 输出字符串

`print` 的第一个参数是字符串字面量时，该参数会作为格式字符串处理。使用 `%s` 输出
字符串变量、函数返回值或字符串数组元素：

```text
let value: string = "Hello";
print("%s\n", value);
```

格式占位符必须与后续参数类型和数量匹配，编译器当前不会完整校验这种匹配关系。不要直接
使用 `print(value)` 输出字符串变量；应始终提供字符串字面量格式：

```text
print("%s", value);
```

`assert` 的可选消息也支持字符串，但当前必须是字符串字面量：

```text
assert(result == 0, "result must be zero");
```

## 与标准库的关系

`string` 是内建语言类型，无需导入模块即可使用。`.len()` 也是内建方法，不属于标准库
函数。当前标准库尚未提供字符串拼接、比较、查找或转换函数；语言层面的字符串支持
不依赖这些标准库 API。标准库的现状见[标准库文档](std.md#current-limits)。

## 字符串数组

固定长度数组可以使用 `string` 作为元素类型。数组长度必须写在类型中，初始化元素数量
必须与声明长度一致：

```text
let names: [string; 3] = ["Alice", "Bob", "Carol"];
print("%s %s %s\n", names[0], names[1], names[2]);
```

可以通过下标读取或替换单个元素：

```text
names[1] = "David";
print("%s\n", names[1]);
```

重复初始化语法同样适用于字符串数组：

```text
let values: [string; 3] = ["empty"; 3];
```

字符串数组遵循普通固定长度数组的规则：

- 下标必须是整数表达式，并在运行时检查上下界。
- `const` 字符串数组不能替换其中的元素。
- 可以声明多维固定长度字符串数组，并逐层索引到字符串元素。
- 当前不能对整个数组或子数组赋值。
- 当前不能把内建数组作为函数参数或返回值；需要动态长度字符串数组时，可以使用
  `std.vec_string.StringVec`。

更多通用数组规则见[数据类型文档](data-types.md#固定长度数组)。

## 动态字符串数组

标准库 `std.vec_string` 提供了专用动态字符串数组 `StringVec`：

```text
import std.vec_string as vec;

fn main(): i32 {
    let values: StringVec = vec.new();
    values.push("Alice");
    values.push("Bob");
    print("%s\n", values.get(1));
    values.free();
    return 0;
}
```

`StringVec` 内部使用 `ptr<string>` 和 Runtime 内存函数实现。`push` 和 `set` 作为语句调用时
会自动把返回的结构体写回接收者；使用完后调用 `values.free()` 释放内部缓冲区。

## 实现模型

字符串字面量在 LLVM IR 中生成为以空字节结尾的全局字符串，`string` 值表示为 `i8*`
指针。字符串变量和数组元素保存该指针，因此：

- 字符串字面量的生命周期覆盖整个程序运行期。
- 给字符串变量或数组元素赋值会替换指针，不会修改原字符串内容。
- 当前没有修改字符串内部字节或字符的语法。
- `print` 的 `%s` 使用 C 字符串约定，以结尾空字节判断字符串结束位置。

## 当前限制

当前语言和标准库尚未提供以下字符串能力：

- 字符串拼接与插值。
- 具有明确内容语义的字符串相等、大小或字典序比较；不要依赖指针比较结果。
- Unicode 字符数量查询、字符串下标和切片。
- 可变字符串及字符替换。
- 动态创建、复制或释放单个字符串的标准库 API。
- 字符与 Unicode 码点级别的处理 API。

## 测试

字符串行为由以下运行测试覆盖：

- [`tests/run-pass/types/strings.tp`](../tests/run-pass/types/strings.tp)：变量、类型推断、
  赋值、函数参数、返回值和转义序列。
- [`tests/run-pass/types/string_arrays.tp`](../tests/run-pass/types/string_arrays.tp)：数组
  初始化、下标读取、元素替换和重复初始化。
- [`tests/run-pass/types/string_length.tp`](../tests/run-pass/types/string_length.tp)：字节长度、
  返回类型、空字符串、UTF-8 字符串和不同形式的接收者。

可以只运行这些测试：

```bash
TEST_FILTER=types/strings.tp sh tests/run-tests.sh build/4yue
TEST_FILTER=types/string_arrays.tp sh tests/run-tests.sh build/4yue
TEST_FILTER=types/string_length.tp sh tests/run-tests.sh build/4yue
```
