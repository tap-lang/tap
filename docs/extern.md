# C 互操作（extern）方案

> **状态**：方案文档。第 3 节的 P0 / P1 / P2 均**未实现**，第 1 节描述的是当前已经可用的能力。
> 本文的依据来自 `src/parser.c`、`src/codegen.c`、`src/run.c`、`src/main.c` 的实际代码，
> 以及 `tests/run-pass/types/` 下的现有用例。

tap 的 FFI 入口只有一个：`extern fn`。它只生成 LLVM 函数声明，不生成函数体，符号名
不加模块前缀，所以直接绑 C 符号。

**关键结论：tap 现在就能调 C。** `tests/run-pass/types/c_string.tp` 已经在调 `strlen`。
真正缺的不是「能不能调」，而是「能不能调**第三方库**」——卡点全在链接环节。

## 1. 现状：已经能做什么

### 1.1 能力矩阵

| 能力 | 状态 | 实现位置 |
|---|---|---|
| `extern fn` 声明，只生成 LLVM 声明 | ✅ | `ast.h:232`（`is_extern`） |
| 符号名保持 C 原名，不加模块前缀 | ✅ | `module.c:766` |
| 标量（i8…i128 / u8…u128 / f32 / f64 / bool） | ✅ | `codegen.c` |
| `c_string`（= `const char *`） | ✅ | `data-type-string.md` |
| `*T` 裸指针、`**T`、固定长度数组 | ✅ | `data-types.md:464` |
| 结构体、枚举（按值） | ⚠️ 见 §3.4 | — |
| 变参 `...`（仅 extern） | ✅ | `parser.c:793` |
| `String` 跨边界（特殊 lowering） | ✅ | `codegen.c:3234` |
| 指针解引用 `*p`、`&x`、`p[i]` | ✅ | `ast.h:47`、`dereference.tp` |
| `extern` 全局变量 | ❌ 见 §3.2 | `parser.c:1719` |
| 函数指针 / 回调 | ❌ 见 §3.3 | `bootstrap.md:197` |
| 链接第三方库（`-l` / `-L`） | ❌ 见 §3.1 | `main.c:76`、`run.c:517` |
| 头文件绑定生成 | ❌ 见 §3.5 | — |

### 1.2 最小可用示例

```tap
extern fn strlen(value: c_string): uint;
const RAW_GREETING: c_string = "tap";

fn main(): i32 {
    let text: String = "hello";
    let raw: c_string = text;      // String 隐式借出 data 指针
    print("%llu\n", strlen(raw));
    let restored: String = raw as String;  // 走 __tap_string_from_c 重新量长度
    return 0;
}
```

`String` → `c_string` 是隐式转换（借出 `data`，尾部本来就保留 NUL）；
`c_string` → `String` 必须显式 `as String`，由 `__tap_string_from_c` 扫描长度。

### 1.3 String 跨 extern 边界的 ABI

这一点必须遵守，否则 Windows 上直接崩。详见 `runtime/include/tap_runtime.h` 顶部注释。

Windows x64 的 C ABI 把 16 字节结构体参数**按引用传**（byval）、返回值走 **sret 隐藏指针**；
而 LLVM 对 IR 里裸的 `{ ptr, i64 }` 聚合是按寄存器传的。SysV 和 AAPCS64 上两者恰好一致，
Win64 上完全对不上。

因此 extern 边界上的 `String` 一律按指针传：

- 入参：`const String *value` —— 编译器在 `codegen.c:3279` alloca 后传地址
- 返回：`void f(String *out, ...)` —— out 是**第一个**参数，无条件写入
  （`codegen.c:3245` 先插一个 `extern_string_out` 槽位）

tap 内部函数不做这个 lowering（两端都是 tap 生成的，LLVM 对裸聚合处理自洽）。

### 1.4 变参

只有 `extern fn` 可以带 `...`，直接绑 C 的变参 ABI。超出具名参数范围的实参按 C 的
默认实参提升规则生成：`float` → `double`，窄整数 → `i32`（`codegen.c:3255`）。

### 1.5 用指针代替 `->`

`*T` 支持下标读写，所以 `p[0].field` 就是 `->` 的等价物，现在就能用，不必等语法糖：

```tap
extern fn get_point(): *Point;

fn main(): i32 {
    let p: *Point = get_point();
    print("%d\n", p[0].x);
    p[0].x = 42;
    return 0;
}
```

## 2. 为什么链接是唯一的硬阻塞

只要能链接进来，就能调。而现在的链接命令是**写死的定长 argv 数组**：

