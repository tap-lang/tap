#ifndef RUN_H
#define RUN_H

#include "codegen.h"

// Resolve the Runtime libraries relative to the compiler or 4YUE_RUNTIME_PATH.
void configure_runtime(const char *compiler_path);
int compile_to_executable(CodeGenContext *context, const char *exe_file);
int compile_and_run(CodeGenContext *context, const char *exe_file);
int run_with_lli(CodeGenContext *context);

#endif
