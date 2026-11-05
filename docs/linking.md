# 各平台链接命令

本文整理 tap 在六个环境下**实际使用**的链接命令，内容以
[`src/run.c`](../src/run.c) 的 `link_object_file()`、[`Makefile`](../Makefile)、
[`CMakeLists.txt`](../CMakeLists.txt) 和 `.github/workflows/` 为准。

## 三个不同的链接环节

容易混，先分清：

| 环节 | 谁做 | 产物 |
|---|---|---|
| ① 构建 tap 编译器本身 | Makefile / CMake | `build/tap`，链 LLVM |
| ② 构建 Runtime 库 | Makefile / CMake | `libtap_runtime.a`（+ `.dll`/`.so`/`.dylib`） |
| ③ `tap run` / `tap build` | `run.c` 调外部链接器 | 用户程序的可执行文件 |

下面重点是 ③，因为它是唯一按平台分叉、且坑最多的。

**要看你这一份 tap 到底跑了什么命令，加 `-v`**（`--verbose` 同义）：

```console
$ tap -v -lz hello.tp
tap: object: /var/folders/…/tap-XJzyfn/program.o (emitted in-process, no external command)
tap: link: ld -o hello /var/folders/…/tap-XJzyfn/program.o …/build/libtap_runtime.a -lz -lSystem -syslibroot … -arch arm64 -platform_version macos 15.5 15.5
tap: keeping temporary files in /var/folders/…/tap-XJzyfn
```

三行分别对应：目标文件（由 LLVM 在本进程里直接写出，**没有外部编译命令**）、
链接命令、保留的临时目录。`-run-lli` 下打的是 `tap: lli: …`，`tap run` 还会多一行
`tap: run: …` 表示最终执行的可执行文件。

几个刻意的设计：

- **走 stderr**，所以 `tap run -v prog.tp > out.txt` 的 `out.txt` 里只有程序自己的输出。
- **命令带引号、可直接复制粘贴**（POSIX 用单引号，Windows 用双引号）。参数里的
  空格、单引号都按各自 shell 的规则转义过。
- **`-v` 下不删临时目录**。回显的链接命令里带着目标文件路径，删掉之后那条命令就没法
  照着跑一遍 —— 而照着跑一遍正是 `-v` 的主要用途。目录路径会单独打一行。

## 对照表

`run.c` 用编译期宏选分支，顺序是：

```c
#if defined(_WIN32) && defined(__MINGW32__)      // MinGW-w64：MINGW64 / UCRT64
#elif defined(_WIN32)                             // MSVC
#elif defined(__APPLE__)                          // macOS
#else
#  if defined(__linux__) && !defined(__CYGWIN__) // Linux：优先直连 ld.lld，缺失时回退 cc
#  elif defined(__CYGWIN__)                      // Cygwin：优先直连 ld.lld（COFF），缺失时回退 cc
#  else                                           // 其他 Unix
#  endif
#endif
```

| 环境 | 链接器 | Runtime 库 | 关键标志 |
|---|---|---|---|
| macOS | `ld`（直接调，不经 cc） | `libtap_runtime.a` | `-lSystem`、`-syslibroot`、`-arch`、`-platform_version` |
| Linux | `ld.lld`（**优先直连，不经 cc**）；PATH 上无 `ld.lld` 或 CRT 探测失败时回退 `cc` 默认链接器 | `libtap_runtime.a` | 直连：`-dynamic-linker`、显式 CRT、`-lm`、`-lgcc` / `-lc`；回退：`-no-pie`、`-lm` |
| MSVC | `lld-link`（直接调，找不到时退回 `clang-cl`） | `tap_runtime.lib` | `/machine:x64`、`/subsystem:console`、`/out:`、`ws2_32.lib`、`clang_rt.builtins-*.lib` |
| MINGW64 | `ld.lld`（找不到时退回 `gcc`） | `libtap_runtime.a` | `-m i386pep`、显式 `crt2.o`/`crtend.o`、`-lws2_32`（**不加 `-lm`、不加 `-lgcc`**） |
| UCRT64 | 同上 | `libtap_runtime.a` | 同上，命令完全一致 |
| Cygwin | `ld.lld`（**优先直连**，走 COFF 模式；找不到时回退 `cc`） | `libtap_runtime.a` | 直连：`-m i386pep`、`--tsaware`、显式 CRT、`libgcc.a` / `libc.a` / `libcygwin.a` / `libm.a`；回退：`-lm`（**不加 `-rtlib=`、不加 `-no-pie`**） |

