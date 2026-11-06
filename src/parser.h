#ifndef PARSER_H
#define PARSER_H

#include <setjmp.h>

#include "lexer.h"
#include "ast.h"

// 语法错误恢复的返回点。
//
// 语法错了之后 token 流没有可靠的同步点，在表达式内部硬恢复只会产生一串级联假错误，
// 所以恢复只发生在**语句边界**：`parse_block` 和 `parse_program` 各在循环里设一个
// 返回点，`parser_error` 打印完就 `longjmp` 回来，丢掉当前这条语句/声明继续往下解析。
//
// 用栈而不是单个 jmp_buf：嵌套代码块会层层设返回点，跳的必须是**最内层**那个。
// 用 longjmp 而不是让每个 parse_* 返回 NULL，是因为后者要求改几十个函数的签名和
// 调用点（`parse_type` / `parse_expression` 的调用方都假设返回值非空）。
typedef struct ParserErrorFrame {
    jmp_buf jump;
    struct ParserErrorFrame *previous;
} ParserErrorFrame;

// 解析器结构体
typedef struct {
    Lexer *lexer;
    Token *current_token;
    int loop_depth;
    // 当前位于第几层变参函数体内。实参列表里的 `...` 只能用来转发本函数的变参，
    // 所以不在变参函数体内时出现 `...` 直接报错。
    int variadic_depth;
    // 当前的恢复点，NULL 表示「不在可恢复的位置」—— 此时语法错误仍然直接退出。
    ParserErrorFrame *error_frame;
    // 已经报了多少条语法错误。非 0 时调用方必须停止编译：AST 里缺了语句，
    // 后续阶段看到的是一个残缺的程序。
    int error_count;
    // 上一条错误所在行，用来压掉同一行上的重复诊断。
    int last_error_line;
} Parser;

Parser *create_parser(Lexer *lexer);
ProgramNode *parse_program(Parser *parser);
void free_parser(Parser *parser);

// 错误处理
void parser_error(Parser *parser, const char *message);

// 是否报过语法错误（报过就不能继续编译）。
int parser_had_error(const Parser *parser);

#endif // PARSER_H
