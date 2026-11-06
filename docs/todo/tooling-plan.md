# 工具链开发计划

基于 [`tooling.md`](tooling.md) 的缺口清单。动笔前先读了 `src/`，**发现清单里有三条需要
纠正** —— 不纠正的话计划会往错的方向走。

## 0. 先纠正三个错误

### 0.1 第 4 条（`tap new` 依赖 `tapm`）已经做完了

`src/run.c:587` 早就有明确提示：

```text
error: cannot find `tapm`; `tap new` is provided by the tapm package manager
install tapm and put it on PATH, or run `tapm new` directly
```

还先找 `<exe>/../bin/tapm`、再找 PATH。这条从清单里划掉。

### 0.2 第 3 条（`tap check`）的方案是错的

原文写「走 lex → parse → 模块加载 → sema → generics → borrow，然后直接返回」。**这样
什么都查不出来。**

`src/sema.h` 自己写着：

> 当前是**影子模式**：只负责填 `ASTNode.resolved_type`，不接管诊断……诊断仍然由
> Codegen 负责。

实测印证：`sema.c` 里**一处 `report_error` 都没有**（只有一处内存分配失败的 `exit(1)`）。
真正的类型检查全在 `codegen.c` —— **87 处 `report_error`**。

| 文件 | `report_error(` 调用 | 其他诊断出口 | 说明 |
|---|---|---|---|
| `codegen.c` | 87 | `print_diagnostic` + `exit` 约 44 处；OOM 14 处 | 类型检查、字段、泛型实参、返回值……都在这 |
| `borrow.c` | 14 | OOM 1 处 | 借用检查（纯 AST 走查，不生成 IR） |
| `generics.c` | 0 | `generic_call_error` + 直接 `print_diagnostic`+`exit`，22 处 `exit(1)` | 单态化的签名校验 |
| `sema.c` | **0** | 1 处（OOM） | 只填 `resolved_type` |

**诊断出口其实有三套**，`helpers.h:40` 说「这是所有语义诊断的统一出口」并不成立：

1. `report_error` / `report_error_at`（`codegen` 87 + `borrow` 14）—— 真·统一出口；
2. `print_diagnostic(stderr, "error", ...)` 后跟 `exit(1)`（`codegen` 约 44 处、`generics`
   全部）—— 格式一样，但绕过了计数和「将来改成累积」的接入点；
3. 裸 `fprintf` + `exit(1)`（OOM、`LLVMVerifyModule` 失败）—— 不该计入「多条错误」。

第 2 类是 P5 的前置：不统一，多报只覆盖 87 个点里的一部分。

所以 `tap check` **必须跑到 codegen**。它能省的只有**链接**，而这恰好是大头：

| 阶段 | 耗时（json_poc.tp，5 次平均） | 占比 |
|---|---|---|
| lex + parse（`-parse`） | 35 ms | 25% |
| → 模块/单态化/sema/借用/codegen/校验（`-ir`） | 45 ms | 32% |
| → 链接（完整编译） | 139 ms | 100% |

**链接占 68%**。`tap check` 跳过链接，等于把 139ms 降到 45ms。这依然是个有价值的入口
（CI、编辑器），只是理由和原文写的不一样：不是「省掉白跑的 codegen」，而是「省掉链接」。

顺带说明：codegen 里带了 `LLVMVerifyModule`（`codegen.c:5620`），所以 `tap check` 也能
抓到 IR 级问题（比如按值递归枚举那个 `Cannot allocate unsized type`）。

### 0.3 第 1 条（多条错误）的顺序是错的

原文写「先只对 Sema 做」。**Sema 根本不报错**，无从改起。

真正的顺序只能是：**borrow.c → parser.c → codegen.c**。前者是纯 AST 走查，改起来最安全；
codegen 一边检查一边生成 IR，改起来最危险，放最后。

另外「统一诊断出口」本身是一件独立的前置工作：codegen 里还有约 44 处
`print_diagnostic` + `exit` 绕过了 `report_error`，不先收拢，多报只能覆盖一部分点。
它是机械替换，验收方式是「全部 compile-fail 用例的 stderr 逐字节不变」，见 P5 的 Step 0。

## 1. 阶段划分

| 阶段 | 内容 | 工作量 | 风险 | 收益 |
|---|---|---|---|---|
| **P1** | `tap check` ✅ | 半天 | 无 | 编译时间 −68%，CI/编辑器入口 |
| **P2** | 调试信息 `-g` ✅ | 2~3 天 | 中 | 能调试 |
| **P3** | 借用检查多条错误 ✅ | 1 天 | 低 | 写代码体验 |
| **P4** | 语法分析错误恢复 ✅ | 1~2 天 | 中 | 语法错一次看全 |
| **P5** | codegen 多条错误 ✅ | 2~3 天 | 高 | 类型错一次看全 |
| — | 增量编译 | — | — | 暂不做（见 §4） |

顺序理由见「顺序与理由」。

## P1：`tap check`

### 目标