## 逐平台命令

### macOS

```sh
ld -o prog prog.o libtap_runtime.a \
   -lSystem \
   -syslibroot "$(xcrun --sdk macosx --show-sdk-path)" \
   -arch arm64 \
   -platform_version macos "$(xcrun --sdk macosx --show-sdk-version)" \
                      "$(xcrun --sdk macosx --show-sdk-version)"
```

- **直接调 `ld` 而不是 `cc`**：必须显式给 SDK 路径、架构和平台版本。
- `libm` 在 libSystem 里，不需要 `-lm`。
- `-static` 不支持（libSystem 只有动态库）。
- 批量链接时 `xcrun` 每次启动都很慢，所以测试脚本会预先查好并存进
  `TAP_MACOS_SDK_PATH` / `TAP_MACOS_SDK_VERSION`，`run.c` 优先读这两个环境变量。
- `-arch` 由编译 tap 时的架构决定：`__aarch64__` → `arm64`，`__x86_64__` → `x86_64`。

### Linux

**优先直接调 `ld.lld`，链接行里没有 `cc`**；工具链不齐时自动回退 `cc` 驱动。
直连时驱动平时偷偷补上的那一套由 `run.c` 自己探测后显式给出：

```sh
ld.lld -o prog \
  --build-id --eh-frame-hdr --hash-style=gnu \
  -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
  <crt_dir>/crt1.o <crt_dir>/crti.o <gcc_dir>/crtbegin.o \
  -L<crt_dir> -L<gcc_dir> -L/usr/lib -L/lib -L/usr/lib64 -L/lib64 \
  prog.o libtap_runtime.a -lm \
  -lgcc --as-needed -lgcc_s --no-as-needed -lc \
  -lgcc --as-needed -lgcc_s --no-as-needed \
  <gcc_dir>/crtend.o <crt_dir>/crtn.o

# -static：换成 -static + crtbeginT.o + 静态库组，并且不给 -dynamic-linker
ld.lld -o prog -static --build-id --eh-frame-hdr --hash-style=gnu \
  <crt_dir>/crt1.o <crt_dir>/crti.o <gcc_dir>/crtbeginT.o \
  -L<crt_dir> -L<gcc_dir> -L/usr/lib -L/lib ... \
  prog.o libtap_runtime.a -lm \
  --start-group -lgcc -lgcc_eh -lc --end-group \
  <gcc_dir>/crtend.o <crt_dir>/crtn.o
```

找不到 `ld.lld` / CRT 时回退 `cc` 驱动（行为和引入直连之前一致）：

```sh
cc -no-pie prog.o libtap_runtime.a -o prog -lm
cc -static -no-pie prog.o libtap_runtime.a -o prog -lm   # -static
```

- **为什么能不经过 cc**：`cc` 只是拼参数的驱动。用 `cc -###` 看过它交给 collect2 的那一串
  （CRT 启动文件、PT_INTERP、`-lc` / `-lgcc`、一串 `-L`）之后，把这些参数显式交给 `ld.lld`
  即可。`-fuse-ld=lld` 是「换掉驱动调用的链接器」，这里是「连驱动一起去掉」。
- **路径怎么来**：`find_linux_link_tools()` 按优先级探测 —— `ld.lld` 走 PATH；CRT 目录先
  glob `/usr/lib/*-linux-gnu/crt1.o`（Debian/Ubuntu multiarch），再退 `/usr/lib64` / `/usr/lib`；
  GCC 目录 glob `/usr/lib/gcc/*/*/crtbegin.o`（要求同目录有 `crtend.o` 和 `libgcc.a`）；
  PT_INTERP 按架构宏给候选（x86_64 → `/lib64/ld-linux-x86-64.so.2` 等），找不到再 glob
  `ld-linux*.so.*`。不写死某一台机器的路径，也不需要 `cc` 在场。
