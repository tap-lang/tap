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
- .exit
0
-- */
```

- 每个用例必须提供 `.exit`。
- `.stdout` 和 `.stderr` 可省略；省略时对应输出必须为空。
- `compile-fail` 必须提供 `.stderr`，其中每个非空行都必须出现在诊断中。
- 文本比较忽略 CRLF 差异和每行尾部空白，保留行首缩进。内嵌期望和外部期望文件都会使用相同规则归一化。

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
TEST_FILTER=module make test
./tests/run-tests.sh ./build/4yue
./tests/run-tests.sh ./build/4yue hello
```

测试脚本当前按文件名串行执行。编译器内部使用独立临时目录，并发执行时不会共享中间文件。
