#include "codegen.h"

// 符号表条目
typedef struct Symbol {
    char *name;
    LLVMValueRef value;
    struct Symbol *next;
} Symbol;

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

    // 创建上下文、模块和构建器 - 使用新的上下文而不是全局上下文
    context->context = LLVMContextCreate();
    context->module = LLVMModuleCreateWithNameInContext(module_name, context->context);
    context->builder = LLVMCreateBuilderInContext(context->context);
    context->engine = NULL;
    context->symbols = NULL; // 初始化符号表为空

    return context;
}

// 在符号表中查找变量
static LLVMValueRef find_symbol(CodeGenContext *context, const char *name) {
    Symbol *current = context->symbols;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current->value;
        }
        current = current->next;
    }
    return NULL;
}

// 在符号表中插入变量
static void insert_symbol(CodeGenContext *context, const char *name, LLVMValueRef value) {
    Symbol *symbol = (Symbol *)malloc(sizeof(Symbol));
    if (!symbol) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    symbol->name = strdup(name);
    symbol->value = value;
    symbol->next = context->symbols;
    context->symbols = symbol;
}

// 释放符号表
static void free_symbols(Symbol *symbols) {
    while (symbols) {
        Symbol *next = symbols->next;
        free(symbols->name);
        free(symbols);
        symbols = next;
    }
}