- **前提条件与回退**：直连要求机器上有 `ld.lld`（`lld` 包）和 C 开发文件（CRT 目标文件、
  `libgcc`、`libc`），且动态链接器能被探测到。`find_linux_link_tools()` 失败时 —— 没装
  `lld` 包，或 musl 发行版的 CRT / PT_INTERP 不在已知路径 —— **自动回退 `cc -no-pie`**，
  由驱动自己补齐 CRT、PT_INTERP 和默认库，链接行回到引入直连之前的写法；`-v` 下会多打
  一行 `tap: note: ... falling back to the cc driver`。只有连 `cc` 都找不到时才报错并提示
  安装。所以没装 lld 的 Linux 机器（含 CI 的 Ubuntu）仍能生成可执行文件，实际链接器是
  驱动默认的 GNU ld。探测在运行期做，同一份二进制在两台机器上会走不同分支。
- **`-lm` 必须放在目标文件和静态库之后**：glibc 2.34 起 libm 并入 libc，但更早的发行版
  仍需显式链接，否则 Runtime 里的 `pow` 报未定义符号。
- **`-no-pie`**：直连 `ld.lld` 不需要（默认产出非 PIE 的 ET_EXEC，正好匹配 IR 里的绝对
  寻址）；回退 `cc` 驱动时必须加。
- **`-Wl,` 两种写法**：直连链接器不认驱动前缀，`-Wl,-rpath,/x` 变成 `-rpath /x` 两个参数
  （`push_link_options_direct_ld()`），参数必须是 LLD 认识的；回退 `cc` 时用
  `push_link_options_driver()` 原样透传 `-Wl,-rpath,/x`，参数得是 GNU ld 认识的。
  **同一个 `-Wl,` 参数在两条路径下不一定都有效**，写之前先 `tap -v` 看链接行首是
  `ld.lld` 还是 `cc`。
- 实测（Arch / GCC 16.2.1 / LLD 23.1.4）：`hello.tp`、`mixed_signedness.tp`（i128）、
  `link_zlib.tp -lz`、`-static`、`-Wl,-rpath,/tmp` 在两条路径下都通过；直连时 `tap -v`
  回显的链接行首是 `/usr/bin/ld.lld`，把 PATH 里的 `ld.lld` 屏蔽后行首变成 `cc`。

其他 Unix（非 Linux、非 Cygwin、非 macOS）仍走 `cc -no-pie ... -lm` 驱动路径。

### MSVC

```bat
:: 首选：直接调 lld-link（不需要 clang-cl）
lld-link /nologo /machine:x64 /subsystem:console /out:prog.exe ^
    prog.obj tap_runtime.lib ws2_32.lib ^
    "<LLVM>\lib\clang\<ver>\lib\windows\clang_rt.builtins-x86_64.lib"

:: 回退：找不到 lld-link 或它配套的 builtins 时仍用 clang-cl
clang-cl /nologo /MT /clang:-rtlib=compiler-rt prog.obj tap_runtime.lib /Feprog.exe ws2_32.lib
```

- **CRT 不在命令行里**：`tap_runtime.lib` 的 `.drectve` 段带 `/DEFAULTLIB:LIBCMT` 和
  `/DEFAULTLIB:OLDNAMES`，lld-link 会自动加。所以 `/MT`（libcmt）和 `/MTd`（libcmtd）
  都不用写死 —— Debug 构建也对。CMake 里 Runtime 是 `MSVC_RUNTIME_LIBRARY=MultiThreaded`。
- `/subsystem:console`：不指定 `/entry` 时，lld-link 按子系统去默认库里找
  `mainCRTStartup`（在 libcmt 里），和 link.exe 的规则一致。
- `clang_rt.builtins-<arch>.lib`：i128 除法/取模的 `__divti3` / `__udivti3` / `__modti3` /
  `__umodti3` 只能来自 compiler-rt —— cl.exe 不支持 `__int128`，Runtime 编不出这组助手。
  路径随 LLVM 版本变，`run.c` 从 `lld-link.exe` 所在目录反推
  `<llvm>/lib/clang/<ver>/lib/windows/` 再遍历版本目录，取第一个存在的。
- `ws2_32.lib`：Runtime 里的 winsock（`WSAStartup` / `socket` / `closesocket`）。
- 静态/动态由 `/MT` 与 `/MD` 区分，没有 `-static` 这个开关。
- `-l foo` 在直连模式下翻译成 `foo.lib`（已有 `.lib` 后缀不重复加），`-L dir` 翻成
  `/LIBPATH:dir`，裸参数（`-Wl,...`）原样给。

