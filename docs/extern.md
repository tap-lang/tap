# C 互操作（extern）方案

> **状态**：方案文档。**P0（链接参数通道）已实现**，见 §2；§3 里剩下的 P1 / P2 均未实现。
> 第 1 节描述的是当前已经可用的能力。
> 本文的依据来自 `src/parser.c`、`src/codegen.c`、`src/run.c`、`src/main.c` 的实际代码，
> 以及 `tests/` 下的现有用例。

tap 的 FFI 入口只有一个：`extern fn`。它只生成 LLVM 函数声明，不生成函数体，符号名
不加模块前缀，所以直接绑 C 符号。

**关键结论：tap 现在就能调 C。** `tests/run-pass/types/c_string.tp` 已经在调 `strlen`；
加上 P0 之后，`tap run -lz hello.tp` 也能调第三方库（`tests/run-pass/extern/link_zlib.tp`）。

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
| 函数指针 / 回调 | ✅ 见 §3.3 | `parser.c` 的 `parse_function_pointer_type` |
| 链接第三方库（`-l` / `-L`） | ✅ | §2、`run.c` 的 `link_object_file` |
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

## 2. 链接参数通道（P0，已实现）

只要能链接进来，就能调。原来链接命令是**写死的定长 argv 数组**，库名没有地方传下去；
现在改成三层：

1. **选项解析**（`main.c`）：收集成 `LinkOptions` 链表，按出现顺序保存
2. **传参**：`compile_to_executable` / `compile_and_run` 多一个 `const LinkOptions *`，
   一路传到 `link_object_file()`
3. **拼命令行**（`run.c`）：`LinkArgv` 动态构建器替换掉四处定长数组，
   `push_link_options()` 按平台把用户参数展开成链接器认识的写法

### 2.1 选项

| 选项 | 语义 |
|---|---|
| `-l<name>` / `-l <name>` | 追加库，两种写法都认（和 cc 一致）；顺序即命令行顺序 |
| `-L<dir>` / `-L <dir>` | 追加库搜索路径 |
| `-Wl,<arg>` | 透传给链接器，逗号拆成多个参数（`-Wl,-rpath,/x` → `-rpath /x`） |

缺参数（`-l` 后面没东西）报 `error: -l requires a library name` 并打用法，
不会当成源码文件名。

### 2.2 顺序

`LinkOptions` 必须是**保序链表**，不能用集合：静态链接的符号解析是单向的。
实际拼出来的顺序固定为

```
<驱动> [标志] <object_file> <runtime.a> <用户 -L/-l/-Wl,> <系统库>
```

用户库排在 `runtime.a` **之后**、系统库（`-lm` / `-lws2_32` / `-lSystem`）**之前**。
`-L` 也保持命令行顺序，保证依赖它的 `-l` 在它后面。

### 2.3 各平台的展开

| 环境 | 驱动 | `-lfoo` | `-Ldir` | `-Wl,a,b` |
|---|---|---|---|---|
| macOS | `ld` | `-lfoo` | `-Ldir` | `a b`（拆成裸参数，`ld` 不认 `-Wl,`） |
| Linux | `cc` | `-lfoo` | `-Ldir` | `-Wl,a,b`（驱动自己拆） |
| Cygwin | `ld.lld`（找不到时 `cc`） | 展开成完整路径（`libfoo.a` / `libfoo.dll.a` / `foo.lib`）—— LLD 的 COFF 模式不认 `-lfoo`；找不到就原样留 `-lfoo` | `-Ldir` | `a b`（`ld.lld` 同样不认 `-Wl,`；回退到 cc 时才用 `-Wl,a,b`） |
| MinGW-w64 | `ld.lld`（找不到时 `gcc`） | `-lfoo` | `-Ldir` | `a b`（`ld.lld` 同样不认 `-Wl,`；回退到 gcc 时才用 `-Wl,a,b`） |
| MSVC | `lld-link`（找不到时 `clang-cl`） | `foo.lib`（已有 `.lib` 后缀则不重复加） | `/LIBPATH:dir` | `<arg>`（要写 link.exe 的写法） |

MSVC 那一行是唯一语义有偏差的：`-lfoo` 得翻译成 `foo.lib` 当输入文件。
直接调 `lld-link` 时 `-L` / 裸参数就是 `/LIBPATH:dir` 和 link.exe 风格的原样参数；
只有回退到 `clang-cl` 时才需要把这两类塞进 `/link` 段（`/link` 之后的一切都归
link.exe，所以那段必须放在最后）。

### 2.4 链接失败时的提示

链接器自己只会说 `cannot find -lsqlite3`，不会告诉你下一步干什么。现在补一段：

```
ld: library 'nosuchlib' not found
link failed with linker exit code: 1
note: requested libraries: -lnosuchlib
note: check that the library is installed and that -L points at the directory containing it
```

### 2.5 不支持链接的几种模式

