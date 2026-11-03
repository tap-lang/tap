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

### 打包成可分发目录

两个 bat 都有 `package`，各自产出 `dist\tap-<ver>-win64-<toolchain>\` + 同名 `.zip`。
两者共用同一套查找规则（见下），差别只在链接器和库的来源。

#### MSVC（`scripts\msvc-build.bat package`，约 132 MB）

```
dist\tap-0.1.0-win64-msvc\
  bin\tap.exe              编译器，需要同目录的 LLVM-C.dll
  bin\LLVM-C.dll
  bin\vcruntime140.dll     VC++ 运行时（目标机装了 VC++ 2015-2022 x64 可以删掉）
  bin\lld-link.exe         链接器，tap run 直接用它（66 MB，静态链 LLVM）
  lib\tap_runtime.lib      每个 tap 程序都会链进去的 Runtime 静态库
  lib\tap_runtime.dll      lli 加载的 Runtime DLL
  lib\compiler-rt\clang_rt.builtins-x86_64.lib   i128 除法要的 __divti3 等
  share\tap\std\*.tp       标准库
  LICENSE  README.md  PACKAGE.txt
dist\tap-0.1.0-win64-msvc.zip
```

这个布局是被 `src/prelude.c` 的查找顺序决定的：`<exe>/../std/prelude.tp`、
`<exe>/../share/tap/std/prelude.tp`，所以 **不要把 `bin\tap.exe` 单独拷出去**。
`TAP_STD_PATH` 和 `TAP_RUNTIME_PATH` 可以覆盖这两条路径。

链接器和 builtins 由 `src/run.c` 按 `tap.exe` 自身位置查找，优先级高于 `PATH`：

```
lld-link.exe            → 先找 <exe>/lld-link.exe，再找 PATH
clang_rt.builtins-*.lib → 先找 <exe>/../lib/compiler-rt/，再从 lld-link 反推 LLVM 目录
```

所以**目标机不用装 LLVM**。实测把 `PATH` 压到只剩 `C:\Windows\system32`，
`tap -v run hello.tp` 的链接行是：

```
"...\bin/lld-link.exe" /nologo /machine:x64 /subsystem:console
  "/out:...\program.exe" "...\program.o"
  "...\bin/../lib/tap_runtime.lib" ws2_32.lib
  "...\lib\compiler-rt\clang_rt.builtins-x86_64.lib"
```

把 `lld-link.exe` 挪走，tap 会去 `PATH` 找 lld-link、再退到 `clang-cl`；都没有就报：

```
error: no linker toolchain found (tried: lld-link, clang-cl)
note: tap needs a linker toolchain: install LLVM (provides lld-link) or the MSVC
      build tools, then put its bin directory on PATH
note: run with -v to see the full link command
```

**包里没有微软的 CRT / SDK 导入库**（`libcmt.lib`、`libucrt.lib`、`ws2_32.lib` 等），
它们不可再分发，所以只留了 `lld-link.exe`（Apache-2.0 WITH LLVM-exception）和
`clang_rt.builtins`（同样可分发）。代价是**目标机仍需装 MSVC 生成工具或 Windows SDK**——
不过不需要 `vcvars`：`lld-link` 自己按注册表 / `vswhere` 定位那些 `lib` 目录，实测
`PATH` 只剩 `C:\Windows\system32`、`LIB` 清空也能链接。

冒烟验证：

```bat
cd dist\tap-0.1.0-win64-msvc
set PATH=C:\Windows\system32
bin\tap.exe --version
bin\tap.exe run ..\..\examples\hello.tp
```

`--version` 能起说明 `LLVM-C.dll` 就位；`run` 能出结果说明连接器、CRT 库、`lib\` 和
`share\tap\std\` 都按自身位置找到了。想再确认 i128，跑
`bin\tap.exe run ..\..\tests\run-pass\operators\mixed_signedness.tp`（`__divti3` 只能
来自 `lib\compiler-rt\`）。

#### MinGW（`scripts\mingw-build.bat package`，约 177 MB）

```
dist\tap-0.1.0-win64-mingw\
  bin\tap.exe
  bin\ld.lld.exe          链接器
  bin\*.dll               自动收集：libLLVM-<ver>(约 146 MB)、libstdc++-6、
                          libgcc_s_seh-1、libwinpthread-1、libxml2-16、libffi-8、
                          libiconv-2、libzstd、zlib1
  lib\libtap_runtime.a     Runtime 静态库
  lib\tap_runtime.dll      lli 加载的 Runtime DLL
  lib\crt2.o crtbegin.o crtend.o default-manifest.o
  lib\libws2_32.a libmingw32.a libmingwex.a libmsvcrt.a libkernel32.a
      libpthread.a libadvapi32.a libshell32.a libuser32.a libucrt.a libucrtbase.a
  share\tap\std\*.tp
  LICENSE  README.md  PACKAGE.txt