```bash
tap check prog.tp          # 通过 → 退出码 0，无输出
tap check                  # 包模式：检查当前包
tap check bad.tp           # 失败 → 退出码 1，报错，不产出任何文件
```

### 改动点

| 文件 | 位置 | 改什么 |
|---|---|---|
| `main.c` | `print_usage()` (23) | 加一行 `check` 说明 |
| `main.c` | 变量声明 (58~71) | `int check_only = 0;` |
| `main.c` | 参数循环，紧跟 `build` 分支 (152) | `else if (strcmp(argv[i],"check")==0 && !input_file && !run_native) check_only = 1;` |
| `main.c` | 互斥检查组 (198~209) | `check` 不能与 `-o` / `-ir` / `-emit-*` / `-run-lli` / `-lex` / `-parse` / `-static` / `run` 同用 |
| `main.c` | 包模式 (173~189) | `check_only && !input_file` 时也走包模式，但**不能创建 `build/` 目录**（`resolve_package_target` 会 `ensure_directory`，见 `module.c:426`） |
| `main.c` | `generate_code` 之后 (280) | 插入短路返回 |
| `docs/cli.md` | 用法表、选项表、各模式说明 | 补 `check` |

### 关键实现

短路点必须在 `generate_code()` **之后**（那里才有诊断），在输出分支**之前**：

```c
generate_code(codegen_context, program);

if (check_only) {
    free_codegen_context(codegen_context);
    free_ast((ASTNode *)program);
    free_parser(parser);
    free_lexer(lexer);
    return 0;
}
```

包模式复用 `resolve_package_target` 会建出 `build/` 目录 —— 检查不该有副作用。两种做法：

- **省事**：加一个 `int create_output_dir` 参数，`check` 传 0；
- **干净**：拆成 `resolve_package_entry(start, &source)`（只读 package.toml）+
  `resolve_package_target` 调用它。推荐后者，`module.c:395` 里两段逻辑本来就可以分开。

### 顺手修掉的一处

`codegen.c:5620` 的 `LLVMVerifyModule` 失败时是裸 `fprintf` + `exit(1)`，不走诊断层 ——
所以按值递归枚举才会甩出带 IR 片段的原始 LLVM 报错。改成 `report_error(NULL, ...)`
（没有节点位置就只打一句话），至少格式统一。这条改动很小，跟着 P1 一起做。

### 验收

- `for f in examples/*.tp; do tap check $f || echo FAIL; done` 全 0
- `tap check` 一个类型错误的文件 → 退出码 1、`build/` 没被创建、没有 `output.ll`
- 计时：`tap check` ≈ 45ms，`tap -o x.exe` ≈ 139ms
- `tap check -ir` → 明确报互斥错误

### 完成记录（2026-11-06）

落地情况和上面写的一致，几处偏差：

- **包模式**改成拆函数：新增 `resolve_package_entry`（只读，`module.c`），
  `resolve_package_target` 改为调用内部 `read_package_info` 再补 `build/` 目录。
  `tap check` 走只读那条，实测不创建 `build/`。
- 顺手补了一条**命令冲突**检查：`tap check run` 原来会把 `run` 当文件名去打开
  （因为 `run` 分支带了 `!check_only` 判断而落到「当输入文件」的兜底分支）。
  现在 `run` / `check` / `build` 只能给一个，多给直接报错。
- `LLVMVerifyModule` 失败改走 `report_error(NULL, ...)`，不再裸 `fprintf`。
- 新增 `scripts/smoke-cli.sh`（当时 18 项，P2 时补到 22 项），挂到 `make test-cli` 和
  ctest 的 `cli-smoke-tests`。

实测：`tap check` 46ms vs 完整编译 138ms（`examples/json_poc.tp`，各 5 次），
274 条语言测试 + CLI 检查全过。

**顺带发现一个既有 bug（与 P1 无关）**：Windows 上 `tap run -o <无扩展名的名字>` 会失败，
报 `failed to start program: ...: No such file or directory`，产物其实生成成功。原因是
`_spawnvp` 对没有扩展名的文件会去找 `<name>.exe`。包模式的产物名正是 `<包根>/build/<name>`
（无扩展名），所以 **Windows 上 `tap run` 不带文件时一直是坏的**。修法见下节。

## 已知问题：Windows 上 `tap run` 的产物没有扩展名

```bash
tap run -o demo hello.tp     # 生成 demo，然后启动失败
tap run -o demo.exe hello.tp # 正常
```

`execute_file` 走 `_spawnvp`，Windows 的 `CreateProcess` 对没有扩展名的路径会自动补
`.exe`，于是找的是 `demo.exe`，而磁盘上是 `demo`。

三种修法：

1. **包模式产物加 `.exe`**（`resolve_package_target` 里 `_WIN32` 下拼 `<name>.exe`）。
   影响 `tap build` 的产物名和文档里的 `<包根>/build/<name>`，但符合 Windows 习惯。
