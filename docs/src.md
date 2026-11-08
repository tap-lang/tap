# 源码结构

tap 当前采用手写前端和 LLVM 后端，主要编译流程如下：

```text
源文件 -> Lexer -> Token -> Parser -> AST -> [模块装载 -> 载荷枚举降级 -> 名字归一
        -> 泛型单态化 -> Sema -> 借用检查] -> LLVM Codegen -> IR / 可执行文件
```

**名字归一在单态化之前**：泛型方法的接收者类型（`board.points.push(x)` 的 `T`）只能从
接收者表达式推，不能从名字字符串推；单态化出来的实例是模板的克隆，克隆自带已改写的节点。

## 核心模块

| 文件 | 职责 |
|---|---|
| [`main.c`](../src/main.c) | 程序入口、命令行参数解析和编译模式分发 |
| [`helpers.c`](../src/helpers.c) / [`helpers.h`](../src/helpers.h) | 通用的路径与字符串辅助函数 |
| [`token.c`](../src/token.c) / [`token.h`](../src/token.h) | Token 类型、名称映射和资源释放 |
| [`lexer.c`](../src/lexer.c) / [`lexer.h`](../src/lexer.h) | 读取源文件并执行词法分析 |
| [`parser.c`](../src/parser.c) / [`parser.h`](../src/parser.h) | 将 Token 流解析为 AST |
| [`module.c`](../src/module.c) / [`module.h`](../src/module.h) | 解析模块路径、递归加载导入并合并函数；详见[模块导入文档](module.md) |
| [`prelude.c`](../src/prelude.c) / [`prelude.h`](../src/prelude.h) | 定位并解析 Prelude，检查重复函数后合并到用户程序 AST |
| [`ast.c`](../src/ast.c) / [`ast.h`](../src/ast.h) | AST 节点定义、构造、打印和释放；详见 [AST 文档](ast.md) |
| [`types.c`](../src/types.c) / [`types.h`](../src/types.h) | 完整类型的深拷贝与等价判断、按名字查声明、限定名切分；单态化和 Codegen 共用 |
| [`generics.c`](../src/generics.c) / [`generics.h`](../src/generics.h) | 泛型单态化：推断类型实参、克隆实例、校验 trait impl 签名 |
| [`names.c`](../src/names.c) / [`names.h`](../src/names.h) | 名字归一：把「根是局部变量」的点号标识符改写成字段访问 / 字段赋值节点（见下面「点号：字段访问和命名空间」） |
| [`sema.c`](../src/sema.c) / [`sema.h`](../src/sema.h) | 语义分析：在生成 IR 之前推断表达式类型并写入 `resolved_type` |
| [`borrow.c`](../src/borrow.c) / [`borrow.h`](../src/borrow.h) | 借用检查：`&T` / `&mut T` 的逃逸、寿命和别名规则；详见[借用文档](borrow.md) |
| [`codegen.c`](../src/codegen.c) / [`codegen.h`](../src/codegen.h) | 遍历 AST、生成 LLVM IR，并将 IR 编译链接为可执行文件 |
| [`run.c`](../src/run.c) / [`run.h`](../src/run.h) | 本地可执行文件与 `lli` 的运行、退出码和临时产物清理 |
| [`runtime/src/runtime.c`](../runtime/src/runtime.c) | 终端、休眠和随机数等跨平台 C Runtime 实现 |
| [`version.c`](../src/version.c) / [`version.h`](../src/version.h) | 版本信息实现和生成结果 |

## 点号：字段访问和命名空间

`.` 在 tap 里同时是**字段访问**、**模块成员**和**类型成员**（`Color.Red`）的分隔符，
而 parser 看到 `a.b` 时无法判断 `a` 是什么：

- 类型可以先用后声明（`fn f(): Point` 写在 `struct Point` 之前合法）；
- `import` 可以写在文件末尾；
- 变量可以和模块别名重名（`import std.vec as vec;` 之后 `let vec: i32` 合法，
  局部名遮蔽别名）。

