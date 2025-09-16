#ifndef AST_H
#define AST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// AST节点类型
enum NodeType {
    NODE_PROGRAM,
    NODE_FUNCTION,
    NODE_STATEMENT,
    NODE_EXPRESSION,
    NODE_IDENTIFIER,
    NODE_LITERAL,
    NODE_RETURN,
    NODE_PRINT,
    NODE_BINARY_OP
};

// 字面量类型
enum LiteralType {
    LITERAL_INT,
    LITERAL_STRING,
    LITERAL_FLOAT,
    LITERAL_BOOL
};

// 二元操作符类型
enum BinaryOpType {
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_LESS_THAN,
    OP_GREATER_THAN,
    OP_LESS_THAN_OR_EQUAL,
    OP_GREATER_THAN_OR_EQUAL,
    OP_AND,
    OP_OR
};

// 基础AST节点结构
typedef struct ASTNode {
    enum NodeType type;
    struct ASTNode *next;  // 用于链表结构
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
    ASTNode *expression;   // 打印表达式
} PrintNode;

// 二元操作节点
typedef struct {
    ASTNode base;
    enum BinaryOpType op_type;
    ASTNode *left;         // 左操作数
    ASTNode *right;        // 右操作数
} BinaryOpNode;

// 创建节点的函数声明
ProgramNode *create_program();
FunctionNode *create_function(char *name);
IdentifierNode *create_identifier(char *name);
LiteralNode *create_int_literal(int value);
LiteralNode *create_string_literal(char *value);
LiteralNode *create_float_literal(double value);
LiteralNode *create_bool_literal(int value);
ReturnNode *create_return(ASTNode *expression);
PrintNode *create_print(ASTNode *expression);
BinaryOpNode *create_binary_op(enum BinaryOpType op_type, ASTNode *left, ASTNode *right);

// 添加子节点的函数
void add_function(ProgramNode *program, FunctionNode *function);
void add_param(FunctionNode *function, IdentifierNode *param);
void add_statement(FunctionNode *function, ASTNode *statement);

// 释放AST的函数
void free_ast(ASTNode *node);

#endif // AST_H