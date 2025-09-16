#include "codegen.h"

// 创建代码生成器上下文
CodeGenContext *create_codegen_context(const char *module_name) {
    CodeGenContext *context = (CodeGenContext *)malloc(sizeof(CodeGenContext));
    if (!context) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }

    // 初始化LLVM
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    // 创建上下文、模块和构建器
    context->context = LLVMGetGlobalContext();
    context->module = LLVMModuleCreateWithName(module_name);
    context->builder = LLVMCreateBuilder();
    context->engine = NULL;

    return context;
}

// 释放代码生成器上下文
void free_codegen_context(CodeGenContext *context) {
    if (context) {
        if (context->engine) {
            LLVMDisposeExecutionEngine(context->engine);
            context->engine = NULL;
        }
        if (context->builder) {
            LLVMDisposeBuilder(context->builder);
            context->builder = NULL;
        }
        if (context->module) {
            LLVMDisposeModule(context->module);
            context->module = NULL;
        }
        free(context);
    }
}

// 初始化执行引擎
static int initialize_execution_engine(CodeGenContext *context) {
    if (context->engine) return 0;

    char *error = NULL;
    if (LLVMCreateExecutionEngineForModule(&context->engine, context->module, &error) != 0) {
        fprintf(stderr, "创建执行引擎失败: %s\n", error);
        LLVMDisposeMessage(error);
        return -1;
    }

    return 0;
}

// 生成函数代码
static void generate_function(CodeGenContext *context, FunctionNode *function) {
    // 创建函数类型: int()
    LLVMTypeRef return_type = LLVMInt32Type();
    LLVMTypeRef param_types[0];
    LLVMTypeRef function_type = LLVMFunctionType(return_type, param_types, 0, 0);

    // 在模块中创建函数
    LLVMValueRef llvm_function = LLVMAddFunction(context->module, function->name, function_type);

    // 创建基本块
    LLVMBasicBlockRef basic_block = LLVMAppendBasicBlock(llvm_function, "entry");
    LLVMPositionBuilderAtEnd(context->builder, basic_block);

    // 生成函数体
    ASTNode *statement = function->body;
    while (statement) {
        switch (statement->type) {
            case NODE_PRINT: {
                PrintNode *print_node = (PrintNode *)statement;
                if (print_node->expression && print_node->expression->type == NODE_LITERAL) {
                    LiteralNode *literal = (LiteralNode *)print_node->expression;
                    if (literal->literal_type == LITERAL_STRING) {
                        // 为print函数创建puts调用
                        LLVMTypeRef char_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
                        LLVMTypeRef puts_type = LLVMFunctionType(LLVMInt32Type(), &char_ptr_type, 1, 0);
                        LLVMValueRef puts_func = LLVMAddFunction(context->module, "puts", puts_type);
                        
                        // 创建字符串常量，确保正确处理
                        LLVMValueRef str = LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value, "str_const");
                        
                        // 正确调用puts函数
                        LLVMBuildCall2(context->builder, puts_type, puts_func, &str, 1, "puts_result");
                    }
                }
                break;
            }
            case NODE_RETURN: {
                ReturnNode *return_node = (ReturnNode *)statement;
                if (return_node->expression && return_node->expression->type == NODE_LITERAL) {
                    LiteralNode *literal = (LiteralNode *)return_node->expression;
                    if (literal->literal_type == LITERAL_INT) {
                        // 返回整数值
                        LLVMBuildRet(context->builder, LLVMConstInt(LLVMInt32Type(), literal->value.int_value, 0));
                    }
                }
                break;
            }
            default:
                break;
        }
        statement = statement->next;
    }

    // 如果没有显式的return语句，添加一个默认的return 0
    if (!LLVMGetBasicBlockTerminator(basic_block)) {
        LLVMBuildRet(context->builder, LLVMConstInt(LLVMInt32Type(), 0, 0));
    }
}

// 生成程序代码
void generate_code(CodeGenContext *context, ProgramNode *program) {
    // 生成每个函数的代码
    ASTNode *function_node = program->functions;
    while (function_node) {
        if (function_node->type == NODE_FUNCTION) {
            generate_function(context, (FunctionNode *)function_node);
        }
        function_node = function_node->next;
    }
}

// 执行生成的代码
int execute_code(CodeGenContext *context, const char *function_name) {
    if (initialize_execution_engine(context) != 0) {
        return -1;
    }

    // 获取函数
    LLVMValueRef function = LLVMGetNamedFunction(context->module, function_name);
    if (!function) {
        fprintf(stderr, "未找到函数: %s\n", function_name);
        return -1;
    }

    // 准备参数 - 使用正确的LLVMGenericValueRef类型
    LLVMGenericValueRef *args = NULL;
    int result = 0;
    
    // 执行函数 - 使用LLVM API的正确方式
    LLVMGenericValueRef result_ref = LLVMRunFunction(context->engine, function, 0, args);
    if (result_ref) {
        result = LLVMGenericValueToInt(result_ref, 0);
        LLVMDisposeGenericValue(result_ref);
    }
    
    return result;
}

// 写入IR到文件
int write_ir_to_file(CodeGenContext *context, const char *filename) {
    char *error = NULL;
    if (LLVMPrintModuleToFile(context->module, filename, &error) != 0) {
        fprintf(stderr, "写入IR文件失败: %s\n", error);
        LLVMDisposeMessage(error);
        return -1;
    }
    return 0;
}

// 写入目标代码到文件
int write_object_to_file(CodeGenContext *context, const char *filename) {
    // 简化的实现
    fprintf(stderr, "写入目标文件功能尚未完全实现\n");
    return -1;
}