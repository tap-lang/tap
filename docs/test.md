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
- .exit
0
-- */
```

- 每个用例必须提供 `.exit`。
- `.stdout` 和 `.stderr` 可省略；省略时对应输出必须为空。
- `.args` 可省略；提供时仅用于 `run-pass` 和 `run-fail`，每行作为一个独立程序参数，并通过 `--` 传给 `tap run` 生成的程序。
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
TEST_FILTER=module make test
./tests/run-tests.sh ./build/tap
./tests/run-tests.sh ./build/tap hello
```

`make test` 默认使用 4 个并行 worker。普通 `run-pass`、`run-fail` 用例通过 `lli` 执行，
带 `.args` 的用例和 `tests/run-pass/basics/hello.tp` 原生编译并链接，持续覆盖程序参数和链接路径。
`make test-native` 会让全部运行用例生成原生可执行文件。可以用 `TEST_JOBS=N` 调整并行度，
或设为 `1` 排查依赖执行顺序的问题；最终结果始终按文件名顺序输出。

`TEST_RUN_MODE` 环境变量可以直接选择运行方式（`fast` 走 `lli`，`native` 生成原生可执行文件），
不传时默认为 `fast`。

### Cygwin 下必须用 native 模式

Cygwin 的 `lli` 有两个限制，导致 `fast` 模式固定有 5 个用例失败：

- **JIT 代码里的 `exit()` 不传递退出码**。用最小 IR 验证：`call void @exit(i32 1)` 后
  `lli` 仍返回 0，而 `ret i32 1` 正常返回 1；同一份 IR 原生编译则返回 1。
  这会让 4 个 `run-fail` 用例的 `.exit` 期望无法满足。
- **栈帧超过 4KB 时解析不到 `___chkstk_ms`**。Cygwin 的目标 triple 是
  `x86_64-pc-windows-cygnus`，LLVM 会为 Windows ABI 插入栈探测调用，
  而 Cygwin 的 `lli` 里没有这个符号。`tests/run-pass/stdlib/env_var.tp`
  的 `[string; 256]`（4KB 栈数组）就会触发。

这两个问题都出自 `lli`，无法在 tap 侧规避，因此 `.github/workflows/cygwin.yml`
固定使用 `TEST_RUN_MODE=native`（实测 88/88 全过）。
