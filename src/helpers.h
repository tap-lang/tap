#ifndef HELPERS_H
#define HELPERS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "ast.h"

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
void print_code_line(FILE *output, const char *source_code, int line, int context_lines);

#endif
