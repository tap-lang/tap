#ifndef AST_H
#define AST_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// AST节点类型
enum NodeType {
    NODE_PROGRAM,                   // 程序节点
    NODE_IMPORT,                    // 模块导入节点
    NODE_FUNCTION,                  // 函数节点
    NODE_STATEMENT,                 // 语句节点
    NODE_EXPRESSION,                // 表达式节点
    NODE_IDENTIFIER,                // 标识符节点
    NODE_LITERAL,                   // 字面量节点
    NODE_RETURN,                    // 返回语句节点
    NODE_BINARY_OP,                 // 二元操作符节点
    NODE_REFERENCE,                 // 取地址表达式节点
    NODE_SIZEOF,                    // 编译期类型大小表达式节点
    NODE_VAR_DECL,                  // 变量声明节点
    NODE_ASSIGNMENT,                // 赋值语句节点
    NODE_ARRAY_LITERAL,             // 数组字面量节点
    NODE_INDEX_EXPRESSION,          // 数组索引表达式节点
    NODE_INDEX_ASSIGNMENT,          // 数组索引赋值节点
    NODE_ENUM,                      // 枚举声明节点
    NODE_ENUM_VARIANT,              // 枚举成员声明节点
    NODE_STRUCT,                    // 结构体声明节点
    NODE_STRUCT_FIELD,              // 结构体字段声明节点
    NODE_STRUCT_LITERAL,            // 结构体字面量节点
    NODE_STRUCT_INIT_FIELD,         // 结构体初始化字段节点
    NODE_FUNCTION_CALL,             // 函数调用节点
    NODE_IF_STATEMENT,              // 条件语句节点
    NODE_FOR_STATEMENT,             // for 循环节点
    NODE_BREAK_STATEMENT,           // break 语句节点
    NODE_CONTINUE_STATEMENT,        // continue 语句节点
    NODE_MATCH_STATEMENT,           // match 解构语句节点
    NODE_MATCH_ARM,                 // match 分支节点
    NODE_TRY,                       // ? 传播表达式节点
    NODE_VAR_TYPE                   // 数据类型节点
};

// 字面量类型
enum LiteralType {
    LITERAL_INT,                    // 整数 int
    LITERAL_UINT,                   // 无符号整数 uint
    LITERAL_I8,                     // 8位有符号整数 i8
    LITERAL_U8,                     // 8位无符号整数 u8
    LITERAL_I16,                    // 16位有符号整数 i16
    LITERAL_U16,                    // 16位无符号整数 u16
    LITERAL_I32,                    // 32位整数 i32
    LITERAL_U32,                    // 32位无符号整数 u32
    LITERAL_I64,                    // 64位整数 i64
    LITERAL_U64,                    // 64位无符号整数 u64
    LITERAL_I128,                   // 128位有符号整数 i128
    LITERAL_U128,                   // 128位无符号整数 u128
    LITERAL_FLOAT,                  // 浮点数 float
    LITERAL_F32,                    // 32位浮点数 f32
    LITERAL_F64,                    // 64位浮点数 f64
    LITERAL_STRING,                 // 字符串 string
    LITERAL_BOOL                    // 布尔值 bool
};

// 二元操作符类型  + - * / == != < > <= >= && ||
enum BinaryOpType {
    OP_ADD,                         // 加号 +
    OP_SUBTRACT,                    // 减号 -
    OP_MULTIPLY,                    // 乘号 *
    OP_DIVIDE,                      // 除号 /
    OP_MODULO,                      // 取模 %
    OP_EQUAL,                       // 等于号 ==
    OP_NOT_EQUAL,                   // 不等于号 !=
    OP_LESS_THAN,                   // 小于号 <
    OP_GREATER_THAN,                // 大于号 >
    OP_LESS_THAN_OR_EQUAL,          // 小于等于号 <=
    OP_GREATER_THAN_OR_EQUAL,       // 大于等于号 >=
    OP_AND,                         // 与运算符 &&
    OP_OR,                          // 或运算符 ||
    // 位运算必须追加在末尾：Codegen 用 `>= OP_EQUAL && <= OP_GREATER_THAN_OR_EQUAL`
    // 判断比较、用 `>= OP_ADD && <= OP_MODULO` 判断算术，插在中间会破坏这两个范围判断。
    OP_BITWISE_AND,                 // 按位与 &
    OP_BITWISE_OR,                  // 按位或 |
    OP_BITWISE_XOR,                 // 按位异或 ^
    OP_SHIFT_LEFT,                  // 左移 <<
    OP_SHIFT_RIGHT                  // 右移 >>
};

