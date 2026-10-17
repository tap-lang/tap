# 已知缺陷

2026-10-17 实测扫描的结论。**基线：`tests/run-tests.sh` 191/191 全绿**，所以下面这些
都是现有测试覆盖不到的地方。全部在 macOS arm64 + LLVM 21 上复现过。

| 编号 | 缺陷 | 严重程度 | 状态 |
|---|---|---|---|
| [B1](#b1-结构体字面量的字段类型完全不检查) | 结构体字面量的字段类型完全不检查，静默破坏内存布局 | **严重** | ✅ 已修 |
| [B2](#b2-实参和-return-的标量类型不检查) | 实参 / `return` 的标量类型不检查，崩出 LLVM 内部错误 | 中 | ✅ 已修 |
| [B3](#b3-bool-参与隐式整数转换) | `bool` 参与隐式整数转换，违反文档承诺 | 中 | ✅ 已修 |
| [B4](#b4-match-绑定泛型载荷后无法调用方法) | `match` 绑定泛型载荷后无法调用方法 | 中 | ✅ 已修 |
| [B5](#b5-通配分支和模块来的载荷类型让编译器段错误) | 通配分支 + 模块来的载荷类型 → 编译器段错误 | **严重** | ✅ 已修 |
| [D1](#d1-整数除零是未定义行为) | 整数除零是未定义行为，跨架构不一致 | 设计缺口 | 待定 |
| [D2](#d2-移位量超过位宽是未定义行为) | 移位量 ≥ 位宽是未定义行为 | 设计缺口 | 待定 |
| [D3](#d3-有符号和无符号混算静默按无符号) | 有符号 / 无符号混算静默按无符号 | 设计缺口 | 待定 |

B1~B5 于 2026-10-17 修复，回归从 191 涨到 **200 个用例**，fast + native 全绿。D1~D3 是
设计缺口，没有动。

**B5 是扫 B1~B4 时顺带发现的**，而且是 2026-10-15「pub 类型加模块前缀」那次改动引入的
**回归**——说明那次的测试没覆盖到「通配分支 + 模块来的载荷类型」这个组合。


复现方式：**B1~B4 的代码块是完整可编译的 `.tp` 文件**，直接 `tap run <文件>` 即可；
**D 系列给的是片段**，要自己套进 `fn main()` 里。

---

## B1. 结构体字面量的字段类型完全不检查

**严重程度：严重。** 不报错、不警告，直接按错误的内存布局写进去，把相邻字段冲掉。

```tap
struct P { x: i32, y: i32 }

fn main(): i32 {
    let p: P = P { x: "hello", y: 7 };
    print("sizeof=%u x=%d y=%d\n", sizeof(P), p.x, p.y);
    return 0;
}
```

实际输出（退出码 0，`x` 每次运行都不同——它是被截断的指针地址）：

```text
sizeof=8  x=12200024  y=1
```

指针是 8 字节，`x` 只有 4 字节，高 4 字节落到 `y` 上——**`y` 从 7 变成了 1**。
`y=1` 是稳定的（指针的高 32 位），`x` 则是 ASLR 下每次都不一样的垃圾值。

宽度不匹配就会破坏布局；宽度相同时只是值不对，不崩：

| 初始化值 | 目标字段 | 结果 |
|---|---|---|
| `"hello"`（8 字节指针） | `i32` | `x` = 截断的指针地址（每次不同），`y` 被冲成 `1` ✗ |
| `1.5`（`f32`） | `i64` | `x=1152921501381427200`（稳定），`y` 侥幸没被冲 ✗ |
| `true` | `i32` | `x=1`，`y=7`（宽度相同，只是值被零扩展） |
| `1` | `i32` | 正常 ✓ |

### 位置

`src/codegen.c` 的 `validate_struct_literal_fields`（约 1458 行）。

### 根因

这个函数只校验三件事：

1. 字段存在（`find_struct_field`）
2. 字段不重复
3. 每个字段都给了初始值

**它从不比较初始化表达式的类型和字段类型。** 循环末尾那句 `validate_var_type(context,
field->field_type)` 校验的是字段**声明**的类型，和初始化值无关。

嵌套结构体那一层能报错（`C { a: B { y: 1 } }` → `struct expression type mismatch`），
因为 `generate_struct_literal_value` 比较了 `struct_name`；所以漏掉的正好是**标量字段**。

### 修法

在 `validate_struct_literal_fields` 里按字段逐个比较：

```c
StructFieldNode *field = find_struct_field(struct_node, init->name, NULL);
if (field && !expression_assignable_to(context, init->expression, field->field_type)) {
    fprintf(stderr, "error: struct '%s' field '%s' type mismatch\n",
            struct_node->name, init->name);
    exit(1);
}
```

`expression_assignable_to` 已经覆盖了标量、结构体、枚举、指针四种目标类型，不用新写判断。

**注意**：这条诊断暂时带不了位置信息——`StructInitFieldNode` 和 `StructLiteralNode` 都
只有 `name` / `expression` / `fields`，**没有 `filename` / `line` / `column`**
（同函数里现有的几条诊断也是裸 `fprintf`）。要带位置得先给这两个 AST 节点加字段并让
parser 填上，属于独立改动。

**已修（2026-10-17）**，用例 `tests/compile-fail/types/struct_literal_field_type.tp`。

---

## B2. 实参和 `return` 的标量类型不检查

**严重程度：中。** 不产生错误代码，但把内部错误直接甩给用户，而且**没有位置信息**。

```tap
fn f(a: i32): i32 { return a; }

fn main(): i32 {
    return f("x");
}
```

实际输出：

```text
LLVM IR verification failed: Call parameter type does not match function signature!
ptr @string_literal.1
 i32  %call_result = call i32 @f(ptr @string_literal.1)
```

实测覆盖 6 种组合，全部崩成同一类 LLVM 错误：

| 场景 | 结果 |
|---|---|
| `f("x")` 传 `i32` 参数 | `Call parameter type does not match function signature!` |
| `f(1.5)` 传 `i32` 参数 | 同上 |
| `f(42)` 传 `string` 参数 | 同上 |
| `f("x")` 传 `f64` 参数 | 同上 |
| `fn g(): i32 { return "x"; }` | `Function return type does not match operand type of return inst!` |
| `fn g(): i32 { return 1.5; }` | 同上 |

### 位置

**两处用了同一个「只查非标量」的条件：**

- `src/codegen.c` 约 2524 行（函数实参）
- `src/codegen.c` 约 3740 行（`return` 语句）

```c
if (... && (param_type->enum_name || param_type->struct_name || param_type->is_pointer) &&
    !expression_assignable_to(context, argument, param_type)) {
```

`return` 那处是同一形状：`current_return_var_type->enum_name || ... || is_pointer`。

### 根因

标量目标（`i32` / `f64` / `string` / `bool`）不满足那个「非标量」条件，检查被整段跳过；
随后 `generate_expression_for_type` 强行按目标类型生成，产出类型不符的 IR，
最后由 LLVM 的 verifier 兜底报错。

### 修法

把两处条件里的「非标量」限定去掉，一律走 `expression_assignable_to`：

```c
if (!erased_pointer_argument && param_type &&
    !expression_assignable_to(context, argument, param_type)) {
```

这个函数末尾会落到 `scalar_types_compatible`，**标量判断本来就是对的**——`let` 那一路
（约 3718 行）就是这么查的，所以 `let x: i32 = 1.5` 能正常报错。

注意保留 `erased_pointer_argument`（Runtime 内存 ABI 的裸指针豁免）和变参部分的
`promote_variadic_argument`，不要一起改掉。

**已修（2026-10-17）**，用例 `call_argument_type.tp` / `return_type_scalar.tp`。
`return` 那处额外排除了 `is_array`——定长数组走自己的 `array return type mismatch`，
而 `expression_assignable_to` 对数组目标恒返回 0。

---

## B3. `bool` 参与隐式整数转换

**严重程度：中。** 违反 `docs/data-types.md` 里明写的规则。

```tap
fn main(): i32 {
    let x: i32 = true;    // 通过，x = 1
    let b: bool = 1;      // 通过
    return 0;
}
```

`docs/data-types.md:27` 写着：

> 整数之间可以隐式转换，按目标类型的位宽和符号做扩展或截断；**`bool` 不参与隐式数值转换**。

### 位置

`src/codegen.c` 的 `scalar_types_compatible`（约 56 行）。

### 根因

```c
static int scalar_types_compatible(enum LiteralType target, enum LiteralType source) {
    if (target == source) return 1;
    if (is_integer_type(target) && is_integer_type(source)) return 1;              // ← 问题在这
    if (is_float_type(target) && is_float_type(source)) return 1;
    if (is_float_type(target) && is_integer_type(source) && source != LITERAL_BOOL) return 1;
    return 0;
}
```

`is_integer_type` **包含 `LITERAL_BOOL`**，所以整数那条分支把 bool 一起放过了。
只有 float 那条（第三行）显式排除了 bool——**函数自己上面两行的注释就写着「bool 不参与
隐式数值转换」，但实现只在转 float 时兑现了。**

结果：`let f: f64 = true` 被挡 ✓，`let x: i32 = true` 放过 ✗。

### 修法

函数开头加一句，让 bool 只和 bool 兼容：

```c
if (target == LITERAL_BOOL || source == LITERAL_BOOL) return target == source;
```

改完 `is_float_type(...) && source != LITERAL_BOOL` 那句里的 `!= LITERAL_BOOL` 可以删掉
（已被前面的判断覆盖），也可以留着当双保险。

**已修（2026-10-17）**：按上面的写法加了一句，并删掉了那个冗余判断。用例
`bool_to_integer.tp` / `integer_to_bool.tp`，正例 `scalar_conversions.tp`（拓宽、收窄、
int→float、f32→f64、bool→bool、实参和返回值两条路径）。

---

## B4. `match` 绑定泛型载荷后无法调用方法

**严重程度：中。** 有明确的报错，不是静默错误，但让「载荷是容器」这种常见写法不可用。

```tap
import std.vec as vec;

enum Holder {
    Item(Vec<i32>),
    Empty,
}

fn main(): i32 {
    let v: Vec<i32> = vec.new();
    v = vec.push(v, 7);
    let h: Holder = Holder.Item(v);
    match (h) {
        Holder.Item(inner) => { print("len=%u\n", inner.len()); }
        Holder.Empty => { print("empty\n"); }
    }
    return 0;
}
```

实际输出：

```text
error: method 'len' is only available on string values
```

对照：字段访问（`inner.x`）、非泛型结构体载荷（`StringBuilder`）、函数参数形式
（`fn f(v: Vec<i32>)` 里的 `v.len()`）都正常。

### 位置

`src/generics.c` 的 `process_statements`（`NODE_MATCH_STATEMENT` 分支）。

### 根因与修法

见 [`TODO.md`](TODO.md) 的「match 绑定遇到泛型结构体载荷时无法调用方法」一节：
两个语句遍历器原先都没处理 `NODE_MATCH_STATEMENT`，补上遍历之后仍失败，因为
`process_statements` 没把分支的载荷绑定登记进它自己的类型环境，泛型方法 `len` 没有特化，
Codegen 的 `find_method_function` 匹配不上。

难点在**执行时机**：结构体字段的类型实参要到 `specialize_generics` 后半程才解析，
而 `process_statements` 跑在前半程。

**已修（2026-10-17）**。实际做法和这里的猜测不同——**不需要等后半程**：函数参数走的是
同一条路（`process_function` 登记的也是未 materialize 的 `param_types`，而
`fn f(v: Vec<i32>) { v.len(); }` 一直正常）。改了三处：

1. `process_statements` 和 `process_expression` **两个** match 分支都先调
   `bind_match_payloads`，再处理分支体 / 分支值（表达式形式走的是另一条路径，只改一处不够）。
2. 载荷类型从**降级后的内部结构体**取：变体首字段下标是 `variant->field_index`，
   第 offset 个绑定对应 `field_index + offset`（和 Codegen 的绑定逻辑一致）。
3. 泛型枚举（`Result<T, E>`）的载荷字段是**类型参数**（`T`/`E`），要按被匹配值的
   `type_arguments` 建 `TypeBinding` 再 `substitute_type`，否则登记进去的仍是 `T`。

被匹配值的类型有两种记法：泛型阶段按 `enum_name` 记，Codegen 阶段按 `struct_name` 记，
**两种都要认**——只认一种会静默 return，症状和没修一样。

用例：`match_generic_payload.tp`（单载荷 + 多载荷）/
`match_generic_enum_payload.tp`（`Result<Vec<i32>, string>` + match 表达式形式）。

---

## B5. 通配分支和模块来的载荷类型让编译器段错误

**严重程度：严重。** 编译器直接 SEGV，连诊断都没有。**这是 2026-10-15「pub 类型加模块
前缀」那次改动引入的回归**——`f702502`（改动前）跑同一个程序是正常的。

```tap
import std.string_builder as sb;

enum E { A(StringBuilder), B }

fn main(): i32 {
    let e: E = E.B;
    match (e) {
        E.A(t) => { print("a\n"); }
        _ => { print("wild\n"); }      // ← 有通配分支就崩
    }
    return 0;
}
```

实际输出：**什么都没有**，退出码 **139**（SIGSEGV），`-o` 也不产出文件。

### 触发条件

三个条件同时满足才崩：

| 条件 | 说明 |
|---|---|
| 有**通配分支** `_ =>` | 写成 `E.B =>` 就正常 |
| 载荷类型**来自其他模块** | `StringBuilder`、`Vec<i32>` 都算；本文件的 `struct S` 不算 |
| 载荷枚举 | 普通枚举没有载荷，不涉及 |

「来自其他模块」之所以关键：那些类型名带 `__tap_module_N.` 前缀，才会让
`resolve_type_name` 真的去查导入绑定。

### 位置与根因

`src/module.c` 的 `rewrite_statement_list`（`NODE_MATCH_STATEMENT` 分支）：

```c
char *renamed_enum = copy_string_or_null(
    resolve_type_name(current_module, bindings, arm->enum_name));
```

**通配分支的 `arm->enum_name` 是 NULL**（`ast.h` 里写明「通配分支为 NULL」）。

改动前的 `resolve_type_name` 只查本模块导出：

```c
ModuleExport *export = find_type_export(module, name);
if (!export || export->is_pub) return NULL;
```

入口文件 `module == NULL`，`find_type_export` 的循环根本不执行 → 返回 NULL → 侥幸没事。

加了三级查找之后，`find_namespace_type_import` 会**遍历导入绑定**并调
`find_public_export(binding->module, NULL)` → `strcmp(export->name, NULL)` → **SEGV**。

ASAN 的调用栈：

```text
#0 strcmp
#1 find_export                 module.c:597
#2 find_public_export          module.c:605
#3 find_namespace_type_import  module.c:918
#4 resolve_type_name           module.c:1388
#5 rewrite_statement_list      module.c:1608
#6 rewrite_functions           module.c:1956
```

### 修法

`resolve_type_name` 入口挡住 NULL。三处 match 分支调用点（`rewrite_statement_list` 和
`rewrite_expression` 的两处）传的都是 `arm->enum_name`，一处挡住就全覆盖：

```c
if (!name) return NULL;
```

用例：`tests/run-pass/types/match_wildcard_payload.tp`（在 HEAD 上复现 139，现在通过）。
**教训**：改「名字查找」这类函数时，先把所有调用点的实参可能为 NULL 的情况过一遍。

---

## D1. 整数除零是未定义行为

**不是编译器 bug，是语言没定义。** 但后果是同一份程序换个架构行为就变。

```tap
fn div(a: i32, b: i32): i32 { return a / b; }

fn main(): i32 {
    print("结果=%d\n", div(1, 0));
    return 0;
}
```

生成的 IR 是裸 `sdiv`，**没有任何检查**：

```llvm
%sdiv_result = sdiv i32 %loaded_var, %loaded_var3
```

| 架构 | `1 / 0` 的行为 |
|---|---|
| **arm64**（本机实测） | 返回 `0`，程序正常继续，退出码 0 |
| **x86-64**（按架构语义，未实测） | `idiv` 触发 `#DE` → `SIGFPE`，进程崩溃 |

对比：数组越界和字符串越界**都有运行时检查**并给出可读消息
（`Array index out of bounds: index=5, length=3`，退出码 1）。除零反而没有。

**建议**：像越界那样插入运行时检查，panic 并给消息；或在 `docs/data-types.md` 里明确
写成「未定义」，让用户知道不能依赖。

---

## D2. 移位量超过位宽是未定义行为

```tap
fn main(): i32 {
    let x: i32 = 1;
    print("%d\n", x << 32);   // 644870   ← 常量折叠后的垃圾值
    print("%d\n", x << 40);   // 644870
    return 0;
}
```

把移位量藏到参数后面挡住折叠，走的又是另一条路：

```tap
fn shl(a: i32, b: i32): i32 { return a << b; }

print("%d\n", shl(1, 32));    // 1        ← 硬件把移位量按 & 31 截断（32 & 31 = 0）
print("%d\n", shl(1, 40));    // 256      （40 & 31 = 8）
```

**同一表达式，常量折叠和运行时给出完全不同的结果** —— 这正是 UB 的危险之处：它不会
稳定地错，而是取决于编译器怎么优化。上面两种形式各自都是可重复的，但它们互相矛盾。

**建议**：要么在文档里写清「移位量必须小于位宽，否则未定义」，要么插入检查。

---

## D3. 有符号和无符号混算静默按无符号

```tap
let a: i32 = -6;
let b: u32 = 2;
print("%d\n", a / b);        // 2147483645
```

`-6` 被当成 `u32` 的 `4294967290`，`/ 2 = 2147483645`。比较同理：

```tap
let a: u32 = 1;
let b: i32 = -1;
print("%d\n", a < b);        // 1（1 < 4294967295）
```

C 也是这个行为（usual arithmetic conversions），但对一个高层语言来说是个坑。

**建议**：至少加诊断（`warning: mixing signed and unsigned in arithmetic`），
或直接禁止混算、要求显式转换。**要不要做取决于语言定位**，所以标成设计缺口而不是 bug。

---

## 已确认正常的（不必重复排查）

| 项目 | 结论 |
|---|---|
| 赋值路径的类型检查 | ✓ 完整：`x = "s"`、`p.x = "s"`、`a[0] = "s"`、`x = 1.5` 都正常报错 |
| `let` 的 struct / string / 嵌套结构体检查 | ✓ |
| `return` 的 struct / 指针类型检查 | ✓（只有标量漏，见 B2） |
| 载荷枚举的 `match` 穷尽性 | ✓ `match is not exhaustive: variant 'E.B' is not covered` |
| 数组越界 | ✓ 运行时有检查，退出码 1 |
| 字符串 `byte_at` / `slice` 越界 | ✓ 运行时有检查，退出码 1 |
| `?` 运算符传播 | ✓ |
| 泛型实参推断、嵌套泛型、`sizeof(导入类型)` | ✓ |
| 模块：同名 `pub` 类型、三级类型名解析 | ✓（2026-10-15 实现，见 `module.md`） |

## 不算 bug 但容易踩的

- **窄类型不改变求值宽度**：`let a: u8 = 0; print("%u", a - 1)` 打出来 `4294967295`
  （按 `i32` 的 `-1` 求值，再按 `%u` 打印），赋回 `u8` 才截断成 `255`。文档写过这条，
  符合预期，但和「u8 会回绕」的直觉不符。
- **`if (1)` 是合法的**：条件允许整数（报错信息原文是 `if condition must be an integer or
  boolean expression`），不是缺陷。
- **在表达式结果上调用方法不支持**：`outer.get(0).get(1)` 报
  `calling a method on an expression result is not supported`，需要先绑到变量。
- **泛型实参不能从枚举载荷反推**：`enum H { Only(Vec<i32>) }` 写 `H.Only(vec.new())` 报
  `cannot infer generic type parameter 'T'`。先绑到带标注的局部变量再传即可
  （`let v: Vec<i32> = vec.new();` 然后 `H.Only(v)`）。这是既有行为，B5 之前也一样。