#### 回退条件

PATH 上找不到 `lld-link.exe`，**或者**从它所在目录反推不到
`clang_rt.builtins-*.lib`（例如 PATH 上只有 MSYS2 自带的 lld-link、没装 clang 包），
就退回 `clang-cl`。这条退路里如果还能找到 `lld-link.exe`，会再加 `-fuse-ld=lld`，
让 clang-cl 也走 lld 而不是 MSVC 的 `link.exe`。

#### 实测（VS 2022 14.44.35207 + LLVM 21.1.1）

- `tap -v` 回显的链接行就是上面那条，里面没有 clang-cl
- 把 `clang-cl.exe` 改名后，`hello.tp` 和 `mixed_signedness.tp`（i128 除法）都能链接并运行
- 把 `lld-link.exe` 改名后，自动退回 clang-cl，同样正常
- `scripts\msvc-build.bat test-native`：240 用例 239 通过 / 1 跳过 / 0 失败
- **不需要 `vcvars`**：把 `PATH` 压到只剩 `C:\Windows\system32`、`LIB` 清空，`lld-link`
  仍然找得到 `libcmt.lib`（只在 MSVC 安装目录里）和 `ws2_32.lib`（只在 SDK 里）——
  它自己按注册表 / `vswhere` 定位这两处 `lib` 目录
- **分发包带连接器**：`scripts\msvc-build.bat package` 把 `lld-link.exe` 和
  `lib\compiler-rt\clang_rt.builtins-*.lib` 一起打进去，`run.c` 优先用
  `<exe>/lld-link.exe`，所以目标机不用装 LLVM。微软的 CRT / SDK 导入库**不打包**
  （不可再分发），仍靠 lld-link 自己查注册表

### MinGW-w64：MINGW64 / UCRT64

```sh
# 首选：直接调 ld.lld（不需要 gcc，也不需要 libgcc）
ld.lld -m i386pep -Bdynamic -o prog.exe \
    <root>/lib/crt2.o <root>/lib/crtbegin.o -L<root>/lib \
    prog.o libtap_runtime.a -lws2_32 \
    -lmingw32 -lmingwex -lmsvcrt -lkernel32 -lpthread -ladvapi32 -lshell32 -luser32 \
    <root>/lib/default-manifest.o <root>/lib/crtend.o

# 回退：PATH 上找不到 ld.lld 时仍用 gcc 驱动
gcc prog.o libtap_runtime.a -o prog.exe -lws2_32
gcc -static prog.o libtap_runtime.a -o prog.exe -lws2_32   # --static
```

**两个环境的命令完全相同**，差别在工具链和 CRT：

| | MINGW64 | UCRT64 |
|---|---|---|
| 目标三元组 | `x86_64-w64-windows-gnu` | 同 |
| CRT | msvcrt | UCRT |
| MSYS2 包前缀 | `mingw-w64-x86_64-*` | `mingw-w64-ucrt-x86_64-*` |
| PATH | `/mingw64/bin` | `/ucrt64/bin` |

- **不能 `clang-cl`**：它按 MSVC 目标去找 MSVC 的 CRT 和 `.lib`，和 MinGW 的 msvcrt/UCRT 体系对不上。
- **不加 `-lm`**：MinGW 没有独立的 libm，`pow` 在 CRT 里。
- **不加 `-lgcc`**：链接行里一个 libgcc 符号都不需要，见下面「为什么可以不要 libgcc」。

#### 为什么可以不要 libgcc

gcc 驱动本身不干活，只是把参数拼给 ld，顺带把 `-lgcc` 塞进链接行。实测 MinGW 下真正用到
libgcc 的只有 4 个符号，全部由 Runtime 自己提供（`runtime/src/runtime.c`）：

| 符号 | 谁引用 | Runtime 里的来源 |
|---|---|---|
| `__main` | `crt2.o`（无条件引用） | `void __main(void) {}` 空实现，MinGW 下它本来就该是空的 |
| `___chkstk_ms` | LLVM 给 >4KB 栈帧插的探测调用 | x86_64 内联汇编，按 Win64 ABI 逐页 touch |
| `__divti3` / `__udivti3` / `__modti3` / `__umodti3` | i128 除法/取模的 libcall | 早已自带（约定和 LLVM 后端匹配，见「坑」第 1 条） |

