# 自举可行性分析

## 结论

**不能。** 以 tap 当前的语言能力，写不出一个自然形态的编译器。

有三个结构性阻塞，它们恰好把「用指针搭递归 AST」这条路全部堵死：

1. **没有指针解引用。** `*p`、`(*p).field`、`p->field` 全都不是合法表达式 ——
   `ast.h` 里根本没有 `NODE_DEREFERENCE`。指针只能当不透明句柄传给 `extern` 函数，
   或者写成 `p[i]` 下标。
2. **堆上无法构造或修改结构体。** `p[0] = Cell { value: 5 }` 报
   `struct literals must be used with a struct target type`；`p[0].value = 9` 报方法错误。
   `malloc` 出来的内存没有任何办法初始化。
3. **没有空指针。** `next: 0` 和 `next: null` 都报 `pointer expression type mismatch`；
   `p != 0` 会生成非法 IR（见「顺带发现的缺陷」）。

编译器最核心的数据结构就是递归 AST。上面三条把最自然的表达方式堵死了。

**但不是遥不可及。** 退到 arena 模式（`Vec` 存节点、`i32` 索引代替指针）后，
前端是能写的 —— 实测写出了可用的 lexer 和递归 AST 求值器，见「关键实验」。

## 怎么测的

全部结论来自实跑，不来自文档（`docs/` 的「未实现」说法有滞后）。

```sh
# 探针目录
/tmp/tap-bootstrap-probe/

# 每个能力一个最小 .tp，编译 + 运行，记录首行诊断
tap <probe>.tp -o out && ./out
```

编译器是仓库自带的 `build/tap`。所有用例都跑过 `tap run` 和 `-run-lli` 两种模式，
结论一致。

## 已具备的能力

自举需要的多数基础件其实已经有了，而且质量不错：

| 能力 | 状态 | 说明 |
|---|---|---|
| 字符串 | ✅ | `len` / `byte_at` / `slice` / 内容比较（`==` `!=`） |
| StringBuilder | ✅ | `std.string_builder`，逐段拼接 |
| Vec\<T\> | ✅ | `std.vec`，含 `Vec<Vec<i32>>` 嵌套实例化 |
| ByteVec | ✅ | `std.byte_vec`，字节缓冲 |
| 定长数组 | ✅ | `[i32; 4]`、`[0; 4]` 重复字面量、下标读写 |
| 结构体 | ✅ | 定义、嵌套定义、字面量、字段读写、值语义、作参数/返回值 |
| 枚举 | ✅ | 无载荷 + 带载荷，`match` 语句/表达式，穷尽性检查 |
| 泛型 | ✅ | 函数、结构体、枚举都能单态化 |
| 错误处理 | ✅ | `Result<T, string>`、`?` 传播、`unwrap` / `expect` |
| 递归函数 | ✅ | `fact(5) == 120` |
| 控制流 | ✅ | `if` / `elseif` / `else`、`while`、`for`、`break`、`continue`、提前 `return` |
| 文件 IO | ✅ | `std.fs`：`open` / `read` / `write` / `append` / `exists` / `remove` |
| 参数与环境变量 | ✅ | `std.env` |
| 堆内存 | ✅ | `std.memory` 的泛型 `malloc<T>` / `realloc<T>` / `free<T>` |
| **调用外部进程** | ✅ | `extern fn system(cmd: string): i32` 实测可用 —— 自举后能调 `clang` / `ld` |
| FFI | ✅ | 不透明指针、`**T` 取地址、整数/字符串参数都能过 |
| `sizeof(T)` | ✅ | |
| 字符串 ↔ 数字 | ✅ | `std.parse` 的 `parse_i64` / `parse_f64` / `format_i64` / `format_f64` |
| 取模 | ✅ | `%` |
| **位运算** | ✅ | `&` `\|` `^` `<<` `>>` `~`，优先级对齐 C |
| **arena 表达递归 AST** | ✅ | 见关键实验 1 |
| **tap 写的 lexer** | ✅ | 见关键实验 2 |

## 缺失的能力

### A 级 —— 结构性阻塞

**A1. 没有指针解引用**

```tp
fn read(node: *Node): i32 { return (*node).value; }   // error: expected factor
fn read(node: *Node): i32 { return node->value; }     // error: expected factor
```

`*Node` 类型能声明，`&局部变量` 能取地址，`&v` 传给 `*Node` 形参也正常 ——
但拿到指针之后**读不到里面的东西**。指针实际上退化成了「只能传给 extern 的不透明句柄」。

**A2. 堆上无法构造/修改结构体**

```tp
let p: *Cell = memory.malloc<Cell>(sizeof(Cell));
p[0] = Cell { value: 5 };    // error: struct literals must be used with a struct target type
p[0].value = 9;              // error: only string methods 'len', 'byte_at', 'slice' are supported
```

