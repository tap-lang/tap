#ifndef MODULE_H
#define MODULE_H

#include "ast.h"

int load_modules(ProgramNode *program, const char *input_file, const char *compiler_path);

// 包模式：从 start_directory 向上找 package.toml，按 [package] 的 entry 和 name 解析出
// 入口文件路径和构建产物路径（都是 malloc 的）。失败返回非 0 并打印诊断。
int resolve_package_target(const char *start_directory, const char *build_directory,
                           char **source_out, char **output_out);

#endif
