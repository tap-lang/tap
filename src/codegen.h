#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"

// Forward declaration of Symbol struct
typedef struct Symbol Symbol;

#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>



// 代码生成器上下文
typedef struct {
    LLVMModuleRef module;           // LLVM模块，用于存储生成的代码
    LLVMBuilderRef builder;         // LLVM构建器，用于生成LLVM IR
    LLVMExecutionEngineRef engine;  // LLVM执行引擎，用于执行生成的代码
    LLVMContextRef context;         // LLVM上下文，用于存储LLVM值
    Symbol *symbols;                // 符号表，用于存储变量和它们对应的LLVM值
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