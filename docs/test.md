# 测试

测试按预期行为分类，测试脚本会自动发现所有 `.tp` 文件：

- `run-pass/`：编译和运行均成功。
- `run-fail/`：编译成功，但程序返回预期的非零退出码。
- `compile-fail/`：编译必须按预期失败。
- `fixtures/`：模块等测试依赖，不会作为测试用例直接执行。
- `llvm/`：LLVM C API 实验代码，不属于语言回归测试。

测试期望可以直接写在 `.tp` 文件的多行注释中：

```text
/** -- test
- .stdout
预期标准输出
- .stderr
预期标准错误
- .args
传给被测程序的第一个参数
传给被测程序的第二个参数
- .flags
-lz
- .skip-on
windows
- .exit
0
-- */
```

- 每个用例必须提供 `.exit`。
- `.stdout` 和 `.stderr` 可省略；省略时对应输出必须为空。
- `.args` 可省略；提供时仅用于 `run-pass` 和 `run-fail`，每行作为一个独立程序参数，并通过 `--` 传给 `tap run` 生成的程序。
- `.flags` 可省略；每行作为一个独立的编译器参数，插在源文件**之前**（例如 `-lz`、`-L/usr/local/lib`、`-Wl,-rpath,/x`）。提供时会强制该用例走 native 模式 —— `-run-lli` 不链接，带 `-l` 的用例在 fast 模式下没有意义。
- `.skip-on` 可省略；每行一个平台名，命中就整条跳过并打印 `SKIP`，不计入通过也不计入失败。平台名是粗粒度的 `darwin` / `linux` / `windows`（MSVC、MinGW、Cygwin 都算 `windows`），用于「这个库某些平台本来就没装」的用例。
- `compile-fail` 必须提供 `.stderr`，其中每个非空行都必须出现在诊断中。
- 文本比较忽略 CRLF 差异和每行尾部空白，保留行首缩进。内嵌期望和外部期望文件都会使用相同规则归一化。

带源码位置的编译诊断统一使用消息在前、位置在后的两行格式：

```text
error: 诊断消息
--> 文件:行:列
   1  上文
-> 2  出错代码
   3  下文
```

默认展示错误行上下各三行的代码上下文；文件开头或结尾不足三行时，只展示实际存在的
行。补充说明使用相同结构，将第一行的 `error` 替换为 `note`。

如果错误输出较长，也可以把期望写到同名外部文件中：

```text
tests/compile-fail/lexer/unterminated_block_comment.tp
tests/compile-fail/lexer/unterminated_block_comment.stderr
```

测试脚本读取期望的优先级为：

1. `.tp` 文件中内嵌的 `- .stdout` / `- .stderr` / `- .exit`。
2. 去掉源文件扩展名后的外部文件，例如 `case.stderr`。

当前 `.exit` 仍必须写在 `.tp` 文件的内嵌测试块中。

```sh
make test
make test-native
make test TEST_JOBS=8
make testc
make testc-native
TEST_FILTER=module make test
./tests/run-tests.sh ./build/tap
./tests/run-tests.sh ./build/tap hello
```

`make test` 默认使用 4 个并行 worker。普通 `run-pass`、`run-fail` 用例通过 `lli` 执行，
带 `.args` 或 `.flags` 的用例和 `tests/run-pass/basics/hello.tp` 原生编译并链接，
持续覆盖程序参数和链接路径。
`make test-native` 会让全部运行用例生成原生可执行文件。可以用 `TEST_JOBS=N` 调整并行度，
或设为 `1` 排查依赖执行顺序的问题；最终结果始终按文件名顺序输出。
`testc` 和 `testc-native` 使用 `src/scripts/run-tests.c`，由 C 运行器负责测试发现、
期望值解析、并发调度、进度显示和结果汇总，分别对应 fast 与 native 模式。

`TEST_RUN_MODE` 环境变量可以直接选择运行方式（`fast` 走 `lli`，`native` 生成原生可执行文件），
不传时默认为 `fast`。

### 命令行冒烟测试

`tests/run-tests.sh` 只验证「编译一个 `.tp` 文件、比对 stdout/stderr/退出码」，**覆盖不到
CLI 本身** —— `tap check` 会不会留下文件、包模式会不会建 `build/`、互斥选项有没有报错，
这些都测不到。这部分由 `scripts/smoke-cli.sh` 负责：