`-lgcc_eh` 实测也不需要（tap 生成的代码没有 C++ 异常表）。

代价是链接器要自己找 CRT：`crt2.o`、`crtbegin.o`、`crtend.o`、`default-manifest.o` 和
`libmingw32.a` 这些都在 sysroot 的 `lib` 目录里，但不同发行版相对 `bin` 的位置不一样
（MSYS2 是 `<prefix>/lib`，w64devkit 是 `<prefix>/x86_64-w64-mingw32/lib`）。
`run.c` 从 `ld.lld` 和 `gcc` 各自的 `bin` 目录反推，探测到 `crt2.o` 就用该目录。

判定顺序：PATH 上有 `ld.lld.exe` 且能找到 CRT 目录 → 走 lld；否则整条退回 gcc，
行为和以前完全一致。所以「装了 gcc 但没装 LLVM 工具」的环境不会因为这次改动而变坏。

实测：把 `gcc` 换成一个必然失败的同名程序后，`TEST_RUN_MODE=native` 全量 240 个用例
仍然 239 通过 / 1 跳过 / 0 失败 —— 说明链接路径确实不碰 gcc。

#### 分发包

`scripts\mingw-build.bat package` 把 `ld.lld.exe` 放进 `bin\`、CRT 目标文件和导入库放进
`lib\`，`run.c` 先找 `<exe>\ld.lld.exe`（再退 PATH），CRT 目录按 `<exe>\..\lib`
反推 —— 正好命中这个布局，不需要改探测逻辑。

和 MSVC 包不同，这个包**能真正自包含**：mingw-w64 的 CRT 和导入库是可再分发的，
所以 `lib\` 里直接带 `crt2.o`、`libmingw32.a`、`libmsvcrt.a` 等。代价是
`libLLVM-<ver>.dll` 有 146 MB，还要连带 `libxml2` / `libffi` / `libiconv` /
`libzstd` / `zlib` / `libstdc++` / `libgcc_s_seh-1` 这一串 DLL，整个包约 177 MB。
包内完整布局和冒烟步骤见[构建与安装](build.md#打包成可分发目录)。

### Cygwin

```sh
# 首选：直接调 ld.lld（Cygwin 打包的 LLD，自带 COFF 模式）
ld.lld -m i386pep --tsaware -o prog.exe \
    /usr/lib/crt0.o /usr/lib/gcc/x86_64-pc-cygwin/<ver>/crtbegin.o \
    -L/usr/lib/gcc/x86_64-pc-cygwin/<ver> -L/usr/lib -L/usr/lib/w32api \
    prog.o libtap_runtime.a \
    --disable-high-entropy-va --disable-nxcompat \
    /usr/lib/gcc/x86_64-pc-cygwin/<ver>/libgcc.a \
    /usr/lib/gcc/x86_64-pc-cygwin/<ver>/libgcc_s.dll.a \
    /usr/lib/libc.a /usr/lib/libcygwin.a /usr/lib/libm.a \
    /usr/lib/w32api/libadvapi32.a /usr/lib/w32api/libshell32.a \
    /usr/lib/w32api/libuser32.a /usr/lib/w32api/libkernel32.a \
    /usr/lib/gcc/x86_64-pc-cygwin/<ver>/crtend.o

