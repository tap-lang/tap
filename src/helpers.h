#ifndef HELPERS_H
#define HELPERS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "ast.h"

// 「不返回」标注。诊断出口内部是 exit(1)，标出来之后调用方的控制流分析才正确 ——
// 否则 `if (bad) report_error(...);` 后面会被认为存在「没 return」的路径。
//
// 必须写在**返回类型前面**：GCC/Clang 的 `__attribute__` 两种位置都接受，但 MSVC 的
// `__declspec(noreturn)` 放在参数列表后面会被当成参数的一部分，报
// `C2085: not in formal parameter list`。
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
// 用节点自带的位置打印诊断。节点没有位置信息时只输出一句话，不打位置行。
void print_node_diagnostic(FILE *output, const char *level,
                           const ASTNode *node, const char *format, ...);

// 报告一条语义错误并终止编译。
//
// node 用于取源码位置，可以传 NULL（此时只输出一句话，不打位置行）。
//
// 这是所有语义诊断的**统一出口**：格式、位置、源码上下文只有一份实现。
// 以前是散在各处的 `fprintf(stderr, "error: ..."); exit(1);`，格式靠手写、
// 大部分不带位置。将来要改成「累积错误」或「结构化诊断（JSON）」，只改这里。
TAP_NORETURN void report_error(const ASTNode *node, const char *format, ...);

// 无位置版本：只跟名字或常量有关、拿不到节点时用。
TAP_NORETURN void report_error_at(const char *filename, int line, int column,
                                  const char *format, ...);

// ---------------------------------------------------------------------------
// 累积诊断：打印之后**继续往下走**，不终止编译
// ---------------------------------------------------------------------------
//
// 只给能安全继续的检查阶段用（borrow.c 的纯 AST 走查，以及 codegen.c 里那些
// 「报完这一条、放弃这条语句」的类型检查）。`report_error` 的 `TAP_NORETURN`
// 不能去掉 —— 一百多处调用点靠它做控制流分析，去掉会引出一片「可能没有返回值」
// 的警告，还会掩盖真正的漏 return。所以另开两个不终止的版本，想改哪一处就改哪一处。
//
// 报够 TAP_MAX_ERRORS 条后由 report_error_limit() 终止：否则一个系统性错误
// （比如类型整体没定义）会刷出成百上千条，反而看不清第一条。
#define TAP_MAX_ERRORS 10

void report_error_soft(const ASTNode *node, const char *format, ...);
void report_error_at_soft(const char *filename, int line, int column,
                          const char *format, ...);
// 可变参数版本。给「自己还想在报错前后多做一点事」的包装器用（例如 codegen 的
// codegen_error：先置毒化标记、再转发出去），省得每个包装器都复制一遍格式化。
void vreport_error_soft(const ASTNode *node, const char *format, va_list arguments);
void vreport_error_at_soft(const char *filename, int line, int column,
                           const char *format, va_list arguments);
// 已经报了多少条（跨阶段累计，直到 tap_error_reset）。
int tap_error_count(void);
void tap_error_reset(void);
// 打印「错误太多」并退出。由 report_error_soft 在达到上限时调用。
TAP_NORETURN void report_error_limit(void);

void print_code_line(FILE *output, const char *source_code, int line, int context_lines);

#endif
