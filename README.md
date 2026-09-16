# tap lang

一个后端使用LLVM的编程语言 `tap` ：自研词法 / 语法分析，生成 **LLVM IR**，默认链接为可执行文件（依赖本机 LLVM 工具链）。

## 依赖

- **C 编译器**：GCC 或 Clang  
- **LLVM**：**21**（CMake 与 Makefile 均会链接 `libLLVM`）
- **构建**：GNU Make 或 CMake ≥ 3.10  


## 构建

**Make（默认生成 `build/tap`）**

```bash
make
```

需要 AddressSanitizer 调试时显式开启（切换编译选项前需清理旧目标文件）：

```bash
make clean
make SANITIZE=1
```

**CMake**

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
# 可执行文件位于 build/tap（与 CMake 生成目录一致）
```

版本号由 C 生成器 `src/scripts/get_version.c` 从 git 生成 `src/version.h`。

## 用法

```text
tap [选项] <源文件>
tap run [选项] <源文件>
```

源文件扩展名无强制要求，仓库内示例多为 `.tp`。

### 命令行选项

| 选项 | 说明 |
|------|------|
| `run` | 编译为本地可执行文件并运行 |
| `-h`, `--help` | 打印帮助 |
| `-V`, `--version` | 打印版本与 git 提交 |
| `-o <文件>` | 指定输出（IR / 目标文件 / 可执行文件名，视模式而定） |
| `-static` | 静态链接生成的本地可执行文件；仅适用于默认编译和 `run` |
| `-ir` | 完整编译后写出 LLVM IR（默认 `output.ll`，可用 `-o` 覆盖） |
| `-emit-obj` | 写出目标文件（默认 `output.o`，可用 `-o` 覆盖） |
| `-emit-wasm` | 写出 WebAssembly 目标文件（默认 `output.wasm`，可用 `-o` 覆盖） |
| `-lex` | 仅词法分析：逐 token 输出（文件、行列、类型、词素） |
| `-parse` | 词法 + 语法分析：将 AST 打印到 stdout，不生成代码 |
| `-run-lli` | 生成临时 IR 并用 `lli` 执行 |

说明：

- **默认**（无 `run` / `-ir` / `-emit-obj` / `-emit-wasm` / `-run-lli`）：直接生成目标文件并链接为可执行文件（默认使用源文件的基础名称，可用 `-o` 指定）。
- **`run`**：执行与默认模式相同的本地编译流程，编译成功后运行可执行文件，返回程序的退出码并删除生成的可执行文件。
- **`-static`**：生成本地可执行文件时启用静态链接；macOS 不支持完整静态链接系统库，因此会给出明确错误。
- **`-lex`** 与 **`-parse`** 互斥于后续流水线：若同时传 `-lex`，只执行词法阶段。

### 示例

```bash
./build/tap tests/run-pass/basics/hello.tp
./build/tap -static -o hello-static tests/run-pass/basics/hello.tp
./build/tap run tests/run-pass/basics/hello.tp
./build/tap -o hello.bin tests/run-pass/basics/hello.tp
./build/tap -ir -o out.ll tests/run-pass/functions/fibonacci.tp
./build/tap -emit-wasm -o out.wasm tests/run-pass/functions/fibonacci.tp
./build/tap -run-lli tests/run-pass/basics/print.tp
./build/tap -lex tests/run-pass/functions/fibonacci.tp
./build/tap -parse tests/run-pass/functions/fibonacci.tp
```

交互式贪吃蛇示例支持 WASD 和终端方向键真实键值，按 `q` 退出：

```bash
./build/tap run examples/snake.tp
```

## For 循环

```text
for (let i: i32 = 0; i < 10; i++) {
    print("%d\n", i);
}
```

循环头支持变量声明或赋值初始化、可选条件，以及赋值、`++`、`--` 更新。三个部分都可以为空，
例如 `for (;;) { ... }`。`break;` 结束当前循环，`continue;` 执行更新表达式后进入下一轮；
嵌套循环中两者只作用于最内层循环。

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
read_key(): i32
sleep_ms(milliseconds: i32): i32
clear_screen(): i32
random(maximum: i32): i32
square(value: i32): i32  # import std.math; 后通过 math.square(...) 调用
```

Prelude 中的函数名不能在用户源码中重复定义。编译器依次从环境变量
`TAP_STD_PATH`、当前目录的 `std`、可执行文件相邻的源码或安装目录查找
`prelude.tp`。`-lex` 和 `-parse` 只处理指定源文件，不加载 Prelude。
终端、休眠和随机数接口的行为见 [Runtime 文档](docs/runtime.md)。

## 开发文档

- [源码结构](docs/src.md)
- [数据类型](docs/data-types.md)
- [字符串](docs/data-type-string.md)
- [AST](docs/ast.md)
- [Runtime](docs/runtime.md)
- [模块导入](docs/module.md)
- [测试](docs/test.md)

## 许可

见仓库根目录 `LICENSE`。
