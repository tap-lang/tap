# 语言与类型系统：还没做的

本文记语言层面的缺口，**不含**借用检查（见 [`borrow.md`](borrow.md)）和字符串能力
（见 [`string.md`](string.md)）。每条都附实测的错误信息，都是 2026-11-06 用
`build/tap -ir -o /dev/null` 跑出来的。

## 缺口一览

| # | 缺口 | 现在什么表现 | 收益 | 代价 |
|---|---|---|---|---|
| 1 | ~~没有固有 impl（`impl Point { }`）~~ | ✅ 已支持（2026-11-09），见下 | 高 | 小 |
| 2 | 泛型类型不能 impl | `impl for a generic type is not supported yet` | 高 | 大 |
| 3 | ~~限定类型名 `vec.Vec<i32>`~~ | ✅ 已支持（2026-11-09），见下 | 中 | 小 |
| 4 | match 无嵌套模式、无分支守卫 | `expected '(' but got 'identifier'` | 中 | 中 |
| 5 | ~~载荷枚举不能按值递归~~ | ✅ 诊断已落地（2026-11-10）；按值递归本身仍不支持 | 中 | 中 |
| 6 | 没有 for-each | `expected '(' but got 'identifier'` | 高 | 大 |
| 7 | ~~表达式结果上不能调方法~~ | ✅ 已支持（2026-11-08），见下 | 高 | 中 |
| 8 | `if` 不是表达式 | 无 `let x = if (c) { 1 } else { 2 }` | 低 | 小 |
| 9 | 没有切片类型 `[T]` / `&[T]` | `expected token ';' but got ']'` | 中 | 大 |

## 1. 固有 impl（没有 trait 的 impl）

### ✅ 已完成（2026-11-09）

按下面「改法」里最省事的那条落地：`parse_impl` 读第一个标识符，后面跟 `for` 就是
`impl Trait for Type`，不跟就把它当**目标类型**、`trait_name` 置 NULL。方法登记、签名校验、
方法查找、Codegen 一条路径都没动 —— 它们本来就是按「类型 + 方法名」找的。

改动只有四处：

- `parse_impl`：多一条分支（`trait_name` 可为 NULL）。
- `create_impl` / `print_ast`：容忍 NULL（AST 打印成 `Impl: Point (inherent)`）。
- `type_implements_trait`：跳过没有 trait 名的 impl，所以固有方法**不**满足任何 trait 约束。
- `validate_impl_signatures`：没有 trait 就跳过签名比对，改为查一次「目标类型存在吗」
  （固有 impl 没有 trait 声明可以对照，类型名打错的话方法会变成谁都调不到的
  `get$Piont`，一声不响地消失）。

**同名冲突的定案和当初的建议不同**：不做「固有优先」。同一个方法名在固有 impl 和 trait impl
里各定义一次，会撞成同一个符号 `peek$Point`，由已有的重名检查报
`error: duplicate function definition 'peek$Point'`（带前一处定义的位置）。两个 trait 给同一
类型实现同名方法本来就是这么报的，沿用同一条规则比新加一层消歧更简单，也不会让
「这个方法到底调的是哪个」变成需要查 impl 顺序的事。