`-ir` / `-emit-obj` / `-emit-wasm` / `-run-lli` 都不链接，`-l` / `-L` / `-Wl,` 被
**静默忽略**而不是报错（`-run-lli` 下 lli 恰好能从宿主进程里解析到一部分符号，
但这属于 lli 的行为，不是 tap 的保证）。

### 2.6 回归用例

`tests/run-pass/extern/link_zlib.tp`：`-lz` 链 zlib，调 `zlibVersion()` 和
`compressBound()`，期望值写死（`compressBound` 的公式和 zlib 版本无关）。

用例通过内嵌的 `- .flags` 段把编译选项传给 `tap run`，这个段会让用例强制走
native 模式 —— `lli` 不链接，带 `-l` 的用例在 fast 模式下没有意义。
zlib 在 macOS SDK 和多数 Linux 发行版上现成（Linux 上要装 `zlib1g-dev` 之类才有
`libz.so` 符号链接），MSVC / MinGW / Cygwin 的工具链不带它，
所以用例用 `- .skip-on windows` 整条跳过，报 `SKIP` 而不是 `FAIL`。

`tests/run-pass/extern/link_winmm.tp`：`-lwinmm` 链 Windows 自带的多媒体库，
调 `timeGetTime()` 验证链接成功 + 符号解析成功 + 返回值合理。
winmm 在 Windows NT 起全系自带（MinGW 的 `libwinmm.a`、MSVC 的 `winmm.lib`），
不需要额外安装，补上了 `link_zlib.tp` 在 Windows 上整条 skip 留下的覆盖缺口。
用例用 `- .skip-on darwin` + `- .skip-on linux` 在非 Windows 平台跳过。

## 3. 缺口与方案

### 3.1 P0：链接参数通道 —— 已完成

实现细节见 §2，这里只留状态和遗留项。

- ✅ `-l` / `-L` / `-Wl,` 三种选项，保序传给链接器
- ✅ 四处定长 argv 改成动态构建，用户库排在 object 和 runtime 之后
- ✅ macOS 的 `ld` 分支把 `-Wl,` 拆成裸参数
- ✅ 链接失败补 `note:` 提示
- ✅ 回归用例 `tests/run-pass/extern/link_zlib.tp`
- ⚠️ **只在 macOS 上实测过**。Linux / Cygwin / MinGW / MSVC 四个分支的代码是按各自
  驱动的约定写的，但没在真机上跑过，需要过一遍 CI
- ⚠️ MSVC 的 `-l` 按 `.lib` 语义走（`-lsqlite3` → `sqlite3.lib`），和 GNU 的
  `-lsqlite3` 找 `libsqlite3.a` / `.so` 不是一回事，跨平台脚本要注意
- ⚠️ 还没有「库找不到」的用例：链接失败的错误文本是平台相关的，
  run-pass / run-fail / compile-fail 三组都放不下，暂时只手工验证

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

### 3.3 P1：函数指针与回调 —— 已完成

**现状**：`*fn(i32, i32): i32` 已经是一种类型。`qsort` 传 tap 函数实测通过
（`tests/run-pass/extern/callback.tp`）。

**语法**

```tap
extern fn qsort(base: *i32, count: uint, size: uint,
                compare: *fn(*i32, *i32): i32): i32;

fn ascending(a: *i32, b: *i32): i32 {
    return a[0] - b[0];
}

fn main(): i32 {
    let values: [i32; 5] = [5, 3, 1, 4, 2];
    qsort(&values[0], 5, sizeof(i32), &ascending);
    return 0;
}
```

- 类型位置的 `*` 后面跟 `fn` 就按函数指针解析，和 `*T` 数据指针没有歧义
- **省略返回类型 = 无返回值**：和普通函数声明同一套规则（`fn f()` 就是 void）。
  C 的 `void (*)(int)` 直写成 `*fn(i32)`，`void (*)(void)` 是 `*fn()`，
  不再需要借一个假返回类型。带返回值的照常写 `*fn(i32): i32`。
  无返回值函数指针的调用只能作语句 —— 当值用会报
  `function 'x' returns no value and cannot be used in an expression`。
- `&f` 取函数地址，`fp(1, 2)` 间接调用，编译期校验参数个数和每个参数的类型
- 函数指针可以作参数、作返回值、作结构体字段、赋给变量、和 `null` 比较
- 调空函数指针有运行时守卫（`Null function pointer call`），不是裸 SIGSEGV

**实现位置**

| 环节 | 位置 |
|---|---|
| 类型表示（`is_function_pointer` + 参数链 + 返回类型） | `ast.h` 的 `VarTypeNode` |
| 语法 `*fn(...): T` | `parser.c` 的 `parse_function_pointer_type` |
| LLVM 类型 + 间接调用 + `&f` + null 守卫 | `codegen.c` 的 `llvm_function_pointer_signature` / `generate_indirect_call` / `generate_reference` |
| 类型等价与克隆 | `types.c` 的 `tap_type_equal` / `tap_type_clone` |
| 单态化时的替换与统一 | `generics.c` 的 `substitute_type` / `unify_type_params` |

**已知限制**

