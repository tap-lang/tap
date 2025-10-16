#include "codegen.h"

extern int debug;

// 创建代码生成器上下文
CodeGenContext *create_codegen_context(const char *module_name) {
    CodeGenContext *context = (CodeGenContext *)malloc(sizeof(CodeGenContext));
    if (!context) {
        fprintf(stderr, "Memory allocation failed\n");
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

// 声明generate_expression函数
static LLVMValueRef generate_expression(CodeGenContext *context, ASTNode *expression);

// 生成条件语句代码
static void generate_if_statement(CodeGenContext *context, IfStatementNode *if_node, LLVMBasicBlockRef *insert_block) {
    // 获取当前函数
    LLVMValueRef current_function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(context->builder));
    
    // 创建条件基本块
    char then_name[64];
    char else_name[64];
    
    // 为基本块生成唯一名称，避免冲突
    static int if_counter = 0;
    sprintf(then_name, "then.%d", if_counter);
    sprintf(else_name, "else.%d", if_counter);
    if_counter++;
    
    LLVMBasicBlockRef then_block = LLVMAppendBasicBlock(current_function, then_name);
    LLVMBasicBlockRef else_block = NULL;
    
    // 如果有else部分，创建else基本块
    if (if_node->alternative) {
        else_block = LLVMAppendBasicBlock(current_function, else_name);
    }
    
    // 生成条件表达式
    LLVMValueRef condition = generate_expression(context, if_node->condition);
    if (!condition) return;
    
    // 直接使用条件表达式作为条件结果
    LLVMValueRef cond_result = condition;
    
    // 保存当前基本块（将作为默认的merge块）
    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(context->builder);
    LLVMBasicBlockRef next_block = NULL;
    
    // 检查是否需要创建下一个基本块（用于非终结指令的情况）
    // 我们只在then和else分支都不包含终结指令时创建
    int needs_next_block = 1; // 使用int代替bool以避免头文件依赖
    
    // 生成条件分支
    if (else_block) {
        LLVMBuildCondBr(context->builder, cond_result, then_block, else_block);
    } else {
        LLVMBuildCondBr(context->builder, cond_result, then_block, current_block);
        needs_next_block = 0;
    }
    
    // 生成then分支代码
    LLVMPositionBuilderAtEnd(context->builder, then_block);
    
    // 生成then分支的语句序列
    ASTNode *statement = if_node->consequence;
    while (statement) {
        switch (statement->type) {
            case NODE_PRINT: {
                PrintNode *print_node = (PrintNode *)statement;
                if (print_node->arguments) {
                    // 获取参数数量
                    int arg_count = 0;
                    ASTNode *arg_node = print_node->arguments;
                    while (arg_node) {
                        arg_count++;
                        arg_node = arg_node->next;
                    }
                    
                    // 准备参数数组
                    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * arg_count);
                    int actual_arg_count = 0;
                    
                    // 处理第一个参数（格式字符串）
                    arg_node = print_node->arguments;
                    if (arg_node->type == NODE_LITERAL && ((LiteralNode *)arg_node)->literal_type == LITERAL_STRING) {
                        // 第一个参数是字符串，用作格式字符串
                        LiteralNode *literal = (LiteralNode *)arg_node;
                        args[0] = LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value, "format_str");
                        actual_arg_count = 1;
                        
                        // 处理剩余参数
                        arg_node = arg_node->next;
                        while (arg_node) {
                            args[actual_arg_count++] = generate_expression(context, arg_node);
                            arg_node = arg_node->next;
                        }
                    } else {
                        // 如果第一个参数不是字符串，使用默认格式
                        args[0] = LLVMBuildGlobalStringPtr(context->builder, "%d", "format_str");
                        args[1] = generate_expression(context, arg_node);
                        actual_arg_count = 2;
                        
                        // 处理剩余参数（如果有多个表达式）
                        arg_node = arg_node->next;
                        while (arg_node) {
                            // 这里简化处理，实际应该动态构建格式字符串
                            args[actual_arg_count++] = generate_expression(context, arg_node);
                            arg_node = arg_node->next;
                        }
                    }
                    
                    // 调用printf函数
                    if (context->printf_func) {
                        LLVMBuildCall2(context->builder, context->printf_type, context->printf_func, args, actual_arg_count, "printf_result");
                    }
                    
                    free(args);
                }
                break;
            }
            case NODE_VAR_DECL: {
                VarDeclNode *var_decl = (VarDeclNode *)statement;
                LLVMTypeRef int_type = LLVMInt32TypeInContext(context->context);
                LLVMValueRef alloca = LLVMBuildAlloca(context->builder, int_type, var_decl->name);
                insert_symbol(context, var_decl->name, alloca);
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
                        needs_next_block = 0;
                    }
                }
                break;
            }
            case NODE_IF_STATEMENT: {
                IfStatementNode *nested_if_node = (IfStatementNode *)statement;
                LLVMBasicBlockRef nested_insert_block = LLVMGetInsertBlock(context->builder);
                generate_if_statement(context, nested_if_node, &nested_insert_block);
                // 如果嵌套的if语句已经设置了终结指令，我们不需要下一个块
                if (LLVMGetBasicBlockTerminator(nested_insert_block)) {
                    needs_next_block = 0;
                }
                break;
            }
            default:
                break;
        }
        statement = statement->next;
    }
    
    // 如果then块没有终结指令，并且需要下一个块，我们需要创建它
    if (needs_next_block && else_block && !LLVMGetBasicBlockTerminator(then_block)) {
        // 创建一个新的基本块用于后续代码
        next_block = LLVMAppendBasicBlock(current_function, "next_block");
        LLVMBuildBr(context->builder, next_block);
    }
    
    // 生成else分支代码
    if (else_block) {
        LLVMPositionBuilderAtEnd(context->builder, else_block);
        
        // 检查else分支是否是另一个if语句（else if）
        if (if_node->alternative->type == NODE_IF_STATEMENT) {
            IfStatementNode *nested_if_node = (IfStatementNode *)if_node->alternative;
            LLVMBasicBlockRef nested_insert_block = LLVMGetInsertBlock(context->builder);
            generate_if_statement(context, nested_if_node, &nested_insert_block);
            // 如果嵌套的if语句已经设置了终结指令，我们不需要下一个块
            if (LLVMGetBasicBlockTerminator(nested_insert_block)) {
                needs_next_block = 0;
            }
        } else {
            // 生成else分支的语句序列
            statement = if_node->alternative;
            while (statement) {
                switch (statement->type) {
                    case NODE_PRINT: {
                PrintNode *print_node = (PrintNode *)statement;
                if (print_node->arguments) {
                    // 获取参数数量
                    int arg_count = 0;
                    ASTNode *arg_node = print_node->arguments;
                    while (arg_node) {
                        arg_count++;
                        arg_node = arg_node->next;
                    }
                    
                    // 准备参数数组
                    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * arg_count);
                    int actual_arg_count = 0;
                    
                    // 处理第一个参数（格式字符串）
                    arg_node = print_node->arguments;
                    if (arg_node->type == NODE_LITERAL && ((LiteralNode *)arg_node)->literal_type == LITERAL_STRING) {
                        // 第一个参数是字符串，用作格式字符串
                        LiteralNode *literal = (LiteralNode *)arg_node;
                        args[0] = LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value, "format_str");
                        actual_arg_count = 1;
                        
                        // 处理剩余参数
                        arg_node = arg_node->next;
                        while (arg_node) {
                            args[actual_arg_count++] = generate_expression(context, arg_node);
                            arg_node = arg_node->next;
                        }
                    } else {
                        // 如果第一个参数不是字符串，使用默认格式
                        args[0] = LLVMBuildGlobalStringPtr(context->builder, "%d", "format_str");
                        args[1] = generate_expression(context, arg_node);
                        actual_arg_count = 2;
                        
                        // 处理剩余参数（如果有多个表达式）
                        arg_node = arg_node->next;
                        while (arg_node) {
                            // 这里简化处理，实际应该动态构建格式字符串
                            args[actual_arg_count++] = generate_expression(context, arg_node);
                            arg_node = arg_node->next;
                        }
                    }
                    
                    // 调用printf函数
                    if (context->printf_func) {
                        LLVMBuildCall2(context->builder, context->printf_type, context->printf_func, args, actual_arg_count, "printf_result");
                    }
                    
                    free(args);
                }
                break;
                    }
                    case NODE_VAR_DECL: {
                        VarDeclNode *var_decl = (VarDeclNode *)statement;
                        LLVMTypeRef int_type = LLVMInt32TypeInContext(context->context);
                        LLVMValueRef alloca = LLVMBuildAlloca(context->builder, int_type, var_decl->name);
                        insert_symbol(context, var_decl->name, alloca);
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
                                needs_next_block = 0;
                            }
                        }
                        break;
                    }
                    case NODE_IF_STATEMENT: {
                        IfStatementNode *nested_if_node = (IfStatementNode *)statement;
                        LLVMBasicBlockRef nested_insert_block = LLVMGetInsertBlock(context->builder);
                        generate_if_statement(context, nested_if_node, &nested_insert_block);
                        // 如果嵌套的if语句已经设置了终结指令，我们不需要下一个块
                        if (LLVMGetBasicBlockTerminator(nested_insert_block)) {
                            needs_next_block = 0;
                        }
                        break;
                    }
                    default:
                        break;
                }
                statement = statement->next;
            }
        }
        
        // 如果else块没有终结指令，并且需要下一个块，跳转到next_block
        if (needs_next_block && !next_block) {
            // 创建一个新的基本块用于后续代码
            next_block = LLVMAppendBasicBlock(current_function, "next_block");
        }
        if (needs_next_block && !LLVMGetBasicBlockTerminator(else_block)) {
            LLVMBuildBr(context->builder, next_block);
        }
    }
    
    // 设置插入点
    if (next_block) {
        LLVMPositionBuilderAtEnd(context->builder, next_block);
        *insert_block = next_block;
    } else {
        // 如果没有next_block，插入点应该回到原始的current_block之后的位置
        // 但在我们的简单实现中，我们只需要确保insert_block被设置为一个有效的块
        *insert_block = current_block;
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
                case OP_EQUAL:
                    return LLVMBuildICmp(context->builder, LLVMIntEQ, left, right, "eq_result");
                case OP_NOT_EQUAL:
                    return LLVMBuildICmp(context->builder, LLVMIntNE, left, right, "ne_result");
                case OP_LESS_THAN:
                    return LLVMBuildICmp(context->builder, LLVMIntSLT, left, right, "lt_result");
                case OP_GREATER_THAN:
                    return LLVMBuildICmp(context->builder, LLVMIntSGT, left, right, "gt_result");
                case OP_LESS_THAN_OR_EQUAL:
                    return LLVMBuildICmp(context->builder, LLVMIntSLE, left, right, "le_result");
                case OP_GREATER_THAN_OR_EQUAL:
                    return LLVMBuildICmp(context->builder, LLVMIntSGE, left, right, "ge_result");
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
                if (print_node->arguments) {
                    // 获取参数数量
                    int arg_count = 0;
                    ASTNode *arg_node = print_node->arguments;
                    while (arg_node) {
                        arg_count++;
                        arg_node = arg_node->next;
                    }
                    
                    // 准备参数数组
                    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (arg_count + 1)); // +1 用于格式字符串
                    int actual_arg_count = 0;
                    
                    // 处理第一个参数（格式字符串）
                    arg_node = print_node->arguments;
                    if (arg_node->type == NODE_LITERAL && ((LiteralNode *)arg_node)->literal_type == LITERAL_STRING) {
                        // 第一个参数是字符串，用作格式字符串
                        LiteralNode *literal = (LiteralNode *)arg_node;
                        args[0] = LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value, "format_str");
                        actual_arg_count = 1;
                        
                        // 处理剩余参数
                        arg_node = arg_node->next;
                        while (arg_node) {
                            args[actual_arg_count++] = generate_expression(context, arg_node);
                            arg_node = arg_node->next;
                        }
                    } else {
                        // 如果第一个参数不是字符串，使用默认格式
                        args[0] = LLVMBuildGlobalStringPtr(context->builder, "%d", "format_str");
                        args[1] = generate_expression(context, arg_node);
                        actual_arg_count = 2;
                        
                        // 处理剩余参数（如果有多个表达式）
                        arg_node = arg_node->next;
                        while (arg_node) {
                            // 这里简化处理，实际应该动态构建格式字符串
                            args[actual_arg_count++] = generate_expression(context, arg_node);
                            arg_node = arg_node->next;
                        }
                    }
                    
                    // 调用printf函数
                    if (context->printf_func) {
                        LLVMBuildCall2(context->builder, context->printf_type, context->printf_func, args, actual_arg_count, "printf_result");
                    } else {
                        // 如果上下文没有printf_func，创建它
                        LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
                        LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
                        LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
                        
                        // printf函数类型: int printf(const char *format, ...)
                        LLVMTypeRef printf_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 1);
                        LLVMValueRef printf_func = LLVMAddFunction(context->module, "printf", printf_type);
                        
                        LLVMBuildCall2(context->builder, printf_type, printf_func, args, actual_arg_count, "printf_result");
                    }
                    
                    free(args);
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
    if (debug) printf("- 生成代码\n");
    // 预定义标准库函数声明（一次性添加）
    // 初始化函数引用和类型
    context->printf_func = NULL;
    context->puts_func = NULL;
    context->printf_type = NULL;
    context->puts_type = NULL;
    
    // 声明printf函数
    LLVMTypeRef int8_type = LLVMInt8TypeInContext(context->context);
    LLVMTypeRef char_ptr_type = LLVMPointerType(int8_type, 0);
    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
    
    // printf函数类型: int printf(const char *format, ...)
    context->printf_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 1);
    context->printf_func = LLVMAddFunction(context->module, "printf", context->printf_type);
    
    // 声明puts函数
    context->puts_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 0);
    context->puts_func = LLVMAddFunction(context->module, "puts", context->puts_type);
    
    
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
            if (debug) printf("  - 生成函数: %s\n", func->name);
            
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
                        if (print_node->arguments && context->printf_func) {
                            // 计数参数数量
                            int actual_arg_count = 0;
                            ASTNode *current_arg = print_node->arguments;
                            while (current_arg) {
                                actual_arg_count++;
                                current_arg = current_arg->next;
                            }
                            
                            // 检查是否只有一个非字符串参数
                            if (actual_arg_count == 1 && 
                                print_node->arguments->type != NODE_LITERAL) {
                                // 只有一个非字符串参数，自动添加%d格式字符串
                                LLVMValueRef expr_value = generate_expression(context, print_node->arguments);
                                if (expr_value) {
                                    LLVMValueRef format_str = LLVMBuildGlobalStringPtr(context->builder, "%d", "format_str");
                                    LLVMValueRef args[] = {format_str, expr_value};
                                    LLVMBuildCall2(context->builder, context->printf_type, context->printf_func, args, 2, "printf_result");
                                }
                                break;
                            }
                            
                            // 对于单个字符串参数或多个参数的情况
                            int total_args = actual_arg_count;
                            LLVMValueRef *args = (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * total_args);
                            
                            // 处理第一个参数
                            ASTNode *first_arg = print_node->arguments;
                            if (first_arg->type == NODE_LITERAL && ((LiteralNode *)first_arg)->literal_type == LITERAL_STRING) {
                                // 第一个参数是字符串，直接作为格式字符串
                                LiteralNode *literal = (LiteralNode *)first_arg;
                                args[0] = LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value, "format_str");
                            } else {
                                // 第一个参数不是字符串，但不是唯一参数，需要添加格式字符串
                                // 这种情况比较特殊，这里简单处理为错误情况
                                LLVMValueRef error_str = LLVMBuildGlobalStringPtr(context->builder, "Error: First argument must be string in multi-argument print\n", "error_str");
                                LLVMBuildCall2(context->builder, context->printf_type, context->printf_func, &error_str, 1, "printf_error");
                                free(args);
                                break;
                            }
                            
                            // 处理剩余参数
                            int arg_index = 1;
                            ASTNode *next_arg = print_node->arguments->next;
                            while (next_arg && arg_index < total_args) {
                                LLVMValueRef arg_value = generate_expression(context, next_arg);
                                if (!arg_value) {
                                    // 错误处理
                                    LLVMValueRef error_str = LLVMBuildGlobalStringPtr(context->builder, "Error: Failed to generate expression in print statement\n", "error_str");
                                    LLVMBuildCall2(context->builder, context->printf_type, context->printf_func, &error_str, 1, "printf_error");
                                    free(args);
                                    break;
                                }
                                args[arg_index++] = arg_value;
                                next_arg = next_arg->next;
                            }
                            
                            // 调用printf函数
                            LLVMBuildCall2(context->builder, context->printf_type, context->printf_func, args, total_args, "printf_result");
                            free(args);
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
                    case NODE_IF_STATEMENT: {
                        LLVMBasicBlockRef insert_block = NULL;
                        generate_if_statement(context, (IfStatementNode *)statement, &insert_block);
                        if (insert_block) {
                            LLVMPositionBuilderAtEnd(context->builder, insert_block);
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

// TODO 写入目标代码到文件
int write_object_to_file(CodeGenContext *context, const char *filename) {
    // 简化的实现
    fprintf(stderr, "写入目标文件功能尚未完全实现\n");
    return -1;
}