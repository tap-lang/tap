## 数据类型

### 整数类型
| 类型 | 描述 |
| --- | --- |
| `int` | 32/64位有符号整数(根据架构决定) |
| `uint` | 32/64位无符号整数(根据架构决定) |
| `i32` | 32位有符号整数 |
| `u32` | 32位无符号整数 |
| `i64` | 64位有符号整数 |
| `u64` | 64位无符号整数 |
| `i8` | 8位有符号整数 |
| `u8` | 8位无符号整数 |
| `i16` | 16位有符号整数 |
| `u16` | 16位无符号整数 |
| `i128` | 128位有符号整数 |
| `u128` | 128位无符号整数 |

以上整数类型已经支持用于变量声明、函数参数和函数返回值。实现约定如下：

- `int` 和 `uint` 使用当前目标架构的指针位宽；在 64 位目标上对应 LLVM `i64`。
- 无类型标注的十进制整数字面量默认为 `i32`，赋值、传参或返回时会转换到目标类型。
- 整数字面量保留十进制原文，可以直接构造 `i128/u128` 常量。
- 窄类型参与运算时按表达式操作数提升；写入更窄的目标类型时按 LLVM 整数截断规则处理。
- 无符号类型使用无符号除法和无符号大小比较；等于和不等于比较不区分符号。
- 当前没有整数字面量后缀，类型由声明位置或表达式上下文确定。

### 常量声明

常量使用 `const NAME: Type = value;` 声明。常量必须显式写出类型，并且初始化后不能再次赋值；数组常量也不能修改其中的元素。

```text
const ANSWER: i32 = 42;
const VALUES: [i32; 4] = [0; 4];
```

### 浮点数类型
| 类型 | 描述 |
| --- | --- |
| `f32` | 32位单精度浮点数 |
| `f64` | 64位双精度浮点数 |

### 字符串类型
| 类型 | 描述 |
| --- | --- |
| `string` | 以空字节结尾的字符串引用 |

字符串支持字面量、变量、常量、函数参数与返回值、`.len()` 字节长度方法，以及固定长度
字符串数组。转义序列、输出方式、数组示例和当前限制见[字符串文档](data-type-string.md)。

### 布尔类型
| 类型 | 描述 |
| --- | --- |
| `bool` | 布尔值，`true`或`false` |

### 复合类型
| 类型 | 描述 |
| --- | --- |
| `[T; N]` | 固定长度数组 |
| `*T` | 指向 `T` 的裸指针 |
| `struct` | 结构体 |
| `enum` | 枚举 |
| `tuple` | 元组 |

### 结构体类型

结构体使用 `struct Name { ... }` 声明字段，字段通过 `value.field` 访问。第一版结构体是值类型，
可以用于局部变量、函数参数和函数返回值。

```text
struct Point {
    x: i32,
    y: i32
}

fn move(point: Point): Point {
    return Point { x: point.x + 1, y: point.y + 1 };
}
```

- 结构体字面量使用 `TypeName { field: value, ... }`。
- 初始化时必须提供所有字段，字段名不能重复，也不能写不存在的字段。
- 可对非 `const` 结构体变量的字段赋值，例如 `point.x = 10;`。
- 当前不支持结构体方法和默认字段值。模块中的结构体可以被导入，但类型名会进入全局类型命名空间，
  还不支持 `module.TypeName` 这种限定类型名。

### 枚举类型

枚举使用 `enum Name { ... }` 声明，成员按声明顺序从 `0` 开始递增。当前枚举在后端按
`i32` 表示，因此可以用 `%d` 打印，也可以用于比较、函数参数和函数返回值。

```text
enum Direction {
    Up,
    Down,
    Left,
    Right
}

fn turn(value: Direction): Direction {
    if (value == Direction.Left) {
        return Direction.Right;
    }
    return Direction.Up;
}
```

