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
- 泛型单态化处理泛型调用和泛型结构体，生成具体声明并移除泛型模板。
- Codegen 遍历不含泛型类型参数的 AST，生成函数、表达式和控制流对应的 LLVM IR。
- 编译流程结束后，调用 `free_ast()` 释放 AST。

## 基础表示

所有具体节点都把 `ASTNode` 放在结构体首字段：

```c
typedef struct {
    const char *file;
    uint32_t line;
    uint32_t column;
} SourceLoc;

typedef struct ASTNode {
    enum NodeType type;
    struct ASTNode *next;
    SourceLoc loc;
    struct VarTypeNode *resolved_type;   // 缓存的完整类型；NULL 表示尚未推断
} ASTNode;
```

因此具体节点指针可以转换为 `ASTNode *`，再通过 `type` 判断实际类型。`next` 用于组织链表，
例如函数列表、语句列表和参数列表；它不是语法树中的普通子节点。

创建节点时必须满足两个基础约定：

1. `base.type` 设置为对应的 `NodeType`。
2. `base.next` 初始化为 `NULL`。

构造函数统一用 `calloc` 分配，所以 `loc` 和 `resolved_type` 默认全 0（位置未知、类型未推断），
不需要逐个显式初始化。

## 源码位置

位置放在 `ASTNode` 里而不是每个节点各存一份三字段，诊断代码因此不必先知道具体节点类型：
`print_node_diagnostic(stderr, "error", (ASTNode *)node, "...")` 直接取 `node->loc`。

位置的填充只有两个入口：

- `parse_statement()` —— 所有语句
- `parse_expression()` —— 所有表达式（含递归进入的内层表达式）

两者在分发**之前**记下当前 token 的位置，返回时调 `stamp_location()`。`stamp_location()`
只填 `file` 为空的节点，所以内层（更精确）的位置优先，外层不会覆盖。

`SourceLoc.file` 指向 `intern_source_file()` 的返回值，**不归节点所有，`free_ast` 不释放**。
之所以要 intern：`module.c` / `prelude.c` 解析完就把模块路径 `free` 掉，而 Lexer 的
`filename` 只是借用那个指针；节点也不能各自 `strdup`（几万个节点就是几万份文件名）。
intern 表是进程级的，同一个路径只存一份，进程退出时由操作系统回收。

## 缓存的类型（resolved_type）

`ASTNode.resolved_type` 存表达式的完整类型（标量也有）。**节点拥有它，`free_ast` 负责释放。**

填充有两条路，写的是同一个字段：

1. `sema.c` 在 Codegen 之前跑一遍，为能确定的表达式填入类型；
2. Codegen 的 `expression_full_type()` 遇到还没填的节点才自己推断，并把结果缓存回去。

所以 `resolved_type` 为 NULL 只表示「还没人算过」，不表示出错。`expression_type()` 也会
优先读它（只认纯标量，数组/指针/结构体/枚举仍走各自带诊断的分支）。设置
`TAP_SEMA_STATS=1` 可以打印 Sema 的覆盖率和 Codegen 的缓存命中率。

缓存是安全的：表达式节点在 AST 里唯一，符号表状态由它在函数里的位置决定，推断结果不随
调用时机变化。有两个地方会重置它，都是自动的：

- 构造函数用 `calloc`，新节点的 `resolved_type` 天然是 NULL；
- 泛型单态化克隆 AST 时走构造函数，克隆出的节点同样从 NULL 开始，会重新推断。

Sema 目前是**影子模式**：只填类型，不接管诊断，推不出来就返回 NULL 交给 Codegen。这样
任何时刻的行为都和没有 Sema 时一致，覆盖率可以逐步提高而不会一次引入大面积回归。

## 节点类型

