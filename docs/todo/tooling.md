# 工具链：还没做的

编译器本体的缺口记在 [`borrow.md`](borrow.md) 和 [`language.md`](language.md)，本文只记
**用起来难受**的部分 —— 诊断、调试、构建流程。

## 缺口一览

| # | 缺口 | 现在什么表现 | 收益 | 代价 |
|---|---|---|---|---|
| 1 | 一次只报一个错误 | 报第一条就 `exit(1)` | 高 | 中 |
| 2 | 没有调试信息 | 生成的二进制没有 DWARF，gdb 只能看汇编 | 高 | 中 |
| 3 | 没有 `tap check` | 纯类型检查得用 `-ir -o /dev/null`，白跑 codegen | 中 | 小 |
| 4 | `tap new` 依赖仓库外的 `tapm` | 没装 `tapm` 的机器上直接失败 | 低 | 小 |
| 5 | 没有增量编译 | 每次全量编译 + 链接 | 中 | 大 |

## 1. 一次只报一个错误

### 现在什么表现

```text
fn main(): i32 {
    let a: i32 = "hello";       // 第 1 个错误
    let b: String = 42;         // 第 2 个
    undefined_function(1);      // 第 3 个
    return 0;
}
```

```text
error: initializer type mismatch for 'a'
--> p30_multi_errors.tp:2:5
```

只报第一条，退出码 1。

### 原因

`helpers.c` 的 `report_error()` 打印完直接 `exit(1)`：

```c
void report_error(const ASTNode *node, const char *format, ...) {
    vprint_diagnostic(stderr, "error", ...);
    exit(1);
}
```

整条流水线都是「遇错即停」，没有错误恢复。

### 改法

分两级：

- **Sema / Borrow / Generics 里可以攒**：这些阶段已经建好了符号表，报错后多数情况下
  还能继续往下走（把一个错误的初始化器当成目标类型继续检查）。加一个 `error_count`，
  `report_error` 改成 `record_error`（不退出），在阶段末尾统一判断。
- **Parser 里很难攒**：语法错了之后 token 流没有可靠的同步点，硬要继续只会产生级联的
  假错误。可以只在「同一个块的语句边界」做恢复（跳过到下一个 `;` 或 `}`）。

代价：中。风险是**级联错误** —— 一个类型错误会让后续所有使用该变量的地方都报一遍，
反而更难读。所以要么先实现「错误类型」（`ErrorType` 吞掉后续冲突），要么只在一个
函数的语句级别攒。

### 建议

先只对 Sema 做，且**上限 10 条**（超过就停止）。Parser 保持现状。

## 2. 没有调试信息

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

要配一个 `-g` 选项（现在 CLI 里没有）。代价：中，但工作量集中在
`generate_statement` / `generate_expression` 的入口，改动面清晰。

### 建议

优先级仅次于「一次报一个错」。没有调试信息，任何稍微复杂的运行时问题都只能靠打日志。

## 3. 没有 `tap check`

CLI 只有「编译 / `run` / `-ir` / `-emit-obj` / `-emit-wasm` / `-run-lli` / `-lex` /
`-parse`」几种模式，**没有只做前端检查的入口**。

现在要确认「这段代码能不能过类型检查和借用检查」，得写：

```bash
build/tap -ir -o /dev/null probe.tp
```

这会完整跑一遍 codegen（还依赖 LLVM 可用），比纯检查慢，而且 `-o /dev/null` 在 Windows
上会打印一行「IR代码已写入到 nul」，需要过滤。

### 改法

`main.c` 加一个 `check` 子命令：走 lex → parse → 模块加载 → sema → generics →
borrow，然后直接返回。改动很小（流水线本来就是分阶段调用的），但对外是很有用的一层
（编辑器集成、CI 快速校验都靠它）。

## 4. `tap new` 依赖仓库外的 `tapm`

`src/main.c:30,76` 的 `new` 只是转发给 `tapm new`。`tapm`（包管理器）不在本仓库，
所以：

- 没装 `tapm` 的机器上 `tap new foo` 直接失败，错误信息来自 exec 失败，不是「请先安装
  tapm」；
- 本仓库的测试完全覆盖不到 `tap new`。

### 建议

要么在 `exec` 失败时补一条明确提示（`tapm not found; install the tap package manager
first`），要么把 `tapm` 收进本仓库。前者是小改动，后者是项目结构决策。

## 5. 没有增量编译

每次 `tap run prog.tp` 都重新做全量：词法 → 语法 → sema → 单态化 → codegen → 链接，
包括**无条件重新生成 `std/prelude.tp` 和所有被 import 的模块的 IR**。

对一个几百行的程序，编译时间主要花在链接和 LLVM 初始化上，这个阶段的优化收益有限；
但如果将来要做编辑器集成（每次击键跑一次），就需要缓存。

### 建议

**先不做。** 目前没有交互式场景，全量编译的绝对时间还能接受。真要做的话，最小方案是
把「单态化结果 + LLVM Module」按「源文件内容哈希 + 依赖哈希」缓存到
`.tap/cache/`，命中就直接链接。

## 建议顺序

| 顺序 | 项目 | 理由 |
|---|---|---|
| 1 | **3 `tap check`** | 改动最小，马上能用（CI 和编辑器都要） |
| 2 | **1 一次报一个错（Sema 版）** | 写代码时最直接的体验提升 |
| 3 | **2 调试信息（`-g`）** | 排查运行时问题的唯一出路 |
| 4 | 4 `tapm` 缺失提示 | 一条消息的事，顺手做 |
| 5 | 5 增量编译 | 等有交互式场景再说 |