所以 parser 把整串拼成一个 `IdentifierNode` 的 `name`，由后面能拿到符号表的阶段再判断。
`names.c` 就是那一趟：解析完成、模块装载之后，用「当前函数已知的局部名字」做**正向判定**，
把「根确定是局部变量」的链改写成 AST 节点：

| 源码 | 改写结果 |
|---|---|
| `p.inner.value`（读） | `FIELD_ACCESS(FIELD_ACCESS(p, inner), value)` |
| `p.x = v`（写） | `FIELD_ASSIGNMENT(FIELD_ACCESS(p, x), v)` |
| `make().x = v` | parser 直接产出 `FIELD_ASSIGNMENT`（接收者不是标识符） |
| `p.inner.bump()`（方法） | `CALL{name: "bump", receiver: FIELD_ACCESS(p, inner)}` |
| `list.get(0).peek()` | `CALL{name: "peek", receiver: CALL{...}}`（parser 直接产出） |

判断依据只有一条：**根是不是局部名或顶层常量**（参数、`let`/`const`、`match` 绑定，以及
`program->constants` 里的顶层常量 —— 模块限定的常量 `c.LABEL` 会取前两段当根）。
不需要再排除「和模块别名同名」的写法 —— 局部名会遮蔽别名，只要根是局部变量，`vec.data`
里的 `vec` 就一定是那个变量。模块成员 / 类型成员的根不是局部变量，自然落到原字符串路径。

改写之后：

| 形态 | 谁产生 | 谁处理 |
|---|---|---|
| `NODE_FIELD_ACCESS` / `NODE_FIELD_ASSIGNMENT` | `names.c`（根是局部变量）；parser（接收者是 `arr[0]`、`make()` 这类表达式） | 借用检查、Sema、Codegen 的字段分支 |
| `NODE_FUNCTION_CALL` + `receiver` | `names.c`（根是局部名/常量）；parser（接收者是表达式） | 泛型单态化、借用检查、Codegen：都用接收者**类型/地址**判定 |
| 带点的 `IdentifierNode` | 模块成员（`math.PI`）、类型成员（`Color.Red`） | 各阶段自己的命名空间解析 |

方法调用同理：`p.method(args)` 的接收者从名字里拆出来挂到 `FunctionCallNode.receiver` 上，
`name` 只剩方法名。于是「接收者是表达式」和「接收者是变量」在后续阶段没有区别 ——
泛型单态化按接收者**类型**实例化（`board.points.push(x)` 的 `T` 来自 `board.points`），
codegen 按接收者**地址**传 `&self` / `&mut self`，借用检查按接收者**字段路径**登记。
这也是 `list.get(0).peek()`、`o.inner.peek()` 能用的原因。

**字段访问只有这一条路。** codegen 里原来那套「按字符串拆点号找字段」的
`field_access_info` / `generate_field_address`、以及 borrow 里的 `split_dotted`，在
2026-11-08 全部删除 —— 两套实现意味着「多级字段、指针接收者、常量字段」这些规则要写两遍，
`&mut a.b.c` 报 `pointer expression type mismatch` 就是这么来的。新增代码不要再引入按
字符串判断字段的逻辑；带点名字只用于命名空间。`TAP_TRACE_NAMES=1` 可以打印每一处改写。

## 模块依赖

依赖关系整体保持从入口到前端、后端和运行层的单向流动：

```text
main
├── lexer -> token
├── parser -> lexer + ast
├── module -> parser + ast
├── prelude -> parser + ast
├── generics -> ast + types
├── sema -> ast + types
├── borrow -> ast + types
├── codegen -> ast + types + LLVM
└── run -> codegen
```

- Lexer 只负责产生 Token，不创建 AST。
- Parser 拥有 AST 的创建过程。
- `types.c` 只放不依赖阶段上下文的纯函数（类型深拷贝/等价判断、按名字查声明、
  限定名切分），单态化和 Codegen 都调它 —— 两边对同一类型必须给出相同答案。
  新增这类"两边都要用"的工具函数请放这里，不要再往 `generics.c` / `codegen.c`
  里各写一份。
- Codegen 和 Run 消费 AST/LLVM 结果，不参与语法解析。
- `main.c` 负责组装各阶段，并在流程结束后释放资源。

