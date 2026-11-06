#ifndef MODULE_H
#define MODULE_H

#include "ast.h"

int load_modules(ProgramNode *program, const char *input_file, const char *compiler_path);

// 包模式：从 start_directory 向上找 package.toml，按 [package] 的 entry 和 name 解析出
// 入口文件路径和构建产物路径（都是 malloc 的）。失败返回非 0 并打印诊断。
// 副作用：会创建 <包根>/<build_directory> 目录。
int resolve_package_target(const char *start_directory, const char *build_directory,
                           char **source_out, char **output_out);

// 只读版：只解析入口文件，**不创建任何目录**。`tap check` 用这个。
// name_out 可以为 NULL；两个输出都是 malloc 的。
int resolve_package_entry(const char *start_directory, char **source_out, char **name_out);

#endif