用例：[`tests/run-pass/traits/inherent_impl.tp`](../../tests/run-pass/traits/inherent_impl.tp)、
[`tests/compile-fail/traits/inherent_impl_unknown_type.tp`](../../tests/compile-fail/traits/inherent_impl_unknown_type.tp)、
[`tests/compile-fail/traits/inherent_method_is_not_a_trait.tp`](../../tests/compile-fail/traits/inherent_method_is_not_a_trait.tp)。
语法说明见 [`../data-types.md`](../data-types.md#固有-impl没有-trait-的-impl)。

**没跟着解决的**：没有接收者的关联函数（`fn new(): Point`）仍然不能通过 `Point.new()` 调用
—— 缺的是调用语法，和 impl 有没有 trait 无关。

### 当初什么表现

```text
struct Point { x: i32 }

impl Point {
    fn peek(&self): i32 { return self.x; }
}
```

```text
error: expected `for` after trait name in impl
```

**整个仓库里没有一个不带 `for` 的 `impl`** —— 所有方法都必须挂在一个 trait 上。想给
一个类型加一个方法，就得先定义一个只用一次的单方法 trait：

```text
trait Peek { fn peek(&self): i32; }
impl Peek for Point { fn peek(&self): i32 { return self.x; } }
```

标准库里 `std.vec` 之类因为全是自由函数反而绕开了这个问题，但一旦写面向对象的代码，
样板量立刻翻倍。

### 改法

`parser.c` 的 `parse_impl`：`impl` 之后读到的名字如果下一个 token 不是 `for`，就把它当
**目标类型**，`trait_name` 置空。后面登记方法、校验签名、`find_method_function` 的查找
路径都不用改 —— 它们本来就是按「类型 + 方法名」找的，只是现在多了一种没有 trait 约束的
来源。

需要额外定的规则：

- **同名冲突**：固有 impl 的方法和 trait impl 的方法同名时谁优先。建议固有 impl 优先
  （Rust 就是这条），并给一条明确的诊断说明另一个来自哪个 trait。
- **`impl Point` 里的方法不参与 trait 约束**：`fn f<T: Peek>(p: &T)` 不能调到固有方法，
  这是对的，不用特殊处理。

代价：小。`parse_impl` 一个分支 + 查找路径一个回退。

## 2. 泛型类型不能 impl

### 现在什么表现

```text
impl Sized2 for Vec<i32> {          // 连具体实例化都不行
    fn size(&self): uint { return self.len; }
}
```

```text
error: impl for a generic type is not supported yet
```

`impl<T> Show for Vec<T>` 更没有讨论的必要 —— 解析阶段就拒了。

### 为什么难

impl 表现在以「类型名」为 key。泛型类型要参与约束求解，key 必须变成「类型构造器 +
类型实参」，还要在单态化时按实参做匹配（`impl Show for Vec<T>` 能不能覆盖
`impl Show for Vec<i32>`？优先级怎么定？）。这和关联类型（见 [`../TODO.md`](../TODO.md)
的 trait 后续）是同一个量级的工作。

### 折中

先支持**具体实例化**（`impl Sized2 for Vec<i32>`）：把类型实参拼进 impl 表的 key
（`Vec<i32>` 而不是 `Vec`），单态化时按完全相同的类型名匹配。这能覆盖「给某个具体容器
加一个方法」的常见需求，改动局限在 impl 表的 key 生成和查找。

泛型形式（`impl<T> Show for Vec<T>`）留到关联类型那一期一起做。

## 3. 限定类型名 `vec.Vec<i32>`

### ✅ 已完成（2026-11-09）

Parser 侧加了 `parse_qualified_name()`（读标识符 + 点号循环，前瞻确认 `.` 后面是标识符），
Module Loader 侧在 `resolve_type_name` 里加了第 0 级查找，专门认 `alias.Type`。

**Parser 侧要改的地方比预想的多**：类型标注在 `parse_type`，但同名语法还出现在另外三处，
它们各有各的解析代码：

- **结构体字面量** `a.Point { x: 1 }`：`parse_primary` 里「标识符后面跟 `{`」的判断原来放在
  点号循环**之前**，只看到 `a` 就否掉了。移到循环之后即可。
- **match 模式** `a.Color.Red`：原来「读枚举名 → 要求 `.` → 读成员名」，只认一段前缀。
  改成整串读完再按**最后一个点**切（前面是枚举名，最后一段是成员名）。
- **impl 目标类型** `impl a.Point { }`：`parse_impl` 改用同一个读法。

**Module Loader 侧只有一处**：`resolve_type_name` 多一级 `find_qualified_type_import`，
按**第一个**点切成「别名 + 类型名」，只认命名空间导入的别名（单成员 / 通配导入没有前缀）。

两个坑：

1. **`find_qualified_type_import` 不能无条件报错。** `resolve_enum_member_name` 会拿
   `c.LABEL` 当前缀来**探测**（`c.LABEL.len` 按最后一个点切出来的），那里的成员是常量不是
   类型 —— 无条件报错会把 `tests/run-pass/modules/constants.tp` 打挂。所以加了
   `report_errors` 参数，探测路径传 0。
2. **已改名的类型本身就是带点的**（`__tap_module_0.Point`）。别名不是任何命名空间导入时
   必须回落到裸名查找，否则重复改写会把名字丢掉。别名认出来但成员不对时才报错：
   `module 'x' has no type 'Y'` / `type 'Y' is private to module 'x'` /
   `'y' in module 'x' is not a type`（都不带位置，`VarTypeNode` 没有行列信息）。

用例：[`run-pass/modules/qualified_type_name.tp`](../../tests/run-pass/modules/qualified_type_name.tp)
（标注 / 字面量 / 泛型实参 / match 模式四个位置）、
[`compile-fail/modules/qualified_type_name_unknown.tp`](../../tests/compile-fail/modules/qualified_type_name_unknown.tp)。
语法说明见 [`../module.md`](../module.md#限定类型名-aliastype)。

### 当初什么表现

```text
import std.vec as vec;

let v: vec.Vec<i32> = vec.new();
```

```text
error: expected token `=` but got `.`
```

`parse_type` 只读一个标识符就返回，后面的 `.` 留给外层的 `let` 解析，于是报「期待 `=`」。

### 为什么需要

类型名的三级查找（本模块 / 裸名导入 / 命名空间导入兜底）在**同时导入两个都导出 `Point`
的模块**时只能报歧义，唯一能显式消歧的写法就是 `a.Point`，而它恰好不支持。现在的绕法是
不写类型名、靠推断，或者改名。

### 改法

`parse_type` 里加一个和 `parse_primary` 一样的点号循环：读到标识符后如果跟 `.`，就把
前一个当模块名、后一个当类型名，走「显式模块前缀」这一级查找（优先级最高）。

代价：小。但要注意和 `*fn(...)`、`[T; N]` 这些类型的组合位置。

## 4. match 没有嵌套模式，也没有分支守卫

### 现在什么表现

```text
enum Shape { Dot(Point), Empty }
struct Point { x: i32, y: i32 }

let r: i32 = match s {
    Shape.Dot(Point { x: a, y: b }) => a + b,   // error: expected `(` but got `identifier`
    Shape.Empty => 0,
};
```

```text
let r: i32 = match s {
    Shape.Dot(n) if n > 0 => n,                 // error: expected `(` but got `identifier`
    Shape.Dot(n) => 0,
};
```

载荷位置只接受标识符列表（`Variant(a, b)`）或单个 `_`，不能再嵌套一层模式，也不能挂
条件。

### 改法

**嵌套模式**：把 `parse_match_pattern` 从「变体名 + 标识符列表」改成递归 —— 每个载荷位置
要么是标识符、要么是 `_`、要么是另一个变体模式、要么是结构体模式。降级阶段要跟着递归
生成字段提取。

**分支守卫**：在 `=>` 前接受 `if <表达式>`，生成的代码里在分支体前加一个条件跳转；
穷尽性检查要放宽（带守卫的分支不算「覆盖」，除非守卫恒真 —— 保守做法是要求末尾仍有
无守卫的兜底分支，和 Rust 一致）。

代价：中。模式语法和降级逻辑都要动，穷尽性检查要重新想。

### 收益

「枚举套结构体」是很常见的形状（`std.json` 的值类型就是），现在必须在 match 里先绑到
局部变量再逐字段取。

## 5. 载荷枚举不能按值递归

### ✅ 诊断已落地（2026-11-10）

按值递归仍然**不支持**（那是类型系统的事，要所有权模型才能自动装箱），但检测补上了：
`validate_user_types` 里按「全按值边」做环检测（`value_path_reaches`），环上出现
指针 / 引用 / 函数指针就断开，数组不打断。命中时报：

```text
error: recursive type 'Node' has infinite size; field 'next' must be a pointer ('*Node')
```

枚举版本的说法是「把载荷包进指针」，位置报在**枚举声明**上（降级出来的结构体是合成的、
没有源码位置）。每个类型只报第一条。

以前这里更糟：`struct Node { next: Node }` 声明期**静默通过**，IR 是 `%Node = type opaque`、
`sizeof(Node)` 变成 0，只有真的写构造时才在字段类型检查里报一条误导的
`struct 'Node' field 'next' type mismatch`；枚举版本则漏到 LLVM 校验才炸。

用例：`compile-fail/types/recursive_struct.tp`、`compile-fail/types/recursive_enum.tp`。

### 原始分析

先澄清一件事：**用指针递归已经是能用的**，两条都实测通过：

```text
struct Node { value: i32, next: *Node }        // 可以
enum Node { Leaf(i32), Pair(*Node, *Node) }    // 可以
```

不行的是**按值**递归 —— 类型大小依赖自身：

```text
struct Node { value: i32, next: Node }
```

```text
error: struct 'Node' field 'next' type mismatch
```

```text
enum Node { Leaf(i32), Pair(Node, Node) }
```

```text
LLVM IR verification failed: Cannot allocate unsized type
  %a = alloca %Node, align 8
storing unsized types is not allowed
```

`enum` 这条是**先漏到 LLVM 才炸**的：前端没查出来，报错信息对用户毫无意义，还带着
IR 片段。这本身就是一个该修的小 bug（不管要不要支持按值递归）。

### 要做的事

1. **必做**：在 sema 阶段检测「类型大小依赖自身」，给出明确诊断
   （`recursive type 'Node' has infinite size; use *Node instead`）。要按结构体 / 枚举
   的字段图做一次环检测，环上不能全是按值边。
2. **可选**：按值递归自动装箱（Rust 的 `Box`），需要先有所有权模型，不建议现在做。

所以这一条的实际工作量在**诊断**上，不在语言能力上。

## 6. 没有 for-each / 迭代协议

### 现在什么表现

```text
for x in v { ... }
```

```text
error: expected `(` but got `identifier`
```

`for` 只有三段式（`for (let i: i32 = 0; i < n; i++)`），没有任何迭代器概念。

### 三条路线

**a. 编译器内建**：`for (x in expr)` 按容器类型展开成 `while` + `len` + `get` 或
下标访问。改动在 `parser.c` + 降级阶段，能覆盖数组和 `Vec`，但对用户自定义类型无效。

**b. 结构化协议**：约定 `len` / `get` 两个方法的形状，编译器按名字查。比 a 通用，
但「按名字查方法」是一种隐式契约，容易出错。

**c. 真正的 `Iterator` trait**：需要关联类型（`type Item`），也就是
[`../TODO.md`](../TODO.md) 里「trait v1 的后续」第一条。代价最大，但方向最正。

**建议**：先做 a（覆盖 `[T; N]` 和 `Vec<T>`，这两个占了绝大多数场景），等关联类型
落地再考虑 c。

## 7. 表达式结果上不能调方法

### ✅ 已完成（2026-11-08）

没有走当初设想的「sema 降级成临时变量」，而是把接收者变成 AST 里的**一等节点**：
`FunctionCallNode.receiver`。`names.c` 把 `p.method()` 拆成 `receiver = p`、
`name = "method"`；parser 对 `expr.method()` 直接建带 receiver 的调用节点。
接收者的地址用 `expression_address` 求，所以字段链、下标、调用结果都能当接收者。

顺带解决的：

- `o.inner.peek()`（多级字段接收者）：原先被解析成「调用 `o.inner.peek`」，报
  `undefined function`；现在是正常的 `FIELD_ACCESS` 接收者。
- `list.get(0).peek()`（调用结果接收者）：不再需要先绑变量。
- 泛型方法的接收者类型现在从接收者**表达式**推断，`board.points.push(x)` 这类
  「容器字段上的方法」也能正确单态化（原先只能查名字 → 查不到类型 → 不实例化）。
- 借用检查里方法接收者的登记粒度跟着细化到字段路径。

用例：[`tests/run-pass/types/method_call_receivers.tp`](../tests/run-pass/types/method_call_receivers.tp)。

### 当初什么表现

```text
let list: Vec<Point> = vec.new();
list.push(inner);
print("%d\n", list.get(0).peek());
```

```text
error: calling a method on an expression result is not supported; bind the value to a variable first
```

`Vec` 套 `Vec`、结构体字段套结构体、`get` 之后再取字段 —— 全都要先绑临时变量：

```text
let first: Point = list.get(0);
print("%d\n", first.peek());
```

`outer.get(0).get(1)`、`items[1].peek()` 是同一个错误。

**但嵌套字段的诊断是坏的**：`o.inner.peek()` 报的是

```text
error: undefined function 'o.inner.peek'
```

因为它被解析成「调用一个叫 `o.inner.peek` 的函数」，而数组下标那条走的是另一条路径、
报得清楚。同一个语义限制给出两种质量完全不同的诊断，这个至少该统一。

### 当初的判断

语法上 `a.b(...)` 一律按「类型 `a` 的方法 `b`」解析，接收者只能是**标识符**；
`codegen` 的 `generate_method_call` 需要接收者符号的地址，表达式结果没有符号。

### 当初的改法（没有采用）

在 sema 阶段把 `expr.method(args)` 降级成：

```text
let __tmp_N = expr;
__tmp_N.method(args);
```

关键是**借用不能逃逸**：`__tmp_N` 的生命周期要覆盖整个调用表达式，而且返回值是引用时
要禁止（`let r = list.get(0).peek_ref();` 会让 `r` 指向一个已经消失的临时变量）。
`borrow.c` 里对「临时变量的借用不能出语句」已经有一套（`BorrowRecord` 的 `is_temp`），
可以扩展成「临时变量的引用不能绑到变量上」。

代价：中。收益很高 —— 这是目前写容器代码最频繁的摩擦点。

## 8. `if` 不是表达式

```text
let x: i32 = if (c) { 1 } else { 2 };   // 不支持
```

`match` 是表达式，`if` 是语句。要么加 `if` 表达式（要求两个分支类型一致、必须有
`else`），要么维持现状并在文档里引导用 `match`。

代价：小（AST 已有 `match` 表达式的实现可以照搬），收益低 —— 三目运算式
`cond ? a : b` 可能更符合 tap 的 C 风格，值得先决定要哪个。

## 9. 没有切片类型 `[T]` / `&[T]`

```text
fn sum(values: &[i32]): i32 { ... }
```

```text
error: expected token `;` but got `]`
```

只有定长数组 `[T; N]`，函数没法接收「任意长度的数组」。现在的替代是 `&Vec<T>`，
但栈上的定长数组传不进去，`&[i32; 3]` 和 `&[i32; 4]` 也不能互相替代。

要做需要引入「胖指针」（data + length），和 `&T` 现在是单个指针的表示冲突 ——
要么给切片单独一种表示，要么统一改成胖指针（后者会动全部 `&T` 的 ABI）。
代价：大，且和 [`borrow.md`](borrow.md) 的 L2 在同一片区域。

## 其余

- **trait 的高级形态**（关联类型、默认方法、`dyn Trait`、运算符重载）：见
  [`../TODO.md`](../TODO.md) 的「trait v1 的后续」一节。
- **`tap` 的 `pub` 可见性只作用于类型和函数**，字段没有单独的可见性，结构体字段全公开。
- **不算 bug 但容易踩的**（窄类型求值宽度、泛型实参不能从枚举载荷反推等）：见
  [`../BUG.md`](../BUG.md) 末尾。

## 已评估、决定不做

### 泛型实参换括号形态（2026-11-08 结案：不做）

现在的 `<...>` 在三个位置有歧义（泛型调用、泛型结构体字面量、泛型枚举变体的显式
实参），靠 `generic_arguments_followed_by` 存/恢复 lexer 游标扫一遍 `<...>` 来区分
（另有 `peek_next_token_type` 为 `<<` / `>>` 服务，词法层因此刻意不合并双字符）。

换形态的收益是删掉这 4 个函数约 60 行（占全仓 22421 行的 0.27%），代价却是：

- **三种括号全被占用，只能再加一个标记**。`[` 不行 —— `arr[0].x` 合法（`parser.c`
  的 factor 后缀链支持索引后再取字段），`Option[i32].Some(1)` 与 `v[i].x` 语法同形，
  只能靠语义区分，比现在的前瞻更糟；`(` 与调用冲突；`{` 与结构体字面量冲突。
- **源码迁移 474 处**（368 个 `.tp` 里 85 个含泛型语法）：类型位约 210、声明位 59、
  表达式位 205（调用 90 / 字面量 46 / 变体 69）。前 269 处正则可覆盖，后 205 处
  与 `<` 比较运算符同形，必须人工过。
- **文档 12 份**（`std.md` 18 处、`data-types.md` 9、本文件、`ast.md`、`TODO.md`、
  `borrow.md`、`module.md`、`bootstrap.md`、`data-type-string.md`、`todo/borrow.md`、
  `todo/tooling-plan.md`、`README.md`）。

结论：收益不足以抵成本。**唯一值得重启的条件**是将来做 IDE 或增量解析时需要 token
缓冲 —— 那时 `generic_arguments_followed_by` 直接操作 lexer 游标就是硬障碍。届时只
走窄版（仅表达式位加标记，删 3 个 wrapper 共 46 行，迁移 205 处，文档不动），不要
全量。若真要合并 `<<` / `>>` 令牌，必须走全量。

## 建议顺序

| 顺序 | 项目 | 理由 |
|---|---|---|
| ~~1~~ | ~~**7 表达式结果上调用方法**~~ | ✅ 已完成（2026-11-08） |
| ~~2~~ | ~~**1 固有 impl**~~ | ✅ 已完成（2026-11-09） |
| ~~3~~ | ~~**3 限定类型名**~~ | ✅ 已完成（2026-11-09） |
| 1 | **4 嵌套模式 + 守卫** | 枚举套结构体的常见形状；中等代价 |
| 2 | **6a for-each（内建版）** | 覆盖数组和 Vec 的主流场景 |
| 3 | **2 具体实例化的 impl** | 泛型形式留到关联类型那一期 |
| 4 | 8 / 9 | 各有前置依赖；5 的诊断已经落地 |