2. **`tap run` 遇到无扩展名产物时自动补**（只改 run 路径，`tap build` 产物名不变）。
   最小改动，但 `tap run -o demo` 和 `tap build` 产出的名字就不一致了。
3. **执行时优先试 `<path>.exe`**（`execute_file` 里加回退）。不改任何产物名，
   但等于在掩盖「Windows 需要扩展名」这个事实。

推荐 1 + 3 一起：产物统一带 `.exe`（1），执行时保留回退（3）以兼容已经生成的旧产物。

## P2：调试信息 `-g`

### 目标

```bash
tap -g prog.tp -o prog.exe
gdb prog.exe
(gdb) break prog.tp:12     # 能命中
(gdb) bt                   # 栈帧带 tap 源码行号
```

### 改动点

| 文件 | 位置 | 改什么 |
|---|---|---|
| `main.c` | 参数循环 | `-g` 解析，`int debug_info = 1;` |
| `codegen.h` | `CodeGenContext` (49) | 加 `LLVMDIBuilderRef di_builder; LLVMMetadataRef di_file; LLVMMetadataRef di_compile_unit; int debug_info;` |
| `codegen.h` | 声明 | `void set_codegen_debug_info(CodeGenContext *, int);` |
| `codegen.c` | `create_codegen_context` (5152) | 若开启：`LLVMCreateDIBuilder` + `LLVMDIBuilderCreateFile` + `LLVMDIBuilderCreateCompileUnit` |
| `codegen.c` | 函数声明循环 (5551 附近) | 每个函数建 `LLVMDIBuilderCreateFunction`，`LLVMSetSubprogram(llvm_function, subprogram)` |
| `codegen.c` | `generate_statement_list` (4855) 循环开头 | 按 `statement->loc` 调 `LLVMSetCurrentDebugLocation2` |
| `codegen.c` | `generate_code` 末尾，`LLVMVerifyModule` 之前 (5620) | `LLVMDIBuilderFinalize` |
| `codegen.c` | `free_codegen_context` | `LLVMDisposeDIBuilder` |

### 关键技术点

1. **`LLVMDebugMetadataVersion()` 要写进模块 flag**，否则 LLVM 会拒绝或忽略调试信息：

   ```c
   LLVMAddModuleFlag(context->module, LLVMModuleFlagBehaviorWarning,
                     "Debug Info Version", 18,
                     LLVMValueAsMetadata(LLVMConstInt(LLVMInt32TypeInContext(ctx),
                                                      LLVMDebugMetadataVersion(), 0)));
   ```

2. **DIFile 按文件缓存**。`ASTNode.loc.file` 可能是 `std/prelude.tp`、被 import 的模块，
   不只有主文件。用一个 `(path → DIFile)` 小表，别每个函数建一份。

3. **行号来源就是 `ASTNode.loc`**（`file` / `line` / `column`），`codegen.c` 每条语句
   都拿得到，不需要新的遍历。

4. **`-g` 与 `-emit-wasm` 的组合**先不管（DWARF 和 wasm 是两套），直接报互斥。

### 验收

- `gdb` 里 `info line main` 能给出 `prog.tp:行号`
- 故意 `null[0]` 触发段错误，`bt` 里能看到 tap 源码行
- 不带 `-g` 时产物**逐字节不变**（用 `cmp` 比对同一份 IR）

### 风险

LLVM 版本差异（C API 的 DI 接口在 14~17 之间改过名字）。本机是 LLVM 23，`DebugInfo.h`
里 55 个符号都在，按 23 的签名写；如果 CI 上还有 21，用 `#if LLVM_VERSION_MAJOR` 兜。

### 完成记录（2026-11-06）

**实际改动**（和上面的计划有几处出入，都已按实际写的记）：

| 文件 | 改了什么 |
|---|---|
| `main.c` | `-g` 解析、`int debug_info = 0;`、帮助文本一行、`-g` × `-emit-wasm` 互斥检查 |
| `codegen.h` | `#include <llvm-c/DebugInfo.h>`；`CodeGenContext` 加 `debug_info` / `source_file` / `di_builder` / `di_compile_unit` / `di_subroutine_type` / `di_current_scope` / `di_files`（`DebugFileEntry*` 链表） |
| `codegen.c` | 六个辅助函数：`split_source_path`、`debug_file_for`（按路径缓存）、`debug_info_initialize`、`debug_info_begin_function`、`debug_set_location_at` / `debug_set_location`、`debug_info_finalize` + `debug_info_dispose` |
| `codegen.c` | `generate_code` 开头（`find_user_main` 之前）初始化、用户函数循环里挂 `DISubprogram`、`generate_statement_list` 每条语句设位置、`LLVMVerifyModule` 之前 `Finalize`、`free_codegen_context` 里先销毁 DIBuilder 再销毁 module |

**和计划的差异**：

1. **`DISubprogram` 的 scope 传 `DIFile`，不是 CU。** 计划里没写这一项，但传 CU 会生成
   `scope: null` —— LLVM 容忍，调试器却会把函数挂到「无源文件」下。改成传 `file` 后是
   `scope: !1`（`!1 = !DIFile`），与 clang 一致。
