# 构建与安装

## 依赖

- **C 编译器**：GCC 或 Clang
- **LLVM**：**>= 21**（CMake 与 Makefile 均会链接 `libLLVM`）
- **构建**：GNU Make 或 CMake ≥ 3.10

## 构建

### Make（默认生成 `build/tap`）

```bash
make
```

需要 AddressSanitizer 调试时显式开启（切换编译选项前需清理旧目标文件）：

```bash
make clean
make SANITIZE=1
```

### CMake

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
# 可执行文件位于 build/tap（与 CMake 生成目录一致）
```

版本号由 C 生成器 `src/scripts/get_version.c` 从 git 生成 `src/version.h`，
细节见[源码结构](src.md#版本生成)。

### Windows 一键脚本

Windows 上不用手拼 `PATH` 和 `LIBS`，`scripts\` 下有两个封装脚本：

```bat
scripts\mingw-build.bat build | rebuild | test | test-native | install | package | clean
scripts\msvc-build.bat  build | rebuild | test | test-native | ctest | install | package | clean
```

详见[测试文档](test.md#windows-上的一键脚本)。

## 安装

两种构建方式都支持 `install`，装出来的布局一致：

```bash
make install                     # 默认装到 /usr/local，可能需要 sudo
make install PREFIX=$HOME/.local # 装到不需要权限的位置
```

CMake 构建则用 `cmake --install build`（可配 `--prefix`）。

Windows 下两个脚本都有 `install [目录]`，走同一套布局（先构建再复制，已存在的其他
文件不会被删）：

```bat
scripts\mingw-build.bat install                 :: 默认 %LOCALAPPDATA%\tap
scripts\mingw-build.bat install D:\dev\tap      :: 指定前缀
scripts\msvc-build.bat  install D:\dev\tap      :: MSVC 工具链，同一个布局
set PREFIX=D:\dev\tap && scripts\mingw-build.bat install
```

前缀按「命令行参数 → `PREFIX` 环境变量 → `%LOCALAPPDATA%\tap`」解析。装完把
`<prefix>\bin` 加入 `PATH`。与 `package` 不同，install 是开发安装：**不携带链接器和
compiler-rt builtins**，`tap run` 仍然需要 PATH 上的 `lld-link`（MinGW 是
`ld.lld`，也可以走 clang-cl 回退）。MSVC 那份唯一多带的是 `LLVM-C.dll`——`tap.exe`
的导入表里就有它，不放在 exe 旁边根本起不来。

装出来的结构：

```text
<prefix>/bin/tap                       编译器
<prefix>/bin/tap_runtime.dll           Windows：fast 模式给 lli 加载的 Runtime DLL
<prefix>/bin/LLVM-C.dll                仅 MSVC 安装：tap.exe 的硬依赖
<prefix>/lib/libtap_runtime.a          原生可执行文件链接的静态 Runtime（MSVC 叫 tap_runtime.lib）
<prefix>/lib/tap_runtime.dll           Unix 上共享 Runtime 落在 lib（Windows 放 bin）
<prefix>/include/tap/tap_runtime.h     Runtime 头文件
<prefix>/share/tap/std/*.tp            标准库
```

**这个布局不是随便定的**：编译器按「可执行文件所在目录 → `../lib`」找 Runtime，按
「当前目录的 `std` → `../std` → `../share/tap/std`」找标准库。装完直接就能用，
不需要设置 `TAP_STD_PATH` 或 `TAP_RUNTIME_PATH`。

`make uninstall` 只删 `install` 放进去的文件，不会动 `<prefix>` 下的其他内容。
打包做阶段性安装时用 `DESTDIR`：

```bash
make install DESTDIR=/tmp/stage PREFIX=/usr
```

## 打包成可分发目录

两个 Windows 脚本都有 `package`，各自产出 `dist\tap-<ver>-win64-<toolchain>\` + 同名 `.zip`。
两者共用同一套查找规则（见下），差别只在链接器和库的来源。目标机不用装 LLVM（包里自带连接器）。

### MSVC（`scripts\msvc-build.bat package`，约 132 MB）

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
  docs\*.md                README 里链到的文档（否则包内链接全是死的）
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

### MinGW（`scripts\mingw-build.bat package`，约 177 MB）

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
  docs\*.md                README 里链到的文档
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

冒烟验证：

```bat
cd dist\tap-0.1.0-win64-mingw
set PATH=C:\Windows\system32
bin\tap.exe --version
bin\tap.exe run ..\..\examples\hello.tp
bin\tap.exe run ..\..\tests\run-pass\operators\mixed_signedness.tp
```

### 为什么不用静态链接 tap

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

### 验证打包结果

解压后按上面的冒烟步骤跑一遍即可。想跑完整回归，直接把包里的 `bin\tap.exe` 当
`tap` 用：

```bash
TEST_JOBS=8 TEST_RUN_MODE=native bash tests/run-tests.sh dist/tap-0.1.0-win64-mingw/bin/tap.exe
```

Windows 上必须用 `TEST_RUN_MODE=native`（`lli` 不可靠），并注意用 Git Bash 跑 ——
MSYS2 的 bash 会把 `TMPDIR=/tmp` 传给原生 `tap.exe`，导致 `failed to create temporary
directory: Windows error 5`。测试细节见[测试文档](test.md)。