// 基础AST节点结构
typedef struct ASTNode {
    enum NodeType type;             // 节点类型
    struct ASTNode *next;           // 用于链表结构
} ASTNode;

// 程序节点
typedef struct {
    ASTNode base;
    ASTNode *imports;      // 模块导入列表
    ASTNode *enums;        // 枚举声明列表
    ASTNode *structs;      // 结构体声明列表
    ASTNode *constants;    // 顶层常量列表
    ASTNode *functions;    // 函数列表
} ProgramNode;

// 模块导入节点
//
// 支持四种写法：
//   import std.math;                 整模块导入，按 `math.sin(...)` 访问（名称空间）
//   import std.math as m;            整模块导入并改名，按 `m.sin(...)` 访问
//   import std.math.*;               通配导入，pub 成员直接按裸名 `sin(...)` 访问
//   import std.math.sin;             单成员导入，按裸名 `sin(...)` 访问
//   import std.math.sin as sine;     单成员导入并改名，按裸名 `sine(...)` 访问
typedef struct {
    ASTNode base;
    char *module_name;     // 点分隔模块名，例如 std.math（解析后已去掉成员段）
    char *member;          // 单成员导入的成员名；NULL 表示整模块/通配导入
    char *alias;           // 绑定的裸名（名称空间导入时是名称空间）；通配导入为 NULL
    int is_wildcard;       // 1 表示 `import mod.*`：导入全部 pub 成员
    char *filename;        // import 所在文件
    int line;              // 模块名所在行
    int column;            // 模块名所在列
} ImportNode;

// 变量类型节点
typedef struct VarTypeNode {
    ASTNode base;
    enum LiteralType type;  // 标量类型，数组节点中保留最终元素类型
    char *enum_name;        // 非空时表示枚举类型，后端按 i32 降低
    char *struct_name;      // 非空时表示结构体类型
    ASTNode *type_arguments; // 泛型具名类型的实参列表（VarTypeNode）
    int is_array;
    int is_pointer;
    uint64_t array_length;
    struct VarTypeNode *element_type; // 数组/指针拥有的递归元素类型
} VarTypeNode;

// 枚举成员声明节点
typedef struct {
    ASTNode base;
    char *name;             // 成员名
    ASTNode *payload_types; // 载荷类型列表（VarTypeNode），无载荷时为 NULL
    unsigned tag;           // 降级时回填：从 0 开始的成员序号
    unsigned field_index;   // 降级时回填：首个载荷字段在内部结构体中的下标
} EnumVariantNode;

// 枚举声明节点
typedef struct {
    ASTNode base;
    char *name;            // 枚举类型名
    ASTNode *type_params;  // 泛型类型参数列表（IdentifierNode）
    ASTNode *variants;     // 枚举成员列表（EnumVariantNode）
    int has_payload;       // 任一成员带载荷时置位；由降级 pass 回填
    int is_pub;            // pub 声明：可以被其他模块导入
} EnumNode;

// 结构体声明节点
typedef struct {
    ASTNode base;
    char *name;            // 结构体类型名
    ASTNode *type_params;  // 泛型类型参数列表（IdentifierNode）
    ASTNode *fields;       // 字段声明列表（StructFieldNode）
    int is_tagged_enum;    // 由载荷枚举降级生成，而非用户声明
    char *tagged_enum_name; // 降级生成时记录来源枚举名；实例化后名字带 $ 后缀，靠它映射回枚举
    int is_pub;            // pub 声明：可以被其他模块导入
} StructNode;

// 结构体字段声明节点
typedef struct {
    ASTNode base;
    char *name;            // 字段名
    VarTypeNode *field_type; // 字段类型
} StructFieldNode;

// 结构体字面量节点
typedef struct {
    ASTNode base;
    char *struct_name;     // 结构体类型名
    ASTNode *type_arguments; // 泛型结构体实参列表（VarTypeNode）
    ASTNode *fields;       // 初始化字段列表（StructInitFieldNode）
} StructLiteralNode;

// 结构体初始化字段节点
typedef struct {
    ASTNode base;
    char *name;            // 字段名
    ASTNode *expression;   // 字段初始化表达式
} StructInitFieldNode;

// 函数节点
typedef struct {
    ASTNode base;
    char *name;            // 函数名
    char *original_name;   // 改名前的原名；模块私有符号被改名成 __tap_module_N.name 后，
                           // 诊断优先用原名，避免泄漏内部符号名
    char *filename;        // 函数定义所在文件
    int line;              // 函数名所在行
    int column;            // 函数名所在列
    int is_extern;         // 外部函数只生成 LLVM 声明
    int is_variadic;       // 参数列表末尾带 `...`（变参 ABI，仅 extern 允许）
    int is_pub;            // pub 声明：可以被其他模块导入
    ASTNode *type_params;  // 泛型类型参数列表（IdentifierNode）
    ASTNode *params;       // 参数列表
    ASTNode *param_types;  // 参数类型列表
    ASTNode *body;         // 函数体语句列表
    VarTypeNode *return_type; // 返回值类型
} FunctionNode;