2. **`-g` 在 `generate_code` 开头初始化，不在 `create_codegen_context`。** 因为初始化要建
   `DIFile`，而文件路径是 `create_codegen_context` 从 `module_name` 拷出来的
   `context->source_file`，放在 `create_codegen_context` 里也行，但那个函数会被 `-lex` /
   `-parse` 这类不生成代码的路径调用，没必要为它们建 DIBuilder。
3. **`di_subroutine_type` 只建一份**，不描述形参。这样 `DISubprogram` 的 `type` 字段有个
   合法值，但 `info locals` 依然是 `No locals.`。

**验证结果**：

```text
# llvm-dwarfdump --debug-line prog.exe（prog.tp 共 8 行）
0x1400014c0      1      0      1    ← fn add(...)
0x1400014cc      2      5      1    ← let sum: i32 = a + b;
0x1400014d3      3      5      1    ← return sum;
0x1400014e4      7      5      1    ← let x: i32 = add(1, 2);
0x1400014f7      8      5      1    ← print("%d\n", x);
```

```text
(gdb) info line prog.tp:8
Line 8 of "prog.tp" starts at address 0x1400014f7 <main+23> and ends at 0x140001505 <main+37>.
(gdb) break prog.tp:2
Breakpoint 1 at 0x1400014cc: file prog.tp, line 2.
(gdb) run
Thread 1 hit Breakpoint 1, add () at prog.tp:2
2	    let sum: i32 = a + b;
(gdb) bt
#0  add () at prog.tp:2
#1  0x00007ff7241014f3 in main () at prog.tp:7
#2  0x00007ff724101535 in main () at prog.tp:6
```

段错误这条也验了（`let p: *i32 = null; p[0] = 42;`，`p[0] = 42;` 在第 3 行）——这正是
加 `-g` 的初衷：

```text
Thread 1 received signal SIGSEGV, Segmentation fault.
main () at crash.tp:3
3	    p[0] = 42;
#0  main () at crash.tp:3
```

**隔离性**：不带 `-g` 生成的 IR 与改动前**逐字节相同**（`cmp nog.ll nog2.ll` 通过，其中
`nog.ll` 是改 `scope` 之前生成的）。IR 里 `!dbg` 的计数：不带 `-g` 是 0，带 `-g` 是 19。

**测试**：全量套件 274 项 / 273 passed / 1 skipped / 0 failed；`scripts/smoke-cli.sh` 加了
4 项 `-g` 用例（IR 里有 `!dbg`、不带 `-g` 时没有、生成了 `DICompileUnit`、与 `-emit-wasm`
互斥），共 22 项全通过。

**没做的**：局部变量与形参的 `DILocalVariable` / `DILocation` 变量位置。代价是给每条
`let` 和每个形参各建一个节点并跟踪 alloca，收益是能在调试器里 `print` 变量。等真的需要
在调试器里看值时再补 —— 行号表已经解决了「崩在哪一行」这个最痛的问题。

## P3：借用检查多条错误

### 目标

一个文件里三处借用错误 → 报三条，而不是一条。

### 为什么先做这里

`borrow.c` 是**纯 AST 走查**，不生成 IR、不碰 LLVM。报错之后继续走最多是「多报几条」，
不会让编译器崩溃 —— 这是三条路径里唯一改起来没有后顾之忧的。

### 改动点

| 文件 | 改什么 |
|---|---|
| `helpers.h` / `helpers.c` | 加 `tap_error_count()` / `tap_error_reset()` / `TAP_NORETURN void report_error_limit()`；新增非 noreturn 的 `report_error_soft(node, fmt, ...)` |
| `borrow.c` | 15 处 `report_error` → `report_error_soft` |
| `borrow.c` | `check_borrows` 返回 `int`（0 通过 / 1 有错） |
| `main.c` | `check_borrows(program)` 的返回值参与 `return` |

### 关键设计

**保留 `report_error` 的 `TAP_NORETURN`。** 103 处调用点靠它做控制流分析（`helpers.h:10`
的注释说得很清楚），去掉会引发一大片「可能没有返回值」的警告，还会掩盖真正的漏 return。

新增的 `report_error_soft` 只打印 + 计数，达到上限（建议 10）时调 `report_error_limit()`
退出。这样：

- 想改哪一处就改哪一处，不用一次动 103 个调用点；
- `borrow.c` 里那些「报完必须 return」的地方保持原样，只把 `report_error` 换成
  `report_error_soft` + `return`。

### 必须逐条检查的地方

`borrow.c` 里有些报错点在**报完之后继续走会重复报**（例如同一个符号在后续语句里被反复
冲突）。逐条过一遍，需要时加 `return` 提前结束当前语句的检查。

### 验收

- 三处借用错误 → 三条诊断，都有正确位置
- `TEST_FILTER=borrow/ sh tests/run-tests.sh build/tap` 全过
  （harness 是**包含匹配**，多报几条不影响现有用例）
