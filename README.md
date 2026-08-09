# 4yue lang

一个后端使用LLVM的编程语言 `4yue` ：自研词法 / 语法分析，生成 **LLVM IR**，默认链接为可执行文件（依赖本机 LLVM 工具链）。

## 依赖

- **C 编译器**：GCC 或 Clang  
- **LLVM**：**≥ 21**（CMake 与 Makefile 均会链接 `libLLVM`；`-run-lli` 会调用 `lli`）
- **构建**：GNU Make 或 CMake ≥ 3.10  

macOS（Homebrew）示例：`brew install llvm`，并保证能解析到对应 `include` / `lib`（仓库里的 Makefile 已按常见路径配置，若版本或前缀不同请自行改 `CFLAGS` / `LDFLAGS`）。

## 构建

**Make（默认生成 `build/4yue`）**

```bash
make
```

**CMake**

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
# 可执行文件位于 build/4yue（与 CMake 生成目录一致）
```

版本号由 `src/scripts/version.sh`（或 Windows 下对应脚本）从 git 生成 `src/version.h`。

## 用法

```text
4yue [选项] <源文件>
4yue run [选项] <源文件>
```

源文件扩展名无强制要求，仓库内示例多为 `.tp`。

### 命令行选项

| 选项 | 说明 |
|------|------|
| `run` | 编译为本地可执行文件并运行 |
| `-h`, `--help` | 打印帮助 |
| `-V`, `--version` | 打印版本与 git 提交 |
| `-o <文件>` | 指定输出（IR / 目标文件 / 可执行文件名，视模式而定） |
| `-ir` | 完整编译后写出 LLVM IR（默认 `output.ll`，可用 `-o` 覆盖） |
| `-emit-obj` | 写出目标文件（能力占位，行为以当前实现为准） |
| `-lex` | 仅词法分析：逐 token 输出（文件、行列、类型、词素） |
| `-parse` | 词法 + 语法分析：将 AST 打印到 stdout，不生成代码 |
| `-run-lli` | 生成临时 IR 并用 `lli` 执行 |
| `-debug` | 打开各阶段调试输出 |

说明：

- **默认**（无 `run` / `-ir` / `-emit-obj` / `-run-lli`）：生成临时 `.ll` 再调用 LLVM 接口编译为可执行文件（默认名 `output`，可用 `-o` 指定）。
- **`run`**：执行与默认模式相同的本地编译流程，编译成功后运行可执行文件，返回程序的退出码并删除生成的可执行文件。
- **`-lex`** 与 **`-parse`** 互斥于后续流水线：若同时传 `-lex`，只执行词法阶段。

### 示例

```bash
./build/4yue tests/hello.tp
./build/4yue run tests/hello.tp
./build/4yue -o hello.bin tests/hello.tp
./build/4yue -ir -o out.ll tests/fibonacci.tp
./build/4yue -run-lli tests/test_print.tp
./build/4yue -lex tests/fibonacci.tp
./build/4yue -parse tests/fibonacci.tp
```

## 模块导入

使用点分隔模块名导入其他 `.tp` 文件。默认名称空间取模块名最后一段，
也可以使用 `as` 重命名：

```text
import std.math;
import modules.helpers as helper;

fn main(): i32 {
    return math.square(5);
}
```

模块路径、递归加载、循环导入、环境变量和错误规则详见[模块导入文档](docs/module.md)。

## 标准库

编译阶段会自动加载 [`std/prelude.tp`](std/prelude.tp)，源文件无需显式导入即可使用以下 `i32` 函数：

```text
min(a: i32, b: i32): i32
max(a: i32, b: i32): i32
abs(value: i32): i32
square(value: i32): i32  # import std.math; 后通过 math.square(...) 调用
```

Prelude 中的函数名不能在用户源码中重复定义。编译器依次从环境变量
`4YUE_STD_PATH`、当前目录的 `std`、可执行文件相邻的源码或安装目录查找
`prelude.tp`。`-lex` 和 `-parse` 只处理指定源文件，不加载 Prelude。

## 开发文档

- [源码结构](docs/src.md)
- [AST](docs/ast.md)
- [模块导入](docs/module.md)

## 许可

见仓库根目录 `LICENSE`。
