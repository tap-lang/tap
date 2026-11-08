# 自举可行性分析

评估基准：`build/tap.exe`（`b0a17e5`），2026-11-08。全部结论来自实跑，探针在
`tmp/boot3/`。本文只记本次评估的结论。

## 结论

**语言层面已无结构性硬阻塞；「前端 + 文本后端 + 外部工具链」的完整闭环已实测
跑通；但完整自举仍是一个独立的大工程。**

| 层 | 结论 | 依据 |
|---|---|---|
| 表达力：能不能写 | ✅ 无硬阻塞 | 指针递归 AST、堆上整体赋值、链式字段、函数指针、目录遍历都可用；唯一缺的全局可变状态可用 `&mut Context` 绕开 |
| 闭环：跑不跑得通 | ✅ 已跑通 | `p10` 用 tap 写 lexer + 递归下降 parser + 文本后端，调 gcc 编译并运行输出 `15`；`p11` 用 tap 拼 LLVM IR，经 `llc` + `gcc` 产出原生 exe 输出 `42` |
| 工程：值不值得做 | ⚠️ 不建议全量重写 | C 实现 24210 行；自举编译器要编译自己就得实现几乎全部语言；验收面 347 个 `.tp` 用例 |

一句话：**能力已不是瓶颈，瓶颈是工程量与后端取舍** —— 接受「外部工具链 + C Runtime」
的半自举，还是自己写后端。

## 实测探针（`tmp/boot3/`）

| 探针 | 验证 | 结果 |
|---|---|---|
| `p01_ptr_ast.tp` | 指针递归结构体 + `null` + 递归求和 | ✅ `sum=6` |
| `p02_heap.tp` | 堆上构造结构体 | ✅ `*p = Cell{...}` / `q[0] = Cell{...}` / `p.value = 9` 三种写法全通过，`whole=5 field=9 index=11` |
| `p03_chain.tp` | 链式字段 + 表达式接收者 | ✅ `chain=7 method=7 expr=13` |
| `p04_enum.tp` | 指针递归载荷枚举 + match 求值 | ✅ `eval=5` |
| `p05_ctx.tp` | `&mut Context` 原地改状态 + `Vec<*Sym>` | ✅ `count=2 name=beta value=20` |
| `p06_global.tp` | 顶层可变全局 | ❌ `expected module import, enum declaration, ...`，解析阶段就拒 |
| `p07_fnptr.tp` | `*fn(i32,i32): i32` 取地址与间接调用 | ✅ `f=3` |
| `p08_dir.tp` | `std.fs.read_dir` 扫目录 | ✅ `count=14 first=byte_vec.tp` |
| `p09_hash.tp` | 在 `Vec` 上手写开放寻址符号表 | ✅ `parser=2 codegen=3 missing=-1` |
| `p10_pipeline.tp` | tap 写 lexer/parser/文本后端 → gcc → 运行 | ✅ 生成 `printf("%d\n", 1 + 2 * (3 + 4));`，`gcc_rc=0 run=15` |
| `p11_llvm_ir.tp` | tap 拼 LLVM IR → `llc` → `gcc` → 原生 exe | ✅ `llc=0 link=0 output=42` |
| `p12_recursive_value.tp` | 按值递归枚举的诊断 | ❌ 仍漏到 `LLVM IR verification failed: Cannot allocate unsized type` |
| `p13_scale.tp` | 1105 行 / 1000 函数的编译规模 | ✅ `check` 0.06s，完整编译+运行 0.37s（本机实测） |
| `p14_mutual.tp` | 无前向声明的互相递归 | ✅ `even=1 odd=1` |

## 能力现状

### 写编译器直接要用、且已具备

| 能力 | 状态 |
|---|---|
| 递归 AST | ✅ `*T` 解引用、`null`、指针字段读写、递归函数与互递归（`p01` / `p14`） |
| 堆上构造节点 | ✅ `*p = S{...}`、`q[0] = S{...}`、逐字段赋值（`p02`） |
| 用枚举表达 AST | ✅ `enum Expr { Num(i32), Add(*Expr, *Expr) }` + `match` 求值（`p04`） |
| 嵌套访问 | ✅ `o.inner.a`、`o.inner.peek()`、`list.get(0).peek()`（`p03`） |
| 编译器状态 | ✅ `&mut Context` 原地修改，不必层层返回结构体（`p05`） |
| 符号表 | ✅ `Vec<*Sym>`；std 无 HashMap，但自写开放寻址表可用（`p09`） |
| 分派表 / visitor | ✅ `*fn(...)` 函数指针，含参数、返回值、null 比较 |
| 模块加载 | ✅ `std.fs.read_dir` / `open_dir` / `is_dir` / `make_dir`（`p08`） |
| 文本生成与工具调用 | ✅ `StringBuilder`、文件 IO、`format_i64`、`system` / `capture`（`p10` / `p11`） |
| 其余基础件 | ✅ 字符串与 `Vec` / `ByteVec`、泛型与 trait、`Result` / `?`、位运算、任何 C ABI 的 `extern fn` |

另有两处语法限制，都有直接写法可替：函数前向声明 `fn f(...);` 不支持（后定义的函数
可直接调用，互递归不受影响）；函数名不能叫 `f32` / `f64`（类型关键字）。

上一版把「整体赋值仍需绕」「链式字段访问不支持」「没有函数指针 / 目录遍历」「没有
HashMap 就只能线性查找」列为缺口，本次实测这些判断均已失效。