```sh
make test-cli
sh scripts/smoke-cli.sh ./build/tap        # 也可以直接给可执行文件路径
```

它跑 22 项检查：`check` 通过时静默返回 0、不产出文件、有错时返回 1 且诊断带行列号、
`check` 与七种输出/执行选项互斥、`check` 与 `-o` 互斥、只允许一个命令、包模式检查不创建
`build/`、子目录里能向上找到 `package.toml`、缺清单时报错、`run` 的回归，以及 `-g` 的
四项（IR 里带 `!dbg`、不带 `-g` 时没有、生成了 `DICompileUnit`、与 `-emit-wasm` 互斥）。

注意脚本会把传入的可执行文件路径转成绝对路径 —— 每条用例都在临时目录里执行，相对路径
（`./build/tap`）换个工作目录就找不到，而「找不到」恰好是非 0 退出，互斥类用例会假通过。

CMake 构建下它注册成 ctest 用例 `cli-smoke-tests`，和 `language-tests` 并列。

### Windows 上的一键脚本

`scripts\` 下有两个 bat，各自封装一条工具链，省得手拼 PATH 和 `LIBS`：

```bat
scripts\mingw-build.bat build | rebuild | test | test-native | package | clean
scripts\msvc-build.bat  build | rebuild | test | test-native | ctest | package | clean
```

- `scripts\mingw-build.bat`：MSYS2 + GCC，走 Makefile。会从 `libLLVM-<ver>.dll.a` 自动读出
  LLVM 主版本（Makefile 里写死的是 `-lLLVM-21`），并用 MSYS2 自己的 `sh` 跑测试 ——
  从 cmd 继承的 sh 会把 `TMPDIR` 拼成 `C:Usersttt...` 这种坏路径。
- `scripts\msvc-build.bat`：MSVC + cl.exe，走 CMake + Ninja，产物在 `out\build\x64-Release`。
- 两个脚本都会先 `cd` 到项目根（`%~dp0..`），所以从哪个目录调用都行。
- 两边都推荐 `test-native`：Windows 上 `lli` 不可靠，见下面两节。
- 可选环境变量：`MSYS_ENV=mingw64` 切换 MSYS2 环境、`TAP_LLVM_VERSION=23` 强制 LLVM
  主版本、`TEST_JOBS=N` 调整并行度（默认 8）。

`package` 打出的分发包（布局、体积、许可边界、冒烟步骤）见
[构建与安装](build.md#打包成可分发目录)。

### Cygwin 下必须用 native 模式

Cygwin 的 `lli` 有两个限制，导致 `fast` 模式固定有 5 个用例失败：

- **JIT 代码里的 `exit()` 不传递退出码**。用最小 IR 验证：`call void @exit(i32 1)` 后
  `lli` 仍返回 0，而 `ret i32 1` 正常返回 1；同一份 IR 原生编译则返回 1。
  这会让 4 个 `run-fail` 用例的 `.exit` 期望无法满足。
- **栈帧超过 4KB 时解析不到 `___chkstk_ms`**。Cygwin 的目标 triple 是
  `x86_64-pc-windows-cygnus`，LLVM 会为 Windows ABI 插入栈探测调用，
  而 Cygwin 的 `lli` 里没有这个符号。`tests/run-pass/stdlib/env_var.tp`
  的 `[String; 256]`（8KB 栈数组）就会触发。

这两个问题都出自 `lli`，无法在 tap 侧规避，因此 `.github/workflows/cygwin.yml`
固定使用 `TEST_RUN_MODE=native`。

### MinGW-w64 也走 native 模式

`.github/workflows/MinGW-w64.yml` 同样固定 `TEST_RUN_MODE=native`：MinGW 的 `lli` 依赖
MSYS2 运行时的 JIT 环境，行为不稳定；直接生成真正的 `.exe` 再跑更接近用户实际场景，
也能顺带验证链接路径（`-lws2_32`、Runtime 自带的 `__divti3` / `__main` / `___chkstk_ms`，
即不依赖 libgcc 的那条路）。