# 回退：PATH 上没有 ld.lld，或 CRT 不在已知位置时用 cc 驱动
cc prog.o libtap_runtime.a -o prog.exe -lm
cc -static prog.o libtap_runtime.a -o prog.exe -lm   # --static
```

- **为什么能直连**：Cygwin 的目标是 PE/COFF，Cygwin 打包的 LLD 自带 COFF 模式
  （`--subsystem` / `--Xlink` / `--tsaware` 这些都是 COFF 专有选项）。
- **库名要展开成完整路径**：LLD 的 COFF 模式只认 `.lib`、`lib*.dll.a` 和直接给的
  `.a` 输入文件，**不认 `-lfoo`** —— 而 Cygwin 的系统库全是裸 `.a`
  （`libc.a` / `libcygwin.a` / `libm.a` / `w32api/libadvapi32.a`）。
  `run.c` 因此自己按 `lib<name>.a` → `lib<name>.dll.a` → `<name>.lib` 的顺序，在
  用户给的 `-L` 目录、GCC 目录、`/usr/lib`、`/usr/lib/w32api` 里找；找不到就原样留
  `-lname`，让 lld 按它自己的规则找。
- **CRT 启动文件**：入口点是 `crt0.o`（带 `WinMainCRTStartup`），`crtbegin.o` /
  `crtend.o` 由 GCC 提供，在 `/usr/lib/gcc/x86_64-pc-cygwin/<ver>/`。版本号随 GCC
  版本变（现在是 14），`run.c` 用 glob 探测，不写死。
- **libgcc 照链，但 i128 靠 Runtime 那份**：libgcc 里的 `__divti3` 是 GCC 约定，和
  LLVM Win64 后端的 libcall 约定对不上；Runtime 自带了按 compiler-rt 约定实现的
  `__divti3` / `__udivti3` / `__modti3` / `umodti3`，链接时优先取我们那份，盖过
  libgcc 的（见「坑」第 1 条）。
- **不能加 `-rtlib=compiler-rt`**：Cygwin 的 clang 包不随附 compiler-rt，
  `clang -print-resource-dir` 指向的目录下只有 `include`、没有 `lib`，链接直接报
  `cannot find .../libclang_rt.builtins.a`。
- **不能加 `-no-pie`**：Cygwin 的驱动不认（报 unused），目标本身非 PIE。
- **`-static` 走 cc 驱动**：直连要换一整套库组（`libgcc_s.dll.a` → `libgcc.a`、
  去掉 `--as-needed`、还要处理 cygwin1.dll 的静态版），收益不大。
- i128 除法见下面「坑」第 1 条。

## 构建 Runtime 库（环节 ②）

```sh
# 必须先编成 PIC，同一个 .o 同时供静态库和共享库
cc -Wall -Wextra -g -Iruntime/include -fPIC -c runtime/src/runtime.c -o runtime.o

ar rcs libtap_runtime.a runtime.o                       # 静态：给原生可执行文件
cc -shared  -o libtap_runtime.so runtime.o              # 共享：给 lli --load（Linux/Cygwin）
cc -dynamiclib -o libtap_runtime.dylib runtime.o        # macOS
cc -shared  -o tap_runtime.dll runtime.o -lws2_32       # MinGW（winsock 要显式链）
```

CMake 里对应 `add_library(tap_runtime STATIC ...)` 和 `add_library(tap_runtime_shared SHARED ...)`，
两者都 `POSITION_INDEPENDENT_CODE ON`；Windows 上两个都链 `ws2_32`。

## 构建 tap 编译器（环节 ①）

| 环境 | 关键链接参数 |
|---|---|
| Makefile · macOS | `-L/opt/homebrew/opt/llvm/lib -Wl,-rpath,/opt/homebrew/opt/llvm/lib -lLLVM-21 -lm` |
| Makefile · Linux | `-L/usr/lib/llvm-21/lib -Wl,-rpath,/usr/lib -lLLVM-21 -lm` |
| Makefile · Windows | `-lLLVM-21 -lm`（无 rpath） |
| CMake · MSVC | `TAP_LLVM_C_ROOT` 下的 `LLVM-C.lib` + `LLVM-C.dll` |
| CMake · 其他 | `find_package(LLVM)` → `target_link_libraries(tap ${llvm_libs})` |

### 静态链接 LLVM

默认链的是单体动态库（`libLLVM.so` / `libLLVM.dylib`）。要摆脱这个依赖：

```sh
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release \
      -DTAP_STATIC_LLVM=ON \
      -DLLVM_DIR=/opt/homebrew/opt/llvm@21/lib/cmake/llvm   # Linux 换成发行版路径
