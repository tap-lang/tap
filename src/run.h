#ifndef RUN_H
#define RUN_H

#include "codegen.h"

// 根据编译器路径或 TAP_RUNTIME_PATH 解析 Runtime 库位置。
void configure_runtime(const char *compiler_path);
int compile_to_executable(CodeGenContext *context, const char *exe_file, int static_link);
int compile_and_run(CodeGenContext *context, const char *exe_file,
                    int program_argc, char **program_argv, int static_link);
int run_with_lli(CodeGenContext *context);

// 把 `tap new ...` 转发给 tapm。forwarded_argv[0] 是 "new"，其后是原样透传的参数。
// 返回 tapm 的退出码；找不到 tapm 时打印提示并返回 1。
int run_tapm(const char *compiler_path, int forwarded_argc, char **forwarded_argv);

#endif
