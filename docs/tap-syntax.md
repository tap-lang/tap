# Tap 语言参考（单文件版）

> 目的：把这一个文件丢给 AI（或新人），读完就能写出、读懂、改对 Tap 代码。
> 内容按「词法 → 类型 → 顶层声明 → 语句 → 表达式 → 模块 → 语义 → 标准库 → 错误写法」
> 排列，规则全部对齐当前编译器实现（`src/lexer.c` / `src/parser.c` 为准）。
> Tap 是静态类型、编译到 LLVM IR 的语言，语法接近 Rust/Go 的混合体，但**不是**二者的子集。

## 1. 最小可运行程序

```tap
fn main(): i32 {
    print("Hello tap! 你好，世界！\n");
    return 0;
}
```

- 源文件扩展名 `.tp`（`.tap` 也接受）。
- 入口是 `main`：可以省略返回类型（等价于无返回值），也可以写 `: i32`。
- `print` 来自自动加载的 `std/prelude.tp`，不需要 import。

## 2. 词法

### 2.1 注释

```tap
// 行注释，到行尾
/* 块注释，
   可以跨行 */
```

### 2.2 标识符

`[A-Za-z_][A-Za-z0-9_]*`，区分大小写。**不支持中文/Unicode 标识符**（字符串里可以随便写）。

### 2.3 关键字（全部，共 43 个）

```text
as  bool  break  char  const  continue  c_string  else  elseif  enum  extern  f32  f64
false  fn  for  i8  i16  i32  i64  i128  if  impl  import  int  let  match  mut  null
pub  return  sizeof  String  struct  trait  true  u8  u16  u32  u64  u128  uint  while
```

`void` 不是关键字——**省略返回类型就是无返回值**。

### 2.4 数字字面量

| 写法 | 例子 | 说明 |
|---|---|---|
| 十进制 | `123`、`1_000_000` | `_` 只能夹在数字之间 |
| 十六进制 | `0xFF`、`0xff` | 前缀 `0x`/`0X` |
| 八进制 | `0o17` | 前缀 `0o`/`0O` |
| 二进制 | `0b1010` | 前缀 `0b`/`0B` |
| 浮点 | `1.5`、`1e10`、`1.5e-3`、`2E+8` | 有小数点或指数即浮点 |

- 没有类型后缀（没有 `1u8`、`1.0f64` 这种写法）。
- 整数字面量默认 `i32`；浮点字面量默认 `f32`。目标类型由声明位置/上下文决定，
  `let x: i64 = 1;` 里的 `1` 会按 `i64` 生成。
- 进制写法的值同样可以按上下文变宽，`0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF` 能当 `u128` 用。

### 2.5 其它字面量

```tap
true         false          // bool
null                       // 空指针，只能给指针类型
'a'  '中'  '😀'  '\n'       // char：单引号，恰好一个 Unicode 码点
"文本"                      // String
```

### 2.6 字符串转义

