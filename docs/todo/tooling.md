# 工具链：还没做的

编译器本体的缺口记在 [`borrow.md`](borrow.md) 和 [`language.md`](language.md)，本文只记
**用起来难受**的部分 —— 诊断、调试、构建流程。

具体怎么落地见 [`tooling-plan.md`](tooling-plan.md)（含分阶段改动点、验收标准、风险）。

## 缺口一览

| # | 缺口 | 现在什么表现 | 收益 | 代价 |
|---|---|---|---|---|
| 1 | ~~一次只报一个错误~~ | **已完成**：借用 / 语法 / 类型错误都改成「报完继续查」，上限 10 条 | — | — |
| 2 | ~~没有调试信息~~ | **已完成**：`tap -g` 生成 DWARF 行号表，`gdb` 可按 `源码:行号` 断点 | — | — |
| 3 | ~~没有 `tap check`~~ | **已完成**：`tap check`，46ms vs 完整编译 138ms | — | — |
| 4 | ~~`tap new` 依赖 `tapm`~~ | **已完成**：`run.c:587` 有明确提示 | — | — |
| 5 | 没有增量编译 | 每次全量编译 + 链接 | 中 | 大 |

## 1. 一次只报一个错误

**借用检查已经改完了（2026-11-06）**：`borrow.c` 的 15 处诊断改用新增的
`report_error_soft`（打印 + 计数，不终止），`check_borrows` 返回状态，`main.c` 据此
在报过错之后不进 Codegen。一个文件里的多处借用错误现在一次全报，上限 10 条
（`error: too many errors (10), stopping now`）。实测三处错误 → 三条诊断，各带位置。

```tap
let a = &mut x;
let b = &mut x;   // error: 'x' is already mutably borrowed
let c = &x;       // error: 'x' is mutably borrowed, so it cannot be borrowed again
x = 2;            // error: 'x' is mutably borrowed and cannot be assigned here
```

