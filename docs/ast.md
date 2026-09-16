# AST

本文档描述 tap 当前实现中的抽象语法树（Abstract Syntax Tree，AST）。内容以
[`src/ast.h`](../src/ast.h) 和 [`src/ast.c`](../src/ast.c) 为准，不包含尚未实现的语言设计。

## 作用与数据流

AST 位于 Parser 和 LLVM Codegen 之间：

```text
源文件 -> Lexer -> Token -> Parser -> AST -> 泛型单态化 -> LLVM Codegen -> LLVM IR
```

- Parser 创建并连接 AST 节点。
- `-parse` 通过 `print_ast()` 将 AST 打印到标准输出。
- 泛型单态化处理泛型调用，生成具体函数并移除泛型模板。
- Codegen 遍历不含泛型类型参数的 AST，生成函数、表达式和控制流对应的 LLVM IR。
- 编译流程结束后，调用 `free_ast()` 释放 AST。

## 基础表示

所有具体节点都把 `ASTNode` 放在结构体首字段：

```c
typedef struct ASTNode {
    enum NodeType type;
    struct ASTNode *next;
} ASTNode;
```

因此具体节点指针可以转换为 `ASTNode *`，再通过 `type` 判断实际类型。`next` 用于组织链表，
例如函数列表、语句列表和参数列表；它不是语法树中的普通子节点。

创建节点时必须满足两个基础约定：

1. `base.type` 设置为对应的 `NodeType`。
2. `base.next` 初始化为 `NULL`。

## 节点类型

| `NodeType` | 结构体 | 主要字段与含义 |
|---|---|---|
| `NODE_PROGRAM` | `ProgramNode` | `imports` 和 `functions` 分别指向导入、函数链表 |
| `NODE_IMPORT` | `ImportNode` | 模块名、名称空间别名及导入声明的源文件位置 |
| `NODE_FUNCTION` | `FunctionNode` | 函数名、泛型类型参数、普通参数、返回类型和函数体；`is_extern` 标记 C ABI 外部声明 |
| `NODE_IDENTIFIER` | `IdentifierNode` | `name` 保存标识符名称 |
| `NODE_LITERAL` | `LiteralNode` | 字面量类型及对应的联合值 |
| `NODE_RETURN` | `ReturnNode` | `expression` 指向返回表达式 |
| `NODE_PRINT` | `PrintNode` | `arguments` 指向打印参数链表 |
| `NODE_BINARY_OP` | `BinaryOpNode` | 操作符、左操作数和右操作数 |
| `NODE_REFERENCE` | `ReferenceNode` | `target` 指向被取地址的变量、结构体字段或索引表达式 |
| `NODE_VAR_DECL` | `VarDeclNode` | 变量名、可选类型、初始化表达式，以及 `is_const` 声明标记 |
| `NODE_ASSIGNMENT` | `AssignmentNode` | 被赋值变量名和新的值表达式 |
| `NODE_ARRAY_LITERAL` | `ArrayLiteralNode` | 初始化元素链表和元素数量 |
| `NODE_INDEX_EXPRESSION` | `IndexExpressionNode` | 数组表达式和下标表达式 |
| `NODE_INDEX_ASSIGNMENT` | `IndexAssignmentNode` | 索引目标和新的元素值 |
| `NODE_FUNCTION_CALL` | `FunctionCallNode` | 函数名、显式泛型类型实参和普通实参链表 |
| `NODE_IF_STATEMENT` | `IfStatementNode` | 条件、真分支和假分支 |
| `NODE_FOR_STATEMENT` | `ForStatementNode` | 初始化、条件、更新和循环体 |
| `NODE_BREAK_STATEMENT` | `ASTNode` | 结束当前循环 |
| `NODE_CONTINUE_STATEMENT` | `ASTNode` | 跳到当前循环的更新块 |
| `NODE_VAR_TYPE` | `VarTypeNode` | 标量、结构体、枚举类型，或通过 `element_type` 递归表示固定长度数组和指针 |

`NODE_STATEMENT` 和 `NODE_EXPRESSION` 当前只是枚举占位项，没有对应的结构体、构造函数或
Parser 产物。

### 程序与函数

AST 根节点是 `ProgramNode`：

```text
ProgramNode
├── imports -> ImportNode -> ImportNode -> ...
└── functions -> FunctionNode -> FunctionNode -> ...
```

`ImportNode.module_name` 保存点分隔模块名，`alias` 保存当前文件使用的名称空间。
Module Loader 消费导入列表、解析限定函数调用并合并模块函数，Codegen 不直接处理
`ImportNode`。加载行为详见[模块导入文档](module.md)。