| 转义 | 含义 | 转义 | 含义 |
|---|---|---|---|
| `\n` | 换行 | `\\` | 反斜杠 |
| `\r` | 回车 | `\"` | 双引号 |
| `\t` | 制表符 | `\'` | 单引号 |
| `\b` | 退格 | `\/` | 斜杠（方便写 JSON） |
| `\f` | 换页 | `\xNN` | 1 字节，恰好两位十六进制 |
| `\v` | 垂直制表 | `\u{...}` | 1–6 位十六进制 Unicode 码点 |
| `\0` | NUL | `\` + 换行 | 续行：吞掉换行和下一行前导空白 |

- 未知转义（如 `\d`）是**编译错误**，不会被原样保留。
- `\u` 必须带花括号：`\u{4E2D}`；码点上限 `0x10FFFF`，代理区 `D800–DFFF` 报错。
- 字符串可以含内嵌 NUL（`\0`、`\x00`、`\u{0}`），长度按解码后的字节数记，不会被截断。
- 字符串本身支持直接写多行（换行就是换行字符）。

### 2.7 运算符

```text
+  -  *  /  %  ++  --  =  ==  !=  <  >  <=  >=  !  &&  ||  &  |  ^  ~
(  )  {  }  [  ]  ;  ,  :  .  =>  ?  ...
```

## 3. 类型

### 3.1 基本类型

| 类别 | 类型 |
|---|---|
| 有符号整数 | `i8` `i16` `i32` `i64` `i128`，`int`（= 指针宽度，64 位） |
| 无符号整数 | `u8` `u16` `u32` `u64` `u128`，`uint`（= 指针宽度） |
| 浮点 | `f32` `f64` |
| 其它标量 | `bool`、`char`（Unicode 码点，底层 u32） |
| 文本 | `String`（`{ ptr, len }` 值类型，UTF-8，不是 C 字符串）、`c_string`（C 的 `const char *`） |
| 空 | 省略返回类型即无返回值，没有 `void` 关键字 |

### 3.2 复合类型语法

```text
[T; N]              固定长度数组，N 必须是正整数字面量（`[i32; 4]`、`[i32; 0x4]`；N ≥ 1）
*T                  原始指针（`*i32`、`*u8`）
*fn(T1, T2): R      函数指针（省略 `: R` 表示无返回值；取值写 `&函数名`）
&T                  共享借用
&mut T              独占借用
Name                具名类型（struct / enum）
mod.Name            带模块前缀的具名类型（`vec.Vec`）
Name<T1, T2>        泛型实例（`Vec<i32>`、`Result<i64, String>`）
&&T                 等价于 `&(&T)`（词法把两个 `&` 合成 `&&`，类型位置会拆开）
```

### 3.3 类型转换

只有显式 `as` 一种写法，可链式。支持的方向就这几类：

| 方向 | 例子 |
|---|---|
| 整数 → 整数 | `n as i8`（窄化按位截断）、`wide as i32` |
| 整数 → 浮点 | `n as f64` |
| 浮点 → 浮点 | `x as f32` |
| String → 整数 / bool | `"42" as i32`、`"true" as bool`（运行时解析，要错误处理用 `std.parse`） |
| String ↔ c_string | `s as c_string`（零拷贝取内部指针）、`c as String`（扫到第一个 NUL） |

**不支持**的方向（会编译报错）：

```tap
let i = 1.5 as i32;      // error: cannot cast f64 to i32（浮点不能转整数）
let p = arr as *i32;     // error: cast target must be a scalar type（数组/指针不能互转）
let f = "1.5" as f64;    // error: cannot cast String to f64（文本转浮点走 std.parse.parse_f64）
```

赋值/传参/返回时的隐式转换只允许：整数之间、浮点之间、int → float，以及 `String` → `c_string`。
`bool`、`char` 不参与隐式数值转换（`let x: i32 = true;` 是错误），`char` 与整数只能显式互转。

## 4. 顶层声明

```text
程序     := { 顶层项 }
顶层项   := ['pub'] ( fn | extern_fn | struct | enum | const | trait | impl ) | import
```

- 顶层只有这几种：`import`、`fn`、`extern fn`、`struct`、`enum`、`const`、`trait`、`impl`。
  **没有**全局变量、没有 `mod` 块、没有 `use`、没有前置声明。
- `pub` 只能修饰 `fn` / `const` / `struct` / `enum` / `trait`；写 `pub import`、`pub impl` 是错误。
- 不写 `pub` 的顶层符号是**模块私有**：别的模块 import 不到，也不会占用全局名字。
- 函数可以调用后面才定义的函数，互相递归也没问题。

### 4.1 函数

```text
['pub'] 'fn' 名字 ['<' 类型参数 {',' 类型参数} '>'] '(' [参数表] ')' [':' 返回类型] 块
类型参数 := 标识符 [':' 约束 {'+' 约束}]
参数表   := 参数 {',' 参数} [',' '...']     // '...' 变参必须在最后
参数     := 标识符 [':' 类型]                // 省略类型 = i32（不推荐）
```

```tap
pub fn add(a: i64, b: i64): i64 {
    return a + b;
}

fn identity<T>(value: T): T {
    return value;
}

fn describe<T: Show>(value: T): String {   // 带 trait 约束
    return value.show();
}
```

`extern fn` 只有签名、没有函数体，结尾是分号。变参 `...` 只能出现在这种 C 声明里，
或者原样转发给另一个变参函数：

```tap
extern fn getchar(): i32;
extern fn printf(format: c_string, ...): i32;