// 标识符节点
typedef struct {
    ASTNode base;
    char *name;            // 标识符名称
    ASTNode *enum_type_arguments; // 显式枚举类型实参，仅 Enum<T>.Member 形式使用
} IdentifierNode;

// 字面量节点
typedef struct {
    ASTNode base;
    enum LiteralType literal_type;
    char *integer_text;       // 十进制整数原文，用于构造 i128/u128 常量
    union {
        uint64_t int_value;
        char *string_value;
        double float_value;
        int bool_value;
    } value;
} LiteralNode;

// 返回语句节点
typedef struct {
    ASTNode base;
    ASTNode *expression;   // 返回表达式
} ReturnNode;

// 二元操作节点
typedef struct {
    ASTNode base;
    enum BinaryOpType op_type;
    ASTNode *left;         // 左操作数
    ASTNode *right;        // 右操作数
} BinaryOpNode;

// 取地址表达式节点
typedef struct {
    ASTNode base;
    ASTNode *target;       // 被取地址的可寻址表达式
} ReferenceNode;

// 编译期类型大小表达式节点
typedef struct {
    ASTNode base;
    VarTypeNode *operand_type; // 要计算 ABI 大小的完整类型
} SizeofNode;


// 变量声明节点
typedef struct {
    ASTNode base;
    char *name;            // 变量名
    VarTypeNode *type;     // 变量类型
    ASTNode *expression;   // 初始化表达式
    int is_const;          // 是否为 const 常量声明
    int is_pub;            // pub 声明：可以被其他模块导入（仅顶层常量有意义）
} VarDeclNode;

// 赋值语句节点
typedef struct {
    ASTNode base;
    char *name;
    ASTNode *expression;
} AssignmentNode;

typedef struct {
    ASTNode base;
    ASTNode *elements;
    uint64_t count;
    int is_repeat;
    uint64_t repeat_count;
} ArrayLiteralNode;

typedef struct {
    ASTNode base;
    ASTNode *array;
    ASTNode *index;
} IndexExpressionNode;

typedef struct {
    ASTNode base;
    IndexExpressionNode *target;
    ASTNode *expression;
} IndexAssignmentNode;


// 函数调用节点
typedef struct {
    ASTNode base;
    char *filename;
    int line;
    int column;
    char *name;            // 函数名
    ASTNode *type_arguments; // 显式泛型实参列表（VarTypeNode），用于泛型函数
    ASTNode *enum_type_arguments; // 显式枚举类型实参，仅 Enum<T>.Member(...) 构造使用
    ASTNode *arguments;    // 参数列表
    int forwards_variadic; // 实参列表末尾带 `...`，表示把当前函数的变参原样转发给被调用者
} FunctionCallNode;

// match 分支节点
typedef struct {
    ASTNode base;
    char *enum_name;       // 模式中的枚举名；通配分支为 NULL
    char *variant_name;    // 模式中的成员名；通配分支为 NULL
    ASTNode *bindings;     // 载荷绑定变量列表（IdentifierNode），可空
    ASTNode *body;         // 分支语句链表（语句形式）
    ASTNode *value;        // 分支值表达式（表达式形式），二选一
    char *filename;        // 分支所在文件，用于诊断
    int line;
    int column;
} MatchArmNode;

// match 解构语句节点
typedef struct {
    ASTNode base;
    ASTNode *expression;   // 被匹配的枚举值
    ASTNode *arms;         // MatchArmNode 链表
} MatchStatementNode;

// `?` 传播表达式节点。
// 内层求值为 Result：成功时整个表达式取载荷值，失败时从当前函数提前返回 Err。
typedef struct {
    ASTNode base;
    ASTNode *inner;        // 被传播的 Result 表达式
    char *filename;        // 所在文件，用于诊断
    int line;
    int column;
} TryNode;

// 创建节点的函数声明
ProgramNode *create_program();
ImportNode *create_import(
    const char *module_name, const char *member, const char *alias, int is_wildcard,
    const char *filename, int line, int column);