- 超过 10 条时在第 10 条后停止，退出码 1

### 完成记录（2026-11-06）

**实际改动**：

| 文件 | 改了什么 |
|---|---|
| `helpers.h` / `helpers.c` | 新增 `report_error_soft` / `report_error_at_soft`（打印 + 计数，不终止）、`tap_error_count` / `tap_error_reset` / `report_error_limit`、`TAP_MAX_ERRORS 10`；抽出 `node_position()` 给四个出口共用 |
| `borrow.c` | 15 处诊断换成 soft 版本；`check_borrows` 返回 `int`（用 `tap_error_count()` 的前后差值） |
| `main.c` | `check_borrows(program)` 非 0 就释放资源返回 1，**不进 Codegen** |

**三处「多报」是坑，不是设计**，逐个修掉了：

1. **`let b = &mut x;` 报两条。** 遍历表达式时 `walk_expression` 已经在 `&mut x` 上查过
   冲突（位置是表达式，列 13），随后 `VAR_DECL` 分支又用 `mark=1` 查了一遍（位置是语句，
   列 5）。改成后者只调新增的 `mark_persistent_borrow` 登记状态、不报诊断 —— 诊断留在
   位置更精确的那次。
2. **`x = 2;` 报两条**（「被独占借用」+「被共享借用」）。`check_write_target` 的三个分支
   改成报完就 `return`，一条语句只出一条。
3. **签名错误报两条。** `check_borrow_signature` 原来是 `if (有文件名) report_error_at(...)`
   后面再跟一条 `report_error(NULL, ...)` 兜底，靠前者的 `exit` 保证不重复。换成 soft 之后
   两条都会执行 —— 改成 `if / else`。

`check_symbol_borrow_conflict` 里同样的「报完 return」也加了：冲突之后不再登记这次借用，
后续语句就少一串级联。

**验证**：

```text
$ tap check multi.tp
error: 'x' is already mutably borrowed
--> multi.tp:4:13
error: 'x' is mutably borrowed, so it cannot be borrowed again
--> multi.tp:5:13
error: 'x' is mutably borrowed and cannot be assigned here
--> multi.tp:6:5
$ echo $?
1
```

- 上限：12 条赋值错误 → 前 10 条各一条，第 10 条之后
  `error: too many errors (10), stopping now`，退出码 1。
- 跨函数：`f()` 和 `g()` 各有错 → 两条都报（`twofn.tp`，位置 4:5 和 11:5）。
- **第一条诊断没变**：把 `tests/compile-fail/borrow/` 下 22 个用例逐条跑一遍，比对
  「期望 stderr 的第一行」和「实际输出的第一条 `error:`」，22/22 相同（文本与位置都没动）。
  唯一的例外是 `return_local_reference.tp` 现在多报一条签名错误 —— 那是第二个真实的
  问题（函数声明成返回引用却没有引用参数），不是重复。
- 全量：276 条（新增 2 条）/ 275 passed / 1 skipped / 0 failed；`make test-cli` 22 项全过；
  7 个示例 `check` 全过；MSVC 产物同样报 3 条。
- `TAP_NO_BORROW=1` 仍然整体跳过借用检查（退出码 0）。

**新增用例**：

- `tests/compile-fail/borrow/multiple_errors.tp` —— 断言三条诊断都在。
- `tests/compile-fail/borrow/error_limit.tp` —— 断言第 10 条之后停下。

**没做的**：当时 parser 和 codegen 还是一次一条（后来的 P4 / P5 都做完了）。`sema.c`
依旧不报诊断，所以「多报」的收益全部集中在借用检查、语法分析和类型检查上。

## P4：语法分析错误恢复

### 目标

```text
fn main(): i32 {
    let a: i32 = ;
    let b = 1 +;
    return 0;
}
```

报两条语法错误，而不是一条。

### 改动点

| 文件 | 改什么 |
|---|---|
| `parser.c` | `parser_error` (37) 保持 noreturn；新增 `parser_error_recover`（打印 + 计数 + 不退出） |
| `parser.c` | `parse_statement` 的外层循环：捕获后跳过 token 直到 `;` / `}` / 下一个 `fn` |
| `ast.h` / `ast.c` | 新增 `NODE_ERROR` 占位节点（或让坏语句直接不进链表） |

### 关键设计

**只在语句边界恢复。** 语法错了之后 token 流没有可靠同步点，在表达式内部硬恢复只会产生
一串级联假错误。所以：

- `parse_statement` 里的 `parser_error` 换成可恢复版；
- 恢复动作：丢掉当前语句已解析的半成品，跳过 token 到最近的 `;` 或 `}`；
- 整条语句丢弃，不生成 AST 节点 —— 这样后续阶段完全看不到坏语句，不会崩。

**不要**在 `parse_type`、`parse_expression` 内部恢复：那些函数的调用方假设返回值非空，
改成可空是连锁改动。

### 验收

- 上面那段报 2 条
- 语法正确时 AST 逐字节不变（`-parse` 输出 `diff`）
- 全部 `compile-fail/` 用例仍通过（它们只要求匹配到预期的那一条）