| `NodeType` | 结构体 | 主要字段与含义 |
|---|---|---|
| `NODE_PROGRAM` | `ProgramNode` | `imports` / `enums` / `structs` / `constants` / `functions` / `traits` / `impls` 分别指向对应的顶层声明链表 |
| `NODE_IMPORT` | `ImportNode` | 模块名、单成员名、绑定裸名、通配标记及导入声明的源文件位置 |
| `NODE_FUNCTION` | `FunctionNode` | 函数名、泛型类型参数、普通参数、返回类型和函数体；`is_extern` 标记 C ABI 外部声明，`is_variadic` 标记参数列表末尾的 `...`，`is_pub` 标记可被其他模块导入 |
| `NODE_STRUCT` | `StructNode` | 结构体名、泛型类型参数和字段声明链表；`is_tagged_enum` 标记它由载荷枚举降级生成，`is_pub` 标记可被其他模块导入 |
| `NODE_STRUCT_LITERAL` | `StructLiteralNode` | 结构体名、泛型类型实参和字段初始化链表 |
| `NODE_ENUM` | `EnumNode` | 枚举名和成员声明链表；`has_payload` 标记是否带载荷，`is_pub` 标记可被其他模块导入 |
| `NODE_ENUM_VARIANT` | `EnumVariantNode` | 成员名、载荷类型链表，以及降级时回填的 `tag` 和 `field_index` |
| `NODE_IDENTIFIER` | `IdentifierNode` | `name` 保存标识符名称；`enum_type_arguments` 保存 `Enum<T>.Member` 的显式枚举类型实参；作为泛型类型参数时 `type_bounds` 保存 `T: Trait` 的约束（trait 名的 `VarTypeNode` 链表） |
| `NODE_LITERAL` | `LiteralNode` | 字面量类型及对应的联合值 |
| `NODE_RETURN` | `ReturnNode` | `expression` 指向返回表达式 |
| `NODE_BINARY_OP` | `BinaryOpNode` | `op_type` 加左右操作数；算术、比较、逻辑和位运算共用 |
| `NODE_BINARY_OP` | `BinaryOpNode` | 操作符、左操作数和右操作数 |
| `NODE_REFERENCE` | `ReferenceNode` | `target` 指向被取地址的变量、结构体字段或索引表达式 |
| `NODE_SIZEOF` | `SizeofNode` | `operand_type` 保存 `sizeof(T)` 中的完整类型 |
| `NODE_VAR_DECL` | `VarDeclNode` | 变量名、可选类型、初始化表达式，以及 `is_const` 声明标记；顶层常量用 `is_pub` 标记可被其他模块导入 |
| `NODE_ASSIGNMENT` | `AssignmentNode` | 被赋值变量名和新的值表达式 |
| `NODE_ARRAY_LITERAL` | `ArrayLiteralNode` | 初始化元素链表和元素数量 |
| `NODE_INDEX_EXPRESSION` | `IndexExpressionNode` | 数组表达式和下标表达式 |
| `NODE_INDEX_ASSIGNMENT` | `IndexAssignmentNode` | 索引目标和新的元素值 |
| `NODE_FUNCTION_CALL` | `FunctionCallNode` | 函数名、显式泛型类型实参和普通实参链表；`enum_type_arguments` 保存 `Enum<T>.Member(...)` 的显式枚举类型实参，`forwards_variadic` 标记实参列表末尾的 `...` 转发 |
| `NODE_IF_STATEMENT` | `IfStatementNode` | 条件、真分支和假分支 |
| `NODE_FOR_STATEMENT` | `ForStatementNode` | 初始化、条件、更新和循环体 |
| `NODE_BREAK_STATEMENT` | `ASTNode` | 结束当前循环 |
| `NODE_CONTINUE_STATEMENT` | `ASTNode` | 跳到当前循环的更新块 |
| `NODE_MATCH_STATEMENT` | `MatchStatementNode` | 被匹配表达式和分支链表 |
| `NODE_MATCH_ARM` | `MatchArmNode` | 模式（`enum_name` / `variant_name`，通配分支两者为 `NULL`）、载荷绑定变量链表、分支体（`body`）或分支值（`value`） |
| `NODE_TRY` | `TryNode` | `?` 传播表达式的内层表达式，以及用于诊断的文件和行列 |
| `NODE_TRAIT` | `TraitNode` | trait 名、方法签名链表和 `is_pub` |
| `NODE_TRAIT_METHOD` | `TraitMethodNode` | 方法名、`has_self` 接收者标记、除接收者外的参数类型链表和返回类型 |
| `NODE_IMPL` | `ImplNode` | 实现的 trait 名、目标类型名，以及块内方法的引用链表（`ImplMethodLink`，指向脱糖后的 `FunctionNode`）；块内方法在解析时就被改名成 `<方法名>$<类型名>` 并挂进 `ProgramNode.functions` |
| `NODE_FIELD_ACCESS` | `FieldAccessNode` | `接收者.字段名`，`object` 是任意表达式；只用于**读取**字段。标识符接收者的一层访问（`a.b`）仍走 `IdentifierNode` 的名字拼接，这个节点表示接收者不是标识符（`arr[0].x` / `make().x`）或多于一层（`a.b.c`）的情况 |
| `NODE_VAR_TYPE` | `VarTypeNode` | 标量、结构体、枚举和泛型类型实参，或通过 `element_type` 递归表示固定长度数组和指针 |