Codegen 会先生成并验证全部函数，再将非 `main` 函数视为内部符号运行
LLVM GlobalDCE。这样既保留了未调用函数的编译错误检查，也不会把不可达的
Prelude 和模块函数写入最终 IR。

## 诊断输出

所有语义诊断走 `helpers.c` 的统一出口：

- `report_error(node, format, ...)` —— 用节点自带的位置，输出 `error: ...`、
  `--> file:line:col` 和源码上下文
- `report_error_at(filename, line, column, format, ...)` —— 位置由调用方显式给出

两者内部都是 `exit(1)`，并且标了 `noreturn`（见 `helpers.h` 的 `TAP_NORETURN`）—— 标出来
之后调用方的控制流分析才正确，否则 `if (bad) report_error(...);` 后面会被当成存在
「没 return」的路径。

没有位置信息时**不打印** `--> <unknown>:0:0`，只输出一句话：诊断要么给出真实位置，
要么只给一句话，假装有位置没有信息量。

### 累积诊断（soft 版本）

`report_error_soft` / `report_error_at_soft` 是同一套格式的**不终止**版本：打印完继续
往下检查，最后用 `tap_error_count()` 判断有没有报过错。只有「报完还能安全继续」的阶段
才能用 —— 目前是 `borrow.c`（纯 AST 走查，不生成 IR，多报几条最坏结果就是多几条诊断）
和 `codegen.c` 里那些「报完这条语句、后面照常查」的类型检查（见下一节）。
另有 `vreport_error_soft` / `vreport_error_at_soft` 两个可变参数版本，给 `codegen_error`
这类「报错前还要多做一件事」的包装器转发用。

- 达到 `TAP_MAX_ERRORS`（10）条时 `report_error_limit()` 打一行
  `error: too many errors (10), stopping now` 并退出，防止一个系统性错误刷成几百条。
- 两条路并存：`TAP_NORETURN` 那一版保留，因为一百多处调用点靠它做控制流分析。
- 用 soft 版本要顺手做两件事：**同一条语句里只报一条**（报完 `return`，别让
  `x = 2` 同时命中「被独占借用」和「被共享借用」），**别为同一个错误报两遍**
  （比如签名检查里 `report_error_at` 后面跟一条 `report_error` 的兜底，原来靠 `exit`
  保证不重复，现在得改成 `else`）。

新增诊断一律用这几个函数，不要再写 `fprintf(stderr, "error: ..."); exit(1);`。

### 类型错误的语句级隔离（codegen）

`codegen.c` 比前两者难：它**一边检查一边生成 IR**，报完错继续走会拿着不一致的类型去调
`LLVMBuild*`。所以那边的约定比「打印完继续」多一条 —— **报错的语句必须自己收尾成合法
IR**，否则一条诊断会变成 `LLVM IR verification failed`，把真正的原因埋掉。

两个入口：

```c
static void codegen_error(CodeGenContext *context, const ASTNode *node,
                          const char *format, ...);          // 用节点位置
static void codegen_error_at(CodeGenContext *context, const char *filename,
                             int line, int column, const char *format, ...);
```

它们做三件事：打印（走 `vreport_error_soft`，共享同一个计数器）、`context->poisoned = 1`、
`context->errors_reported++`。

- `poisoned` 是**语句级**的：`generate_statement_list` 在每条语句开头把它清掉。所以毒化
  不外溢 —— 同一个函数里的多处类型错误能一次报全。这比「一个函数只报一条」强得多，
  代价是每个转 soft 的位置都得单独想清楚「报完这一句，IR 还合法吗」。
- 收尾方式只有两种：**补一个目标类型的零值**（`zero_var_value` / `LLVMConstNull`，
  用于 `let` 初始化、`return`、实参）或**整条跳过**（用于无返回值调用当表达式用）。
  想不出怎么收尾就别转 soft。
- `errors_reported` 是整份程序的累计值，`main.c` 靠 `codegen_had_error()` 在
  `generate_code` 之后短路：报过错就不写 IR、不链接、不运行（`check` 也返回 1）。
- 函数收尾的「补一条零值 `ret`」逻辑本来就有（所有路径都 `return` 的宽松语义），
  报错后基本块没有终结指令时正好靠它兜底。