`Vec<Cell>.data` 是 `*Cell`，同样写不进去。唯一能改结构体的办法是
`let c = vec.get(...); c.field = x; vec = vec.set(..., c)` —— 整体读出来、改完、整体写回。

**A3. 没有空指针**

```tp
struct Node { value: i32, next: *Node }
let p: *Node = null;                  // error: pointer expression type mismatch
p[0] = Node { value: 5, next: 0 };    // error: pointer expression type mismatch
if (p != 0) { }                       // LLVM IR verification failed: Both operands to ICmp ...
```

没有「空」这个概念，链表/树就没有终止标记。

**A4. 递归载荷枚举不支持**

```tp
enum Expr { Num(i32), Add(Expr, Expr) }   // LLVM IR verification failed: Cannot allocate unsized type
```

`docs/TODO.md` 第 12 条已记录：成员载荷不能是枚举自身，需要先有指针或 Box 语义。

### B 级 —— 影响效率与可写性

**B1. 位运算 —— 已补齐。** 补齐前 `&` `|` `^` `<<` `>>` 一个都没有（`|` 和 `^` 在词法
阶段就报 `unrecognized character`），只有 `%`。现已实现完整位运算（含一元 `~`），
优先级对齐 C，见 [`data-types.md`](data-types.md) 的整数类型一节。

- 补齐前的影响：自己做后端生成机器码/ELF 不可能；哈希、掩码、位标志都要用乘除取模模拟
  （`x << n` 写成 `x * 2^n`，非负数下 `x & 0xFF` 写成 `x % 256`，但负数语义不同）。
- 现在这一条不再构成限制：生成汇编文本和直接生成机器码都不受阻。

**B2. 没有全局可变状态。** 顶层只能 `const`，而且是编译期常量 —— 赋值报
`undefined variable`。编译器的全局符号表、计数器、错误列表没法直接放全局。

- 绕过：所有函数接收并返回一个 `Context` 结构体（值语义，每次改完要返回）。
- 或者给 Runtime 加一对 `__tap_get_global` / `__tap_set_global`。

**B3. 嵌套字段不能链式访问。**

```tp
print("%d\n", o.inner.a);        // error: undefined variable 'o.inner.a'
let x: Inner = o.inner;          // ✅ 分两步就行
print("%d\n", x.a);
```

单层 `a.b` 可以，两层就不行。对 AST 操作是重灾区 —— 本来全是 `node->child->field`
这种链，现在每层都要落一个临时变量。

**B4. 没有哈希表 / Map。** `std/` 里只有 `vec` / `byte_vec` / `string_builder`。
符号表只能线性查找，O(n²)。

**B5. 没有函数指针。** `*fn(i32, i32): i32` 报 `expected type`。
visitor、回调、分派表只能靠 `switch` 硬编码。

**B6. 没有目录遍历。** `std.fs` 只有单文件操作。编译器要扫 `std/*.tp` 加载 Prelude
和模块，现在只能硬编码文件名清单。

### C 级 —— 小缺口，都有绕法

| 缺口 | 报错 | 绕法 |
|---|---|---|
| `float` → `int` | `initializer type mismatch` | 加个 Runtime 转换函数 |
| 字符串 `+` 拼接 | `strings only support comparison operators` | StringBuilder |
| `main` 不能返回结构体 | — | 收口到辅助函数（`docs/TODO.md` 已记） |
| 结构体字段是数组时字面量初始化 | `expression does not produce an array value` | 先建局部数组再赋值 |
| 结构体数组元素取字段 | 方法错误 | 先取出元素再访问字段 |

## 关键实验

### 实验 1：arena 模式表达递归 AST

绕开「没有指针解引用」的办法：节点存 `Vec`，用 `i32` 索引代替指针，`-1` 表示空。

```tp
import std.vec as vec;

struct Node {
    kind: i32,
    left: i32,
    right: i32,
    value: i32,
}

fn eval(nodes: Vec<Node>, index: i32): i32 {
    let n: Node = vec.get<Node>(nodes, index);
    if (n.kind == 0) { return n.value; }
    let l: i32 = eval(nodes, n.left);
    let r: i32 = eval(nodes, n.right);
    if (n.kind == 1) { return l + r; }
    return l * r;
}
```

构造 `(1 + 2) * 3` 求值，**实测输出 `9`**。

说明：递归树结构本身是能表达的，代价是每次访问要 `get`（值拷贝），每次修改要
`get` → 改 → `set` 三步。

### 实验 2：用 tap 写 lexer