`NODE_STATEMENT` 和 `NODE_EXPRESSION` 当前只是枚举占位项，没有对应的结构体、构造函数或
Parser 产物。

### 程序与函数

AST 根节点是 `ProgramNode`：

```text
ProgramNode
├── imports -> ImportNode -> ImportNode -> ...
├── enums / structs / constants / functions -> ...
└── traits / impls -> ...
```

`ImportNode.module_name` 保存点分隔模块名，`member` 保存单成员导入的成员名（整模块/通配导入
为 `NULL`），`alias` 保存绑定的裸名（名称空间导入时是名称空间，通配导入为 `NULL`），
`is_wildcard` 标记 `import mod.*`。Parser 先按点分路径贪心读入 `module_name`，成员段与模块
路径的切分由 Module Loader 按文件系统完成（整条路径和去掉最后一段的前缀都是模块时报歧义，
详见[模块导入文档](module.md)）。
Module Loader 消费导入列表、解析限定/裸名函数调用并合并模块函数，Codegen 不直接处理
`ImportNode`。

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
- `IfStatementNode`：`if`、`elseif`（或等价的 `else if`）和 `else`。
- `ForStatementNode`：经典三段式 `for` 循环。
- `NODE_BREAK_STATEMENT`：`break;`，结束最内层循环。
- `NODE_CONTINUE_STATEMENT`：`continue;`，进入最内层循环的更新阶段。
- `MatchStatementNode`：`match (value) { Enum.Member(a, b) => ... }`，解构带载荷枚举；
  分支体可以是代码块或单条语句，`_` 表示通配分支。

`IfStatementNode.consequence` 指向真分支的语句链表。`alternative` 一律是语句链表：

- `else`：指向块内语句组成的链表。
- `elseif` / `else if`：指向长度为 1 的链表，表头就是那个 `IfStatementNode`（它自己的
  `next` 为 `NULL`），形成嵌套条件链。`else if` 是 `elseif` 的等价写法，两者生成的 AST
  完全相同。

消费方（AST 转储、Codegen、泛型单态化克隆）一律按链表遍历，**不要**用
`alternative->type == NODE_IF_STATEMENT` 去区分这两种形态：else 块的链表表头恰好是 `if`
语句时同样命中，那样会把块内其余语句整段丢掉（曾经的真实 bug，回归用例见
`tests/run-pass/control-flow/else_block_leading_if.tp`）。

`ForStatementNode` 对应 `for (initializer; condition; update) { body }`。三个循环头字段均可为空；
条件为空时 Codegen 将其视为真。初始化支持 `let` 或赋值，更新支持赋值、后缀 `++` 和后缀 `--`。
Codegen 使用循环上下文栈解析 `break` 和 `continue` 的目标基本块，因此嵌套循环只影响最内层。

### 表达式

当前 Parser 可以构造：

- 整数字面量和字符串字面量。
- 标识符。
- 函数调用。
- 括号表达式。
- 二元算术、比较与逻辑表达式。
- 可嵌套的固定长度数组字面量、多维数组索引表达式和指针索引表达式。
- 取地址表达式；`&value` 使用 `ReferenceNode` 保存可寻址目标。
- 解引用表达式；`*pointer` 使用 `DereferenceNode` 保存被解引用的指针。只支持取值，
  `*p = x` 这种左值写法还不支持（写操作用 `p[0] = x`）。
- 类型大小表达式；`sizeof(T)` 使用 `SizeofNode` 保存完整类型并产生 `uint` 常量。
- 一元前缀；Parser 将 `-value` 转换成 `0 - value` 的 `BinaryOpNode`，
  将 `!value` 转换成 `value == false` 的 `BinaryOpNode`。

`BinaryOpType` 定义了以下操作符：

| 分类 | 操作符 |
|---|---|
| 算术 | `+`、`-`、`*`、`/`、`%` |
| 比较 | `==`、`!=`、`<`、`>`、`<=`、`>=` |
| 逻辑 | `&&`、`||` |

一元前缀操作符为 `&`（取地址）、`*`（解引用）、`-`（一元负号）和 `!`（逻辑非）。
因子位置的 `*` 一定是前缀：二元乘号由 `parse_term` 处理，所以 `2 * *p` 读作 `2 * (*p)`。