还有一层**纯 AST 的前置校验**（`validate_user_types`：重复声明、未定义类型）在生成任何
IR 之前跑，也全部用 `codegen_error`。它报过错就**直接返回**，IR 一行都不生成 —— 那种
情况下 AST 已经不可信。而「缺返回值」这类只看语句形状的检查拆到了
`validate_return_shapes`，放在短路之后，所以「函数缺返回值」和「`let` 初始化类型不匹配」
能在同一轮里一起报出来。

### 语法错误恢复

`parser.c` 是另一套：它**在语句边界恢复**，报完一条语法错误后跳过坏语句继续解析，
所以一个文件里的多处语法错误也是一次全报。

实现用的是 `setjmp` / `longjmp`，而不是让每个 `parse_*` 返回 NULL：

- `parser_error` 打印完，如果 `parser->error_frame` 非空就 `longjmp` 回去；为空时照旧
  `exit(1)`（不在可恢复位置）。
- 恢复点在两处循环里：`parse_block`（语句）和 `parse_program`（顶层声明）。它们各自压一个
  `ParserErrorFrame` 到 `parser->error_frame` 链上，所以嵌套块跳的永远是最内层那个。
- 恢复动作：`recover_to_statement_end` 跳到 `;`（消费掉）或 `}`（留给块循环）；
  `recover_to_declaration` 跳到下一个顶层声明，**只在花括号深度 0 时认边界**，否则
  函数体/impl 体里的 `fn` 会被当成同步点。
- 两条都必须**至少前进一个 token**，否则错误正好出在 `;` 或 `fn` 上时外层循环会原地打转。
- 同一行只报一条（`last_error_line`）：一行里连带出来的「expected `)`」之类都是第一个
  错误的后果，报出来只会淹没原因。

为什么不让 `parse_*` 返回 NULL：`parse_type` / `parse_expression` 的调用方全都假设返回值
非空，改成可空是几十个函数的连锁改动。`longjmp` 只改 `parser_error` 一个地方。

**报过语法错误之后 `parse_program` 仍然正常返回一棵残缺的 AST**，所以 `main.c` 必须查
`parser_had_error(parser)` 并停在那里 —— 少了语句的程序交给后续阶段只会产生假错误。
唯一例外是 `-parse`：它把残缺的树打出来有助于排查，但退出码仍是 1。

## 模块加载

Parser 将 `import a.b;` 保存为 `ImportNode`，Module Loader 再把模块名转换为
`a/b.tp`。名称空间、通配（`a.b.*`）和单成员（`a.b.c [as name]`）三种导入都在这一步解析：
模块文件使用同一套 Lexer 和 Parser；其递归依赖加载完成后，函数链表会使用唯一内部符号合并到
入口程序，限定调用按当前文件的名称空间解析、裸名调用按通配/单成员绑定解析，最后统一交给
Codegen。具体语法、查找顺序、去重和错误规则见[模块导入文档](module.md)。

## Prelude 标准库

[`std/prelude.tp`](../std/prelude.tp) 使用 tap 源码实现第一阶段标准库函数。
普通编译、IR 生成和运行模式会先解析用户源码和显式导入，再由 `prelude.c` 解析 Prelude，
检查两边是否存在同名函数，最后将两个函数列表合并后交给 Codegen。

查找顺序如下：

1. `TAP_STD_PATH/prelude.tp`
2. 当前工作目录下的 `std/prelude.tp`
3. 编译器可执行文件相邻源码目录下的 `std/prelude.tp`
4. 安装前缀下的 `share/tap/std/prelude.tp`

这种方式保持标准库函数与普通用户函数使用同一套 Parser、AST 和 Codegen；
`-lex`、`-parse` 模式不会加载 Prelude，便于单独观察目标源文件。

## 版本生成

版本模板是 [`version.h.ini`](../src/version.h.ini)。构建时会先编译
[`scripts/get_version.c`](../src/scripts/get_version.c)，再由这个 C 生成器根据 Git 信息生成 `src/version.h`。

Make 和 CMake 都会在编译前触发版本文件生成。