cmake --build build-static --parallel
```

开关做的事：

```cmake
set(LLVM_LINK_LLVM_DYLIB OFF)
execute_process(COMMAND llvm-config --link-static --libs --system-libs all ...)
```

**为什么不用 `llvm_map_components_to_libnames(... all)`**：`all` 依赖全局属性
`LLVM_COMPONENT_LIBS`，那是 LLVM **自己构建时**才设置的，外部项目里展开成空 ——
一个库都不链，链接期所有 LLVM 符号 undefined。实测踩过。

改用 `llvm-config --link-static --libs --system-libs all`：它给出准确的组件列表
**和依赖顺序**（静态链接对顺序敏感），`all` 包含所有后端 —— `src/codegen.c` 调了
`LLVMInitializeAllTargetInfos` / `AllTargets` / `AllTargetMCs` / `AllAsmPrinters` /
`AllAsmParsers`，漏一个后端就会在链接期报错。

**`--system-libs` 不能漏**：LLVM 的静态库依赖 zlib / zstd / libxml2（本机实测缺 `-lz`
时链接报 `_uncompress` undefined）。`llvm-config --system-libs` 会把它们带出来。

`llvm-config` 输出的是 `-lLLVMxxx` 形式，所以还需要显式的库搜索路径
（`target_link_directories(tap PRIVATE ${LLVM_LIBRARY_DIRS})`）。

**还必须补两个东西**（实测依次踩到）：

1. **C++ 运行库**：LLVM 组件静态库是 C++ 写的，而 tap 是纯 C 项目（链接驱动是 `cc`），
   不会自动带 `libc++` —— 缺了就是满屏 `std::__1::...` undefined。
2. **Homebrew 的 `<prefix>/lib`**：`--system-libs` 里的 zstd 由 Homebrew 装在那里，
   不在 LLVM 自己的 lib 目录下，缺了报 `library 'zstd' not found`。

**`--system-libs` 里的 z3 / zstd 要分开处理**：

| 库 | 处理 | 原因 |
|---|---|---|
| `z3` | **去掉** | Homebrew 只提供 dylib；tap 不用 SMT 求解器，LLVM 的静态库也没有引用它 |
| `zstd` | **静态链** | `libzstd.a` 存在（`/opt/homebrew/opt/zstd/lib/`），静态链进去就不用带动态依赖 |

**实测结果（macOS / Homebrew LLVM 21.1.5 / arm64）**：

```
tap  156 MB   （动态版 340 KB）
依赖：libSystem / libz / libxml2 / libc++  —— 全部 /usr/lib，系统自带
```

**没有任何 Homebrew 路径**，拷到同架构的 macOS 上直接能跑。功能验证：`--version` 正常、
`tap run examples/fibonacci.tp` 跑通。

对比动态版：

| | 动态版 | 静态版 |
|---|---|---|
| 体积 | 340 KB | 156 MB |
| 依赖数 | 2 | 4 |
| **关键依赖** | `libLLVM.dylib`（152 MB，**目标机必须有 Homebrew LLVM**） | 无（全是系统库） |

依赖数量变多但都是系统库 —— 这才是「静态」的意义：不是依赖更少，而是**不依赖用户环境里可能没装的东西**。

**libz / libxml2 / libc++ 能再去掉吗？** 实测都不行：

| 库 | 结论 | 依据 |
|---|---|---|
| `libc++` | ❌ | LLVM 组件静态库是 C++ 写的，必然引用；macOS 也没有 `libc++.a` |
| `libz` | ❌ | 过滤掉 `-lz` 后链接失败（LLVM 的 Support 用 zlib 做压缩） |
| `libxml2` | ❌ | 去掉 `LLVMWindowsManifest` 组件后**依然**被引用，是别的组件在用 |

而且 macOS **不提供**这三个的静态库（`/usr/lib/libz.a`、`libxml2.a`、`libc++.a` 都不存在，
Homebrew 也没有），所以「静态链」这条路也堵死了。

**但它们都在 `/usr/lib`，任何 macOS 都自带**，不构成部署负担。
macOS 上能做到的极限就是「只依赖系统库」，现在就是。

**「全静态」在各平台的可能性**：

| 平台 | 全静态 | 说明 |
|---|---|---|
| Linux | ✅ | 再叠加 `-DCMAKE_EXE_LINKER_FLAGS=-static`。glibc 全静态有 NSS 警告，tap 用不到那些接口 |
| macOS | ❌ | libSystem 只有 `.dylib`，Apple 不支持静态链接系统库。`TAP_STATIC_LLVM` 只能去掉对 `libLLVM.dylib` 的依赖，二进制仍依赖 `/usr/lib/libSystem.B.dylib`（任何 macOS 都有） |
| MSVC | ⚠️ | `/MT` 静态 CRT（`set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")`），但官方 LLVM 包只提供 `LLVM-C.dll` |
| MinGW | ✅ | `-static`，或用 `-static-libgcc -static-libstdc++` 只静态运行时 |