每个 `FunctionNode` 包含：

- `name`：函数名。
- `type_params`：由 `IdentifierNode` 组成的泛型类型参数链表。
- `params`：由 `IdentifierNode` 组成的形参链表。
- `param_types`：由 `VarTypeNode` 组成的参数类型链表。
- `body`：函数体语句链表。
- `return_type`：可选返回类型；未声明时为 `NULL`。

`params` 和 `param_types` 是两条平行链表。Codegen 按相同位置读取参数及其类型；缺少类型时
默认按 `i32` 处理。

### 语句

当前函数体和条件分支可包含以下语句：

- `VarDeclNode`：变量声明和初始化。
- `AssignmentNode`：变量赋值，以及 `++`、`--` 展开后的更新。
- `IndexAssignmentNode`：固定长度数组或指针的元素赋值，固定长度数组支持多维索引链。
- `ReturnNode`：返回表达式。
- `PrintNode`：一个或多个打印参数。
- `IfStatementNode`：`if`、`elseif` 和 `else`。
- `ForStatementNode`：经典三段式 `for` 循环。
- `NODE_BREAK_STATEMENT`：`break;`，结束最内层循环。
- `NODE_CONTINUE_STATEMENT`：`continue;`，进入最内层循环的更新阶段。

`IfStatementNode.consequence` 指向真分支的语句链表。`alternative` 有两种形态：

- `else`：指向语句链表。
- `elseif`：指向另一个 `IfStatementNode`，形成嵌套条件链。

`ForStatementNode` 对应 `for (initializer; condition; update) { body }`。三个循环头字段均可为空；
条件为空时 Codegen 将其视为真。初始化支持 `let` 或赋值，更新支持赋值、后缀 `++` 和后缀 `--`。
Codegen 使用循环上下文栈解析 `break` 和 `continue` 的目标基本块，因此嵌套循环只影响最内层。

### 表达式

当前 Parser 可以构造：

- 整数字面量和字符串字面量。
- 标识符。
- 函数调用。
- 括号表达式。
- 二元算术与比较表达式。
- 可嵌套的固定长度数组字面量、多维数组索引表达式和指针索引表达式。
- 取地址表达式；`&value` 使用 `ReferenceNode` 保存可寻址目标。
- 一元负号；Parser 将 `-value` 转换成 `0 - value` 的 `BinaryOpNode`。

`BinaryOpType` 定义了以下操作符：

| 分类 | 操作符 |
|---|---|
| 算术 | `+`、`-`、`*`、`/` |
| 比较 | `==`、`!=`、`<`、`>`、`<=`、`>=` |
| 逻辑 | `&&`、`||` |

其中逻辑操作符目前只有 AST 枚举定义，Parser 和 Codegen 尚未完成对应处理。

### 字面量与类型

`LiteralNode` 使用 `LiteralType` 和联合字段保存值：

| `LiteralType` | 联合字段 |
|---|---|
| `LITERAL_INT`、`LITERAL_UINT` | `int_value` / `integer_text` |
| `LITERAL_I8`、`LITERAL_U8` | `int_value` / `integer_text` |
| `LITERAL_I16`、`LITERAL_U16` | `int_value` / `integer_text` |
| `LITERAL_I32`、`LITERAL_U32` | `int_value` / `integer_text` |
| `LITERAL_I64`、`LITERAL_U64` | `int_value` / `integer_text` |
| `LITERAL_I128`、`LITERAL_U128` | `int_value` / `integer_text` |
| `LITERAL_FLOAT`、`LITERAL_F32`、`LITERAL_F64` | `float_value` |
| `LITERAL_STRING` | `string_value` |
| `LITERAL_BOOL` | `bool_value` |

`VarTypeNode` 当前也复用 `LiteralType` 表示声明类型。需要区分：`LiteralNode.literal_type`
描述表达式中的值，`VarTypeNode.type` 描述变量、参数或函数返回值的类型注解。
`VarTypeNode.is_array` 和 `VarTypeNode.is_pointer` 通过 `element_type` 递归描述 `[T; N]`
和 `*T`，数组额外记录 `array_length`。

十进制整数字面量默认创建为 `LITERAL_I32`，同时在 `integer_text` 中保留原文，使 Codegen
可以直接构造超过 64 位的 `i128/u128` 常量。浮点和布尔构造函数已经存在，但尚未接入对应的
表达式解析流程。

## 链表关系

以下字段通过 `ASTNode.next` 连接：