fn log(fmt: c_string, ...): i32 {   // 普通函数的变参只能转发，不能自己解析
    return printf(fmt, ...);
}
```

### 4.2 struct

```text
['pub'] 'struct' 名字 ['<' 类型参数 '>'] '{' 字段 {',' 字段} [','] '}'
字段 := 标识符 ':' 类型
```

```tap
struct Point {
    x: i32,
    y: i32,
}
```

- 至少一个字段；**不支持默认字段值**、不支持字段省略初始化。
- struct 是值类型：赋值、传参、返回都是拷贝。

### 4.3 enum

```text
['pub'] 'enum' 名字 ['<' 类型参数 '>'] '{' 成员 {',' 成员} [','] '}'
成员 := 标识符 ['(' 类型 {',' 类型} ')']
```

```tap
enum Color {
    Red,
    Green,
    Custom(u8, u8, u8),    // 带载荷
}
```

- 至少一个成员；载荷类型可以是多个。
- 构造：`Color.Red`、`Color.Custom(1, 2, 3)`。
- **所有成员都没载荷**时，枚举值只能用 `==` / `!=` 比较，不能 `match`（见 §5.5）。
- 泛型枚举构造要能推出类型实参，推不出就显式写：`Result<i64, String>.Ok(1)`；
  否则报 `generic enum construction needs a type annotation`。

### 4.4 const

```tap
const MAX: i32 = 100;          // 顶层常量，类型必须显式写
pub const PI: f64 = 3.14;      // 跨模块可见
```

局部也可以用 `const`，规则相同（必须写类型、必须初始化）。

### 4.5 trait 与 impl

```text
['pub'] 'trait' 名字 '{' { 'fn' 方法名 '(' 'self' 接收者... ')' [':' 返回类型] ';' } '}'

'impl' [trait名 'for'] 类型名 '{' { ['pub'] 'fn' ... 方法体 } '}'
```

```tap
trait Show {
    fn show(self): String;              // 接收者必须是 self / &self / &mut self
}

struct Point { x: i32, y: i32 }

impl Show for Point {                   // trait 实现
    fn show(self): String {
        return "point";
    }
}

impl Point {                            // 固有 impl（没有 trait）
    fn sum(&self): i32 {
        return self.x + self.y;
    }
    pub fn scale(&mut self, k: i32) {   // 固有 impl 的方法默认模块私有，pub 才跨模块
        self.x = self.x * k;
    }
}
```

- 方法体里接收者三种写法都行：`fn f(self)`（按值，拿到副本）、`fn f(&self)`、`fn f(&mut self)`；
  也可以照普通参数写成 `fn f(v: Point, ...)`，效果与 `self` 相同。
- trait 方法只能声明签名，不能有默认实现；trait 不支持关联类型、`dyn`、运行时多态。
- 同一个类型可以既有一个 trait impl，又有自己的固有 impl（方法名别撞）。
- `impl` 的目标类型不能是泛型实例（`impl Vec<T>` 暂不支持）。

### 4.6 import

```text
'import' 模块路径 ['as' 别名] ';'
模块路径 := 标识符 {'.' 标识符} ['.' '*']
```

```tap
import std.vec;                 // 绑定模块名 vec
import std.vec as v;            // 改名
import std.vec.Vec;             // 直接绑定成员名 Vec
import std.vec.*;               // 通配，不绑定裸名
```

- `import` 只能出现在顶层；写在函数体里是语法错误。
- 路径里的点先整条当模块名找，找不到再把最后一段当成员名（所以 `std.vec.Vec` 成立）。

## 5. 语句

```text
语句 := let | const | return | if | while | for | match
      | break | continue | 赋值 | 自增自减 | 调用 | 块
```

- 语句之间用 `;` 结束（`if` / `for` / `while` / `match` / 块本身不加 `;`）。
- 花括号是**真作用域**，可以嵌套、可以遮蔽同名变量。

### 5.1 变量

```tap
let x = 1;              // 类型可推断
let y: i64 = 2;         // 显式类型
let z: [i32; 3] = [1, 2, 3];
const LIMIT: i32 = 10;  // 局部常量也要写类型
```

没有 `mut` 修饰符——**局部变量天然可写**，`mut` 只出现在借用语法 `&mut T` 里。

### 5.2 赋值

```tap
x = 5;           // 标识符
arr[0] = 5;      // 下标
p.x = 5;         // 字段（接收者可以是任意可寻址表达式：make().x、a.b.c）
*p = 5;          // 解引用写，等价于 p[0] = 5
i++;  i--;       // 只能用于标识符（不是表达式，没有 ++i）
```

赋值是**语句**，没有返回值的写法（`let a = b = 1;` 不合法）。

### 5.3 条件

```tap
if (x > 0) {          // 括号必须有
    ...
} else if (x == 0) {  // `else if` 与 `elseif` 完全等价，随便用
    ...
} elseif (x < -10) {
    ...
} else {
    ...
}
```

条件表达式必须是 `bool`（`if (1)` 是错误）。`if` 不是表达式，不能写 `let a = if ...`。

### 5.4 循环

```tap
while (i < 10) {
    i++;
}