优先级由低到高为：`||` → `&&` → 比较 → `+ -` → `* / %` → 一元前缀 → 下标与方法。
`%` 与 `*`、`/` 同级且左结合，按操作数是否有符号分别生成 `srem` 或 `urem`。
`&&` 和 `||` 在 Codegen 中按短路求值生成：左侧结果已能决定整体结果时，右侧完全不求值。

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
| `LITERAL_NULL` | 无（见下） |

`LITERAL_NULL` 是唯一**没有自己类型**的字面量。它只能出现在能确定 `*T` 目标类型的位置：
`let p: *T = null`、`p == null` / `p != null`、作为 `*T` 实参。所以：

- `expression_full_type()` 对它返回 `NULL`（不是 `create_var_type(LITERAL_NULL)`），
  否则这个伪类型会泄进 `tap_type_equal` / `scalar_types_compatible` 等所有类型运算；
- `expression_type()` 返回默认整数只是占位，真正类型由 `generate_expression_for_type()`
  的目标类型决定；
- 其余没有目标类型的位置一律报 `null needs a target type`，不猜。

解引用时插入运行时检查（和除零、数组越界同一套 `emit_integer_guard`），所以
`let y = *p` 在 `p` 是 null 时输出 `Null pointer dereference` 并退出，而不是段错误。

`VarTypeNode` 当前也复用 `LiteralType` 表示声明类型。需要区分：`LiteralNode.literal_type`
描述表达式中的值，`VarTypeNode.type` 描述变量、参数或函数返回值的类型注解。
`VarTypeNode.is_array` 和 `VarTypeNode.is_pointer` 通过 `element_type` 递归描述 `[T; N]`
和 `*T`，数组额外记录 `array_length`。具名类型的 `type_arguments` 保存 `Vec<i32>` 中的
`i32` 等泛型实参。

十进制整数字面量默认创建为 `LITERAL_I32`，同时在 `integer_text` 中保留原文，使 Codegen
可以直接构造超过 64 位的 `i128/u128` 常量。浮点字面量（`1.5` 和 `1e10` 两种写法）创建为
`LITERAL_F32`，布尔字面量 `true` / `false` 创建为 `LITERAL_BOOL`。

## 链表关系

以下字段通过 `ASTNode.next` 连接：

| 链表头 | 元素类型 |
|---|---|
| `ProgramNode.imports` | `ImportNode` |
| `ProgramNode.functions` | `FunctionNode` |
| `FunctionNode.params` | `IdentifierNode` |
| `FunctionNode.type_params` | `IdentifierNode` |
| `StructNode.type_params` | `IdentifierNode` |
| `StructNode.fields` | `StructFieldNode` |
| `FunctionNode.param_types` | `VarTypeNode` |
| `FunctionNode.body` | 语句节点 |
| `FunctionCallNode.arguments` | 表达式节点 |
| `FunctionCallNode.type_arguments` | `VarTypeNode` |
| `VarTypeNode.type_arguments` | `VarTypeNode` |
| `StructLiteralNode.type_arguments` | `VarTypeNode` |
| `IfStatementNode.consequence` | 语句节点 |
| `IfStatementNode.alternative` | 语句节点（一律按链表遍历，`elseif` 是长度 1 的链表） |

添加导入、函数、类型参数、参数、语句或实参时，应使用 `add_import()`、`add_function()`、
`add_type_param()`、`add_param()`、`add_param_type()`、`add_statement()`、`add_type_argument()`、
`add_argument()` 和 `add_type_argument()`。
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

## 载荷枚举的降级

模块和 Prelude 加载完成后、泛型单态化之前，`lower_payload_enums()`（`src/tagged.c`）会把
**带载荷**的枚举展开成一个同名 `StructNode`：字段 `0` 是 `i32` 判别标签，之后按声明顺序
展平各成员的载荷，字段名形如 `circle$0`（`$` 在源语言中写不出来，不会与用户字段冲突）。
生成的 `StructNode` 带 `is_tagged_enum` 标记，用于和用户声明的同名结构体区分。

降级同时回填每个成员的 `tag` 和 `field_index`。无载荷的枚举只回填 `tag`，保持 `i32` 表示，
不做其他改写——这样既有的枚举语义完全不变。

