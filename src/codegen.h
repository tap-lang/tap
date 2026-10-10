#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"

// Forward declaration of Symbol struct
typedef struct Symbol Symbol;

// 在现有头文件包含部分添加以下内容
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h> 
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/IRReader.h>      
#include <llvm-c/Transforms/PassBuilder.h>

// 符号表条目
typedef struct Symbol {
    char *name;
    LLVMValueRef value;
    enum LiteralType type;
    const VarTypeNode *declared_type; // 符号借用 AST 中的完整声明类型
    const VarTypeNode *array_type; // 数组符号借用 AST 中的递归类型
    int is_const;
    struct Symbol *next;
} Symbol;

typedef struct LoopContext {
    LLVMBasicBlockRef continue_block;
    LLVMBasicBlockRef break_block;
    struct LoopContext *parent;
} LoopContext;

// 变参转发映射。tap 的普通函数没法在函数体里转发变参（LLVM 没有对应的指令），
// 所以「变参 + 函数体恰好是一句纯转发」的函数在编译期就被合并到目标函数上：
// 它不生成 LLVM 函数体，调用点直接按目标的签名生成代码。
// 典型例子是 Prelude 里的 `fn print(format: string, ...) { return __tap_printf(format, ...); }`。
typedef struct VariadicForward {
    const char *from;         // 包装函数名，例如 print
    FunctionNode *owner;      // 包装函数自身的节点：诊断里报它的源语言名，避免泄漏 __tap_ 内部符号
    FunctionNode *target;     // 真正干活的变参函数，例如 __tap_printf
    int auto_format;          // 单实参时按实参类型自动挑格式串（语言给 print 保留的便利行为）
    struct VariadicForward *next;
} VariadicForward;

// 代码生成器上下文
typedef struct {
    LLVMModuleRef module;           // LLVM模块，用于存储生成的代码
    LLVMBuilderRef builder;         // LLVM构建器，用于生成LLVM IR
    LLVMExecutionEngineRef engine;  // LLVM执行引擎，用于执行生成的代码
    LLVMContextRef context;         // LLVM上下文，用于存储LLVM值
    Symbol *symbols;                // 符号表，用于存储变量和它们对应的LLVM值
    LLVMValueRef printf_func;       // printf函数引用
    LLVMTypeRef printf_type;        // printf函数类型
    LLVMValueRef exit_func;
    LLVMTypeRef exit_type;
    ProgramNode *program;           // 当前正在生成的AST，不拥有其内存
    enum LiteralType current_return_type;
    const VarTypeNode *current_return_var_type;
    LoopContext *current_loop;
    VariadicForward *variadic_forwards; // 变参转发映射表
    // LLVMValueRef puts_func;         // puts函数引用
    // LLVMTypeRef puts_type;          // puts函数类型
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
int write_wasm_to_file(CodeGenContext *context, const char *filename);

#endif // CODEGEN_H