// 生成表达式代码
static LLVMValueRef generate_expression(CodeGenContext *context, ASTNode *expression) {
    if (!expression) return NULL;
    
    switch (expression->type) {
        case NODE_LITERAL: {
            LiteralNode *literal = (LiteralNode *)expression;
            if (literal->literal_type == LITERAL_INT) {
                return LLVMConstInt(LLVMInt32TypeInContext(context->context), literal->value.int_value, 0);
            }
            break;
        }
        case NODE_IDENTIFIER: {
            IdentifierNode *identifier = (IdentifierNode *)expression;
            LLVMValueRef var = find_symbol(context, identifier->name);
            if (var) {
                return LLVMBuildLoad2(context->builder, LLVMInt32TypeInContext(context->context), var, "loaded_var");
            } else {
                fprintf(stderr, "错误：未定义的变量 '%s'\n", identifier->name);
                exit(1);
            }
            break;
        }
        case NODE_BINARY_OP: {
            BinaryOpNode *binary_op = (BinaryOpNode *)expression;
            LLVMValueRef left = generate_expression(context, binary_op->left);
            LLVMValueRef right = generate_expression(context, binary_op->right);
            
            if (!left || !right) return NULL;
            
            switch (binary_op->op_type) {
                case OP_ADD:
                    return LLVMBuildAdd(context->builder, left, right, "add_result");
                case OP_SUBTRACT:
                    return LLVMBuildSub(context->builder, left, right, "sub_result");
                case OP_MULTIPLY:
                    return LLVMBuildMul(context->builder, left, right, "mul_result");
                case OP_DIVIDE:
                    return LLVMBuildSDiv(context->builder, left, right, "div_result");
                default:
                    fprintf(stderr, "错误：不支持的二元操作符\n");
                    exit(1);
            }
            break;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *function_call = (FunctionCallNode *)expression;
            
            // 查找函数
            LLVMValueRef llvm_function = LLVMGetNamedFunction(context->module, function_call->name);
            if (!llvm_function) {
                fprintf(stderr, "错误：未定义的函数 '%s'\n", function_call->name);
                exit(1);
            }
            
            // 生成参数值
            int arg_count = 0;
            ASTNode *arg_node = function_call->arguments;
            while (arg_node) {
                arg_count++;
                arg_node = arg_node->next;
            }
            
            // 为参数创建数组
            LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * arg_count);
            
            // 生成每个参数的代码
            arg_node = function_call->arguments;
            int arg_index = 0;
            while (arg_node) {
                args[arg_index] = generate_expression(context, arg_node);
                arg_node = arg_node->next;
                arg_index++;
            }
            
            // 调用函数 - 使用正确的LLVM API
            // 我们需要创建一个函数类型来匹配参数
            LLVMTypeRef int_type = LLVMInt32TypeInContext(context->context);
            LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * arg_count);
            for (int i = 0; i < arg_count; i++) {
                param_types[i] = int_type;
            }
            LLVMTypeRef function_type = LLVMFunctionType(int_type, param_types, arg_count, 0);
            
            // 现在调用函数
            LLVMValueRef result = LLVMBuildCall2(context->builder, function_type, llvm_function, args, arg_count, "call_result");
            free(param_types);
            free(args);
            
            return result;
        }
        default:
            fprintf(stderr, "错误：不支持的表达式类型\n");
            exit(1);
    }
    
    return NULL;
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
        free_symbols(context->symbols);
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
    // 计算参数数量
    int param_count = 0;
    ASTNode *param_node = function->params;
    while (param_node) {
        param_count++;
        param_node = param_node->next;
    }
    
    // 创建参数类型数组
    LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * param_count);
    for (int i = 0; i < param_count; i++) {
        param_types[i] = LLVMInt32TypeInContext(context->context);
    }
    
    // 创建函数类型: int(param1, param2, ...) - 使用上下文
    LLVMTypeRef return_type = LLVMInt32TypeInContext(context->context);
    LLVMTypeRef function_type = LLVMFunctionType(return_type, param_types, param_count, 0);

    // 在模块中创建函数
    LLVMValueRef llvm_function = LLVMAddFunction(context->module, function->name, function_type);
    free(param_types);

    // 创建基本块
    LLVMBasicBlockRef basic_block = LLVMAppendBasicBlock(llvm_function, "entry");
    LLVMPositionBuilderAtEnd(context->builder, basic_block);
    
    // 处理参数
    param_node = function->params;
    int param_index = 0;
    while (param_node) {
        IdentifierNode *param = (IdentifierNode *)param_node;
        // 为参数创建alloca并存储
        LLVMValueRef alloca = LLVMBuildAlloca(context->builder, LLVMInt32TypeInContext(context->context), param->name);
        // 获取函数参数
        LLVMValueRef arg_value = LLVMGetParam(llvm_function, param_index);
        // 设置参数名
        LLVMSetValueName(arg_value, param->name);
        // 存储参数值
        LLVMBuildStore(context->builder, arg_value, alloca);
        // 添加到符号表
        insert_symbol(context, param->name, alloca);
        
        param_node = param_node->next;
        param_index++;
    }

    // 生成函数体
    ASTNode *statement = function->body;
    while (statement) {
        switch (statement->type) {
            case NODE_PRINT: {
                PrintNode *print_node = (PrintNode *)statement;
                if (print_node->expression) {
                    if (print_node->expression->type == NODE_LITERAL) {
                        LiteralNode *literal = (LiteralNode *)print_node->expression;
                        if (literal->literal_type == LITERAL_STRING) {
                             // 为print函数创建puts调用 - 使用上下文
                             LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
                             LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
                             LLVMTypeRef puts_type = LLVMFunctionType(LLVMInt32TypeInContext(context->context), &char_ptr_type, 1, 0);
                             LLVMValueRef puts_func = LLVMAddFunction(context->module, "puts", puts_type);
                            
                            // 创建字符串常量，确保正确处理
                            LLVMValueRef str = LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value, "str_const");
                            
                            // 正确调用puts函数
                            LLVMBuildCall2(context->builder, puts_type, puts_func, &str, 1, "puts_result");
                        } else if (literal->literal_type == LITERAL_INT) {
                            // 打印整数字面量
                            char buffer[32];
                            snprintf(buffer, sizeof(buffer), "%d", literal->value.int_value);
                            
                            // 为print函数创建puts调用 - 使用上下文
                            LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
                            LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
                            LLVMTypeRef puts_type = LLVMFunctionType(LLVMInt32TypeInContext(context->context), &char_ptr_type, 1, 0);
                            LLVMValueRef puts_func = LLVMAddFunction(context->module, "puts", puts_type);
                            
                            // 创建字符串常量
                            LLVMValueRef str = LLVMBuildGlobalStringPtr(context->builder, buffer, "int_str_const");
                            
                            // 正确调用puts函数
                            LLVMBuildCall2(context->builder, puts_type, puts_func, &str, 1, "puts_result");
                        }
                    } else {
                        // 处理表达式（包括变量和计算表达式）
                        // 首先获取表达式的值
                        LLVMValueRef expr_value = generate_expression(context, print_node->expression);
                        
                        if (expr_value) {
                            // 为printf函数创建声明 - 使用上下文
                            LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
                            LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
                            LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
                            
                            // printf函数类型: int printf(const char *format, ...)
                            // 对于可变参数函数，只需要指定第一个参数类型
                            LLVMTypeRef printf_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 1);
                            LLVMValueRef printf_func = LLVMAddFunction(context->module, "printf", printf_type);
                            
                            // 创建格式字符串
                            LLVMValueRef format_str = LLVMBuildGlobalStringPtr(context->builder, "%d\n", "format_str");
                            
                            // 调用printf函数直接打印整数表达式的值
                            LLVMValueRef printf_args[] = {format_str, expr_value};
                            LLVMBuildCall2(context->builder, printf_type, printf_func, printf_args, 2, "printf_result");
                        }
                    }
                }
                break;
            }
            case NODE_VAR_DECL: {
                VarDeclNode *var_decl = (VarDeclNode *)statement;
                // 创建整型变量 - 使用上下文
                LLVMTypeRef int_type = LLVMInt32TypeInContext(context->context);
                // 分配变量内存
                LLVMValueRef alloca = LLVMBuildAlloca(context->builder, int_type, var_decl->name);
                // 将变量添加到符号表
                insert_symbol(context, var_decl->name, alloca);
                // 生成表达式代码并存储结果
                if (var_decl->expression) {
                    LLVMValueRef expr_value = generate_expression(context, var_decl->expression);
                    LLVMBuildStore(context->builder, expr_value, alloca);
                }
                break;
            }
            case NODE_RETURN: {
                ReturnNode *return_node = (ReturnNode *)statement;
                if (return_node->expression) {
                    LLVMValueRef expr_value = generate_expression(context, return_node->expression);
                    if (expr_value) {
                        LLVMBuildRet(context->builder, expr_value);
                    }
                }
                break;
            }
            default:
                break;
        }
        statement = statement->next;
    }

    // 如果没有显式的return语句，添加一个默认的return 0 - 使用上下文
    if (!LLVMGetBasicBlockTerminator(basic_block)) {
        LLVMBuildRet(context->builder, LLVMConstInt(LLVMInt32TypeInContext(context->context), 0, 0));
    }
}