### 仍然缺的（按对自举的影响排序）

1. **全局可变状态** —— 唯一没有语言级绕法的缺口（`p06`，Runtime 也没有 global slot），
   但 `&mut Context` 够用。
2. **字符串 API** —— `contains` / `find` / `split` / `trim` / `replace`、UTF-8 码点
   操作、格式化追加（见 [`todo/string.md`](todo/string.md)）。写 lexer 和诊断时摩擦
   最大，都能在 tap 层补，不必改 Runtime。
3. **借用 L2（结构体存 `&T`）** —— 对自举影响小：AST 用 `*T` 裸指针即可，不进借用检查。
4. **`[T]` 切片、`if` 表达式、固有 impl、match 嵌套模式** —— 体验问题，见
   [`todo/language.md`](todo/language.md)。
5. **三处诊断 / 语法小坑** —— 按值递归枚举漏到 IR 校验（`p12`）；`match (*e)` 不接受
   解引用主体，先 `let value: Expr = e[0];` 再 match 即可；括号解引用链 `(*q).x` /
   `(*q).m()` 仍不认，写 `q.x` 或 `q[0].x` 是通的。都只需前端补检查。
6. **严格自举的后端** —— 自己产出目标文件 / 链接，见下一节。

## 后端：两条路线

**推荐：路线 B —— 生成 IR / 汇编文本 + 外部工具**，本次实测闭环。`p11` 用
`StringBuilder` 拼出 `.ll`，`llc -filetype=obj` → `gcc` 链接 → 原生 exe 输出 `42`；
`p10` 证明「生成文本的那一半」可以完全用 tap 写（文件 IO、`format_i64`、`system` /
`capture`）。不需要碰 LLVM C API，也不需要改链接命令。代价：依赖外部 `llc` / `gcc`
（或 `clang`），且严格意义上仍是「半自举」。

**路线 A —— 进程内调 LLVM C API**，成本常被低估（以下为依据 C API 形态的推断，
未实测）：LLVM C API 里有宏与 inline 包装，而 `extern fn` 只能绑符号；变参调用与
`LLVMValueRef*` 数组需要一层 C shim；`run.c` 的链接行、句柄类型映射也都要动。即便
做通，产物仍链 LLVM，「自举」只完成编译器逻辑那一半。

**严格自举还差什么**：Runtime 1789 行 C（`runtime/src/runtime.c` 1557 + 头文件
232）与 driver/linker（`run.c` 2023 行）仍是 C，tap 程序要链 `libtap_runtime.a`。
要不依赖任何 C 组件，得自己产出目标文件（COFF / ELF）并做链接 —— 位运算已够，
但这是另一个量级的工作，且与语言能力无关。

## 工作量

| 项 | 规模 |
|---|---|
| `src/*.c` | 21159 行 |
| `src/*.h` + Runtime | 1262 + 1789 行，合计 **24210 行**（上一版约 1.23 万行，接近翻倍） |
| `codegen.c` | 6034 |
| `borrow.c` / `generics.c` / `module.c` | 1698 / 2202 / 2104 |
| `std/*.tp` | 1547（**可直接复用**，不必重写） |
| `tests/*.tp` | 347（run-pass 142 / compile-fail 160 / fixtures 27 / 其余 18） |

两个放大因素：

1. **自我描述成本**。C 版编译器是用 C 写的，只需要一个 C 编译器；tap 版编译器用
   tap 写，必须能编译 tap 自身用到的全部特性 —— 借用检查、泛型单态化、trait、
   模块三级查找、带载荷枚举、函数指针。也就是说 `borrow.c`、`generics.c` 这些
   「编译别人」的逻辑，本身也要被重新实现一遍。
2. **诊断一致性**。160 个 compile-fail 用例要求错误信息、源码位置和多错误聚合
   一致，这部分工作量容易漏算。

粗估：按 1.2–1.4 倍放大，等价实现约 3 万行 tap。规模本身不是瓶颈（`p13`：1105 行 /
1000 函数，`tap check` 0.06s、完整编译运行 0.37s）。

## 建议路径：分阶段子集自举

不要一次重写全量，按「每步都能独立验收」推进：

1. 把 `p10` 的 C 文本后端换成 LLVM IR 文本（`p11` 已验证工具链），得到只处理
   表达式子集的 stage1。
2. 扩到语句 / 结构体 / 函数 / 模块的**固定子集**，让 stage1 能生成可执行的 IR；
   子集选得越小，越早能进第 3 步。
3. 找固定点：用 C 版编译器编译 stage1，再用 stage1 编译 stage1 自己的源码，比较
   两次产物的行为。这一步才叫「自举验证」，且不要求全语言。
4. 逐特性扩展（借用检查、泛型单态化、trait、模块），每轮以 347 个用例为回归线。
   **全量等价另立项目**，不要和 1–3 混做。

## 复现

```sh
./build/tap.exe run tmp/boot3/p10_pipeline.tp    # 需要 PATH 上有 gcc
./build/tap.exe run tmp/boot3/p11_llvm_ir.tp     # 需要 PATH 上有 llc 和 gcc
```

本机 `llc` 23.1.3，默认目标 `x86_64-w64-windows-gnu`。Windows 下 `sys.capture`
要用反斜杠路径 —— cmd 不认 `tmp/boot3/...` 这种正斜杠开头的命令。
