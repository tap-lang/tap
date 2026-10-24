# tap lang

一个后端使用LLVM的编程语言 `tap`。

[![MacOS & Linux](https://github.com/tap-lang/tap/actions/workflows/ci.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/ci.yml)    [![Cygwin](https://github.com/tap-lang/tap/actions/workflows/cygwin.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/cygwin.yml)    [![MSVC](https://github.com/tap-lang/tap/actions/workflows/msvc.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/msvc.yml)    [![MINGW64](https://github.com/tap-lang/tap/actions/workflows/mingw64.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/mingw64.yml)   [![UCRT64](https://github.com/tap-lang/tap/actions/workflows/ucrt64.yml/badge.svg)](https://github.com/tap-lang/tap/actions/workflows/ucrt64.yml)

## 代码示例
```tap
fn main(){
    print("Hello tap! 你好，世界！😀\n");
}
```

## 依赖

- **C 编译器**：GCC 或 Clang  
- **LLVM**：**>= 21**（CMake 与 Makefile 均会链接 `libLLVM`）
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

## 安装

两种构建方式都支持 `install`，装出来的布局一致：

```bash
make install                    # 默认装到 /usr/local，可能需要 sudo
make install PREFIX=$HOME/.local  # 装到不需要权限的位置
```

CMake 构建则用 `cmake --install build`（可配 `--prefix`）。

装出来的结构：

```text
<prefix>/bin/tap                       编译器
<prefix>/lib/libtap_runtime.a          原生可执行文件链接的静态 Runtime
<prefix>/lib/libtap_runtime.{dylib,so} fast 模式给 lli 加载的共享 Runtime
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

## 用法

```text
tap [选项] <源文件>
tap run [选项] [源文件]
tap build
tap init <名字>
```

`tap run` / `tap build` 不给文件时按**包模式**工作：从当前目录向上找到 `package.toml`，
按里面的 `entry` 编译，产物放到 `<包根>/build/<name>`。`run` 编译完接着运行，`build` 只编译。
包布局的约定见 [tapm 仓库](https://github.com/) 的 `docs/design.md`。

`tap init <名字>` **只是转发给 `tapm init`**——脚手架是包管理器的活，模板只该有一份。
tapm 是可独立安装的二进制，编译器先找自己旁边的 `tapm`，再按 `PATH` 找；都找不到时
给出明确提示而不是让 `spawn` 抛一句看不懂的错。**构建路径不依赖它**：`tap build` /
`tap run` 完全不需要 tapm 在场。

源文件扩展名无强制要求，仓库内示例多为 `.tp`；模块解析同时接受 `.tp` 和 `.tap`
（同名时 `.tp` 优先）。

### 命令行选项

| 选项 | 说明 |
|------|------|
| `run` | 编译为本地可执行文件并运行 |
| `build` | 包模式：编译当前包，产物放到 `<包根>/build/<name>` |
| `init <名字>` | 转发给 `tapm init`，新建项目 |
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
- **`run`**：执行与默认模式相同的本地编译流程，编译成功后运行可执行文件，返回程序的退出码。
  给了 `-o`、或走包模式时，产物**保留**（那是调用方明确要的文件）；只有没给 `-o` 的文件模式
  才走临时文件并在运行后清理。
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

`examples/tetris.tp` 是俄罗斯方块：7-bag 随机发牌、软降与硬降、消行计分和按等级加速，
方块由彩色 emoji 拼成（同一块内按格子相对位置斜向取色，一块本身就是一组彩色组合），
落点提示按 `T` 开关（默认关），WASD / 方向键操作，按 `q` 退出：

```bash
./build/tap run examples/tetris.tp
```

`examples/json_poc.tp` 是只用 tap 和标准库实现的 JSON 解析器与序列化器，
覆盖 object / array / string / number / true / false / null，演示递归结构体、
泛型动态数组、字符串构建、字符串转义和数值格式化的用法：

```bash
./build/tap run examples/json_poc.tp
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

## 带载荷枚举

枚举成员可以携带载荷，即 tagged union：

```text
enum Shape {
    Circle(f64),
    Rect(f64, f64),
    Empty,
}

fn area(shape: Shape): f64 {
    match (shape) {
        Shape.Circle(radius) => {
            return 3.141592653589793 * radius * radius;
        }
        Shape.Rect(width, height) => {
            return width * height;
        }
        Shape.Empty => {
            return 0.0;
        }
    }
    return 0.0;
}
```

用 `EnumName.MemberName(值...)` 构造，不带载荷的成员不写括号。`match` 按成员解构，圆括号里
按顺序绑定载荷；分支体可以是代码块或单条语句，`_` 是通配分支。分支必须覆盖全部成员或包含
`_`，否则编译报错。不带载荷的枚举仍然按 `i32` 表示，继续用 `==` 比较。

细节和当前限制见[数据类型文档](docs/data-types.md#带载荷枚举)。

## 模块导入

使用点分隔模块名导入其他 `.tp` 文件。默认名称空间取模块名最后一段，
也可以使用 `as` 重命名；此外还支持通配导入和单成员导入，**调用时不用模块前缀**：

```text
import std.math;                 // 按 math.square(5) 访问
import modules.helpers as helper; // 按 helper.xxx(...) 访问
import std.math.*;               // 通配导入：pub 成员按裸名访问
import std.math.square;          // 单成员导入：按裸名 square(...) 访问
import std.math.square as sq;    // 单成员导入并改名：按 sq(...) 访问

fn main(): i32 {
    return math.square(5);
}
```

三种导入的完整规则见[模块导入文档](docs/module.md)。

顶层声明默认是**模块私有**的，加 `pub` 才能被其他模块导入：

```text
pub fn square<T>(value: T): T { ... }
pub const PI: f64 = 3.141592653589793;
pub struct Point { x: i32 }
```

`pub` 可以修饰函数、`extern fn`、常量和类型。类型名一律带模块前缀，`pub` 决定的是
「能不能从模块外引用」——外部写裸类型名（`Point`）或走名称空间导入即可。因此两个模块
可以各自拥有同名的类型，**pub 的也一样**。

模块路径、递归加载、循环导入、环境变量和错误规则详见[模块导入文档](docs/module.md)。

## 标准库

编译阶段会自动加载 [`std/prelude.tp`](std/prelude.tp)，源文件无需显式导入即可使用：

```text
read_key(): i32
sleep_ms(milliseconds: i32): i32
clear_screen(): i32
panic(message: string): i32
exit(code: i32): i32
print(format: string, ...): i32
Result<T, E> 及 is_ok / is_err / unwrap_or / unwrap / expect
```

[`std.math`](std/math.tp) 需要显式导入，提供 `PI`、`square<T>`、`abs<T>`、`pow<T>` 和
`pow_f64`；前三个的类型参数由实参推断。

[`std.io`](std/io.tp) 需要显式导入，提供 `printf`（标准输出）和 `eprintf`（标准错误）；
零导入的 `print` 是标准输出的便利版本，单实参时按类型自动挑格式串。

Prelude 中的函数名不能在用户源码中重复定义。编译器依次从环境变量
`TAP_STD_PATH`、当前目录的 `std`、可执行文件相邻的源码或安装目录查找
`prelude.tp`。`-lex` 和 `-parse` 只处理指定源文件，不加载 Prelude。
终端、休眠和随机数接口的行为见 [Runtime 文档](docs/runtime.md)。

其余模块（`std.parse`、`std.json`、`std.vec`、`std.string_builder` 等）需要显式导入，
函数清单和示例见[标准库文档](docs/std.md)。


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
