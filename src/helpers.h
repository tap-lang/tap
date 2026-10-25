#ifndef HELPERS_H
#define HELPERS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "ast.h"

// 「不返回」标注。诊断出口内部是 exit(1)，标出来之后调用方的控制流分析才正确 ——
// 否则 `if (bad) report_error(...);` 后面会被认为存在「没 return」的路径。
#if defined(__GNUC__) || defined(__clang__)
#define TAP_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define TAP_NORETURN __declspec(noreturn)
#else
#define TAP_NORETURN
#endif

char *get_basename_no_ext(const char *input_file);
int tap_executable_path(char *buffer, size_t size);
char *read_source_file(const char *filename);
void print_diagnostic(FILE *output, const char *level, const char *filename,
                      int line, int column, const char *format, ...);
// 变参版本：供需要转发 va_list 的包装函数使用。
void vprint_diagnostic(FILE *output, const char *level, const char *filename,
                       int line, int column, const char *format, va_list arguments);
// 用节点自带的位置打印诊断。节点没有位置信息时退化成 <unknown>:0:0。
void print_node_diagnostic(FILE *output, const char *level,
                           const ASTNode *node, const char *format, ...);

// 报告一条语义错误并终止编译。
//
// node 用于取源码位置，可以传 NULL（位置退化成 <unknown>:0:0）。
//
// 这是所有语义诊断的**统一出口**：格式、位置、源码上下文只有一份实现。
// 以前是散在各处的 `fprintf(stderr, "error: ..."); exit(1);`，格式靠手写、
// 大部分不带位置。将来要改成「累积错误」或「结构化诊断（JSON）」，只改这里。
void report_error(const ASTNode *node, const char *format, ...) TAP_NORETURN;

// 无位置版本：只跟名字或常量有关、拿不到节点时用。
void report_error_at(const char *filename, int line, int column,
                     const char *format, ...) TAP_NORETURN;
void print_code_line(FILE *output, const char *source_code, int line, int context_lines);

#endif