for (let i = 0; i < 10; i++) {   // 三段都可以省略：for (;;) { }
    print("%d\n", i);
}

for (; index < size; index = index + 1) { ... }   // 更新部分也可以是一条语句
```

`break;` / `continue;` 只能出现在循环体内，否则是语法错误。
**没有** `loop`、`for x in arr`、迭代器语法。

### 5.5 match

```tap
match (color) {
    Color.Red => { print("red\n"); }
    Color.Custom(r, g, b) => { print("%d\n", r); }   // 绑定载荷
    _ => { print("other\n"); }
}
```

- 目标必须写在括号里：`match (expr) { ... }`。
- **只能 match「带载荷的枚举」**（至少一个成员写成 `Variant(T)`）。纯 C 风格的枚举
  （所有成员都没有载荷）会报 `match requires a value of a payload enum type`，
  这种情况直接用 `==` 比较：`if (c == Color.Red) { ... }`。
- 模式只有两种：`Enum.Variant`（可带载荷绑定）和 `_`；枚举名可以带模块前缀（`a.Color.Red`）。
- 分支体可以是块，也可以是单条语句（`Color.Red => print("x\n");`）。
- 作为**表达式**用时每个分支写值，需要目标类型上下文：

```tap
let name: String = match (color) {
    Color.Red => "red",
    _ => "other",
};
```

## 6. 表达式

### 6.1 优先级（低 → 高）

| 级别 | 运算符 | 结合性 |
|---|---|---|
| 1 | `\|\|` | 左 |
| 2 | `&&` | 左 |
| 3 | `\|` | 左 |
| 4 | `^` | 左 |
| 5 | `&` | 左 |
| 6 | `==` `!=` `<` `>` `<=` `>=` | 左 |
| 7 | `<<` `>>` | 左 |
| 8 | `+` `-` | 左 |
| 9 | `*` `/` `%` | 左 |
| 10 | 前缀 `&` `&mut` `*` `-` `!` `~`，后缀 `?` `[i]` `.f` `.m()` `as T` | — |

> 注意 `&` 有两义：一元位置是取地址，二元位置（优先级 5）是按位与。`<<`/`>>` 只用于整数移位。

### 6.2 基本表达式

```tap
42                 // 整数字面量（默认 i32）
1.5                // 浮点字面量（默认 f32）
"text"             // String
'c'                // char
true  false        // bool
null               // 指针空值
name               // 标识符
obj.field          // 字段访问
arr[index]         // 下标
f(a, b)            // 函数调用
f<T>(a)            // 显式泛型实参
obj.method(a)      // 方法调用（语法与字段+调用相同）
Enum.Variant       // 无载荷枚举值
Enum.Variant(a)    // 带载荷
Enum<T>.Variant(a) // 显式类型实参的泛型枚举
sizeof(i64)        // 编译期大小，结果类型 uint
(x + 1)            // 括号
```

### 6.3 复合字面量

```tap
Point { x: 1, y: 2 }            // 结构体字面量，字段逗号分隔，允许尾逗号
let p: Point = Point { x: 1, y: 2 };
[1, 2, 3]                       // 数组字面量，元素类型由第一个元素决定
[0; 10]                         // 重复初始化：10 个 0
```

- 结构体字面量必须写出**全部**字段，顺序无所谓；没有 `..default` 展开。
- 泛型结构体字面量能从字段反推类型实参（`Box { value: 3 }`），也可以显式写 `Box<i64> { value: 4 }`。
- 数组字面量在需要数组的位置可以直接写（`return [1, 2];`、结构体字段里）。
- 数组长度至少为 1，所以**没有空数组**（`[i32; 0]` 会报
  `array length must be between 1 and INT64_MAX`）。

### 6.4 一元与后缀

```tap
&value            // 共享借用
&mut value        // 独占借用
&function_name    // 取函数指针（类型要写成 *fn(...): R）
*pointer          // 解引用（读）
-value            // 取负（降级为 0 - value）
!flag             // 逻辑非（降级为 flag == false）
~bits             // 按位取反（降级为 bits ^ -1）
result?           // Result 传播：失败时从当前函数提前返回 Err
value as i64      // 类型转换，可链式：n as i64 as f64
```

函数指针只能通过 `&函数名` 取得（直接写函数名是类型错误）：

```tap
fn double(v: i32): i32 { return v * 2; }

fn apply(cb: *fn(i32): i32, value: i32): i32 {
    return cb(value);
}

