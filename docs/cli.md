# 命令行

## 用法

```text
tap [选项] <源文件>
tap run [选项] [源文件] [-- 程序参数...]
tap build
tap new <名字>
```

| 形式 | 说明 |
|---|---|
| `tap <源文件>` | 默认：编译为目标文件并链接成可执行文件 |
| `tap run ...` | 编译为本地可执行文件并运行，返回程序的退出码 |
| `tap build` | 包模式：编译当前包，产物放到 `<包根>/build/<name>` |
| `tap new <名字>` | 转发给 `tapm new`，新建项目 |
| `tap -h` / `-V` | 打印帮助 / 版本与 git 提交 |

## 选项

| 选项 | 说明 |
|------|------|
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
| `-l<名字>` | 链接第三方库；可重复，顺序即命令行顺序（也认 `-l <名字>`） |
| `-L<目录>` | 追加库搜索路径；可重复（也认 `-L <目录>`） |
| `-Wl,<参数>` | 把参数透传给链接器，逗号分隔多个 |
| `-v`, `--verbose` | 把实际执行的链接 / lli / 运行命令回显到 stderr |
| `-- <程序参数>` | 其后的参数原样传给被 `run` 的程序；**只能与 `run` 同用** |

## 各模式的说明

- **默认**（无 `run` / `-ir` / `-emit-obj` / `-emit-wasm` / `-run-lli`）：直接生成目标文件并
  链接为可执行文件（默认使用源文件的基础名称，可用 `-o` 指定）。
- **`run`**：执行与默认模式相同的本地编译流程，编译成功后运行可执行文件，返回程序的退出码。
  给了 `-o`、或走包模式时，产物**保留**（那是调用方明确要的文件）；只有没给 `-o` 的文件模式
  才走临时文件并在运行后清理。
- **`-static`**：生成本地可执行文件时启用静态链接；macOS 不支持完整静态链接系统库，因此会
  给出明确错误。
- **`-lex`** 与 **`-parse`** 互斥于后续流水线：若同时传 `-lex`，只执行词法阶段。
- **`-v`**：只影响输出，不改变编译结果。回显的命令带引号、可直接复制粘贴；因为输出走
  stderr，`tap run -v prog.tp > out.txt` 的 `out.txt` 里仍然只有程序自己的输出。
  `-v` 下**不删临时目录**（回显的链接命令里带着目标文件路径，删了就没法照着跑），
  目录路径会单独打一行。各平台的链接命令见[链接文档](linking.md)。

互斥规则由 `main.c` 在解析完参数后统一检查，冲突时直接报错而不是静默取其一：
`run` 不能与其他输出/执行模式组合，`--` 程序参数只能跟 `run` 一起用，
`-static` 只能用于生成或运行本地可执行文件。

## 包模式

`tap run` / `tap build` 不给文件时按**包模式**工作：从当前目录向上找到 `package.toml`，
按里面的 `entry` 编译，产物放到 `<包根>/build/<name>`。`run` 编译完接着运行，`build` 只编译。
包布局的约定见 tapm 仓库的 `docs/design.md`，
解析细节见[模块导入文档](module.md#包模式tap-build--tap-run-不带文件时)。

## tap new

`tap new <名字>` **只是转发给 `tapm new`**——脚手架是包管理器的活，模板只该有一份。
tapm 是可独立安装的二进制，编译器先找自己旁边的 `tapm`，再按 `PATH` 找；都找不到时
给出明确提示而不是让 `spawn` 抛一句看不懂的错。**构建路径不依赖它**：`tap build` /
`tap run` 完全不需要 tapm 在场。查找顺序和透传规则见
[模块导入文档](module.md#tap-new转发给-tapm)。

## 源文件扩展名

源文件扩展名无强制要求，仓库内示例多为 `.tp`；模块解析同时接受 `.tp` 和 `.tap`
（同名时 `.tp` 优先）。

## 示例

```bash
./build/tap tests/run-pass/basics/hello.tp
./build/tap -static -o hello-static tests/run-pass/basics/hello.tp
./build/tap run tests/run-pass/basics/hello.tp
./build/tap run tests/run-pass/stdlib/env_args.tp -- alpha beta   # 程序参数用 -- 透传
./build/tap -o hello.bin tests/run-pass/basics/hello.tp
./build/tap -ir -o out.ll tests/run-pass/functions/fibonacci.tp
./build/tap -emit-wasm -o out.wasm tests/run-pass/functions/fibonacci.tp
./build/tap -run-lli tests/run-pass/basics/print.tp
./build/tap -lex tests/run-pass/functions/fibonacci.tp
./build/tap -parse tests/run-pass/functions/fibonacci.tp
```

交互式示例（贪吃蛇、俄罗斯方块、JSON 解析器）见 [`examples/README.md`](../examples/README.md)。
