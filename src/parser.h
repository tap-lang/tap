#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

// 解析器结构体
typedef struct {
    Lexer *lexer;
    Token *current_token;
    int loop_depth;
    // 当前位于第几层变参函数体内。实参列表里的 `...` 只能用来转发本函数的变参，
    // 所以不在变参函数体内时出现 `...` 直接报错。
    int variadic_depth;
} Parser;

Parser *create_parser(Lexer *lexer);
ProgramNode *parse_program(Parser *parser);
void free_parser(Parser *parser);

// 错误处理
void parser_error(Parser *parser, const char *message);

#endif // PARSER_H
