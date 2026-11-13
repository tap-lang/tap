# tap lang

一个后端使用 LLVM 的编程语言：静态类型、泛型单态化、带载荷枚举、模块系统，编译为原生可执行文件。

[![MacOS & Linux](https://github.com/tap-lang/tap/actions/workflows/ci.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/ci.yml)    [![MSVC](https://github.com/tap-lang/tap/actions/workflows/msvc.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/msvc.yml)    [![MINGW64](https://github.com/tap-lang/tap/actions/workflows/mingw64.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/mingw64.yml)   [![UCRT64](https://github.com/tap-lang/tap/actions/workflows/ucrt64.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/ucrt64.yml)

<!-- [![Cygwin](https://github.com/tap-lang/tap/actions/workflows/cygwin.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/cygwin.yml)   -->

## 代码示例

```tap
fn main(){
    print("Hello tap! 你好，世界！😀\n");
}
```

## 快速开始

需要 C 编译器（GCC 或 Clang）、**LLVM >= 21**，以及 GNU Make 或 CMake >= 3.10：

```bash
make
./build/tap run examples/hello.tp
```

`make install` 默认装到 `/usr/local`（`PREFIX=` 可改），装完不需要设置任何环境变量；
Windows 用 `scripts\mingw-build.bat install [目录]` 或 `scripts\msvc-build.bat install [目录]`，
装出来的布局一致。
构建选项、安装布局、Windows 一键脚本见[构建与安装](docs/build.md)。

## 语言特性

- **带载荷枚举**：tagged union，`match` 按成员解构，分支必须穷尽
- **泛型**：函数与结构体单态化，类型实参由实参推断，支持 trait 约束
- **借用检查**：`&T` / `&mut T` 共享与独占借用，字段粒度冲突、NLL 寿命与跨函数签名检查；`*T` 不检查
- **模块系统**：`pub` 可见性、别名、通配与单成员导入
- **C 互操作**：`extern fn` 直接绑 libc，`std.net` / `std.fs` 全靠它实现
- **零导入 Prelude**：`print`、`panic`、`Result<T, E>` 开箱可用
- **多平台**：macOS、Linux、Windows（MSVC 与 MinGW-w64）
- **可调试**：`tap -g` 生成 DWARF 行号表，`gdb` / `lldb` 能按 `源码:行号` 断点

各特性的细节见[数据类型](docs/data-types.md#带载荷枚举)、[借用与引用](docs/borrow.md)、
[模块导入](docs/module.md)、[标准库](docs/std.md)和 [C 互操作](docs/extern.md)。

## 文档

**使用**

- [构建与安装](docs/build.md) —— 依赖、Make / CMake、安装布局、打包成可分发目录
- [命令行](docs/cli.md) —— 编译模式、全部选项、包模式、示例
- [示例程序](examples/README.md) —— 贪吃蛇、俄罗斯方块、JSON 解析器、TCP echo

**语言**

- [单文件语法参考](docs/tap-syntax.md) —— 一份文件覆盖全部语法（喂给 AI 用）
- [控制流](docs/control-flow.md) —— `if` / `while` / `for`、`break` / `continue`、块作用域
- [数据类型](docs/data-types.md) —— 整数、浮点、字符串、结构体、枚举、泛型、trait
- [借用与引用](docs/borrow.md) —— `&T` / `&mut T`、借用检查、与 `*T` 的分工
- [字符串](docs/data-type-string.md)
- [模块导入](docs/module.md) —— 路径查找、`pub`、别名、通配与单成员导入
- [标准库](docs/std.md) —— Prelude 与各模块的函数清单
- [C 互操作](docs/extern.md) —— `extern fn` 方案与 ABI 细节

**实现**

- [源码结构](docs/src.md) —— 编译流水线、模块依赖、诊断输出
- [AST](docs/ast.md)
- [Runtime](docs/runtime.md)
- [各平台链接命令](docs/linking.md)
- [测试](docs/test.md)

## 许可

见仓库根目录 `LICENSE`。
