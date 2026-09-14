#ifndef RUN_H
#define RUN_H

#include "codegen.h"

// 根据编译器路径或 4YUE_RUNTIME_PATH 解析 Runtime 库位置。
void configure_runtime(const char *compiler_path);
int compile_to_executable(CodeGenContext *context, const char *exe_file, int static_link);
int compile_and_run(CodeGenContext *context, const char *exe_file,
                    int program_argc, char **program_argv, int static_link);
int run_with_lli(CodeGenContext *context);

#endif