泛型枚举的 `type_params` 会被搬到生成的结构体上，因此泛型单态化能照常按实参实例化出
`Option$i32`。实例化后的结构体保留 `tagged_enum_name` 记录来源枚举名，Codegen 靠它把
`Option$i32` 映射回 `Option` 枚举声明：判别标签和字段下标取自模板枚举（各次实例化都相同），
字段类型取自实例化后的结构体。

之所以选择展开成结构体而不是让 Codegen 直接理解枚举，是因为结构体的传参、返回、字段访问和
泛型实例化路径都已经存在，展开之后这些都能直接复用。代价是内存布局不紧凑（所有成员的载荷
并存），因此布局被当作内部实现细节：源码只能通过构造和 `match` 访问枚举值，将来换更紧凑的
编码不会影响源语言。

`validate_var_type()` 负责把类型注解中的载荷枚举从 `enum_name` 改写成 `struct_name`；
判定顺序是「枚举存在且不带载荷 → 保持枚举，否则查同名结构体」。

## 泛型单态化

Parser 把 `fn identity<T>(...)` 中的 `T` 保存到 `FunctionNode.type_params`，把
`identity<i32>(...)` 中的 `i32` 保存到 `FunctionCallNode.type_arguments`。泛型结构体的
参数和实参分别保存在 `StructNode.type_params`、`VarTypeNode.type_arguments` 以及
`StructLiteralNode.type_arguments`。模块与 Prelude
加载完成后，`specialize_generics()` 根据显式类型实参、普通实参类型和调用目标类型推断绑定，
克隆并替换函数 AST。例如 `identity<i32>(1)` 会生成内部函数 `identity$i32` 并改写调用名称。
克隆 `SizeofNode` 时也会替换 `operand_type`，因此泛型函数可以使用 `sizeof(T)`。

相同类型组合只生成一个具体函数或结构体，递归泛型调用也会指向同一个具体实例。例如
`Vec<i32>` 会生成带稳定内部名称的具体结构体。全部可达调用处理完成后，泛型模板会从
`ProgramNode.functions` 和 `ProgramNode.structs` 删除，因此 Codegen 只接收具体类型。

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
3. 添加构造函数并初始化全部字段。用 `calloc` 分配，`base.loc` 留全 0 —— 语句和表达式
   的位置由 Parser 在 `parse_statement()` / `parse_expression()` 里统一填充。
4. 在 Parser 中创建并连接节点。**新增语句只需要接入 `parse_statement()` 一处**：`parse_block`
   和函数体都走它，不要再写第二份语句分派。
5. 在 `print_ast()` 中添加可读输出。
6. 在 `free_ast()` 中释放节点拥有的资源。
7. 在 `module.c` 的 `rewrite_expression()` 里补分支 —— 漏了会在跨模块改名时把子表达式丢掉。
8. 在 `generics.c` 补三处：`clone_expression()`（克隆，漏了报 `cannot clone generic
   expression type N`）、`materialize_expression()`（补全嵌套泛型实例）、
   `process_expression()`（类型推断）。
9. 在 `sema.c` 的 `compute_expression_type()` 里补类型推断。漏了不致命（会回退到 Codegen
   推断），但 `TAP_SEMA_STATS=1` 的覆盖率会掉。
10. 在 Codegen 里补三个 switch：`expression_full_type()`、`expression_type()`、
    `generate_expression()`。
11. 添加 Parser 输出测试和运行测试。

第 7、8 步最容易漏，而且症状都不直观：漏了 `rewrite_expression` 是「某些模块组合下行为诡异」，
漏了 `clone_expression` 是直接报一个内部错误。

## 当前限制与注意事项

- 语句和表达式的源码位置由 Parser 自动填充（见「源码位置」），新增节点不必自己处理。
  顶层声明（函数 / 结构体 / 枚举 / trait / impl）不走 `parse_statement()`，其中
  `FunctionNode` 仍保留自己的 `filename` / `line` / `column` 三字段，尚未迁到 `base.loc`。
- `LiteralType` 同时承担字面量类型和声明类型，后续类型系统扩展时应考虑拆分。
- `params` 与 `param_types` 使用平行链表；混合有类型和无类型参数时容易发生位置错配。
- `print_ast()` 当前直接访问 `VarDeclNode.type`，无类型变量声明可能导致空指针访问。
- `NODE_STATEMENT` 和 `NODE_EXPRESSION` 仍是预留能力，没有对应的结构体、构造函数或
  Parser 产物。
