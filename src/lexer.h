#ifndef LEXER_H
#define LEXER_H

#include "token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// 词法分析器结构体
typedef struct {
    const char *filename;
    char *source;   // create_lexer 读取文件分配，由 free_lexer 释放
    char *current;
    int line;
    int column;
} Lexer;

// 函数声明（从 path 读入源码）
Lexer *create_lexer(const char *filename);
Token *get_next_token(Lexer *lexer);
void print_lexer(Lexer *lexer);
void free_lexer(Lexer *lexer);

#endif // LEXER_H
