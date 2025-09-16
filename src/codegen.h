#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"

// 确保正确包含LLVM头文件
#ifdef __APPLE__
    #include "/opt/homebrew/opt/llvm/include/llvm-c/Core.h"
    #include "/opt/homebrew/opt/llvm/include/llvm-c/ExecutionEngine.h"
    #include "/opt/homebrew/opt/llvm/include/llvm-c/Target.h"
    #include "/opt/homebrew/opt/llvm/include/llvm-c/Analysis.h"
    #include "/opt/homebrew/opt/llvm/include/llvm-c/BitWriter.h"
#else
    #include <llvm-c/Core.h>
    #include <llvm-c/ExecutionEngine.h>
    #include <llvm-c/Target.h>
    #include <llvm-c/Analysis.h>
    #include <llvm-c/BitWriter.h>
#endif

// 代码生成器上下文
typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMExecutionEngineRef engine;
    LLVMContextRef context;

    // 存储函数和变量的映射表
    // 注意：在实际实现中可能需要更复杂的数据结构
} CodeGenContext;

// 函数声明
CodeGenContext *create_codegen_context(const char *module_name);
void generate_code(CodeGenContext *context, ProgramNode *program);
void free_codegen_context(CodeGenContext *context);

// 执行生成的代码
int execute_code(CodeGenContext *context, const char *function_name);

// 写入IR到文件
int write_ir_to_file(CodeGenContext *context, const char *filename);

// 写入目标代码到文件
int write_object_to_file(CodeGenContext *context, const char *filename);

#endif // CODEGEN_H