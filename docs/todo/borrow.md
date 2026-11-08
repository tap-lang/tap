# 借用检查：还没做的

[`../borrow.md`](../borrow.md) 写的是**已经能用**的部分。本文记还没做的，每条都给出
「现在什么表现」「为什么现在不行」「改起来多大」。文中所有「现在什么表现」都在
2026-11-06 用 `build/tap` 实测过，不是推测。

已经落地的：L0（`&T` / `&mut T` + 词法作用域检查）、L1（跨函数借用签名）、
`&self` / `&mut self` 接收者、`&mut T → &T` 收窄、语句内临时借用互斥、
`std.vec` / `std.byte_vec` 的 `&` / `&mut` 接口、**1c（返回结构体字面量时逐字段检查
借用是否指向本帧）**、**2（NLL：借用到最后一次使用为止）**。

## 缺口一览

| # | 缺口 | 现在什么表现 | 收益 | 代价 |
|---|---|---|---|---|
| 1 | 结构体存借用不查寿命 | 「返回字面量」已挡住；经变量中转、进容器仍能带出悬垂 | 高 | 大 |
| 2 | ~~没有 NLL~~ | ✅ 已实现（2026-11-08），见下 | 中 | 中 |
| 3 | ~~借用粒度是变量，不是字段~~ | ✅ 已细化到字段路径（2026-11-07） | 中 | 中 |
| 4 | `Vec<&T>` 推不出类型实参 | `cannot infer compatible generic arguments for 'push'` | 低 | 中 |
| 5 | 不支持 `&&T` | `expected type` | 低 | 小 |
| 6 | 多引用参数不能返回借用 | 报「两个引用参数，无法确定借自哪个」 | 中 | 大（等于 1） |
| 7 | ~~`StringBuilder` 仍是按值接口~~ | ✅ 已迁移到 `&` / `&mut`（2026-11-07） | 中 | 小 |

## 1. L2：结构体存借用的寿命

