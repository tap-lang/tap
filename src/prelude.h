#ifndef PRELUDE_H
#define PRELUDE_H

#include "ast.h"

// Prelude 提供的顶层名字（不透明）。
//
// 模块装载阶段需要知道「哪些裸名已经属于 Prelude」，才能拦住 `import mod.*;`
// 静默换掉 `print` 这种事。但 Prelude 是在所有模块装载完之后才注入的，那时候
// 裸名早就被改写成模块符号了 —— 所以先单独收集一份名字，不注入。
typedef struct PreludeNames PreludeNames;

// 解析 Prelude 并收集名字。找不到/解析失败返回 NULL（load_prelude 会给出完整诊断）。
PreludeNames *collect_prelude_names(const char *compiler_path);

// 名字是否由 Prelude 提供。is_type 可为 NULL；非 NULL 时写入它是不是类型。
int prelude_provides_name(const PreludeNames *names, const char *name,
                          int *is_type);

void prelude_names_free(PreludeNames *names);

int load_prelude(ProgramNode *program, const char *compiler_path);

#endif