1. **调用点只认标识符**：`handler.run(7)`、`table[0](3)` 都调不了。
   原因是语法上 `name(...)` 只接受标识符，`a.b(...)` 一律当「类型 a 的方法 b」。
   前者报 `'handler.run' is a function pointer field; read it into a variable first`，
   后者是 `expected token ')'`。绕法都是先取临时变量：
   ```tap
   let run: *fn(i32): i32 = handler.run;
   run(7);
   ```
   字段和数组元素本身**能**存函数指针，只是不能就地调用。
2. **不做 String 的指针化 lowering**：回调签名里出现 `String` 时不保证和 C 的 ABI
   一致（extern 边界那套 `const String *` 只对直接调用生效）。要传字符串就在 C 侧包一层。
3. **不能解引用 / 下标**：函数指针不是数据指针，`*fp`、`fp[0]` 都报错。
4. **没有函数重载解析**：`&f` 只能指向唯一一个叫 `f` 的函数。

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

即使 P0 已经落地，胶水层仍然是**平台差异收敛**的正路：把第三方库包成 `__tap_xxx`
简单签名塞进 Runtime，tap 侧只声明 Runtime 函数。

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
| 胶水层 | 平台差异天然收敛；`String` 的 ABI 细节不泄漏到 .tp | 每个库都要写胶水；Runtime 会越来越胖 |
| `-l` 直连（P0） | 零胶水，能直接摸到整个 C API | 用户要自己处理 ABI 和平台差异 |

两者不冲突：能用胶水就用胶水，胶水覆盖不到的边角用 `-l` 直连。

## 5. 分阶段路线

| 阶段 | 内容 | 解锁面 | 风险 |
|---|---|---|---|
| 1 | **P0 链接通道**（§2）✅ | 打通「能链进来」，配合 `*T` + `p[0].field` 已能覆盖大部分库 | 低 |
| 2 | **extern 全局变量**（§3.2） | 配置结构体、版本查询、`errno` 类 | 低 |
| 3 | **函数指针**（§3.3）✅ | 所有回调式 API | 高 |
| 4 | 结构体 by-value ABI（§3.4） | 只在有明确需求时做 | 高 |

阶段 1 已完成，`tap run -lsqlite3 -L/usr/local/lib hello.tp` 是现在的最小可用闭环。

## 6. 验证方式

每一项都要有对应用例，且**必须在六个环境全绿**（macOS / Linux / MSVC / MINGW64 /
UCRT64 / Cygwin），因为 FFI 的坑几乎全部是平台 ABI 差异。

用例通过内嵌的 `- .flags` 段把编译选项传给 `tap run`（这个段会让用例强制走 native
模式），通过 `- .skip-on` 声明不适用的平台（报 `SKIP`，不算失败）。

| 阶段 | 用例 | 验证点 | 状态 |
|---|---|---|---|
| P0 | `tests/run-pass/extern/link_zlib.tp` | 链接一个真实第三方库并调用 | ✅ macOS / Linux |
| P0 | `tests/run-pass/extern/link_winmm.tp` | Windows 自带库（winmm）链接并调用 | ✅ Windows (UCRT64) |
| P0 | 同上 | 库顺序正确（`-l` 在 obj 之后） | ✅ |
| P0 | 同上 | `-l` 不存在时给出可读的错误，不是裸链接器报错 | ⚠️ 只手工验证 |
| P1 全局 | `tests/run-pass/extern/global.tp` | 读 + 写 extern 全局 | 未写 |
| P1 函数指针 | `tests/run-pass/extern/callback.tp` | 调 `qsort` 传 tap 函数 | ✅ |
| P1 函数指针 | `tests/run-pass/types/function_pointer.tp` | `&f` / 赋值 / 参数 / 返回值 / 字段 / `null` | ✅ |
| P1 函数指针 | `tests/compile-fail/types/function_pointer_signature.tp` | 签名不符要报错 | ✅ |
| P1 函数指针 | `tests/compile-fail/types/function_pointer_argument_count.tp` | 实参个数不符要报错 | ✅ |

用例沿用 `tests/run-tests.sh` 的 `/** -- test` 内联期望格式，
参考 `tests/run-pass/types/c_string.tp`。

## 7. 待确认

1. 是否要源码级指令（`@link("sqlite3")`）让包自带链接需求？这需要动 lexer / parser /
   AST，属于 P0 之后的独立阶段，但能让 `package.toml` 表达链接依赖。
2. ~~函数指针的语法最终形态~~ —— **已定：保持 `*fn(...)`**，不引入裸 `fn(...)`。
   理由：LLVM 的函数类型无大小，裸 `fn(...)` 一旦落到变量/参数/字段就必须退化成指针，
   表示上和 `*fn(...)` 完全相同，区分只在名义上，却要在等价规则、赋值转换、泛型统一
   和两份并行的类型推断里各加一套分支，换不到任何新程序。等将来真加闭包（需要
   函数指针 + 环境指针的胖指针）再拆，那时区分是被表示逼出来的。
   同期解决的是返回类型必填问题：省略 `: T` 即 void，见 §3.3。