### 风险

级联错误。缓解：恢复后如果**连续两条错误落在同一行**，只报第一条。

### 完成记录（2026-11-06）

**实际改动**：

| 文件 | 改了什么 |
|---|---|
| `parser.h` | `ParserErrorFrame`（`jmp_buf` + 链指针）；`Parser` 加 `error_frame` / `error_count` / `last_error_line`；`parser_had_error()` |
| `parser.c` | `parser_error` 有恢复点时 `longjmp`、没有时照旧 `exit(1)`；`recover_to_statement_end` / `recover_to_declaration`；`parse_block` 与 `parse_program` 的循环各压一个恢复点 |
| `main.c` | `parse_program` 之后查 `parser_had_error`，报过错就返回 1（`-parse` 例外：照常打印残缺 AST，但退出码 1） |

**和计划的差异**：

1. **用 `setjmp` / `longjmp`，没让 `parse_*` 返回 NULL。** 计划里写「`parser_error` 保持
   noreturn；新增 `parser_error_recover`」，但真正的问题不是「打印完要不要退出」，而是
   「怎么从深层递归回到语句边界」—— 报错点埋在 `parse_type` / `parse_expression` 里，
   调用方全都假设返回值非空，改成可空是几十个函数的连锁改动。`longjmp` 只改
   `parser_error` 一个地方，恢复点压栈保证嵌套块跳最内层。
2. **没有新增 `NODE_ERROR` 节点。** 计划里给了两个选项，选了「坏语句直接不进链表」：
   后续阶段完全看不到坏语句，`sema` / `codegen` 一行都不用改。
3. **多了一个 `main.c` 的短路。** 因为现在 `parse_program` 会**正常返回**一棵残缺的 AST，
   不短路的话后面会在缺语句的程序上报一串假错误。

**验证**：

```text
$ tap check two.tp
error: expected factor (integer, string, array, identifier, or parenthesized expression)
--> two.tp:2:18
error: expected factor (integer, string, array, identifier, or parenthesized expression)
--> two.tp:3:16
$ echo $?   → 1
```

- **AST 逐字节不变**：136 个合法文件（`tests/run-pass` + `examples`）分别用「HEAD 编译出来的
  基线二进制」和「新二进制」跑 `-parse`，输出**逐字节相同**。
- **首条诊断没变**：132 个 `compile-fail/` 用例逐条比对「第一条 `error:`」，132/132 相同；
  而且**每个用例的错误条数都没变**（说明没有引入新的级联）。
- **上限**：12 处语法错误 → 前 10 条 + `error: too many errors (10), stopping now`。
- **畸形输入不会死循环**：`{`、裸 `fn`、缺 `}`、多 `}`、缺函数体、顶层 `let`、
  缺分号等 8 个样本全部在 10 秒内以退出码 1 结束。
- 全量 279 条（新增 3 条）/ 278 passed / 1 skipped / 0 failed；`make test-cli` 22 项全过；
  MSVC 产物同样报 2 条（`setjmp` / `longjmp` 两个工具链都支持）。

**新增用例**：

- `tests/compile-fail/parser/multiple_errors.tp` —— 同函数两处错误各一条。
- `tests/compile-fail/parser/recovery_across_functions.tp` —— 两个函数各一处错误，证明
  恢复点不止一层。
- `tests/compile-fail/parser/error_limit.tp` —— 第 10 条之后停下。

**没做的**：`impl` 方法列表没有自己的恢复点（方法签名出错会跳到顶层恢复点，效果等价于
「丢掉整个 impl」）；表达式内部仍然不恢复（这是刻意的，见上）。

## P5：codegen 多条错误

### 目标

```text
fn main(): i32 {
    let a: i32 = "hello";
    let b: String = 42;
    undefined_function(1);
    return 0;
}
```

报三条。

### 为什么最后做

codegen 是**一边检查一边生成 IR**。报完错继续往下走，很可能会拿着一个不一致的类型去调
`LLVMBuild*`，触发 LLVM 内部断言（不是干净的诊断，是 abort）。这是三个阶段里唯一需要
「错误隔离」设计的。

### Step 0（前置重构）：统一诊断出口

**必须先做，否则多报只覆盖 87 个点里的一部分。** `codegen.c` 里还有约 44 处是

```c
print_diagnostic(stderr, "error", function->filename, function->line, function->column,
                 "variadic function '%s' must forward ...", function->name);
exit(1);
```

机械替换成 `report_error((const ASTNode *)function, "variadic function '%s' ...", ...)`
（`report_error` 自己会 `exit`，所以删掉后面的 `exit(1)`）。

`generics.c` 同理：`generic_call_error` 已经是个包装函数，改它内部实现即可；散落的
`print_diagnostic` + `exit` 一并收进去。

**这一步骤的验收方式很干净**：全部 `compile-fail/` 用例的 stderr 应该**逐字节不变**。
做法是先跑一遍全量套件存下每个用例的输出，改完再跑一遍 `diff`。任何一处格式变了都能
立刻发现。