```tp
struct Token { kind: i32, value: i64 }

fn is_digit(c: u8): bool { return c >= 48 && c <= 57; }

fn lex(source: string, tokens: Vec<Token>): Vec<Token> {
    let i: uint = 0;
    while (i < source.len()) {
        let c: u8 = source.byte_at(i);
        if (is_digit(c)) {
            let value: i64 = 0;
            while (i < source.len() && is_digit(source.byte_at(i))) {
                value = value * 10 + (source.byte_at(i) - 48);
                i = i + 1;
            }
            tokens = vec.push<Token>(tokens, Token { kind: 0, value: value });
        } else {
            tokens = vec.push<Token>(tokens, Token { kind: c, value: 0 });
            i = i + 1;
        }
    }
    return tokens;
}
```

输入 `"12+345*6"`，**实测输出**：

```text
kind=0 value=12
kind=43 value=0
kind=0 value=345
kind=42 value=0
kind=0 value=6
```

字符串遍历、数字累加、`&&`、`Vec<Token>`、嵌套 `while`、`break`/提前 `return` 全都正常。
**前端是能写的。**

## 后端怎么办

即使前端用 tap 写出来，后端有两条路，都不算绝对阻塞：

**路线 A —— 调 LLVM C API（和现在一样）**

- 不透明句柄（`LLVMContextRef` 等）→ `*i8` ✅
- 变参 API 的 `LLVMValueRef*` 数组 → `Vec<*i8>.data` 是 `**i8` ✅（`&v` 取地址实测可用）
- 需要改 `run.c` 的链接命令加 `-lLLVM`
- 缺点：绕了一圈还是要链 LLVM，「自举」只完成了编译器自身逻辑那一半

**路线 B —— 生成汇编文本 + 调外部工具**

- `system("clang -o out out.s")` ✅ 实测可用
- 生成文本用 StringBuilder ✅
- 位运算已补齐，「生成汇编文本」和「直接生成机器码」两条路都不再受运算符限制
- 缺点：依赖外部工具链；要做优化就得自己实现

## 如果要自举：补齐顺序

按「解锁什么」排序，不是按难度：

| 优先级 | 补什么 | 解锁 |
|---|---|---|
| 1 | 指针解引用 `*p` / `p->field` + 空指针字面量 + 指针比较 | 自然形态的 AST、链表、符号表 |
| 2 | 递归载荷枚举（指针版 `enum Expr { Add(*Expr, *Expr) }`） | 用枚举表达 AST |
| 3 | 全局可变状态 | 编译器状态不用层层传递 |
| 4 | 链式字段访问 `a.b.c` | 代码量能降下来 |
| 5 | 哈希表（或自己在 Vec 上写） | 符号表性能 |
| 6 | `float` → `int` | 常量折叠、字面量处理 |

位运算已于本次补齐，不再列在待办里。

补完 1–2，自举才具备「可能」；补完 3–6 才谈得上「顺手」。

**工作量参考**：C 版编译器 `src/*.c` 约 1.1 万行（`codegen.c` 3913 行最大），
加头文件与 Runtime 共约 1.23 万行。tap 的表达力弱于 C（没有指针解引用、没有位运算），
重写同样的东西代码量只会更多。

## 顺带发现的缺陷

实测过程中撞到三个编译器缺陷，和自举无关，但值得单独修：

**1. 类型不匹配时不给诊断，直接生成非法 IR**

```tp
let n: i64 = parse.parse_i64("123");   // 返回的是 Result<i64, string>
```

```text
LLVM IR verification failed: SExt only operates on integer
  %int_sext = sext %"Result$i64$string" %call_result to i64
```

正确行为应该是报 `expected i64, found Result<i64, string>`。现在把一个结构体做了 `sext`，
一路走到 IR 校验才炸。**任何类型不匹配都可能走到这条路**，诊断体验很差。

**2. 指针比较生成非法 IR**

```tp
if (p != 0) { }   // Both operands to ICmp instruction are not of the same type!
```

**3. `print` 打印多级指针失败**

```tp
let arr: **i8 = &v;
print("%d\n", arr);   // SExt only operates on integer
```

（`&v` 本身没问题，是 `print` 处理 `**i8` 时出错。）

## 最小可行路径

如果只是想验证方向，不建议一上来就补指针。按这个顺序走，每步都能独立验收：

1. **前端原型（现在就能做）** —— 用 arena 模式写 lexer + parser + 一个求值器，
   跑通 `examples/` 里的表达式。这一步能暴露 B3（链式字段）和 A2（改结构体）到底有多痛。
2. **加指针解引用 + 空指针**（编译器改造）—— 然后回头看第 1 步的代码，评估能简化多少。
3. **决定后端路线** —— 调 LLVM 还是生成汇编，这一步决定了要不要先补位运算。
4. **再谈完整自举** —— 到这时工作量才是可估的。
