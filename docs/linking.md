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

## 对照表

`run.c` 用编译期宏选分支，顺序是：

```c
#if defined(_WIN32) && defined(__MINGW32__)   // MinGW-w64：MINGW64 / UCRT64
#elif defined(_WIN32)                          // MSVC
#elif defined(__APPLE__)                       // macOS
#else
#  ifdef __CYGWIN__                            // Cygwin
#  else                                        // Linux 及其他 Unix
#  endif
#endif
```

| 环境 | 链接器 | Runtime 库 | 关键标志 |
|---|---|---|---|
| macOS | `ld`（直接调，不经 cc） | `libtap_runtime.a` | `-lSystem`、`-syslibroot`、`-arch`、`-platform_version` |
| Linux | `cc` | `libtap_runtime.a` | `-no-pie`、`-lm` |
| MSVC | `clang-cl` | `tap_runtime.lib` | `/MT`、`/clang:-rtlib=compiler-rt`、`/Fe`、`ws2_32.lib` |
| MINGW64 | `gcc` | `libtap_runtime.a` | `-lws2_32`（**不加 `-lm`**） |
| UCRT64 | `gcc` | `libtap_runtime.a` | 同上，命令完全一致 |
| Cygwin | `clang` | `libtap_runtime.a` | `-lm`（**不加 `-rtlib=`、不加 `-no-pie`**） |

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

### Linux（及其他 Unix）

```sh
cc -no-pie prog.o libtap_runtime.a -o prog -lm
cc -static -no-pie prog.o libtap_runtime.a -o prog -lm   # --static
```

- `-lm` **必须在目标文件和静态库之后**：glibc 2.34 起 libm 并入 libc，但更早的发行版
  仍需显式链接，否则 Runtime 里的 `pow` 报未定义符号。
- `-no-pie`：让代码模型和 IR 里的绝对寻址一致。

### MSVC

```bat
clang-cl /nologo /MT /clang:-rtlib=compiler-rt prog.obj tap_runtime.lib /Feprog.exe ws2_32.lib
```

- 用 `clang-cl` 而非 `link.exe`：能直接吃 LLVM 生成的 obj。
- `/MT` 必须和 Runtime 库一致（CMake 里 `MSVC_RUNTIME_LIBRARY=MultiThreaded`），
  否则 CRT 混用。
- `/clang:-rtlib=compiler-rt`：i128 除法/取模会生成 `__divti3`/`__modti3`，
  clang-cl 默认不自动加 compiler-rt 的 builtins，必须显式选。
- `ws2_32.lib`：Runtime 里的 winsock（`WSAStartup` / `socket` / `closesocket`）。
- 静态/动态由 `/MT` 与 `/MD` 区分，没有 `-static` 这个开关。

### MinGW-w64：MINGW64 / UCRT64

```sh
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

- 用 GNU 驱动 `gcc`，**不能用 `clang-cl`**：它按 MSVC 目标去找 MSVC 的 CRT 和 `.lib`，
  和 MinGW 的 msvcrt/UCRT + libgcc 体系对不上。
- **不加 `-lm`**：MinGW 没有独立的 libm，`pow` 在 CRT 里。
- `__divti3` 由 libgcc 提供，不用显式指定。

### Cygwin

```sh
clang prog.o libtap_runtime.a -o prog.exe -lm
clang -static prog.o libtap_runtime.a -o prog.exe -lm   # --static
```

- 用 `clang` 而不是 `cc`：Cygwin 的 `cc` 是 GCC 13，驱动版本和生成 obj 的 LLVM 对不上。
- **不能加 `-rtlib=compiler-rt`**：Cygwin 的 clang 包不随附 compiler-rt，
  `clang -print-resource-dir` 指向的目录下只有 `include`、没有 `lib`，链接直接报
  `cannot find .../libclang_rt.builtins.a`。
- **不能加 `-no-pie`**：Cygwin 的 clang 驱动不认（报 unused），目标本身非 PIE。
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
   `_WIN32 && __clang__ && !_MSC_VER` 下自带一份约定匹配的实现，链接静态库时盖过 libgcc。
2. **`-lm` 位置**：必须放最后，否则老 glibc / Cygwin 上 `pow` 报未定义。
3. **`ws2_32`**：Windows 全系（MSVC 用 `.lib`，MinGW/Cygwin 用 `-lws2_32`）。
4. **clang-cl 只用于 MSVC 目标**：拿它链 MinGW 会去要 MSVC 的 CRT。
5. **`-no-pie`**：Linux 要，Cygwin 不要（驱动不认）。
6. **`-static`**：macOS 不支持，其余平台可用。