| 链表头 | 元素类型 |
|---|---|
| `ProgramNode.imports` | `ImportNode` |
| `ProgramNode.functions` | `FunctionNode` |
| `FunctionNode.params` | `IdentifierNode` |
| `FunctionNode.type_params` | `IdentifierNode` |
| `FunctionNode.param_types` | `VarTypeNode` |
| `FunctionNode.body` | 语句节点 |
| `PrintNode.arguments` | 表达式节点 |
| `FunctionCallNode.arguments` | 表达式节点 |
| `FunctionCallNode.type_arguments` | `VarTypeNode` |
| `IfStatementNode.consequence` | 语句节点 |
| `IfStatementNode.alternative` | 语句链表或单个 `IfStatementNode` |

添加导入、函数、类型参数、参数、语句或实参时，应使用 `add_import()`、`add_function()`、
`add_type_param()`、`add_param()`、`add_param_type()`、`add_statement()`、`add_type_argument()`、
`add_argument()` 和 `add_print_argument()`。
这些函数目前通过遍历链表追加元素，单次追加的复杂度为 O(n)。

## 创建与所有权

节点由 `create_*` 系列函数分配。所有权约定如下：

- `create_function()`、`create_identifier()`、`create_var_decl()` 和
  `create_function_call()` 会复制传入的名称。
- `create_string_literal()` 会复制字符串内容。
- 传给构造函数的子节点在构造成功后归父节点所有，例如二元表达式的左右操作数。
- 节点加入链表后归链表所属节点所有，调用方不应单独释放。
- AST 根节点最终由 `free_ast((ASTNode *)program)` 统一释放。

`free_ast()` 会先递归释放 `next` 链表，再根据节点类型释放子节点、字符串和节点本身。

## 打印 AST

使用 `-parse` 可以检查 Parser 生成的 AST：

```bash
./build/tap -parse tests/run-pass/functions/fibonacci.tp
```

例如下面的代码：

```text
fn add(a: i32, b: i32): i32 {
    let result: i32 = a + b;
    return result;
}
```

对应的主要输出结构为：

```text
Program
  Function: add -> i32
    Param: a : i32
    Param: b : i32
    Body:
      VarDecl: result : i32
        init:
          BinaryOp: +
            Identifier: a
            Identifier: b
      Return
        Identifier: result
```

`print_ast()` 只用于调试和测试，不参与代码生成。

## 泛型单态化

Parser 把 `fn identity<T>(...)` 中的 `T` 保存到 `FunctionNode.type_params`，把
`identity<i32>(...)` 中的 `i32` 保存到 `FunctionCallNode.type_arguments`。模块与 Prelude
加载完成后，`specialize_generics()` 根据显式类型实参、普通实参类型和调用目标类型推断绑定，
克隆并替换函数 AST。例如 `identity<i32>(1)` 会生成内部函数 `identity$i32` 并改写调用名称。

相同类型组合只生成一个具体函数，递归泛型调用也会指向同一个具体实例。全部可达调用处理完成后，
泛型模板会从 `ProgramNode.functions` 删除，因此 Codegen 只接收具体类型。

## Codegen 如何消费 AST

单态化完成后，Codegen 对 `ProgramNode.functions` 执行两轮遍历：

1. 根据函数名、参数类型和返回类型创建全部 LLVM 函数声明。
2. 遍历每个函数体，为语句和表达式生成 LLVM IR。

表达式节点由表达式生成逻辑递归处理；函数体及条件分支按 `next` 链表顺序生成。添加新节点时，
必须同步检查 Codegen 是否需要新的分支处理。

## 扩展 AST 的检查清单

新增一种语法节点时，应依次完成：

1. 在 `NodeType` 中增加节点类型。
2. 定义以 `ASTNode base` 开头的具体结构体。
3. 添加构造函数并初始化全部字段。
4. 在 Parser 中创建并连接节点。
5. 在 `print_ast()` 中添加可读输出。
6. 在 `free_ast()` 中释放节点拥有的资源。
7. 在 Codegen 中实现对应语义。
8. 添加 Parser 输出测试和运行测试。

## 当前限制与注意事项

- `ImportNode` 和 `FunctionNode` 保存源文件、行号和列号，用于模块与重复定义诊断；其他 AST 节点仅按需保存源码位置。
- `LiteralType` 同时承担字面量类型和声明类型，后续类型系统扩展时应考虑拆分。
- `params` 与 `param_types` 使用平行链表；混合有类型和无类型参数时容易发生位置错配。
- `print_ast()` 当前直接访问 `VarDeclNode.type`，无类型变量声明可能导致空指针访问。
- `NODE_STATEMENT`、`NODE_EXPRESSION`、逻辑操作符以及浮点/布尔字面量解析仍是预留能力。