| 环境 | 位置 | argv |
|---|---|---|
| MinGW-w64 | `run.c:531` / `run.c:535` | `gcc obj runtime.a -o exe -lws2_32` |
| MSVC | `run.c:553` | `clang-cl /MT ... obj runtime.lib /Fe exe ws2_32.lib` |
| macOS | `run.c:583` | `ld -o exe obj runtime.a -lSystem -syslibroot ...` |
| Cygwin | `run.c:617` / `run.c:621` | `clang obj runtime.a -o exe -lm` |
| Linux | `run.c:627` / `run.c:631` | `cc -no-pie obj runtime.a -o exe -lm` |

`main.c` 的选项解析（`main.c:76`–`main.c:115`）只有
`-o / -static / -ir / -emit-obj / -emit-wasm / -run-lli / -lex / -parse`，
没有任何 `-l` / `-L`。**没有地方把库名传下去。**

## 3. 缺口与方案

### 3.1 P0：链接参数通道

**影响**：调不了任何需要额外链接的库（zlib、sqlite3、libcurl…）。

**方案**：CLI 增加三个选项，收集成链表往下传到 `link_object_file()`。

| 选项 | 语义 |
|---|---|
| `-l<name>` | 追加 `-l<name>`，顺序即命令行顺序 |
| `-L<dir>` | 追加库搜索路径 |
| `-Wl,<arg>` | 原样透传给链接器，逗号拆分成多个参数 |

**实现点**：

1. `main.c:76` 附近的选项循环里加解析分支，允许 `-l foo` 和 `-lfoo` 两种写法
2. 新增一个 `LinkOptions`（或直接 `char **` 链表）从 `main.c` 传到 `codegen.c` /
   `run.c`；`compile_to_executable` / `compile_and_run` 的签名要跟着扩
3. `link_object_file()`（`run.c:517`）加参数，四处 argv 数组从定长改成 `malloc` 动态拼
4. **顺序**：用户库必须排在 `object_file` 和 `runtime.a` **之后**。静态链接对顺序敏感，
   库在前会导致符号解析不到
5. macOS 分支（`run.c:583`）直接调 `ld`，`-Wl,` 前缀在那里不成立，要拆成裸参数；
   `-l` 在 `ld` 下语义相同，可以原样传
6. `-static` 与 `-l` 的交互：`-static` 只影响驱动标志，不改用户库的顺序，无需特殊处理
7. 交互式模式 `-run-lli` 不需要链接，选项应被忽略而不是报错

**风险**：低。改动集中在一个函数和选项解析，但四个平台分支都要过一遍 CI。

### 3.2 P1：`extern` 全局变量

**现状**：`parser.c:1719` 硬性要求 `extern` 后面必须是 `fn`，报
`extern must be followed by a function declaration`。

**影响**：C 库大量用全局变量——版本号（`sqlite3_version`）、配置结构体、
`errno` 这类。现在完全够不着。

**方案**：允许 `extern let NAME: T;`。

**实现点**：

1. `parser.c:1719` 放开限制，`extern let` 走 `parse_var_decl` 并标记 `is_extern`
2. `ast.h` 的 `VarDeclNode` 加 `is_extern` 字段（或复用现有 flag）
3. `codegen.c` 生成 external linkage 的 `LLVMAddGlobal`，**不**生成 initializer
4. 读取走 `LLVMBuildLoad`，写入走 `LLVMBuildStore`（只允许可写变量，`const` 不行）
5. `module.c:766` 的符号名逻辑要覆盖全局变量，同样保持 C 原名

**风险**：低。这是性价比最高的一项。

### 3.3 P1：函数指针与回调

**现状**：`*fn(i32, i32): i32` 报 `expected type`（`docs/bootstrap.md:197`）。

**影响**：所有回调式 API 都用不了——`qsort`、libcurl 的 write callback、
`sqlite3_exec`、各种事件循环。

**方案**：把 `*fn(参数列表): 返回类型` 作为一种类型。

**实现点**：

1. `VarTypeNode`（`ast.h:165`）加 `is_function_pointer`、参数类型链、返回类型
2. `parser.c` 的类型解析里识别 `*fn(` 前缀；注意和 `*` 解引用、`*T` 指针的歧义
3. `codegen.c` 生成 `LLVMPointerType(LLVMFunctionType(...), 0)`
4. 调用点：函数值是表达式时走 `LLVMBuildCall2` 的 indirect call，不能只认具名函数
5. `&tap_fn` 取函数地址：`module.c` 已有符号改写逻辑，能拿到稳定符号名
6. `null` 要能赋给函数指针类型（C 侧常用 NULL 表示"不设置回调"）

