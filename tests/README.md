# 测试目录

测试按预期行为分类，测试脚本会自动发现所有 `.tp` 文件：

- `run-pass/`：编译和运行均成功，退出码必须为 `0`。
- `run-fail/`：编译成功，但程序返回预期的非零退出码；必须提供同名 `.exit` 文件。
- `compile-fail/`：编译必须失败；必须提供同名 `.stderr` 文件，其中每个非空行都应出现在诊断中。
- `fixtures/`：模块等测试依赖，不会作为测试用例直接执行。
- `llvm/`：LLVM C API 实验代码，不属于语言回归测试。

同名 `.stdout` 和 `.stderr` 文件用于断言输出。运行成功和运行失败用例未提供输出文件时，对应输出必须为空。

```sh
make test
TEST_FILTER=module make test
./tests/run-tests.sh ./build/4yue
./tests/run-tests.sh ./build/4yue hello
```

测试脚本当前按文件名串行执行。编译器内部使用独立临时目录，并发执行时不会共享中间文件。
