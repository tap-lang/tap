#ifndef AST_H
#define AST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// AST节点类型
enum NodeType {
    NODE_PROGRAM,                   // 程序节点
    NODE_FUNCTION,                  // 函数节点
    NODE_STATEMENT,                 // 语句节点
    NODE_EXPRESSION,                // 表达式节点
    NODE_IDENTIFIER,                // 标识符节点
    NODE_LITERAL,                   // 字面量节点
    NODE_RETURN,                    // 返回语句节点
    NODE_PRINT,                     // 打印语句节点
    NODE_BINARY_OP,                 // 二元操作符节点
    NODE_VAR_DECL,                  // 变量声明节点
    NODE_FUNCTION_CALL,             // 函数调用节点
    NODE_IF_STATEMENT               // 条件语句节点
};

// 字面量类型
enum LiteralType {
    LITERAL_INT,                    // 整数
    LITERAL_STRING,                 // 字符串
    LITERAL_FLOAT,                  // 浮点数
    LITERAL_BOOL                    // 布尔值
};

// 二元操作符类型  + - * / == != < > <= >= && ||
enum BinaryOpType {
    OP_ADD,                         // 加号
    OP_SUBTRACT,                    // 减号
    OP_MULTIPLY,                    // 乘号
    OP_DIVIDE,                      // 除号
    OP_EQUAL,                       // 等于号
    OP_NOT_EQUAL,                   // 不等于号
    OP_LESS_THAN,                   // 小于号
    OP_GREATER_THAN,                // 大于号
    OP_LESS_THAN_OR_EQUAL,          // 小于等于号
    OP_GREATER_THAN_OR_EQUAL,       // 大于等于号
    OP_AND,                         // 与运算符
    OP_OR                           // 或运算符
};

// 基础AST节点结构
typedef struct ASTNode {
    enum NodeType type;             // 节点类型
    struct ASTNode *next;           // 用于链表结构
} ASTNode;

// 程序节点
typedef struct {
    ASTNode base;
    ASTNode *functions;    // 函数列表
} ProgramNode;

// 函数节点
typedef struct {
    ASTNode base;
    char *name;            // 函数名
    ASTNode *params;       // 参数列表
    ASTNode *body;         // 函数体语句列表
} FunctionNode;

// 标识符节点
typedef struct {
    ASTNode base;
    char *name;            // 标识符名称
} IdentifierNode;

// 字面量节点
typedef struct {
    ASTNode base;
    enum LiteralType literal_type;
    union {
        int int_value;
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

// 打印语句节点
typedef struct {
    ASTNode base;
    ASTNode *arguments;    // 参数列表
} PrintNode;

// 二元操作节点
typedef struct {
    ASTNode base;
    enum BinaryOpType op_type;
    ASTNode *left;         // 左操作数
    ASTNode *right;        // 右操作数
} BinaryOpNode;

// 变量声明节点
typedef struct {
    ASTNode base;
    char *name;            // 变量名
    ASTNode *expression;   // 初始化表达式
} VarDeclNode;

// 函数调用节点
typedef struct {
    ASTNode base;
    char *name;            // 函数名
    ASTNode *arguments;    // 参数列表
} FunctionCallNode;

// 创建节点的函数声明
ProgramNode *create_program();
FunctionNode *create_function(char *name);
IdentifierNode *create_identifier(char *name);
LiteralNode *create_int_literal(int value);
LiteralNode *create_string_literal(char *value);
LiteralNode *create_float_literal(double value);
LiteralNode *create_bool_literal(int value);
ReturnNode *create_return(ASTNode *expression);
PrintNode *create_print();
void add_print_argument(PrintNode *print_node, ASTNode *argument);
BinaryOpNode *create_binary_op(enum BinaryOpType op_type, ASTNode *left, ASTNode *right);
VarDeclNode *create_var_decl(char *name, ASTNode *expression);
FunctionCallNode *create_function_call(char *name);

// 添加子节点的函数
void add_function(ProgramNode *program, FunctionNode *function);
void add_param(FunctionNode *function, IdentifierNode *param);
void add_statement(FunctionNode *function, ASTNode *statement);
void add_argument(FunctionCallNode *function_call, ASTNode *argument);

// 条件语句节点
typedef struct {
    ASTNode base;
    ASTNode *condition;    // 条件表达式
    ASTNode *consequence;  // 条件为真时执行的语句
    ASTNode *alternative;  // 条件为假时执行的语句
} IfStatementNode;

// 创建条件语句节点
IfStatementNode *create_if_statement(ASTNode *condition, ASTNode *consequence, ASTNode *alternative);

// 释放AST的函数
void free_ast(ASTNode *node);

#endif // AST_H