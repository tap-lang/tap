#ifndef TOKEN_H
#define TOKEN_H

#include <stdlib.h>

// 标记类型
enum TokenType {
    // 关键字
    TOKEN_FN,        // fn 函数定义
    TOKEN_RETURN,    // return 返回语句
    TOKEN_PRINT,     // print 打印函数
    TOKEN_LET,       // let 语句
    TOKEN_IF,        // if 条件语句
    TOKEN_ELSE,      // else 语句
    TOKEN_ELSEIF,    // elseif 语句

    // 标识符和字面量
    TOKEN_IDENTIFIER,  // 标识符
    TOKEN_INT,         // 32/64位整数
    TOKEN_UINT,        // 32/64位无符号整数
    TOKEN_I8,          // 8位有符号整数
    TOKEN_U8,          // 8位无符号整数
    TOKEN_I16,         // 16位有符号整数
    TOKEN_U16,         // 16位无符号整数
    TOKEN_I32,         // 32位整数
    TOKEN_U32,         // 32位无符号整数
    TOKEN_I64,         // 64位整数
    TOKEN_U64,         // 64位无符号整数
    TOKEN_I128,        // 128位有符号整数
    TOKEN_U128,        // 128位无符号整数
    TOKEN_FLOAT,       // 浮点数
    TOKEN_F32,         // 32位浮点数
    TOKEN_F64,         // 64位浮点数
    TOKEN_BOOL,        // 布尔值
    TOKEN_STRING,      // 字符串
    TOKEN_ARRAY,       // 数组

    // 运算符
    TOKEN_PLUS,                     // +
    TOKEN_MINUS,                    // -
    TOKEN_MULTIPLY,                 // *
    TOKEN_DIVIDE,                   // /
    TOKEN_ASSIGN,                   // =
    TOKEN_EQUAL,                    // ==
    TOKEN_NOT_EQUAL,                // !=
    TOKEN_LESS_THAN,                // <
    TOKEN_GREATER_THAN,             // >
    TOKEN_LESS_THAN_OR_EQUAL,       // <=
    TOKEN_GREATER_THAN_OR_EQUAL,    // >=
    TOKEN_AND,                      // &&
    TOKEN_OR,                       // ||

    // 分隔符
    TOKEN_LPAREN,      // (
    TOKEN_RPAREN,      // )
    TOKEN_LBRACE,      // {
    TOKEN_RBRACE,      // }
    TOKEN_SEMICOLON,   // ;
    TOKEN_COMMA,       // ,
    TOKEN_COLON,       // :
    TOKEN_LBRACKET,    // [
    TOKEN_RBRACKET,    // ]

    // 特殊标记
    TOKEN_EOF          // 文件结束
};

// 各 TokenType 对应的可读名称（与枚举顺序一致）
extern const char *TokenNames[];

// 标记结构体
typedef struct {
    enum TokenType type;
    char *lexeme;      // 标记的文本内容
    int line;          // 行号
    int column;        // 列号

    // 根据标记类型存储不同的值
    union {
        unsigned long long int_value;
        char *string_value;
        double float_value;
        int bool_value;
    } value;
} Token;

void free_token(Token *token);

#endif // TOKEN_H