建议在 P1 完成后就做（`tap check` 正好是验证工具），不必等到 P5。

### 关键设计：语句级隔离 + 函数级毒化

1. `CodeGenContext` 加 `int poisoned;`
2. 新增 `codegen_error(context, node, fmt, ...)`：打印 + 计数 + 置 `poisoned`
3. `generate_statement_list`（`codegen.c:4855`）的循环里，每条语句执行完检查
   `context->poisoned`：

   ```c
   for (; statement; statement = statement->next) {
       if (context->poisoned) break;      // 本函数剩下的语句不再生成
       ...
   }
   ```

4. 函数收尾时如果 `poisoned`，**按返回类型补一条 `ret`**（`zero_var_value` 已经现成），
   保证模块仍能通过 `LLVMVerifyModule` —— 否则一条错误会变成「IR 校验失败」，掩盖真正的
   诊断。
5. 每个函数之间互相独立，所以下一个函数还能正常检查 —— 这是「多报」的主要来源，通常
   已经够用（一个函数里的多个错误报不全，但每个函数都能报一条）。

### 分级推进

不要一次改 87 处。按「报错后还能安全继续」的程度分三批：

| 批次 | 类型 | 例子 |
|---|---|---|
| 1 | 纯检查、不改 IR 的 | 实参类型不匹配、返回值类型不匹配、`let` 初始化类型不匹配 |
| 2 | 报错点之后本来就会 return/continue 的 | 越界检查、`match` 分支类型不一致 |
| 3 | 报错点已经生成了半个 IR 的 | 结构体字面量、方法调用 |

只改第 1 批就能覆盖绝大多数实际场景（用户写的错误大多是类型不匹配）。

### 验收

- 上面那段报 3 条
- 全量测试全过（当前 276 条；`compile-fail/` 是包含匹配，多报不影响）
- **关键**：随便挑 20 个 `compile-fail/` 用例，确认它们的输出里第一条诊断没变 ——
  防止「为了多报，把原来的第一条诊断搞丢了」。P3 的做法可以照抄：逐条比对
  「期望 stderr 的第一行」和「实际输出的第一条 `error:`」。

### 完成记录（2026-11-07）

**实际改动**：

| 文件 | 改了什么 |
|---|---|
| `helpers.h` / `helpers.c` | `vreport_error_soft` / `vreport_error_at_soft`（可变参数版，给 `codegen_error` 这类包装器转发用）；`report_error_soft` 改成它们的薄封装 |
| `codegen.h` | `CodeGenContext` 加 `poisoned`（语句级）与 `errors_reported`（整份程序累计）；`codegen_had_error()` |
| `codegen.c` | `codegen_error` / `codegen_error_at`；`generate_statement_list` 逐条清 `poisoned`；`validate_user_types` 整段转 soft 并在报错后短路；新增 `validate_return_shapes` |
| `main.c` | `generate_code` 之后查 `codegen_had_error`，报过错就返回 1（不写文件 / 不链接 / 不运行，`check` 也返回 1） |

**和计划的差异**：

1. **毒化是语句级，不是函数级。** 计划写的是「报完一条就丢掉这个函数剩下的语句」，
   实际做下来太弱：一个函数里写错三处仍然只报一条。改成「报错的那条语句自己收尾成
   合法 IR（补零值 / 整条跳过），`generate_statement_list` 每条语句开头清 `poisoned`」，
   同一个函数里的多处错误就能一次报全。代价是每个转 soft 的位置都必须单独想清楚
   「报完这一句，IR 还合法吗」—— 想不清楚就别转。
2. **多了一个纯 AST 前置校验的分层。** `validate_user_types`（重复声明、未定义类型）
   报过之后 AST 已经不可信，直接 `return`，IR 一行都不生成；而「缺返回值」这类
   只看语句形状的检查拆到 `validate_return_shapes`，放在短路之后，所以
   「函数缺返回值」和「`let` 初始化类型不匹配」能在同一轮里一起报出来。
3. **删掉了一处重复诊断。** 函数体里「无返回值函数不能带返回值」原来报一次，现在
   `validate_implicit_return` 已经报过，body 那处只负责补 `ret void`。
4. **`generics.c` 只做了 Step 0，没转 soft。** 泛型单态化阶段的诊断都是「模板写错了」，
   一次一条足够；多报的收益集中在类型不匹配上。

**验证**：

```text
$ tap check three.tp
error: initializer type mismatch for 'x'
--> three.tp:6:5
error: function 'add' argument 2 type mismatch
--> three.tp:7:18
$ echo $?   → 1
```

- **零回归**：135 个 `compile-fail/` 用例的 stdout / stderr / 退出码与改动前
  **逐字节相同**（`diff -r` 无输出）。这是 P5 唯一的硬指标 —— 多报是「变多」，
  但已有的那条不能变。