dist\tap-0.1.0-win64-mingw.zip
```

和 MSVC 包的关键区别：**这个包是真正自包含的**。mingw-w64 的 CRT 目标文件和导入库是
public-domain/BSD，`ld.lld` 和 LLVM 是 Apache-2.0 WITH LLVM-exception，都可以随包分发，
所以 `lib\` 里直接带上 `crt2.o` 和那些 `.a`。目标机什么都不用装。

`bin\` 里的 DLL 由 `scripts/collect-dlls.sh` **递归解析导入表自动收集**，不靠手工清单：
`libLLVM` 拖 `libxml2`/`libffi`/`libiconv`/`libzstd`，`libstdc++` 又拖 `libgcc_s_seh`，
漏一个的表现是 `--version` 什么都不打印、退出码 127。脚本的判断规则是「MSYS2 的 `bin`
里有同名文件才算外部依赖，没有的一律当系统 DLL 跳过」，所以不需要维护系统 DLL 白名单。

`run.c` 的 MinGW 分支按 `<exe>\ld.lld.exe` 找链接器，再按 `<exe>\..\lib` 找 CRT 目录 ——
正好对上这个布局，所以 `lib\` 里同时放 CRT 和 Runtime 就行。实测 `PATH` 只剩
`C:\Windows\system32` 时的链接行：

```
"...\bin/ld.lld.exe" -m i386pep -Bdynamic -o "...\program.exe"
  "...\lib/crt2.o" "...\lib/crtbegin.o" "-L...\lib" "...\program.o"
  "...\bin/../lib/libtap_runtime.a"
  -lws2_32 -lmingw32 -lmingwex -lmsvcrt -lkernel32 -lpthread -ladvapi32 -lshell32 -luser32
  "...\lib/default-manifest.o" "...\lib/crtend.o"
```

`lib\` 里还带了 40 个常用 Windows 系统导入库（`winmm`、`gdi32`、`ole32` 等，共 8.2 MB），
所以 `-lwinmm` 这类链接开箱可用。

#### 为什么不用静态链接 tap

试过了，不行。MSYS2 的 LLVM 带全套静态库（`libLLVMCore.a` 等），
`g++ -static` 能链出一个 196 MB 的单文件，`objdump -p` 确认零非系统依赖，
`--version` 也正常 —— 但一进 `run` 就卡死，内存涨到 4 GB 以上。

gdb 附加看到的栈：

```
#3  llvm::ValueSymbolTable::makeUniqueName
#4  llvm::ValueSymbolTable::createValueName
#5  llvm::Value::setNameImpl
#7  LLVMSetValueName2
#8  generate_code at src/codegen.c:5540
```

给函数参数命名时 LLVM 的符号表去重逻辑陷入了死循环。换成
`-static-libgcc -static-libstdc++`（winpthread 保持动态）也一样卡，说明和 winpthread
是否静态无关，是静态链接 LLVM 本身在这个组合下的病理行为。再加上 196 MB 比 177 MB
的动态包还大，就没有继续深挖的价值 —— 自动收集 DLL 已经解决了「不用手工整理」的问题。

冒烟验证：

```bat
cd dist\tap-0.1.0-win64-mingw
set PATH=C:\Windows\system32
bin\tap.exe --version
bin\tap.exe run ..\..\examples\hello.tp
bin\tap.exe run ..\..\tests\run-pass\operators\mixed_signedness.tp
```

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