**风险**：**高**。这是本文改动面最大的一项，涉及类型系统、parser、codegen 三处，
且要处理与现有 `*` 语法的歧义。

### 3.4 P1：结构体按值跨 extern

**现状**：只有 `String` 做了 lowering（`codegen.c:3234`），任意结构体按值传参
没有任何处理。

**影响**：SysV / AAPCS64 上碰巧对（裸聚合的寄存器分配恰好和 C ABI 一致），
**Win64 上会错**——大于 16 字节要 `byval`，8 / 16 字节要拆 `rcx:rdx` 或 `xmm0`。

**建议**：**先不做**。用 `*T` 传结构体指针规避，C 侧本来也推荐这么写。
真要做的话需要在 extern 边界按大小分类插入 `byval` 属性或手工拆寄存器，
是整个 FFI 里最容易出隐蔽 bug 的地方。

### 3.5 P2：其余

| 缺口 | 说明 | 建议 |
|---|---|---|
| 头文件绑定生成 | 手写 `extern fn`，没有 translate-c | 工具链问题，非语言问题；可后置 |
| `union` | 带载荷 enum 的布局和 C union 对不上 | 需要时用 `*u8` + 偏移手工访问 |
| 宏常量 | C 的 `#define` 常量无对应物 | 用 tap 的 `const` 手写一遍 |
| 只读指针 | `const T *` 不存在，`const` 常量不能取地址 | 影响不大，先不排期 |

## 4. 过渡方案：C 胶水层

在 P0 落地之前，有一条**零语言改动**的路：把第三方库包成 `__tap_xxx` 简单签名塞进
Runtime，tap 侧只声明 Runtime 函数。

```c
// runtime/src/runtime.c
int32_t __tap_sqlite_open(const String *path) { ... }
```

```tap
extern fn __tap_sqlite_open(path: String): i32;
```

这正是标准库现在的做法（`std/fs.tp`、`std/net.tp` 全是这个模式），也是
process 包跨平台改造要走的路线——把 `fork`/`execvp` 的平台差异收进
`__tap_process_run`，`.tp` 侧只留简单签名。

| | 优点 | 缺点 |
|---|---|---|
| 胶水层 | 今天就能用；平台差异天然收敛 | 每个库都要写胶水；Runtime 会越来越胖 |

## 5. 分阶段路线

| 阶段 | 内容 | 解锁面 | 风险 |
|---|---|---|---|
| 1 | **P0 链接通道**（§3.1） | 打通「能链进来」，配合 `*T` + `p[0].field` 已能覆盖大部分库 | 低 |
| 2 | **extern 全局变量**（§3.2） | 配置结构体、版本查询、`errno` 类 | 低 |
| 3 | **函数指针**（§3.3） | 所有回调式 API | 高 |
| 4 | 结构体 by-value ABI（§3.4） | 只在有明确需求时做 | 高 |

阶段 1 做完，`tap -lsqlite3 -L/usr/local/lib hello.tp` 就能跑通，这是最小的可用闭环。

## 6. 验证方式

每一项都要有对应用例，且**必须在六个环境全绿**（macOS / Linux / MSVC / MINGW64 /
UCRT64 / Cygwin），因为 FFI 的坑几乎全部是平台 ABI 差异。

| 阶段 | 用例 | 验证点 |
|---|---|---|
| P0 | `tests/run-pass/extern/link_lib.tp` | 链接一个真实第三方库并调用 |
| P0 | 同上 | 库顺序正确（`-l` 在 obj 之后） |
| P0 | 同上 | `-l` 不存在时给出可读的错误，不是裸链接器报错 |
| P1 全局 | `tests/run-pass/extern/global.tp` | 读 + 写 extern 全局 |
| P1 函数指针 | `tests/run-pass/extern/callback.tp` | 调 `qsort` 传 tap 函数 |
| P1 函数指针 | 同上 | `null` 赋给函数指针 |

用例沿用 `tests/run-tests.sh` 的 `/** -- test` 内联期望格式，
参考 `tests/run-pass/types/c_string.tp`。

## 7. 待确认

1. `-l` 的写法：只支持 `-lfoo`，还是同时支持 `-l foo`？（建议两者都支持，和 cc 一致）
2. 是否要源码级指令（`@link("sqlite3")`）让包自带链接需求？这需要动 lexer / parser /
   AST，属于 P0 之后的独立阶段，但能让 `package.toml` 表达链接依赖。
3. 函数指针的语法最终形态：`*fn(i32): i32`，还是引入 `fn(i32): i32` 作为一等函数类型
   再配 `*`？前者改动小，后者语义更干净。