FunctionNode *create_function(char *name);
EnumNode *create_enum(char *name);
EnumVariantNode *create_enum_variant(char *name);
StructNode *create_struct(char *name);
StructFieldNode *create_struct_field(char *name, VarTypeNode *field_type);
StructLiteralNode *create_struct_literal(char *struct_name);
StructInitFieldNode *create_struct_init_field(char *name, ASTNode *expression);
IdentifierNode *create_identifier(char *name);
VarTypeNode *create_var_type(enum LiteralType type);
VarTypeNode *create_enum_type(const char *name);
VarTypeNode *create_struct_type(const char *name);
// 数组类型接管 element_type 的所有权。
VarTypeNode *create_array_type(VarTypeNode *element_type, uint64_t length);
// 指针类型接管 element_type 的所有权。
VarTypeNode *create_pointer_type(VarTypeNode *element_type);
LiteralNode *create_int_literal(uint64_t value);
LiteralNode *create_int_literal_text(const char *value);
LiteralNode *create_string_literal(char *value);
LiteralNode *create_float_literal(double value);
LiteralNode *create_bool_literal(int value);
ReturnNode *create_return(ASTNode *expression);
BinaryOpNode *create_binary_op(enum BinaryOpType op_type, ASTNode *left, ASTNode *right);
ReferenceNode *create_reference(ASTNode *target);
SizeofNode *create_sizeof(VarTypeNode *operand_type);
VarDeclNode *create_var_decl(
    char *name, VarTypeNode *type, ASTNode *expression, int is_const);
AssignmentNode *create_assignment(const char *name, ASTNode *expression);
ArrayLiteralNode *create_array_literal(void);
void add_array_element(ArrayLiteralNode *array, ASTNode *element);
void set_array_repeat(ArrayLiteralNode *array, uint64_t repeat_count);
IndexExpressionNode *create_index_expression(ASTNode *array, ASTNode *index);
IndexAssignmentNode *create_index_assignment(
    IndexExpressionNode *target, ASTNode *expression);
FunctionCallNode *create_function_call(const char *name);

// 添加子节点的函数
void add_function(ProgramNode *program, FunctionNode *function);
void add_constant(ProgramNode *program, VarDeclNode *constant);
void add_enum(ProgramNode *program, EnumNode *enum_node);
void add_enum_type_param(EnumNode *enum_node, IdentifierNode *type_param);
void add_enum_variant(EnumNode *enum_node, EnumVariantNode *variant);
void add_enum_variant_payload(EnumVariantNode *variant, VarTypeNode *type);
MatchStatementNode *create_match_statement(ASTNode *expression);
TryNode *create_try(ASTNode *inner);
MatchArmNode *create_match_arm(char *enum_name, char *variant_name);
void add_match_arm(MatchStatementNode *statement, MatchArmNode *arm);
void add_match_binding(MatchArmNode *arm, IdentifierNode *binding);
void add_struct(ProgramNode *program, StructNode *struct_node);
void add_struct_type_param(StructNode *struct_node, IdentifierNode *type_param);
void add_struct_field(StructNode *struct_node, StructFieldNode *field);
void add_var_type_argument(VarTypeNode *type, VarTypeNode *type_argument);
void add_struct_literal_type_argument(
    StructLiteralNode *literal, VarTypeNode *type_argument);
void add_struct_init_field(StructLiteralNode *literal, StructInitFieldNode *field);
void add_import(ProgramNode *program, ImportNode *import_node);
void add_param(FunctionNode *function, IdentifierNode *param);
void add_type_param(FunctionNode *function, IdentifierNode *type_param);
void add_param_type(FunctionNode *function, VarTypeNode *type);
void add_statement(FunctionNode *function, ASTNode *statement);
void add_argument(FunctionCallNode *function_call, ASTNode *argument);
void add_type_argument(FunctionCallNode *function_call, VarTypeNode *type_argument);
void append_type_argument(ASTNode **list, VarTypeNode *type_argument);

// 条件语句节点
typedef struct {
    ASTNode base;
    ASTNode *condition;    // 条件表达式
    ASTNode *consequence;  // 条件为真时执行的语句
    ASTNode *alternative;  // 条件为假时执行的语句
} IfStatementNode;

// 创建条件语句节点
IfStatementNode *create_if_statement(ASTNode *condition, ASTNode *consequence, ASTNode *alternative);

// for 循环节点
typedef struct {
    ASTNode base;
    ASTNode *initializer;
    ASTNode *condition;
    ASTNode *update;
    ASTNode *body;
} ForStatementNode;

ForStatementNode *create_for_statement(
    ASTNode *initializer, ASTNode *condition, ASTNode *update, ASTNode *body);
ASTNode *create_break_statement(void);
ASTNode *create_continue_statement(void);

// 释放AST的函数
void free_ast(ASTNode *node);

// 将语法树打印到 stdout（用于 -parse）
void print_ast(const ProgramNode *program);

#endif // AST_H