Homebrew 的 LLVM **没有** `libLLVM.a`（单体静态库），只有 206 个组件 `.a` —— 这正是
必须用 `llvm_map_components_to_libnames` 而不能简单加 `-static` 的原因。

## 测试模式

| 环境 | `TEST_RUN_MODE` | 原因 |
|---|---|---|
| macOS / Linux | `fast`（默认，lli + `--load`） | — |
| MSVC / MINGW64 / UCRT64 / Cygwin | `native` | Windows 上 lli 没有可靠的 JIT 环境：UCRT 不导出 `snprintf`，JIT 解析不到 |

## 坑汇总

1. **i128 除法（最隐蔽）**：LLVM 的 Win64 后端把 `sdiv i128` 降级成 `__divti3` libcall，
   约定是「按 byval 指针传参 + XMM0 返回」，**只和 compiler-rt 匹配**。Cygwin 默认 rtlib
   是 libgcc，它的 `__divti3` 是 GCC 约定，对不上 → 一做 i128 除法就段错误（崩在首次输出前，
   回归点 `tests/run-pass/operators/mixed_signedness.tp`）。解法：`runtime.c` 在
   `(defined(_WIN32) && !defined(_MSC_VER)) || defined(__CYGWIN__)` 下自带一份约定匹配的
   实现。这个守卫早先是 `_WIN32 && __clang__ && !_MSC_VER`，因为只有 clang 才会用
   compiler-rt 约定；MinGW 的链接行去掉 libgcc 之后 gcc 构建也要靠它，所以放宽成了只看
   `_WIN32`。**再后来发现 Cygwin 的编译器不定义 `_WIN32`**（只定义 `__CYGWIN__`），守卫里
   得把 Cygwin 单独列出来 —— 否则 Cygwin 上这份实现根本不编译，i128 除法照样崩。
2. **LLD 的 COFF 模式不认 `-lfoo`**：Cygwin 的 `ld.lld` 走 COFF 模式，库搜索只认 `.lib`、
   `lib*.dll.a` 和直接给的 `.a` 输入文件。而 Cygwin 的系统库全是裸 `.a`
   （`libc.a` / `libcygwin.a` / `libm.a` / `w32api/libadvapi32.a`），所以直连时必须把库名
   展开成完整路径。同一个 `ld.lld`，Linux 的 ELF 模式可以照抄 `-lgcc`，Cygwin 的 COFF 模式
   不行 —— 「同名链接器、不同 flavor、不同库约定」的典型例子。
3. **`-lm` 位置**：必须放在目标文件和静态库之后，否则老 glibc / Cygwin 上 `pow` 报未定义；
   Linux 直连时它后面还有 libgcc / libc / crtend.o，别机械理解成「命令行最后一个参数」。
4. **`ws2_32`**：Windows 全系（MSVC 用 `.lib`，MinGW/Cygwin 用 `-lws2_32`）。
5. **clang-cl 只用于 MSVC 目标**：拿它链 MinGW 会去要 MSVC 的 CRT。
6. **`-no-pie`**：Linux 直连 `ld.lld` 不需要（默认非 PIE），但 Linux 回退的 `cc` 和其他
   Unix 的 cc 驱动仍要；Cygwin 不要（驱动不认）。
7. **`-static`**：macOS 不支持，其余平台可用。Linux 直连时换成 `-static` + `crtbeginT.o` +
   `--start-group -lgcc -lgcc_eh -lc --end-group`，回退时是 `cc -static -no-pie`；
   MinGW 走 `ld.lld` 时翻译成 `-Bstatic`；Cygwin 的直连暂不支持，走 cc 驱动。
8. **`-Wl,` 只在驱动下有效**：macOS 的 `ld`、Linux 直连的 `ld.lld`、MinGW 直连的 `ld.lld`、
   Cygwin 直连的 `ld.lld` 都不认这个前缀，`run.c` 会把逗号拆成多个裸参数
   （`push_link_options_direct_ld()`）；驱动路径（Linux 回退的 `cc`、Cygwin 回退的 `cc`、
   MinGW 回退的 gcc、其他 Unix 的 cc）用 `push_link_options_driver()` 原样透传。
   Linux 同一份二进制里两种写法都在，取决于这次是否探测到 `ld.lld`。
