#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

// 解析器结构体
typedef struct {
    Lexer *lexer;
    Token *current_token;
    int loop_depth;
} Parser;

Parser *create_parser(Lexer *lexer);
ProgramNode *parse_program(Parser *parser);
void free_parser(Parser *parser);

// 错误处理
void parser_error(Parser *parser, const char *message);

#endif // PARSER_H