> **1c 已经落地（2026-11-07）。** 返回结构体 / 数组字面量时按字段递归检查，字段里的借用
> 只要指向本帧就报 `field 'src' of the returned struct borrows a value that lives in this
> function`，见 [`../borrow.md`](../borrow.md#1-借用不能逃出本函数)。下面「现在什么表现」
> 描述的是**当时**的状态，保留下来当作 1b 的动机；真正还漏的是经变量中转、进容器、跨函数
> 传递这几条路径，那需要 1b 的生命周期参数。

### 现在什么表现（1c 之前）

能编译，能跑，返回的是悬垂指针：

```text
struct Holder { src: &i32 }

fn make(): Holder {
    let local: i32 = 7;
    return Holder { src: &local };   // 编译通过
}
```

`tap -ir` 退出码 0，无任何诊断。现在这一条会报错，但下面这种仍然漏：

```text
fn sneaky(): Holder {
    let local: i32 = 7;
    let h: Holder = Holder { src: &local };
    return h;                        // 目前不报：借用是经变量转手出去的
}
```

### 为什么现在不行

借用检查只在**单个函数体内**维护「栈帧秩」：`storage_rank` 是符号在符号表里的深度，
`pointee_rank` 是引用指向的东西在哪个帧（`-1` 表示函数外）。跨函数只知道一条约定 ——
「返回借用的函数恰好有一个引用参数，返回值借自它」（规则 4）。

结构体把借用藏进字段之后，类型里**没有寿命信息**，`Holder` 和 `i32` 的关系在类型系统
里根本不存在，调用方无从判断。

### 三条路线

**a. 直接禁止**（`struct Holder { src: &i32 }` 报错）

```text
error: struct field cannot hold a reference; use a raw pointer instead
```

改动最小：`sema` 或 `borrow.c` 里扫一遍结构体字段类型，见到 `is_reference` 就报。
代价是砍掉合法用法（「函数内临时组装一个持有借用的视图结构体」是很自然的写法）。

**b. 生命周期参数**（`struct Holder<'a> { src: &'a i32 }`）

Rust 的路线，要做对必须连单态化一起改：**单态化的 key 从「类型」变成「类型 + 区域」**，
`substitute_type` / `unify_type` / impl 表查找全都要带上区域变量。改动量大概是 L0+L1
的总和，且会渗透到 `generics.c` 的每一处。

**c. 折中：允许存，但禁止从函数返回**（✅ 已实现，2026-11-07）

规则改成「结构体字面量里的借用，其源必须来自本函数的参数（`pointee_rank < 0`）」：

```text
fn view(pair: &Pair): Holder {
    return Holder { src: &pair.left };   // 合法：借自参数
}

fn bad(): Holder {
    let local: i32 = 7;
    return Holder { src: &local };       // error: 结构体把本帧的借用带出了函数
}
```

实现上不需要改类型系统：`borrow.c` 已经能算「这个借用来自本帧还是别处」（规则 1 用的
就是这套判定），只要在**返回结构体字面量**时对每个字段递归检查一遍即可。能挡住
`make(): Holder` 这类最常见、也最危险的悬垂，代价小一个数量级。

落在 `src/borrow.c` 的 `check_escaping_borrow`，从 `NODE_RETURN` 进来；递归走结构体
字段和数组元素，**不进函数调用实参**（`wrap(Holder { src: &local })` 里借用会不会逃出去
取决于 `wrap` 做什么，静态判断只会误报）。用例：
[`return_struct_holding_local_borrow.tp`](../../tests/compile-fail/borrow/return_struct_holding_local_borrow.tp)
（最基本的 `make()`）、
[`return_nested_struct_holding_local_borrow.tp`](../../tests/compile-fail/borrow/return_nested_struct_holding_local_borrow.tp)
（第二层字段 + 穿过返回引用的调用）、
[`struct_borrow.tp`](../../tests/run-pass/types/struct_borrow.tp)（不该报的三种写法）。

剩下的洞（把借用装进结构体后，结构体活得比源长）仍然存在，但那种情况通常需要显式
存到别的变量里再跨作用域，比「函数直接返回」少见得多。**下一步要真想堵住它，可以给
`BorrowSymbol` 加一个「持有本帧借用」的状态，跟着 `let` / 赋值传播** —— 但赋值、字段
赋值、按值传参都要跟着维护，误报风险明显高于 1c，所以单独做。

## 2. 没有 NLL（✅ 已完成，2026-11-08）

> 已按下面的「改法」落地，实现在 `src/borrow.c` 的「NLL：活跃性预扫描」一节：
>
> 1. `BorrowRecord` 增加 `dead_at`（借用在第几条语句之后失效，`INT_MAX` = 作用域结束）。
> 2. `prepare_nll()` 在每个函数正式遍历**之前**跑一趟预扫描：按和 `walk_statement`
>    **完全相同的顺序**给语句编号（1 起），记下每个名字最后一次出现在哪条语句
>    （`LastUseEntry.last_stmt`）以及它是否出现在「指针可能活过这条语句」的位置
>    （`leaks`）。
> 3. `let r = ...` 建立持久借用时，`apply_borrow_lifetime()` 把刚建的记录 `dead_at`
>    设成 `last_use(r)`；冲突判定（`collect_overlapping_borrows`）跳过 `dead_at`
>    已经过去的记录。
>
> 落地时的取舍（都是往保守方向）：
>
> - **`leaks` 就不收窄**：引用装进结构体/数组、作为调用实参、作为返回值、赋给别的
>   变量 —— 指针可能活过这条语句，寿命仍是整个作用域。
> - **一次都没用过的引用也不收窄**：没找到使用点就不敢断定它真的不再使用。
> - **预扫描遇到不认识的节点 → 整个函数退回词法语义**。语句编号一旦和正式遍历错位，
>   `last_use` 就会偏小、借用提前失效（= 漏报悬垂），所以宁可整函数放弃。
> - **`let s = r;` 按别名处理**：`s` 的活跃期并到 `r` 上（`AliasEdge` + 不动点传播），
>   否则 `let r = &mut x; let s = r; use(s); x = 1;` 会让 r 的借用提前结束。
> - **`TAP_BORROW_LEXICAL=1` 退回词法语义**，方便对比和排查误报。
>
> 用例：`tests/run-pass/types/borrow_last_use.tp`（独占/共享借用失效、字段粒度、
> 方法返回的借用、循环体内）；三个 compile-fail 守边界：
> `conflict_before_last_use.tp`、`alias_copy_keeps_borrow_alive.tp`、
> `argument_leak_keeps_borrow_alive.tp`。全量 299 tests：298 passed, 1 skipped, 0 failed。
>
> 下面保留原来的分析和改法，作为设计记录。

### 当初什么表现

```text
let value: i32 = 1;
let exclusive: &mut i32 = &mut value;
exclusive[0] = 7;
print("%d\n", value);      // error: 'value' is mutably borrowed and cannot be used here
```

`exclusive` 在上一行之后就没再用过，但 `value` 依然被独占借到作用域结束。

### 为什么现在不行

检查器是**作用域秩**模型：`&mut x` 一旦绑到变量，就留下一条 `BorrowRecord`（带建立时
的作用域秩），出作用域才清。没有「最后一次使用」的概念，也就没有活跃点分析。

### 改法

给每个引用符号记一个 `last_use_stmt`（遍历 AST 时给每条语句编号，任何读取该符号的地方
都刷新它），冲突判定从「借用秩 >= 当前秩」改成「借用秩 >= 当前秩 **且** last_use >= 当前语句」。

代价是要在 `borrow.c` 里加一趟编号 + 一遍活跃性扫描（现在的实现是单趟前向遍历）。
副作用是**报错位置会漂移**：以前错误固定落在 `&mut` 所在作用域，之后会落在第一次冲突的
语句上 —— 这正是 `borrow.md` 里说的「有意的保守」被放弃的部分。

### 建议

先只对 `&mut` 做（`&` 共享借用的冲突本来就少），并且保留一个开关（`TAP_BORROW_LEXICAL=1`）
回到词法语义，方便对比排查。

## 3. 借用粒度是变量，不是字段（✅ 已完成，2026-11-07）

> 已按下面的「改法」落地：`BorrowSymbol` 上的两个 `*_borrow_rank` 整数换成挂在
> `BorrowContext` 上的 `BorrowRecord` 链表，每条记录带 `base + path`。路径的段写成
> `/left`、`/left/next`（字段链）、`/items/#0`（常量下标）、`/items/*`（非常量下标，
> 通配），空串表示整个对象。冲突判定改成 `paths_overlap()`（两条路径互为前缀即重叠）。
> 新用例：[`run-pass/types/borrow_field_granularity.tp`](../tests/run-pass/types/borrow_field_granularity.tp)、
> [`compile-fail/borrow/borrow_parent_while_child_borrowed.tp`](../tests/compile-fail/borrow/borrow_parent_while_child_borrowed.tp)、
> [`compile-fail/borrow/use_whole_while_field_borrowed.tp`](../tests/compile-fail/borrow/use_whole_while_field_borrowed.tp)。
> 全量 289 tests：288 passed, 1 skipped, 0 failed。
>
> 落地时的两个取舍：
>
> 1. **路径穿过引用/指针就不跟踪**（`base` 置空，记录根本不参与比较）。这样
>    「两个引用参数可能指向同一块内存」那个风险自动消失 —— 不是「保守按冲突算」，
>    而是「这两处都不归本帧管」。
> 2. **方法返回借用的登记仍然是整个接收者**（路径为空）。方法签名里没有「返回的
>    引用指向哪个字段」的信息，`let r = p.peek()` 之后 `p.right = x` 照样报。
>
> 顺带发现一个**独立的 Codegen 缺口**：`&mut a.b.c`（多级字段取地址，`a` 是结构体
> 值或 `&mut` 引用）会报 `pointer expression type mismatch`。借用检查这一侧认得这条
> 路径（`/inner/value` 这种记录是正常的），纯粹是取地址那一步没打通。要修得动
> `codegen.c` 的字段地址生成，没在这一项里做。

### 当初什么表现

```text
struct Pair { left: i32, right: i32 }

let p: Pair = Pair { left: 1, right: 2 };
let a: &i32 = &p.left;
let b: &mut i32 = &mut p.right;   // error: 'p' is already borrowed,
                                  //        so it cannot be borrowed mutably
p.right = 5;                      // error: 'p' is borrowed and cannot be assigned here
```

读 `p.right` 是允许的（`check_plain_use` 只拦 `&mut` 借出时的独占冲突），但**写另一个
字段**被拦。两个字段在内存上完全不重叠，这里报错是纯保守。

### 为什么当时不行

`BorrowSymbol` 只存 `mutable_borrow_rank` / `shared_borrow_rank` 两个整数，记的是
「这个**变量**被借走了」，没有记「被借走的是哪个字段」。

### 当时的改法

把两个秩换成一张「借用路径」小表：每条借用记 `base 符号 + 字段/下标链`（例如
`p.left`、`items[2].x`）。冲突判定从「同一个符号」改成「路径有前缀关系」：

- `p` 和 `p.left` → 冲突（前者包含后者）
- `p.left` 和 `p.right` → 不冲突
- `p.left` 和 `p.left` → 冲突

判定逻辑直接复用 `borrow.c` 里已有的 `resolve_lvalue` 那套「左值路径」遍历。

## 4. `Vec<&T>` 推不出类型实参

### 现在什么表现

```text
let a: i32 = 1;
let refs: Vec<&i32> = vec.new();   // 这一行没问题
refs.push(&a);                     // error: cannot infer compatible generic arguments for 'push'
```

显式写 `refs.push<&i32>(&a)` 也一样报错。

### 为什么现在不行

P2 为了修 `values.push2(10)`（接收者是 `Vec2<i32>`、模板首参是 `&mut Vec2<T>`），在
`generics.c` 加了 `receiver_core_type()`，**只在接收者位置**两边各剥一层引用。
普通实参的类型统一仍然要求引用与非引用严格相等，而 `push<T>(vec: &mut Vec<T>, value: T)`
的 `T` 要绑定到 `&i32`，`unify_type` 遇到「模板参数 vs 引用类型」这条路没打通。

### 改法

1. `unify_type` 里允许模板参数直接绑定到引用类型（`&i32` 整体作为实参）；
2. `substitute_type` 要能替换 `element_type` 里的类型参数 —— 否则 `Vec<&T>` 的
   `element_type` 里的 `T` 不会被特化；
3. `sizeof(&T)` 按指针大小算（现在应该已经是，但要有用例守着）。

顺带会把 `Option<&T>`、`Holder { src: &T }`（泛型版）一起打通。

### 注意

这条和 1、6 是耦合的：`Vec<&i32>` 一旦能构造，就出现了「借用存在堆上的容器里」，
寿命更没人管。所以**建议排在 1 之后**，或者先做 1c 再做这条。

## 5. 不支持 `&&T`

```text
let a: &i32 = &x;
let b: &&i32 = &a;      // error: expected type
```

`parse_type` 里 `&` 之后只接受 `mut` 或类型名，不接受第二个 `&`。

顺带一提：反方向也没有「把裸指针包回受检查借用」的语法。现在只允许 `&T → *T`（遗忘
借用），想写 `&*raw` 把 `*T` 显式升级成 `&T`（仍然不验证寿命，但至少进检查器视野）
没有语法支持。

改动都很小（`parse_type` 加一个分支；`&*expr` 加一个一元运算符），但价值取决于是否
做 1、4 —— 单做没有实际用途。

## 6. 多引用参数不能返回借用

```text
fn pick(left: &i32, right: &i32): &i32 {
    return left;      // error: 两个引用参数，无法确定借自哪个
}
```

这是规则 4 的直接后果。要解决只有两条路：

- **显式生命周期参数**：`fn pick<'a>(left: &'a i32, right: &'a i32): &'a i32` —— 就是 1b；
- **调用点推断**：每个调用点按实参的秩决定返回借用的秩，等于给每个调用点单独特化 ——
  和「单态化 key 是类型」的现状冲突，实际上也退化成 1b。

所以这条**不建议单独做**，跟着 1 一起。

## 7. `StringBuilder` 迁移到引用接口（✅ 已完成，2026-11-07）

> 已按下面的「改法」整体迁移：`append` / `append_byte` / `clear` 收 `&mut StringBuilder`
> 且返回 `void`，`len` / `to_string` 收 `&StringBuilder`，`free` 保持按值。`std.json`、
> `std.fs`、`std.parse`、`examples/json_poc.tp`、测试和 `docs/std.md` 同步改完；
> 现在的用法是一句 `builder.append(x);`，不再有 `builder = sb.append(builder, x);`。
> 顺带验证了「`&mut StringBuilder` 参数里再取 `&mut builder.buffer`」这条路径
> （`std.string_builder` 的实现就得这么写）。全量测试 0 failed；迁移之后
> `builder.append(...)` 是一次真实的独占借用，`tests/compile-fail/borrow/builder_append_while_shared.tp`
> 守着「手上还攥着 `&StringBuilder` 时不能追加」这条。

P2 把 `Vec` / `ByteVec` 改成了 `&` / `&mut`，但 `StringBuilder` 没动：

```text
pub fn append<T>(builder: StringBuilder, value: T): StringBuilder {
    let text: String = value as String;
    bytes.extend(&mut builder.buffer, text);
    return builder;                    // 每次都要返回、每次都要写回
}
```

`std/json.tp` 里有 **32 处** `sb.append(builder, ...)` / `builder.append(builder, ...)`
的写回式调用。

### 改法

```text
pub fn append<T>(builder: &mut StringBuilder, value: T) { ... }
pub fn append_byte(builder: &mut StringBuilder, value: u8) { ... }
pub fn len(builder: &StringBuilder): uint { ... }
pub fn to_string(builder: &StringBuilder): String { ... }
pub fn free(builder: StringBuilder): i32 { ... }   // 保持按值，同 Vec
```

调用点从 `builder = sb.append(builder, x);` 变成 `builder.append(x);`，`std.json` 里
每个 `append_escaped_byte` 之类的辅助函数也要跟着改签名（它们现在靠返回值串起来）。

### 收益

- 少一次结构体拷贝（`StringBuilder` 只有一个 `ByteVec` 字段，拷贝本身便宜，但写回是
  每个字符一次）；
- 借用检查能拦住「拿着 builder 的借用再 append」（现在按值接收者只在**调用**那一刻
  要求没被借走，写回之后新结构体是另一份）。

### 风险

`StringBuilder` 是 `pub` API，`docs/std.md` 里的示例要同步改。没有外部用户，风险低。

## 建议顺序

按「收益 / 代价」排，前面几条做完再考虑后面：

| 顺序 | 项目 | 理由 |
|---|---|---|
| ~~1~~ | ~~**1c 结构体借用禁止出函数**~~ | ✅ 已完成（2026-11-07） |
| ~~2~~ | ~~**7 `StringBuilder` 迁移**~~ | ✅ 已完成（2026-11-07） |
| ~~3~~ | ~~**3 借用粒度到字段**~~ | ✅ 已完成（2026-11-07） |
| ~~4~~ | ~~**2 NLL**~~ | ✅ 已完成（2026-11-08） |
| 1 | **1b 生命周期参数** | 真正的 L2；会动单态化，单独一期做 |
| 2 | 4 / 5 / 6 | 都依赖 1b，跟着做 |

NLL 落地之后，「把 `&mut` 塞进小作用域」这类别扭写法已经不需要了。剩下的问题按
收益排：**1b（生命周期参数）** 是唯一能堵住「借用经变量中转 / 进容器带出函数」的
办法，也是 4 / 5 / 6 的前置。别用「给变量记『持有本帧借用』状态」的方式硬撑 ——
误报会立刻出现在正常代码里。

## 怎么验证

临时探针（`-ir` 会跑完整流水线但跳过链接，最快的全量检查）：

```bash
build/tap -ir -o /dev/null probe.tp
```

改完 `borrow.c` 之后跑借用相关用例：

```bash
TEST_FILTER=borrow/ sh tests/run-tests.sh build/tap
TEST_FILTER=types/reference sh tests/run-tests.sh build/tap
TEST_FILTER=types/borrow sh tests/run-tests.sh build/tap
TEST_FILTER=traits/self sh tests/run-tests.sh build/tap
```

`TAP_NO_BORROW=1` 跳过整个借用检查，用来确认某段代码到底是被借用检查还是别的阶段
拦下的。`TAP_BORROW_LEXICAL=1` 关掉 NLL、退回词法语义，用来判断「这条报错是不是
NLL 没收窄造成的」。
