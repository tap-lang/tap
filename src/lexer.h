#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
    TOKEN_I32,         // 32位整数
    TOKEN_I64,         // 64位整数
    TOKEN_F64,         // 64位浮点数
    TOKEN_F32,         // 32位浮点数
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

// 标记名称
static const char *TokenNames[] = {
    "fn", "return", "print", "let", "if", "else", "elseif",
    "identifier", "int", "i32", "i64", "f32", "f64", "bool", "string", "array",
    "+", "-", "*", "/", "=", "==", "!=", "<", ">", "<=", ">=", "&&", "||",
    "(", ")", "{", "}", ";", ",", ":", "[", "]",
    "EOF"
};

// 标记结构体
typedef struct {
    enum TokenType type;
    char *lexeme;      // 标记的文本内容
    int line;          // 行号
    int column;        // 列号
    
    // 根据标记类型存储不同的值
    union {
        int int_value;
        char *string_value;
        double float_value;
        int bool_value;
    } value;
} Token;

// 词法分析器结构体
typedef struct {
    const char *filename;
    const char *source;
    const char *current;
    int line;
    int column;
} Lexer;

// 函数声明
Lexer *create_lexer(const char *filename, const char *source);
Token *get_next_token(Lexer *lexer);
void free_token(Token *token);
void free_lexer(Lexer *lexer);

#endif // LEXER_H