- 枚举成员通过 `EnumName.MemberName` 访问，例如 `Direction.Left`。
- 成员列表支持可选尾逗号。
- 当前枚举不支持自定义成员值；`Direction.Up` 为 `0`，`Direction.Right` 为 `3`。

### 固定长度数组

数组是同类型、固定长度、栈上分配的局部变量，元素类型可以继续是数组：

```text
let values: [i32; 4] = [10, 20, 30, 40];
let zeros: [i32; 4] = [0; 4];
values[1] = 99;
print("%d\n", values[1]);

let matrix: [[i32; 3]; 2] = [[1, 2, 3], [4, 5, 6]];
matrix[1][0] = 40;
print("%d\n", matrix[1][0]);
```

- 数组必须显式声明每层元素类型和长度，嵌套字面量的每一层必须与声明形状一致。`[value; N]` 可用于将同一个值重复初始化为长度为 `N` 的数组。
- 下标可以是最多 64 位的整数表达式，每一维都会在运行时检查上下界。
- 多维数组只有索引到最终标量元素后才能用于表达式或赋值。
- 当前不支持整个数组或子数组赋值、数组参数和动态长度。函数可以返回固定长度数组，
  返回值可用于数组变量初始化、继续返回或索引表达式。

### 指针类型

`*T` 表示指向 `T` 的裸指针，主要用于 Runtime ABI、引用传参和标准库容器。指针可以作为
变量、函数参数和函数返回值使用，也可以通过下标读写元素。`&value` 取得可写变量的地址并
产生 `*T`：

```text
import std.memory as memory;

fn increment(value: *i32): i32 {
    value[0] = value[0] + 1;
    return 0;
}

fn main(): i32 {
    let count: i32 = 0;
    increment(&count);

    let values: *string = memory.malloc(64);
    values[0] = "hello";
    values[1] = "4yue";
    print("%s %s\n", values[0], values[1]);

    values = memory.realloc(values, 128);
    return memory.free(values);
}
```

目前可以对变量、结构体字段和数组/指针元素取地址，例如 `&value`、`&point.x` 和
`&values[index]`；`let value_ref = &value;` 可以自动推断出对应的指针类型。因为尚未实现
只读指针类型，`const` 常量不能取地址。指针下标不会自动检查边界，调用方必须保证容量足够。
普通程序优先使用标准库封装，例如 `std.memory` 和 `std.vec_string`，只有编写底层库、引用传参
或对接 C ABI 时才建议直接使用 `*T`。

### 泛型函数

函数名后使用 `<T>` 声明类型参数，调用时可以显式指定类型，也可以由函数实参或目标返回类型
推断。类型实参直接写成 `identity<i32>(value)`，不需要 `::`：

```text
fn identity<T>(value: T): T {
    return value;
}

fn first<T, U>(left: T, right: U): T {
    return left;
}

fn main(): i32 {
    let inferred: i32 = identity(42);
    let explicit: string = identity<string>("4yue");
    let selected: string = first("left", 10);
    return inferred;
}
```

编译器会在 LLVM Codegen 前按实际类型为每组调用生成具体函数，即编译期单态化；运行时不保存
泛型类型信息。同一个泛型参数在一次调用中必须推断为兼容类型，无法推断或显式类型实参数量
不匹配时会报错。当前只支持泛型函数，不支持泛型结构体、类型约束和默认类型实参。

### 动态字符串数组

当前标准库提供了第一版专用动态数组 `std.vec_string.StringVec`，用于保存可变数量的
`string`：

```text
import std.vec_string as vec;

fn main(): i32 {
    let values: StringVec = vec.new();
    values.push("hello");
    values.push("4yue");
    print("%s\n", values.get(1));
    values.free();
    return 0;
}
```

`StringVec` 通过堆内存保存元素，`push` 会在容量不足时自动扩容。`values.push(...)`
和 `values.set(...)` 作为语句使用时会自动把返回的结构体写回 `values`。泛型函数已经可用，
但泛型结构体尚未实现，因此当前仍没有 `Vec<T>`。
