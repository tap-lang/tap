#ifndef PARSER_H
#define PARSER_H

// mingw-w64 在 x86_64 上默认把 `setjmp` 展开成 `_setjmp(buf, __builtin_sponentry())`：
// 缓冲里记一个栈帧地址，`longjmp` 走 `RtlUnwindEx` 做 SEH 展开回到那个帧。
//
// 这条路在本项目里会把编译器打崩 —— Release（`-O3`）下 `parse_program` 的帧地址和
// `RtlVirtualUnwind` 期望的边界对不上，展开时走进垃圾 pdata，SIGSEGV（Debug 的 `-g`
// 无优化构建不会）。六个环境里只有 MINGW64 的 Release 会中，因为它用 msvcrt + SEH
// 版的 setjmp；Linux / macOS / MSVC 的 setjmp 不走这条路。
//
// `__USE_MINGW_SETJMP_NON_SEH` 是 mingw-w64 自己给的开关：定义了它就退回
// `_setjmp(buf, NULL)` + 普通的寄存器恢复，完全不碰 SEH 展开。tap 是纯 C，没有
// C++ 析构、没有 `__try`，跳过去不需要展开任何东西 —— 这正是这个开关的适用场景。
#if defined(__MINGW32__) || defined(__MINGW64__)
#define __USE_MINGW_SETJMP_NON_SEH 1
#endif

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