- **上限**：12 处 `let` 类型错误 → 前 10 条 + `error: too many errors (10), stopping now`。
- 全量 282 条（新增 3 条）/ 281 passed / 1 skipped / 0 failed；`make test-cli` 22 项全过；
  7 个示例 `check` 全过。

**新增用例**：

- `tests/compile-fail/types/multiple_errors.tp` —— 三处错误分别来自 `return`、`let`
  初始化、实参三个不同的诊断点。
- `tests/compile-fail/types/missing_return_multiple.tp` —— 两个函数各缺一处返回值，
  证明纯 AST 前置校验也在累积。
- `tests/compile-fail/types/error_limit.tp` —— 第 10 条之后停下。

**没做的**：Step 0 合并之后 codegen 有 112 处硬诊断，只转了 17 处（第 1 批 + 前置校验）。
剩下的分两类：
一类是「报错点已经生成了半个 IR」（结构体字面量、方法调用），要继续得先想清楚
怎么把半个 IR 收尾；另一类是 `break`/`continue` 在循环外、`main` 签名不对这种
全局唯一的问题，多报没有意义。

## 验证矩阵

每个阶段都要跑：

```bash
# 全量
TEST_JOBS=8 TEST_RUN_MODE=native bash tests/run-tests.sh ./build/tap
# 只跑相关
TEST_FILTER=borrow/  sh tests/run-tests.sh build/tap
TEST_FILTER=traits/  sh tests/run-tests.sh build/tap
# 7 个示例
for f in examples/*.tp; do ./build/tap -o /tmp/e.exe $f || echo "FAIL $f"; done
```

新增的 CLI 行为（`check`、`-g`）**没有 harness 覆盖**（`tests/run-tests.sh` 只比对编译
输出），已经补了 `scripts/smoke-cli.sh`，见文末。

## 顺序与理由

| 顺序 | 阶段 | 理由 |
|---|---|---|
| 1 | **P1 `tap check`** ✅ | 零风险、半天、马上能用；也是后面 P3/P5 的验证工具（改完直接 `tap check` 看报几条） |
| 2 | **P2 `-g`** ✅ | 自包含（一个 flag + 一个文件），不碰诊断路径。现在没有任何运行时问题的排查手段，这是最痛的 |
| 3 | **P3 借用多条错误** ✅ | 安全、独立，顺便把 `report_error_soft` 这套基础设施搭好 |
| 4 | **P4 语法恢复** ✅ | 需要新节点类型，改 AST |
| 5 | **P5 Step 0 统一诊断出口** ✅ | 机械替换，验收方式干净（stderr 逐字节不变）；P5 正文的前置 |
| 6 | **P5 codegen 多条错误** ✅ | 唯一有崩溃风险的，放最后；前面几条积累的 soft 基础设施正好复用 |

**为什么 P2 排在 P3 前面**（和 `tooling.md` 的建议不同）：`tooling.md` 把多条错误排第二，
但它要动 100 多个调用点、有级联噪声风险；`-g` 只碰 codegen 的入口和收尾，改坏了也只是
「调试信息不准」，不会影响编译正确性。先拿确定性收益。

**为什么 P5 Step 0 排在 P4 之后**：它虽然是纯机械替换，但一次性动 40 多处，最好等
`report_error_soft`（P3）和 AST 错误节点（P4）都就位了再做 —— 那时 `report_error` 的
签名和语义已经稳定，替换一次到位，不用返工。

## 不做的事

**增量编译**（`tooling.md` 第 5 条）：实测前端 + codegen 只有 45ms，其中还有约 30ms 是
进程启动加载 LLVM。真要做缓存，收益上限就是 10~15ms，却要引入「单态化结果 + LLVM
Module」的哈希与失效逻辑。**不值得，先搁置。** 等 P1 上线后如果 CI 还嫌慢，再回来看。

## 顺带建议：加一个 CLI 冒烟脚本（已完成，2026-11-06）

`tests/run-tests.sh` 只覆盖「编译一个 .tp 文件、比对 stdout/stderr/退出码」，覆盖不到
命令行本身。`tap check` 和 `-g` 的正确性没法用它验证。

已落地 `scripts/smoke-cli.sh`（和 `collect-dlls.sh` 并列），22 项检查，挂到
`make test-cli` 与 ctest 的 `cli-smoke-tests`。用法：

```bash
make test-cli                        # 走 Makefile
sh scripts/smoke-cli.sh ./build/tap  # 直接跑
```

覆盖内容：`check` 静默返回 0 / 不产出文件 / 有错返回 1 / 诊断带行列、七种选项互斥、
只能给一个命令、包模式不建 `build/`、包模式从子目录可用、缺 `package.toml` 报错、
`run` 回归、`-g` 的四个用例。

一个坑：脚本里每条用例都 `cd` 到临时目录，所以传进来的 `tap` 路径**必须先转绝对路径**，
否则 `./build/tap` 换个工作目录就找不到 —— 而「找不到」恰好是非 0 退出，互斥类用例会
**假通过**。第一版就踩了这个坑（10 项失败里有一半是真失败，另一半是假通过）。