let r = apply(&double, 5);   // 10
```

`?` 绑定最紧（`a?[0]` 是 `(a?)[0]`），只能用在返回 `Result` 的函数里。

### 6.5 字符串操作

```tap
let s = "hello";
s.len()            // 字节长度，uint
s.char_count()     // Unicode 码点数
s.byte_at(0)       // 取单字节，u8
s.slice(1, 3)      // 子串（按字节下标，左闭右开）
s.contains("ell")  // bool
s.starts_with("he")
s.ends_with("lo")
s.replace("l", "L")
"a" + "b"          // 拼接，结果是新 String
"s" == "t"         // 按内容比较
```

这些是编译器内建方法（不是 std 里的函数），只对 `String` 生效。

## 7. 模块与可见性

- 一个 `.tp` 文件就是一个模块；模块名就是文件名（`std/vec.tp` → `std.vec`）。
- 引用别的模块的符号：`import` 后写 `别名.符号`（`vec.push(...)`），或直接 import 成员名。
- 只有 `pub` 的顶层 `fn` / `const` / `struct` / `enum` / `trait` 能被外部使用；
  固有 impl 的方法要写 `pub fn` 才跨模块。
- 标准库自动加载 `std/prelude.tp`，其余按需 import。

```tap
import std.vec;
import std.vec.Vec;

fn main(): i32 {
    let mut_ish: Vec<i32> = vec.new<i32>();   // 变量本身不需要 mut
    vec.push(&mut_ish, 1);
    return 0;
}
```

## 8. 语义要点（写代码前必读）

1. **函数无前置声明**：直接调用后面定义的函数即可，编译器自己收齐。
2. **值语义**：struct、数组、`String` 赋值/传参都是拷贝；要避免拷贝就传 `&T`。
3. **借用检查**：`&T` 共享借用、`&mut T` 独占借用，同一作用域内二者不能同时存在；
   需要修改时函数签名要写 `&mut T`，调用处写 `&mut value`。借用检查在编译期完成（NLL 风格）。
4. **`?` 传播**：只能用在返回 `Result<T, E>` 的函数里，`Option` 式的空值不适用。
5. **整数默认 i32**：`let n = 10;` 的 `n` 是 `i32`；与 `i64` 混算需要显式 `as i64`。
6. **没有隐式 float→int**：`let i: i32 = 1.5;` 是错误，必须 `1.5 as i32`。
7. **`match` 分支需要目标类型**（表达式形式），变量声明处写类型标注即可。
8. **字符串不可变**：没有 `push_str`、索引赋值；拼接用 `+`，构造用 `StringBuilder`/`Vec`。
9. **没有**：运算符重载、函数重载、默认参数、闭包/lambda、泛型方法定义在 impl 里带新的类型参数、
   指针算术、`goto`、多返回值、异常、`try/catch`、`async`。
10. **编译错误是报位置的**：`error: ...` + `--> 文件:行:列`，按提示改即可。

## 9. 标准库速查

自动加载（`prelude`）：

```text
print(format: String, ...): i32        // 格式化输出，格式化符号见下
panic(message: String): i32
exit(code: i32): i32
is_ok<T, E>(r: Result<T, E>): bool
is_err<T, E>(r: Result<T, E>): bool
unwrap_or<T, E>(r: Result<T, E>, fallback: T): T
unwrap<T>(r: Result<T, String>): T
expect<T>(r: Result<T, String>, message: String): T
read_key(): i32
sleep_ms(ms: i32): i32
clear_screen(): i32

