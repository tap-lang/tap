# 源码结构

tap 当前采用手写前端和 LLVM 后端，主要编译流程如下：

```text
源文件 -> Lexer -> Token -> Parser -> AST -> LLVM Codegen -> IR / 可执行文件
```

## 核心模块

| 文件 | 职责 |
|---|---|
| [`main.c`](../src/main.c) | 程序入口、命令行参数解析和编译模式分发 |
| [`helpers.c`](../src/helpers.c) / [`helpers.h`](../src/helpers.h) | 通用的路径与字符串辅助函数 |
| [`token.c`](../src/token.c) / [`token.h`](../src/token.h) | Token 类型、名称映射和资源释放 |
| [`lexer.c`](../src/lexer.c) / [`lexer.h`](../src/lexer.h) | 读取源文件并执行词法分析 |
| [`parser.c`](../src/parser.c) / [`parser.h`](../src/parser.h) | 将 Token 流解析为 AST |
| [`module.c`](../src/module.c) / [`module.h`](../src/module.h) | 解析模块路径、递归加载导入并合并函数；详见[模块导入文档](module.md) |
| [`prelude.c`](../src/prelude.c) / [`prelude.h`](../src/prelude.h) | 定位并解析 Prelude，检查重复函数后合并到用户程序 AST |
| [`ast.c`](../src/ast.c) / [`ast.h`](../src/ast.h) | AST 节点定义、构造、打印和释放；详见 [AST 文档](ast.md) |
| [`codegen.c`](../src/codegen.c) / [`codegen.h`](../src/codegen.h) | 遍历 AST、生成 LLVM IR，并将 IR 编译链接为可执行文件 |
| [`run.c`](../src/run.c) / [`run.h`](../src/run.h) | 本地可执行文件与 `lli` 的运行、退出码和临时产物清理 |
| [`runtime/src/runtime.c`](../runtime/src/runtime.c) | 终端、休眠和随机数等跨平台 C Runtime 实现 |
| [`version.c`](../src/version.c) / [`version.h`](../src/version.h) | 版本信息实现和生成结果 |

## 模块依赖

依赖关系整体保持从入口到前端、后端和运行层的单向流动：

```text
main
├── lexer -> token
├── parser -> lexer + ast
├── module -> parser + ast
├── prelude -> parser + ast
├── codegen -> ast + LLVM
└── run -> codegen
```

- Lexer 只负责产生 Token，不创建 AST。
- Parser 拥有 AST 的创建过程。
- Codegen 和 Run 消费 AST/LLVM 结果，不参与语法解析。
- `main.c` 负责组装各阶段，并在流程结束后释放资源。

Codegen 会先生成并验证全部函数，再将非 `main` 函数视为内部符号运行
LLVM GlobalDCE。这样既保留了未调用函数的编译错误检查，也不会把不可达的
Prelude 和模块函数写入最终 IR。

## 模块加载

Parser 将 `import a.b;` 保存为 `ImportNode`，Module Loader 再把模块名转换为
`a/b.tp`。名称空间、通配（`a.b.*`）和单成员（`a.b.c [as name]`）三种导入都在这一步解析：
模块文件使用同一套 Lexer 和 Parser；其递归依赖加载完成后，函数链表会使用唯一内部符号合并到
入口程序，限定调用按当前文件的名称空间解析、裸名调用按通配/单成员绑定解析，最后统一交给
Codegen。具体语法、查找顺序、去重和错误规则见[模块导入文档](module.md)。

## Prelude 标准库

[`std/prelude.tp`](../std/prelude.tp) 使用 tap 源码实现第一阶段标准库函数。
普通编译、IR 生成和运行模式会先解析用户源码和显式导入，再由 `prelude.c` 解析 Prelude，
检查两边是否存在同名函数，最后将两个函数列表合并后交给 Codegen。

查找顺序如下：

1. `TAP_STD_PATH/prelude.tp`
2. 当前工作目录下的 `std/prelude.tp`
3. 编译器可执行文件相邻源码目录下的 `std/prelude.tp`
4. 安装前缀下的 `share/tap/std/prelude.tp`

这种方式保持标准库函数与普通用户函数使用同一套 Parser、AST 和 Codegen；
`-lex`、`-parse` 模式不会加载 Prelude，便于单独观察目标源文件。

## 版本生成

版本模板是 [`version.h.ini`](../src/version.h.ini)。构建时会先编译
[`scripts/get_version.c`](../src/scripts/get_version.c)，再由这个 C 生成器根据 Git 信息生成 `src/version.h`。

Make 和 CMake 都会在编译前触发版本文件生成。