细节见 [`tooling-plan.md`](tooling-plan.md) 的 P3 完成记录，规则层面的说明见
[`../borrow.md`](../borrow.md#诊断一次报完)。

**语法错误也改完了（2026-11-06）**：`parser.c` 用 `setjmp` / `longjmp` 在**语句边界**
恢复，报完一条跳过坏语句继续解析，同样上限 10 条。一条语句里的错误不恢复（在表达式
内部硬恢复只会产生一串假错误），但不同语句、不同函数的错误一次全报：

```tap
fn main(): i32 {
    let a: i32 = ;     // error: expected factor ...
    let b: i32 = ;     // error: expected factor ...（也报出来）
    return 0;
}
```

细节见 [`tooling-plan.md`](tooling-plan.md) 的 P4 完成记录，实现说明见
[`../src.md`](../src.md#语法错误恢复)。

**类型错误也改完了（2026-11-07）**：`codegen.c` 里「报完还能安全继续」的十几个点改用了
`report_error_soft`，报错的语句自己收尾成合法 IR（补零值或整条跳过），后面的语句照常查。
另外把纯 AST 的前置校验（重复声明、未定义类型、缺返回值）整段改成累积：

```tap
fn bad(): i32 { return "text"; }        // error: return type mismatch

fn main(): i32 {
    let x: i32 = "hello";               // error: initializer type mismatch for 'x'
    let y: i32 = add(1, "two");         // error: function 'add' argument 2 type mismatch
    return 0;
}
```

三条一起报，退出码 1。细节见 [`tooling-plan.md`](tooling-plan.md) 的 P5 完成记录，
实现说明见 [`../src.md`](../src.md#累积诊断soft-版本)。

**还剩的**：codegen 的 112 处硬诊断里只转了 17 处。剩下的多是「报错点已经生成了半个 IR」
（结构体字面量、方法调用）或全局唯一的问题（`break` 在循环外、`main` 签名不对），
多报的收益不大。

### 原因

`helpers.c` 的 `report_error()` 打印完直接 `exit(1)`：

```c
void report_error(const ASTNode *node, const char *format, ...) {
    vprint_diagnostic(stderr, "error", ...);
    exit(1);
}
```

整条流水线都是「遇错即停」，没有错误恢复（借用检查与 parser 已在 2026-11-06 改成
「报完继续」，见上）。

### 改法

分三级，**顺序是 borrow → parser → codegen**（不是 Sema，Sema 根本不报错 —— 见下节）：

- **`borrow.c`（15 处）先做** ✅ 已完成：纯 AST 走查，不生成 IR，报错后继续最多是多报几条，
  不会崩。
- **`parser.c` 次之** ✅ 已完成：语法错了之后 token 流没有可靠同步点，只在**语句边界**
  恢复（跳过到 `;` / `}`），坏语句整条丢弃。实现用 `setjmp` / `longjmp`，没有改
  `parse_*` 的签名。
- **`codegen.c`（88 处）最后** ✅ 已完成：它一边检查一边生成 IR，报完错继续走会拿着
  不一致的类型去调 `LLVMBuild*`，触发 LLVM 断言。做法是「报错的语句自己收尾成合法 IR
  + 逐条清毒化标记」，见计划文档 P5。

### 关键约束

`report_error` 是 `TAP_NORETURN`，**103 处调用点靠它做控制流分析**（`helpers.h:10` 的注释
说明了原因）。不能直接去掉这个标注 —— 会引发一大片「可能没有返回值」的警告，还会掩盖
真正的漏 return。做法是**新增**一个非 noreturn 的 `report_error_soft`，逐个调用点迁移
（已落地，见 `helpers.h` 的「累积诊断」一节）。

代价：中。风险是**级联错误** —— 一个类型错误会让后续所有使用该变量的地方都报一遍，
反而更难读。所以要么先实现「错误类型」（`ErrorType` 吞掉后续冲突），要么只在一个
函数的语句级别攒。

借用检查这一层的实际经验：级联是真的会出现（`let b = &mut x` 报一次、绑定登记时又报
一次），解法是**同一条语句只报一条、同一个错误只查一次** —— 报完之后该 `return` 就
`return`，该只登记状态就别再检查。这两条在 `borrow.c` 里都踩过。

parser 那一层的经验：**恢复点要能表达嵌套**（用一个 `jmp_buf` 会被内层块覆盖，
必须压栈），并且**恢复动作必须至少前进一个 token**（错误正好出在 `;` 上时不前进就
原地打转）。

## 2. 没有调试信息（已完成，2026-11-06）

`tap -g` 已经落地：每个函数一个 `DISubprogram`、每条语句一个 `DILocation`，产物带 DWARF
行号表。实测（Windows + `gdb`，`prog.tp` 第 2 行是 `let sum: i32 = a + b;`）：

```text
(gdb) info line prog.tp:8
Line 8 of "prog.tp" starts at address 0x1400014f7 <main+23> and ends at ...
(gdb) break prog.tp:2
Breakpoint 1 at 0x1400014cc: file prog.tp, line 2.
(gdb) run
Thread 1 hit Breakpoint 1, add () at prog.tp:2
(gdb) bt
#0  add () at prog.tp:2
#1  0x00007ff7241014f3 in main () at prog.tp:7
#2  0x00007ff724101535 in main () at prog.tp:6
```

范围：**只做行号表**，不描述局部变量与形参的类型，所以 `info locals` 还是 `No locals.`、
也不能 `print` 变量。不开 `-g` 时生成的 IR 与改动前**逐字节相同**（`cmp` 验证过），
`-g` 与 `-emit-wasm` 互斥（WASM 的调试信息是另一套格式，没做）。

段错误也能直接定位到源码行（`let p: *i32 = null; p[0] = 42;`）：

```text
Thread 1 received signal SIGSEGV, Segmentation fault.
main () at crash.tp:3
3	    p[0] = 42;
```

> **2026-11-08 更新**：`*p` / `p[i]` / `p.field` 现在都插了空指针守卫，上面这条
> 会打印 `Null pointer dereference at crash.tp:3:5` 后以状态 1 退出，**不再产生段错误**
> —— 而且消息自带行号列号，不用调试器也能定位到那一行。验证行号表要换一个真的非法
> 地址 —— 例如拿一个合法指针写远偏移：`let p: *i32 = &x; p[100000000] = 42;`
> （实测仍然 SIGSEGV；整数 `as *i32` 不支持，报 `pointer expression type mismatch`）。

落地细节见 [`tooling-plan.md`](tooling-plan.md) 的 P2 完成记录。

下面保留当初的分析。

`src/` 下搜不到任何 `DWARF` / `LLVMDIBuilder` / `DebugInfo` 引用，生成的二进制里没有
行号表，`gdb ./prog` 只能看汇编、`break main.c:12` 之类的写法全不可用。

### 影响

- 崩溃（段错误、`assert` 失败）时拿不到源码行，只能靠 `print` 二分。
- 和 C 侧联调时（`std.fs` / `std.net` 全走 extern），栈回溯里 tap 那几帧全是地址。

### 改法

LLVM 的调试信息是标准流程：`LLVMCreateDIBuilder` → 给每个函数建
`DISubprogram`、给每条语句建 `DILocation` → `LLVMSetCurrentDebugLocation`。
`codegen.c` 里每条语句本来就有 `loc`（`ASTNode.loc` 存着 file/line/column），数据是
齐的，缺的是把它接到 LLVM 上。

要配一个 `-g` 选项。代价：中，但工作量集中在
`generate_statement` / `generate_expression` 的入口，改动面清晰。

### 建议

优先级仅次于「一次报一个错」。没有调试信息，任何稍微复杂的运行时问题都只能靠打日志。

## 3. 没有 `tap check`（已完成，2026-11-06）

`tap check [选项] [源文件]` 已经落地：跑完整流水线到代码生成和 IR 校验，然后停住 ——
不写 IR、不写目标文件、不链接、不运行。通过时静默返回 0，有诊断返回 1。不给文件时按包模式
工作，且**不会创建 `build/`**。

实测 46ms vs 完整编译 138ms（`examples/json_poc.tp`）。落地细节和一处顺带修的
命令冲突问题见 [`tooling-plan.md`](tooling-plan.md) 的 P1 完成记录。

下面保留当初的分析，解释为什么它能省的只有链接。

CLI 只有「编译 / `run` / `-ir` / `-emit-obj` / `-emit-wasm` / `-run-lli` / `-lex` /
`-parse`」几种模式，**没有只做检查、不产出、不链接的入口**。

现在要确认「这段代码能不能过类型检查和借用检查」，得写：

```bash
build/tap -ir -o /dev/null probe.tp
```

这会完整跑一遍 codegen（还依赖 LLVM 可用）并把 IR 写出去，而且 `-o /dev/null` 在 Windows
上会打印一行「IR代码已写入到 nul」，需要过滤。

### 能省的是什么

**不能跳过 codegen。** `src/sema.h` 自己写着是影子模式「不接管诊断」，实测 `sema.c` 里
一处 `report_error` 都没有 —— 类型检查全在 `codegen.c`（88 处）。所以 `tap check` 必须
跑到 codegen，能省的是**链接**：

| 阶段 | 耗时（`examples/json_poc.tp`，5 次平均） |
|---|---|
| lex + parse（`-parse`） | 35 ms |
| → 模块/单态化/sema/借用/codegen/校验（`-ir`） | 45 ms |
| → 链接（完整编译） | 139 ms |

**链接占 68%。** `tap check` 把它砍掉，139ms → 45ms。顺带还能省掉写 IR 文件和那行
「IR代码已写入到 nul」的噪声。

因为 `generate_code` 内部带了 `LLVMVerifyModule`（`codegen.c:5620`），`tap check` 也能抓
IR 级问题（比如按值递归枚举的 `Cannot allocate unsized type`）。

### 改法

`main.c` 加一个 `check` 子命令：走完整流水线到 `generate_code`，然后直接返回，不进任何
输出分支。改动很小（流水线本来就是分阶段调用的），但对外是很有用的一层（编辑器集成、
CI 快速校验都靠它）。

具体步骤、包模式怎么处理（不能创建 `build/` 目录）、验收标准见
[`tooling-plan.md`](tooling-plan.md) 的 P1。

## 4. `tap new` 依赖仓库外的 `tapm`（已完成）

`src/main.c:80` 的 `new` 只是转发给 `tapm new`。`tapm`（包管理器）不在本仓库 ——
但**缺 `tapm` 时的提示早就补上了**（`src/run.c:587`）：

```text
error: cannot find `tapm`; `tap new` is provided by the tapm package manager
install tapm and put it on PATH, or run `tapm new` directly
```

查找顺序也对：先看 `<exe>` 同目录（安装布局下两个可执行文件都在 `<prefix>/bin`），
再查 PATH。

剩下的唯一问题：本仓库的测试覆盖不到 `tap new`（要真装一个 `tapm` 才测得动）。
不值得为它写测试，划掉。

## 5. 没有增量编译

每次 `tap run prog.tp` 都重新做全量：词法 → 语法 → sema → 单态化 → codegen → 链接，
包括**无条件重新生成 `std/prelude.tp` 和所有被 import 的模块的 IR**。

对一个几百行的程序，编译时间主要花在链接和 LLVM 初始化上，这个阶段的优化收益有限；
但如果将来要做编辑器集成（每次击键跑一次），就需要缓存。

### 建议

**先不做，而且理由比原来更强。** 实测前端 + codegen 只有 45ms，其中约 30ms 还是进程
启动加载 LLVM 的固定开销 —— 真正能省的上限是 10~15ms。为这点收益引入「单态化结果 +
LLVM Module」的哈希与失效逻辑，不划算。

真要做的触发条件是：`tap check` 上线后 CI 还嫌慢，或者有了每次击键跑一遍的编辑器集成。
届时最小方案是把「单态化结果 + LLVM Module」按「源文件内容哈希 + 依赖哈希」缓存到
`.tap/cache/`，命中就直接链接。

## 建议顺序

| 顺序 | 项目 | 理由 |
|---|---|---|
| — | ~~3 `tap check`~~ | 已完成（2026-11-06） |
| — | ~~2 调试信息（`-g`）~~ | 已完成（2026-11-06） |
| — | ~~1 多条错误（borrow 起步）~~ | 已完成（2026-11-06），顺带把 `report_error_soft` 搭好了 |
| — | ~~1 多条错误（parser 恢复）~~ | 已完成（2026-11-06），用 `setjmp` / `longjmp`，没动 AST |
| — | ~~1 多条错误（codegen）~~ | 已完成（2026-11-07），语句级隔离 + 纯 AST 前置校验累积 |
| — | ~~4 `tapm` 提示~~ | 已完成 |
| — | 5 增量编译 | 收益上限只有 10~15ms，搁置 |

**为什么 `-g` 排在多条错误前面**（和本文早先的建议不同）：多条错误要动 103 个调用点、
有级联噪声风险；`-g` 只碰 codegen 的入口和收尾，改坏了也只是「调试信息不准」，不影响
编译正确性。先拿确定性收益。

完整的分阶段计划（改动点、验收标准、风险）见
[`tooling-plan.md`](tooling-plan.md)。