// 生成程序代码
void generate_code(CodeGenContext *context, ProgramNode *program) {
    // 第一步：先为所有函数添加声明（函数原型）
    // 这样当一个函数调用另一个函数时，被调用的函数已经在模块中注册
    // 但我们不生成函数体，只是创建函数声明
    ASTNode *function_node = program->functions;
    while (function_node) {
        if (function_node->type == NODE_FUNCTION) {
            FunctionNode *func = (FunctionNode *)function_node;
            
            // 计算参数数量
            int param_count = 0;
            ASTNode *param_node = func->params;
            while (param_node) {
                param_count++;
                param_node = param_node->next;
            }
            
            // 创建参数类型数组
            LLVMTypeRef *param_types = malloc(sizeof(LLVMTypeRef) * param_count);
            for (int i = 0; i < param_count; i++) {
                param_types[i] = LLVMInt32TypeInContext(context->context);
            }
            
            // 创建函数类型
            LLVMTypeRef return_type = LLVMInt32TypeInContext(context->context);
            LLVMTypeRef function_type = LLVMFunctionType(return_type, param_types, param_count, 0);
            
            // 添加函数声明到模块
            LLVMAddFunction(context->module, func->name, function_type);
            
            free(param_types);
        }
        function_node = function_node->next;
    }
    
    // 第二步：重新设置function_node指针到程序开始
    function_node = program->functions;
    
    // 第三步：生成每个函数的代码（函数体）
    while (function_node) {
        if (function_node->type == NODE_FUNCTION) {
            FunctionNode *func = (FunctionNode *)function_node;
            
            // 打印每个函数的名称
            printf("生成函数: %s\n", func->name);
            
            // 获取之前创建的函数声明
            LLVMValueRef llvm_function = LLVMGetNamedFunction(context->module, func->name);
            if (!llvm_function) {
                fprintf(stderr, "错误：函数声明未找到: %s\n", func->name);
                exit(1);
            }
            
            // 创建基本块（函数体的入口点）
            LLVMBasicBlockRef basic_block = LLVMAppendBasicBlock(llvm_function, "entry");
            LLVMPositionBuilderAtEnd(context->builder, basic_block);
            
            // 处理参数
            int param_count = 0;
            ASTNode *param_node = func->params;
            while (param_node) {
                param_count++;
                param_node = param_node->next;
            }
            
            param_node = func->params;
            int param_index = 0;
            while (param_node) {
                IdentifierNode *param = (IdentifierNode *)param_node;
                // 为参数创建alloca并存储
                LLVMValueRef alloca = LLVMBuildAlloca(context->builder, LLVMInt32TypeInContext(context->context), param->name);
                // 获取函数参数
                LLVMValueRef arg_value = LLVMGetParam(llvm_function, param_index);
                // 设置参数名
                LLVMSetValueName(arg_value, param->name);
                // 存储参数值
                LLVMBuildStore(context->builder, arg_value, alloca);
                // 添加到符号表
                insert_symbol(context, param->name, alloca);
                
                param_node = param_node->next;
                param_index++;
            }
            
            // 生成函数体
            ASTNode *statement = func->body;
            while (statement) {
                switch (statement->type) {
                    case NODE_PRINT: {
                        PrintNode *print_node = (PrintNode *)statement;
                        if (print_node->expression) {
                            if (print_node->expression->type == NODE_LITERAL) {
                                LiteralNode *literal = (LiteralNode *)print_node->expression;
                                if (literal->literal_type == LITERAL_STRING) {
                                     // 为print函数创建puts调用 - 使用上下文
                                     LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
                                     LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
                                     LLVMTypeRef puts_type = LLVMFunctionType(LLVMInt32TypeInContext(context->context), &char_ptr_type, 1, 0);
                                     LLVMValueRef puts_func = LLVMAddFunction(context->module, "puts", puts_type);
                                    
                                    // 创建字符串常量，确保正确处理
                                    LLVMValueRef str = LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value, "str_const");
                                    
                                    // 正确调用puts函数
                                    LLVMBuildCall2(context->builder, puts_type, puts_func, &str, 1, "puts_result");
                                } else if (literal->literal_type == LITERAL_INT) {
                                    // 打印整数字面量
                                    char buffer[32];
                                    snprintf(buffer, sizeof(buffer), "%d", literal->value.int_value);
                                    
                                    // 为print函数创建puts调用 - 使用上下文
                                    LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
                                    LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
                                    LLVMTypeRef puts_type = LLVMFunctionType(LLVMInt32TypeInContext(context->context), &char_ptr_type, 1, 0);
                                    LLVMValueRef puts_func = LLVMAddFunction(context->module, "puts", puts_type);
                                    
                                    // 创建字符串常量
                                    LLVMValueRef str = LLVMBuildGlobalStringPtr(context->builder, buffer, "int_str_const");
                                    
                                    // 正确调用puts函数
                                    LLVMBuildCall2(context->builder, puts_type, puts_func, &str, 1, "puts_result");
                                }
                            } else {
                                // 处理表达式（包括变量和计算表达式）
                                // 首先获取表达式的值
                                LLVMValueRef expr_value = generate_expression(context, print_node->expression);
                                
                                if (expr_value) {
                                    // 为printf函数创建声明 - 使用上下文
                                    LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
                                    LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
                                    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
                                    
                                    // printf函数类型: int printf(const char *format, ...)
                                    // 对于可变参数函数，只需要指定第一个参数类型
                                    LLVMTypeRef printf_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 1);
                                    LLVMValueRef printf_func = LLVMAddFunction(context->module, "printf", printf_type);
                                    
                                    // 创建格式字符串
                                    LLVMValueRef format_str = LLVMBuildGlobalStringPtr(context->builder, "%d\n", "format_str");
                                    
                                    // 调用printf函数直接打印整数表达式的值
                                    LLVMValueRef printf_args[] = {format_str, expr_value};
                                    LLVMBuildCall2(context->builder, printf_type, printf_func, printf_args, 2, "printf_result");
                                }
                            }
                        }
                        break;
                    }
                    case NODE_VAR_DECL: {
                        VarDeclNode *var_decl = (VarDeclNode *)statement;
                        // 创建整型变量 - 使用上下文
                        LLVMTypeRef int_type = LLVMInt32TypeInContext(context->context);
                        // 分配变量内存
                        LLVMValueRef alloca = LLVMBuildAlloca(context->builder, int_type, var_decl->name);
                        // 将变量添加到符号表
                        insert_symbol(context, var_decl->name, alloca);
                        // 生成表达式代码并存储结果
                        if (var_decl->expression) {
                            LLVMValueRef expr_value = generate_expression(context, var_decl->expression);
                            LLVMBuildStore(context->builder, expr_value, alloca);
                        }
                        break;
                    }
                    case NODE_RETURN: {
                        ReturnNode *return_node = (ReturnNode *)statement;
                        if (return_node->expression) {
                            LLVMValueRef expr_value = generate_expression(context, return_node->expression);
                            if (expr_value) {
                                LLVMBuildRet(context->builder, expr_value);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
                statement = statement->next;
            }
            
            // 如果没有显式的return语句，添加一个默认的return 0 - 使用上下文
            if (!LLVMGetBasicBlockTerminator(basic_block)) {
                LLVMBuildRet(context->builder, LLVMConstInt(LLVMInt32TypeInContext(context->context), 0, 0));
            }
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