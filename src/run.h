#ifndef RUN_H
#define RUN_H

#include "codegen.h"

int compile_to_executable(CodeGenContext *context, const char *exe_file);
int compile_and_run(CodeGenContext *context, const char *exe_file);
void run_with_lli(CodeGenContext *context);

#endif