enum Result<T, E> { Ok(T), Err(E) }    // prelude 内声明，全程序可见
```

`print` 的格式化符号：`%d`（整数）、`%u`（无符号）、`%s`（String / c_string）、
`%f`（浮点）、`%c`（char/整数当字符）、`%x`/`%X`（十六进制）、`%p`（指针）、`%%`（百分号）。

按需 import 的模块：

| 模块 | 主要符号 |
|---|---|
| `std.vec` | `Vec<T>`、`new<T>()`、`with_capacity<T>(uint)`、`len`、`capacity`、`reserve`、`push`、`get`、`get_ref`、`set`、`clear`、`free` |
| `std.byte_vec` | `ByteVec`、`new`、`with_capacity`、`len`、`push(&mut, u8)`、`extend(&mut, String)`、`get`、`set`、`to_string`、`free` |
| `std.string_builder` | `StringBuilder`、`new`、`with_capacity`、`len`、`append<T>`、`append_byte`、`clear`、`to_string`、`free` |
| `std.fs` | `File`、`open`、`read`、`write`、`close`、`eof`、`read_text`、`write_text`、`append_text`、`exists`、`remove`、`Dir`、`open_dir`、`next_entry`、`read_dir`、`make_dir`、`is_dir` |
| `std.io` | `printf`、`eprintf`（format + 变参） |
| `std.env` | `argc()`、`arg(i)`、`args()`、`var(name)`、`vars()` |
| `std.sys` | `system(cmd)`、`capture(cmd)`、`popen`、`pclose`、`read_stream`、`exit_code`、`is_windows` |
| `std.parse` | `parse_i64(String): Result<i64, String>`、`parse_f64`、`format_i64`、`format_f64` |
| `std.math` | `PI`、`square<T>`、`pow<T>`、`pow_f64`、`abs<T>` |
| `std.memory` | `malloc<T>(uint): *T`、`realloc<T>`、`free<T>` |
| `std.random` | `random(max: i32): i32` |
| `std.json` | `escape`、`escape_quoted`、`unescape`、`append_escaped`、`append_quoted` |
| `std.net` | `Socket`、`parse_ipv4`、`build_address`、`listen_on`、`connect_to`、`accept_from`、`receive`、`send_text`、`send_bytes`、`close_socket`、`is_open`、`local_port` |

约定：读操作收 `&T`、写操作收 `&mut T`、`free` 收值本身（消费掉）。

## 10. 常见非法写法

```tap
let x = 1                 // 缺分号
if x > 0 { }              // if 的条件必须加括号：if (x > 0)
let mut x = 1;            // 没有 mut 变量修饰符
for x in arr { }          // 没有 for-in，只能 for (init; cond; update)
fn f() -> i32 { }         // 返回类型用冒号：fn f(): i32
struct P { x: i32 };      // struct 结尾不写分号
impl Vec<T> { }           // impl 不支持泛型目标类型
let a = b = 1;            // 赋值没有值，不能链式
x += 1;                   // 没有复合赋值，写 x = x + 1 或 x++
let c = s[0];             // String 不能下标：variable 's' is not an array or pointer
let e: [i32; 0] = [];     // 数组长度至少 1，没有零长数组
let i = 1.5 as i32;       // 浮点不能转整数
let p = a as *i32;        // 数组/指针不能互转
let p = &1;               // 不能借用字面量：reference target is not addressable
match (color) { .. }      // 纯 C 风格枚举（无载荷）不能 match，用 == 比较
match (*e) { .. }         // match 目标不能直接是解引用，先绑定到局部变量
```

## 11. 完整示例

```tap
import std.vec;
import std.vec.Vec;

trait Describe {
    fn describe(self): String;
}

struct Point {
    x: i32,
    y: i32,
}

impl Describe for Point {
    fn describe(self): String {
        return "point";
    }
}

impl Point {
    fn sum(&self): i32 {
        return self.x + self.y;
    }
    fn shift(&mut self, dx: i32, dy: i32) {
        self.x = self.x + dx;
        self.y = self.y + dy;
    }
}

enum Shape {
    Dot,
    Rect(i32, i32),
}

const SCALE: i32 = 2;

fn area(shape: Shape): i32 {
    match (shape) {
        Shape.Dot => { return 0; }
        Shape.Rect(w, h) => { return w * h * SCALE; }
        _ => { return -1; }
    }
}

fn main(): i32 {
    let p: Point = Point { x: 1, y: 2 };
    print("%s sum=%d\n", p.describe(), p.sum() * SCALE);

    let q = p;              // 值拷贝
    q.shift(1, 1);
    print("p=%d q=%d\n", p.sum(), q.sum());

    let n = area(Shape.Rect(3, 4));
    print("area=%d\n", n);

    let values: Vec<i32> = vec.new<i32>();
    vec.push(&mut values, 10);
    vec.push(&mut values, 20);
    for (let i = 0; i < vec.len(&values); i++) {
        print("%d ", vec.get(&values, i));
    }
    print("\n");

    let chars = "tap😀";
    print("bytes=%d chars=%d\n", chars.len(), chars.char_count());
    return 0;
}
```

---

**给 AI 的三条使用建议**：① 写新代码前先看 §4/§5/§6 的骨架，别套 Rust 语法；
② 类型不确定就显式标注（`let x: i64 = ...`），Tap 不做很多隐式推断；
③ 涉及借用/修改时先确认签名里的 `&` 还是 `&mut`，这是最常见的编译错误来源。
