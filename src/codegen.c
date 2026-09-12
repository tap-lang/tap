#include "codegen.h"
#include "helpers.h"

#include <limits.h>

extern int debug;

static enum LiteralType default_integer_type(void) {
    return LITERAL_I32;
}

static int is_integer_type(enum LiteralType type) {
    switch (type) {
        case LITERAL_INT:
        case LITERAL_UINT:
        case LITERAL_I8:
        case LITERAL_U8:
        case LITERAL_I16:
        case LITERAL_U16:
        case LITERAL_I32:
        case LITERAL_U32:
        case LITERAL_I64:
        case LITERAL_U64:
        case LITERAL_I128:
        case LITERAL_U128:
        case LITERAL_BOOL:
            return 1;
        default:
            return 0;
    }
}

static int is_unsigned_type(enum LiteralType type) {
    switch (type) {
        case LITERAL_UINT:
        case LITERAL_U8:
        case LITERAL_U16:
        case LITERAL_U32:
        case LITERAL_U64:
        case LITERAL_U128:
        case LITERAL_BOOL:
            return 1;
        default:
            return 0;
    }
}

static unsigned integer_type_bits(enum LiteralType type) {
    switch (type) {
        case LITERAL_BOOL: return 1;
        case LITERAL_I8:
        case LITERAL_U8: return 8;
        case LITERAL_I16:
        case LITERAL_U16: return 16;
        case LITERAL_I32:
        case LITERAL_U32: return 32;
        case LITERAL_I64:
        case LITERAL_U64: return 64;
        case LITERAL_I128:
        case LITERAL_U128: return 128;
        case LITERAL_INT:
        case LITERAL_UINT: return (unsigned)(sizeof(void *) * CHAR_BIT);
        default: return 0;
    }
}

static enum LiteralType integer_type_for(unsigned bits, int is_unsigned) {
    if (bits <= 8) return is_unsigned ? LITERAL_U8 : LITERAL_I8;
    if (bits <= 16) return is_unsigned ? LITERAL_U16 : LITERAL_I16;
    if (bits <= 32) return is_unsigned ? LITERAL_U32 : LITERAL_I32;
    if (bits <= 64) return is_unsigned ? LITERAL_U64 : LITERAL_I64;
    return is_unsigned ? LITERAL_U128 : LITERAL_I128;
}

static enum LiteralType common_integer_type(enum LiteralType left, enum LiteralType right) {
    unsigned left_bits = integer_type_bits(left);
    unsigned right_bits = integer_type_bits(right);
    unsigned bits = left_bits > right_bits ? left_bits : right_bits;
    int use_unsigned = is_unsigned_type(left) || is_unsigned_type(right);
    return integer_type_for(bits, use_unsigned);
}

static LLVMTypeRef get_llvm_type(CodeGenContext *context, enum LiteralType type) {
    if (is_integer_type(type)) {
        return LLVMIntTypeInContext(context->context, integer_type_bits(type));
    }

    switch (type) {
        case LITERAL_FLOAT:
        case LITERAL_F32:
            return LLVMFloatTypeInContext(context->context);
        case LITERAL_F64:
            return LLVMDoubleTypeInContext(context->context);
        case LITERAL_STRING:
            return LLVMPointerType(LLVMInt8TypeInContext(context->context), 0);
        default:
            fprintf(stderr, "unsupported type: %d\n", type); // 中文：不支持的类型
            return NULL;
    }
}

// Recursively lower scalar and multidimensional array types to LLVM types.
static LLVMTypeRef get_llvm_var_type(CodeGenContext *context, const VarTypeNode *type) {
    if (type->is_array) {
        return LLVMArrayType2(
            get_llvm_var_type(context, type->element_type), type->array_length);
    }
    return get_llvm_type(context, type->type);
}

static Symbol *find_symbol(CodeGenContext *context, const char *name) {
    for (Symbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) return symbol;
    }
    return NULL;
}

static void insert_symbol(CodeGenContext *context, const char *name, LLVMValueRef value,
                          enum LiteralType type, int is_const) {
    Symbol *symbol = malloc(sizeof(Symbol));
    if (!symbol) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    symbol->name = strdup(name);
    symbol->value = value;
    symbol->type = type;
    symbol->array_type = NULL;
    symbol->is_const = is_const;
    symbol->next = context->symbols;
    context->symbols = symbol;
}

static void insert_array_symbol(CodeGenContext *context, const char *name,
                                LLVMValueRef value, const VarTypeNode *array_type,
                                int is_const) {
    insert_symbol(context, name, value, array_type->type, is_const);
    // The declaration AST outlives Codegen, so no type copy is required here.
    context->symbols->array_type = array_type;
}

static void free_symbols(Symbol *symbols) {
    while (symbols) {
        Symbol *next = symbols->next;
        free(symbols->name);
        free(symbols);
        symbols = next;
    }
}

// Keep stack allocation bounded by placing every local slot in the function entry block.
static LLVMValueRef create_entry_alloca(
    CodeGenContext *context, LLVMTypeRef type, const char *name) {
    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(context->builder);
    LLVMValueRef function = LLVMGetBasicBlockParent(current_block);
    LLVMBasicBlockRef entry_block = LLVMGetEntryBasicBlock(function);
    LLVMBuilderRef alloca_builder = LLVMCreateBuilderInContext(context->context);
    LLVMValueRef first_instruction = LLVMGetFirstInstruction(entry_block);

    // Insert before existing instructions so the slot dominates every use in loops and branches.
    if (first_instruction) {
        LLVMPositionBuilderBefore(alloca_builder, first_instruction);
    } else {
        LLVMPositionBuilderAtEnd(alloca_builder, entry_block);
    }
    LLVMValueRef storage = LLVMBuildAlloca(alloca_builder, type, name);
    LLVMDisposeBuilder(alloca_builder);
    return storage;
}

static FunctionNode *find_function(CodeGenContext *context, const char *name) {
    if (!context->program) return NULL;
    for (ASTNode *node = context->program->functions; node; node = node->next) {
        if (node->type == NODE_FUNCTION) {
            FunctionNode *function = (FunctionNode *)node;
            if (strcmp(function->name, name) == 0) return function;
        }
    }
    return NULL;
}

// 非标识符接收者由 Parser 降低为保留的内部调用名称。
static int is_internal_string_len_call(const FunctionCallNode *call) {
    return strcmp(call->name, "__4yue_builtin_string_len") == 0;
}

// 标识符接收者沿用限定调用表示，此函数取出点号前的变量名范围。
static const char *named_string_len_receiver(
    const FunctionCallNode *call, size_t *receiver_length) {
    const char *dot = strchr(call->name, '.');
    if (!dot || dot == call->name || strchr(dot + 1, '.') || strcmp(dot + 1, "len") != 0) {
        return NULL;
    }
    *receiver_length = (size_t)(dot - call->name);
    return call->name;
}

// 接收者名称不是独立的零结尾字符串，因此按指定长度查询局部符号。
static Symbol *find_symbol_with_length(
    CodeGenContext *context, const char *name, size_t length) {
    for (Symbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        if (strlen(symbol->name) == length && strncmp(symbol->name, name, length) == 0) {
            return symbol;
        }
    }
    return NULL;
}

static enum LiteralType function_return_type(FunctionNode *function) {
    return function && function->return_type ? function->return_type->type : default_integer_type();
}

static const VarTypeNode *function_return_var_type(FunctionNode *function) {
    return function ? function->return_type : NULL;
}

static enum LiteralType function_param_type(FunctionNode *function, unsigned index) {
    ASTNode *type = function ? function->param_types : NULL;
    while (type && index > 0) {
        type = type->next;
        index--;
    }
    return type && type->type == NODE_VAR_TYPE
        ? ((VarTypeNode *)type)->type
        : default_integer_type();
}

static const VarTypeNode *function_param_var_type(FunctionNode *function, unsigned index) {
    ASTNode *type = function ? function->param_types : NULL;
    while (type && index > 0) {
        type = type->next;
        index--;
    }
    return type && type->type == NODE_VAR_TYPE ? (VarTypeNode *)type : NULL;
}

static const char *llvm_function_name(const FunctionNode *function) {
    return function && !function->is_extern && strcmp(function->name, "main") == 0
        ? "__4yue_user_main"
        : function->name;
}

static const char *llvm_call_name(const char *name) {
    return strcmp(name, "main") == 0 ? "__4yue_user_main" : name;
}

static int var_type_equal(const VarTypeNode *left, const VarTypeNode *right) {
    if (!left || !right) return left == right;
    if (left->is_array != right->is_array) return 0;
    if (!left->is_array) return left->type == right->type;
    return left->array_length == right->array_length &&
           var_type_equal(left->element_type, right->element_type);
}

// Resolve the recursive type produced by an identifier or a chain of indexes.
static const VarTypeNode *indexed_value_type(CodeGenContext *context, ASTNode *expression) {
    if (expression && expression->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)expression)->name;
        Symbol *symbol = find_symbol(context, name);
        if (!symbol) {
            fprintf(stderr, "error: undefined variable '%s'\n", name); // 中文：未定义的变量
            exit(1);
        }
        if (!symbol->array_type) {
            fprintf(stderr, "error: variable '%s' is not an array\n", name); // 中文：变量不是数组
            exit(1);
        }
        return symbol->array_type;
    }

    if (expression && expression->type == NODE_INDEX_EXPRESSION) {
        IndexExpressionNode *index = (IndexExpressionNode *)expression;
        const VarTypeNode *container_type = indexed_value_type(context, index->array);
        if (!container_type->is_array) {
            fprintf(stderr, "error: index target is not an array\n"); // 中文：索引目标不是数组
            exit(1);
        }
        return container_type->element_type;
    }

    if (expression && expression->type == NODE_FUNCTION_CALL) {
        FunctionCallNode *call = (FunctionCallNode *)expression;
        FunctionNode *function = find_function(context, call->name);
        const VarTypeNode *return_type = function_return_var_type(function);
        if (!return_type || !return_type->is_array) {
            fprintf(stderr, "error: function '%s' does not return an array\n", call->name); // 中文：函数不返回数组
            exit(1);
        }
        return return_type;
    }

    fprintf(stderr, "error: invalid array index target\n"); // 中文：数组索引目标无效
    exit(1);
}

static enum LiteralType expression_type(CodeGenContext *context, ASTNode *expression) {
    if (!expression) return default_integer_type();

    switch (expression->type) {
        case NODE_LITERAL:
            return ((LiteralNode *)expression)->literal_type;
        case NODE_IDENTIFIER: {
            Symbol *symbol = find_symbol(context, ((IdentifierNode *)expression)->name);
            return symbol ? symbol->type : default_integer_type();
        }
        case NODE_INDEX_EXPRESSION: {
            const VarTypeNode *type = indexed_value_type(context, expression);
            if (type->is_array) {
                fprintf(stderr, "error: multidimensional arrays must be indexed to a scalar element\n"); // 中文：多维数组必须索引到标量元素
                exit(1);
            }
            return type->type;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            size_t receiver_length = 0;
            // len() 返回与目标架构指针同宽的无符号整数。
            if (is_internal_string_len_call(call) ||
                (named_string_len_receiver(call, &receiver_length) &&
                 !find_function(context, call->name))) {
                return LITERAL_UINT;
            }
            FunctionNode *function = find_function(context, call->name);
            return function_return_type(function);
        }
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            if (binary->op_type >= OP_EQUAL && binary->op_type <= OP_GREATER_THAN_OR_EQUAL) {
                return LITERAL_BOOL;
            }
            return common_integer_type(expression_type(context, binary->left),
                                       expression_type(context, binary->right));
        }
        default:
            return default_integer_type();
    }
}

static LLVMValueRef cast_integer(CodeGenContext *context, LLVMValueRef value,
                                 enum LiteralType source, enum LiteralType target) {
    if (!value || source == target) return value;
    if (!is_integer_type(source) || !is_integer_type(target)) return value;

    unsigned source_bits = integer_type_bits(source);
    unsigned target_bits = integer_type_bits(target);
    LLVMTypeRef target_type = get_llvm_type(context, target);

    if (target == LITERAL_BOOL && source_bits != 1) {
        LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(value), 0, 0);
        return LLVMBuildICmp(context->builder, LLVMIntNE, value, zero, "to_bool");
    }
    if (source_bits == target_bits) return value;
    if (source_bits > target_bits) {
        return LLVMBuildTrunc(context->builder, value, target_type, "int_trunc");
    }
    if (is_unsigned_type(source)) {
        return LLVMBuildZExt(context->builder, value, target_type, "int_zext");
    }
    return LLVMBuildSExt(context->builder, value, target_type, "int_sext");
}

static LLVMValueRef generate_expression(CodeGenContext *context, ASTNode *expression);
static LLVMValueRef generate_function_call(CodeGenContext *context, FunctionCallNode *call);
static LLVMValueRef generate_integer_binary(CodeGenContext *context, BinaryOpNode *binary,
                                            enum LiteralType operand_type);
static unsigned function_param_count(FunctionNode *function);

static LLVMValueRef integer_constant(CodeGenContext *context, LiteralNode *literal,
                                     enum LiteralType type) {
    LLVMTypeRef llvm_type = get_llvm_type(context, type);
    if (literal->integer_text) {
        return LLVMConstIntOfStringAndSize(llvm_type, literal->integer_text,
                                          (unsigned)strlen(literal->integer_text), 10);
    }
    return LLVMConstInt(llvm_type, literal->value.int_value, 0);
}

static LLVMValueRef generate_expression_as(CodeGenContext *context, ASTNode *expression,
                                           enum LiteralType target) {
    if (expression && expression->type == NODE_LITERAL && is_integer_type(target)) {
        LiteralNode *literal = (LiteralNode *)expression;
        if (is_integer_type(literal->literal_type)) {
            return integer_constant(context, literal, target);
        }
    }
    if (expression && expression->type == NODE_BINARY_OP && is_integer_type(target)) {
        BinaryOpNode *binary = (BinaryOpNode *)expression;
        if (binary->op_type >= OP_ADD && binary->op_type <= OP_DIVIDE) {
            return generate_integer_binary(context, binary, target);
        }
    }

    enum LiteralType source = expression_type(context, expression);
    return cast_integer(context, generate_expression(context, expression), source, target);
}

static LLVMValueRef generate_index_address(
    CodeGenContext *context, IndexExpressionNode *index_expression,
    const VarTypeNode **element_type_out) {
    LLVMValueRef array_address = NULL;
    const VarTypeNode *array_type = NULL;

    // The first index starts from array storage; later indexes start from a subarray address.
    if (index_expression->array &&
        index_expression->array->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)index_expression->array)->name;
        Symbol *symbol = find_symbol(context, name);
        if (!symbol) {
            fprintf(stderr, "error: undefined variable '%s'\n", name); // 中文：未定义的变量
            exit(1);
        }
        if (!symbol->array_type) {
            fprintf(stderr, "error: variable '%s' is not an array\n", name); // 中文：变量不是数组
            exit(1);
        }
        array_address = symbol->value;
        array_type = symbol->array_type;
    } else if (index_expression->array &&
               index_expression->array->type == NODE_INDEX_EXPRESSION) {
        array_address = generate_index_address(
            context, (IndexExpressionNode *)index_expression->array, &array_type);
        if (!array_type->is_array) {
            fprintf(stderr, "error: index target is not an array\n"); // 中文：索引目标不是数组
            exit(1);
        }
    } else if (index_expression->array &&
               index_expression->array->type == NODE_FUNCTION_CALL) {
        FunctionCallNode *call = (FunctionCallNode *)index_expression->array;
        FunctionNode *function = find_function(context, call->name);
        array_type = function_return_var_type(function);
        if (!array_type || !array_type->is_array) {
            fprintf(stderr, "error: function '%s' does not return an array\n", call->name); // 中文：函数不返回数组
            exit(1);
        }
        LLVMTypeRef llvm_array_type = get_llvm_var_type(context, array_type);
        array_address = create_entry_alloca(context, llvm_array_type, "array_return_tmp");
        LLVMBuildStore(context->builder, generate_function_call(context, call), array_address);
    } else {
        fprintf(stderr, "error: invalid array index target\n"); // 中文：数组索引目标无效
        exit(1);
    }

    // Every dimension performs its own signed/unsigned bounds check.
    enum LiteralType index_type = expression_type(context, index_expression->index);
    if (!is_integer_type(index_type) || integer_type_bits(index_type) > 64) {
        fprintf(stderr, "error: array index must be an integer of at most 64 bits\n"); // 中文：数组下标必须是最多 64 位的整数
        exit(1);
    }

    LLVMValueRef raw_index = generate_expression(context, index_expression->index);
    enum LiteralType check_type = is_unsigned_type(index_type) ? LITERAL_U64 : LITERAL_I64;
    LLVMValueRef index = cast_integer(context, raw_index, index_type, check_type);
    LLVMTypeRef index_llvm_type = get_llvm_type(context, check_type);
    LLVMValueRef zero = LLVMConstInt(index_llvm_type, 0, 0);
    LLVMValueRef length = LLVMConstInt(index_llvm_type, array_type->array_length, 0);
    LLVMValueRef lower_ok = is_unsigned_type(index_type)
        ? LLVMConstInt(LLVMInt1TypeInContext(context->context), 1, 0)
        : LLVMBuildICmp(context->builder, LLVMIntSGE, index, zero, "array_index_nonnegative");
    LLVMValueRef upper_ok = LLVMBuildICmp(
        context->builder,
        is_unsigned_type(index_type) ? LLVMIntULT : LLVMIntSLT,
        index, length, "array_index_in_range");
    LLVMValueRef in_bounds = LLVMBuildAnd(
        context->builder, lower_ok, upper_ok, "array_index_valid");

    LLVMValueRef function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(context->builder));
    LLVMBasicBlockRef pass_block =
        LLVMAppendBasicBlockInContext(context->context, function, "array_index_pass");
    LLVMBasicBlockRef fail_block =
        LLVMAppendBasicBlockInContext(context->context, function, "array_index_fail");
    LLVMBuildCondBr(context->builder, in_bounds, pass_block, fail_block);

    LLVMPositionBuilderAtEnd(context->builder, fail_block);
    LLVMValueRef printf_arguments[3] = {
        LLVMBuildGlobalStringPtr(context->builder,
            "Array index out of bounds: index=%lld, length=%llu\n", "array_bounds_format"), // 中文：数组下标越界：下标、长度
        index,
        LLVMConstInt(LLVMInt64TypeInContext(context->context), array_type->array_length, 0)
    };
    LLVMBuildCall2(context->builder, context->printf_type, context->printf_func,
                   printf_arguments, 3, "array_bounds_printf");
    LLVMValueRef exit_argument =
        LLVMConstInt(LLVMInt32TypeInContext(context->context), 1, 0);
    LLVMBuildCall2(context->builder, context->exit_type, context->exit_func,
                   &exit_argument, 1, "");
    LLVMBuildUnreachable(context->builder);

    LLVMPositionBuilderAtEnd(context->builder, pass_block);
    LLVMTypeRef llvm_array_type = get_llvm_var_type(context, array_type);
    LLVMValueRef indexes[2] = {
        LLVMConstInt(LLVMInt64TypeInContext(context->context), 0, 0),
        index
    };
    if (element_type_out) *element_type_out = array_type->element_type;
    return LLVMBuildGEP2(context->builder, llvm_array_type, array_address,
                         indexes, 2, "array_element_ptr");
}

static LLVMValueRef generate_integer_binary(CodeGenContext *context, BinaryOpNode *binary,
                                            enum LiteralType operand_type) {
    LLVMValueRef left = generate_expression_as(context, binary->left, operand_type);
    LLVMValueRef right = generate_expression_as(context, binary->right, operand_type);

    switch (binary->op_type) {
        case OP_ADD: return LLVMBuildAdd(context->builder, left, right, "add_result");
        case OP_SUBTRACT: return LLVMBuildSub(context->builder, left, right, "sub_result");
        case OP_MULTIPLY: return LLVMBuildMul(context->builder, left, right, "mul_result");
        case OP_DIVIDE:
            return is_unsigned_type(operand_type)
                ? LLVMBuildUDiv(context->builder, left, right, "udiv_result")
                : LLVMBuildSDiv(context->builder, left, right, "sdiv_result");
        case OP_EQUAL:
            return LLVMBuildICmp(context->builder, LLVMIntEQ, left, right, "eq_result");
        case OP_NOT_EQUAL:
            return LLVMBuildICmp(context->builder, LLVMIntNE, left, right, "ne_result");
        case OP_LESS_THAN:
            return LLVMBuildICmp(context->builder,
                is_unsigned_type(operand_type) ? LLVMIntULT : LLVMIntSLT,
                left, right, "lt_result");
        case OP_GREATER_THAN:
            return LLVMBuildICmp(context->builder,
                is_unsigned_type(operand_type) ? LLVMIntUGT : LLVMIntSGT,
                left, right, "gt_result");
        case OP_LESS_THAN_OR_EQUAL:
            return LLVMBuildICmp(context->builder,
                is_unsigned_type(operand_type) ? LLVMIntULE : LLVMIntSLE,
                left, right, "le_result");
        case OP_GREATER_THAN_OR_EQUAL:
            return LLVMBuildICmp(context->builder,
                is_unsigned_type(operand_type) ? LLVMIntUGE : LLVMIntSGE,
                left, right, "ge_result");
        default:
            fprintf(stderr, "error: unsupported binary operator\n"); // 中文：不支持的二元操作符
            exit(1);
    }
}

static LLVMValueRef generate_string_length(
    CodeGenContext *context, FunctionCallNode *call,
    const char *receiver_name, size_t receiver_length) {
    LLVMValueRef string_value = NULL;
    unsigned argument_count = 0;
    for (ASTNode *argument = call->arguments; argument; argument = argument->next) {
        argument_count++;
    }

    if (receiver_name) {
        // name.len() 的接收者保存在限定调用名称中，实参数量必须为零。
        if (argument_count != 0) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "method 'len' expects 0 arguments, but got %u",
                             argument_count);
            exit(1);
        }
        Symbol *symbol = find_symbol_with_length(context, receiver_name, receiver_length);
        if (!symbol) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "undefined variable '%.*s'",
                             (int)receiver_length, receiver_name);
            exit(1);
        }
        if (symbol->array_type || symbol->type != LITERAL_STRING) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "method 'len' is only available on string values");
            exit(1);
        }
        string_value = LLVMBuildLoad2(
            context->builder, get_llvm_type(context, LITERAL_STRING),
            symbol->value, "string_len_receiver");
    } else {
        // 其他后缀形式把接收者放在内部调用的第一个参数中。
        unsigned user_argument_count = argument_count > 0 ? argument_count - 1 : 0;
        if (argument_count != 1) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "method 'len' expects 0 arguments, but got %u",
                             user_argument_count);
            exit(1);
        }
        ASTNode *receiver_expression = call->arguments;
        if (expression_type(context, receiver_expression) != LITERAL_STRING) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "method 'len' is only available on string values");
            exit(1);
        }
        string_value = generate_expression(context, receiver_expression);
    }

    // 从零开始逐字节扫描，以第一个结尾空字节的位置作为 UTF-8 字节长度。
    LLVMTypeRef index_type = get_llvm_type(context, LITERAL_UINT);
    LLVMTypeRef byte_type = LLVMInt8TypeInContext(context->context);
    LLVMBasicBlockRef initial_block = LLVMGetInsertBlock(context->builder);
    LLVMValueRef function = LLVMGetBasicBlockParent(initial_block);
    LLVMBasicBlockRef condition_block = LLVMAppendBasicBlockInContext(
        context->context, function, "string_len_condition");
    LLVMBasicBlockRef increment_block = LLVMAppendBasicBlockInContext(
        context->context, function, "string_len_increment");
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(
        context->context, function, "string_len_end");

    LLVMBuildBr(context->builder, condition_block);
    LLVMPositionBuilderAtEnd(context->builder, condition_block);
    LLVMValueRef index = LLVMBuildPhi(context->builder, index_type, "string_length");
    LLVMValueRef zero_index = LLVMConstInt(index_type, 0, 0);
    LLVMAddIncoming(index, &zero_index, &initial_block, 1);
    LLVMValueRef byte_address = LLVMBuildGEP2(
        context->builder, byte_type, string_value, &index, 1, "string_byte_address");
    LLVMValueRef byte = LLVMBuildLoad2(
        context->builder, byte_type, byte_address, "string_byte");
    LLVMValueRef at_end = LLVMBuildICmp(
        context->builder, LLVMIntEQ, byte, LLVMConstInt(byte_type, 0, 0),
        "string_len_at_end");
    LLVMBuildCondBr(context->builder, at_end, end_block, increment_block);

    LLVMPositionBuilderAtEnd(context->builder, increment_block);
    LLVMValueRef next_index = LLVMBuildAdd(
        context->builder, index, LLVMConstInt(index_type, 1, 0), "string_len_next");
    LLVMBuildBr(context->builder, condition_block);
    LLVMAddIncoming(index, &next_index, &increment_block, 1);

    LLVMPositionBuilderAtEnd(context->builder, end_block);
    return index;
}

static LLVMValueRef generate_function_call(CodeGenContext *context, FunctionCallNode *call) {
    size_t receiver_length = 0;
    const char *receiver_name = named_string_len_receiver(call, &receiver_length);
    // 已解析到真实函数的 module.len() 优先按模块调用处理。
    if (receiver_name && find_function(context, call->name)) receiver_name = NULL;
    if (is_internal_string_len_call(call) || receiver_name) {
        return generate_string_length(context, call, receiver_name, receiver_length);
    }

    if (strcmp(call->name, "assert") == 0) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "assert can only be used as a statement"); // 中文：assert 只能作为语句使用
        exit(1);
    }

    FunctionNode *function = find_function(context, call->name);
    const char *callee_name = function ? llvm_call_name(call->name) : call->name;
    LLVMValueRef llvm_function = LLVMGetNamedFunction(context->module, callee_name);
    if (!llvm_function || !function) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "undefined function '%s'", call->name); // 中文：未定义的函数
        exit(1);
    }

    unsigned count = 0;
    for (ASTNode *argument = call->arguments; argument; argument = argument->next) count++;
    unsigned expected_count = function_param_count(function);
    if (count != expected_count) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "function '%s' expects %u arguments, but got %u",
                         call->name, expected_count, count);
        exit(1);
    }

    LLVMValueRef *arguments = count ? malloc(sizeof(LLVMValueRef) * count) : NULL;
    ASTNode *argument = call->arguments;
    for (unsigned i = 0; i < count; i++, argument = argument->next) {
        arguments[i] = generate_expression_as(context, argument, function_param_type(function, i));
    }

    LLVMTypeRef function_type = LLVMGlobalGetValueType(llvm_function);
    LLVMValueRef value = LLVMBuildCall2(context->builder, function_type, llvm_function,
                                        arguments, count, "call_result");
    free(arguments);
    return value;
}

static LLVMValueRef generate_expression(CodeGenContext *context, ASTNode *expression) {
    if (!expression) return NULL;

    switch (expression->type) {
        case NODE_LITERAL: {
            LiteralNode *literal = (LiteralNode *)expression;
            if (is_integer_type(literal->literal_type)) {
                return integer_constant(context, literal, literal->literal_type);
            }
            if (literal->literal_type == LITERAL_STRING) {
                return LLVMBuildGlobalStringPtr(context->builder, literal->value.string_value,
                                                "string_literal");
            }
            if (literal->literal_type == LITERAL_FLOAT || literal->literal_type == LITERAL_F32 ||
                literal->literal_type == LITERAL_F64) {
                return LLVMConstReal(get_llvm_type(context, literal->literal_type),
                                     literal->value.float_value);
            }
            break;
        }
        case NODE_IDENTIFIER: {
            IdentifierNode *identifier = (IdentifierNode *)expression;
            Symbol *symbol = find_symbol(context, identifier->name);
            if (!symbol) {
                fprintf(stderr, "error: undefined variable '%s'\n", identifier->name); // 中文：未定义的变量
                exit(1);
            }
            if (symbol->array_type) {
                fprintf(stderr, "error: array '%s' must be accessed with an index\n", identifier->name); // 中文：数组必须通过下标访问
                exit(1);
            }
            return LLVMBuildLoad2(context->builder, get_llvm_type(context, symbol->type),
                                  symbol->value, "loaded_var");
        }
        case NODE_INDEX_EXPRESSION: {
            const VarTypeNode *element_type = NULL;
            LLVMValueRef address = generate_index_address(
                context, (IndexExpressionNode *)expression, &element_type);
            if (element_type->is_array) {
                fprintf(stderr, "error: multidimensional arrays must be indexed to a scalar element\n"); // 中文：多维数组必须索引到标量元素
                exit(1);
            }
            return LLVMBuildLoad2(context->builder, get_llvm_var_type(context, element_type),
                                  address, "array_element");
        }
        case NODE_ARRAY_LITERAL:
            fprintf(stderr, "error: array literals can only be used to initialize array variables\n"); // 中文：数组字面量只能用于数组变量初始化
            exit(1);
        case NODE_FUNCTION_CALL:
            {
                FunctionCallNode *call = (FunctionCallNode *)expression;
                FunctionNode *function = find_function(context, call->name);
                const VarTypeNode *return_type = function_return_var_type(function);
                if (return_type && return_type->is_array) {
                    fprintf(stderr, "error: array-returning function '%s' must be used as an array value\n",
                            call->name); // 中文：返回数组的函数必须作为数组值使用
                    exit(1);
                }
            }
            return generate_function_call(context, (FunctionCallNode *)expression);
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            enum LiteralType left_type = expression_type(context, binary->left);
            enum LiteralType right_type = expression_type(context, binary->right);
            enum LiteralType operand_type = common_integer_type(left_type, right_type);
            return generate_integer_binary(context, binary, operand_type);
        }
        default:
            break;
    }

    fprintf(stderr, "error: unsupported expression type\n"); // 中文：不支持的表达式类型
    exit(1);
}

static LLVMValueRef promote_printf_integer(CodeGenContext *context, ASTNode *expression) {
    enum LiteralType type = expression_type(context, expression);
    LLVMValueRef value = generate_expression(context, expression);
    if (!is_integer_type(type)) return value;
    if (integer_type_bits(type) < 32) {
        return cast_integer(context, value, type, LITERAL_I32);
    }
    return value;
}

static void generate_print(CodeGenContext *context, PrintNode *print_node) {
    if (!print_node->arguments || !context->printf_func) return;

    ASTNode *first = print_node->arguments;
    unsigned count = 0;
    for (ASTNode *argument = first; argument; argument = argument->next) count++;

    if (first->type == NODE_LITERAL &&
        ((LiteralNode *)first)->literal_type == LITERAL_STRING) {
        LLVMValueRef *arguments = malloc(sizeof(LLVMValueRef) * count);
        arguments[0] = LLVMBuildGlobalStringPtr(context->builder,
            ((LiteralNode *)first)->value.string_value, "format_string");
        ASTNode *argument = first->next;
        for (unsigned i = 1; i < count; i++, argument = argument->next) {
            arguments[i] = promote_printf_integer(context, argument);
        }
        LLVMBuildCall2(context->builder, context->printf_type, context->printf_func,
                       arguments, count, "printf_result");
        free(arguments);
        return;
    }

    enum LiteralType type = expression_type(context, first);
    const char *format = "%d";
    if (is_integer_type(type) && integer_type_bits(type) > 32) {
        format = is_unsigned_type(type) ? "%llu" : "%lld";
    } else if (is_unsigned_type(type)) {
        format = "%u";
    }

    LLVMValueRef arguments[2] = {
        LLVMBuildGlobalStringPtr(context->builder, format, "format_string"),
        promote_printf_integer(context, first)
    };
    LLVMBuildCall2(context->builder, context->printf_type, context->printf_func,
                   arguments, 2, "printf_result");
}

static void generate_statement_list(CodeGenContext *context, ASTNode *statement);

static void generate_assignment(CodeGenContext *context, AssignmentNode *assignment) {
    Symbol *symbol = find_symbol(context, assignment->name);
    if (!symbol) {
        fprintf(stderr, "error: undefined variable '%s'\n", assignment->name); // 中文：未定义的变量
        exit(1);
    }
    if (symbol->is_const) {
        fprintf(stderr, "error: cannot assign to constant '%s'\n", assignment->name); // 中文：不能给常量赋值
        exit(1);
    }
    if (symbol->array_type) {
        fprintf(stderr, "error: assigning an entire array is not supported yet\n"); // 中文：第一版数组暂不支持整个数组赋值
        exit(1);
    }
    LLVMValueRef value = generate_expression_as(context, assignment->expression, symbol->type);
    LLVMBuildStore(context->builder, value, symbol->value);
}

static int array_element_type_compatible(
    enum LiteralType expected, enum LiteralType actual) {
    return expected == actual || (is_integer_type(expected) && is_integer_type(actual));
}

// Recursively validate nested literals and store every scalar leaf.
static void generate_array_initializer(
    CodeGenContext *context, LLVMValueRef storage,
    const VarTypeNode *array_type, ArrayLiteralNode *literal) {
    uint64_t initializer_count = literal->is_repeat
        ? literal->repeat_count
        : literal->count;
    if (initializer_count != array_type->array_length) {
        fprintf(stderr,
            "error: array initializer has %llu elements, but declared length is %llu\n", // 中文：数组初始化元素数量与声明长度不一致
            (unsigned long long)initializer_count,
            (unsigned long long)array_type->array_length);
        exit(1);
    }

    LLVMTypeRef llvm_array_type = get_llvm_var_type(context, array_type);
    const VarTypeNode *element_type = array_type->element_type;
    ASTNode *element = literal->elements;
    for (uint64_t index = 0; index < initializer_count; index++) {
        ASTNode *current_element = literal->is_repeat ? literal->elements : element;
        LLVMValueRef indexes[2] = {
            LLVMConstInt(LLVMInt64TypeInContext(context->context), 0, 0),
            LLVMConstInt(LLVMInt64TypeInContext(context->context), index, 0)
        };
        LLVMValueRef element_address = LLVMBuildGEP2(
            context->builder, llvm_array_type, storage, indexes, 2,
            "array_init_element_ptr");

        if (element_type->is_array) {
            if (!current_element || current_element->type != NODE_ARRAY_LITERAL) {
                fprintf(stderr, "error: multidimensional array initialization requires nested array literals\n"); // 中文：多维数组初始化需要嵌套数组字面量
                exit(1);
            }
            generate_array_initializer(
                context, element_address, element_type, (ArrayLiteralNode *)current_element);
            if (!literal->is_repeat) element = element->next;
            continue;
        }

        if (!current_element || current_element->type == NODE_ARRAY_LITERAL) {
            fprintf(stderr, "error: array element type mismatch\n"); // 中文：数组元素类型不匹配
            exit(1);
        }
        enum LiteralType actual_type = expression_type(context, current_element);
        if (!array_element_type_compatible(element_type->type, actual_type)) {
            fprintf(stderr, "error: array element type mismatch\n"); // 中文：数组元素类型不匹配
            exit(1);
        }
        LLVMValueRef value = generate_expression_as(
            context, current_element, element_type->type);
        LLVMBuildStore(context->builder, value, element_address);
        if (!literal->is_repeat) element = element->next;
    }
}

static LLVMValueRef generate_array_value(
    CodeGenContext *context, ASTNode *expression, const VarTypeNode *expected_type) {
    if (!expected_type || !expected_type->is_array) {
        fprintf(stderr, "error: expected array type\n"); // 中文：期望数组类型
        exit(1);
    }

    LLVMTypeRef llvm_array_type = get_llvm_var_type(context, expected_type);
    if (expression && expression->type == NODE_IDENTIFIER) {
        IdentifierNode *identifier = (IdentifierNode *)expression;
        Symbol *symbol = find_symbol(context, identifier->name);
        if (!symbol) {
            fprintf(stderr, "error: undefined variable '%s'\n", identifier->name); // 中文：未定义的变量
            exit(1);
        }
        if (!symbol->array_type || !var_type_equal(symbol->array_type, expected_type)) {
            fprintf(stderr, "error: array type mismatch\n"); // 中文：数组类型不匹配
            exit(1);
        }
        return LLVMBuildLoad2(context->builder, llvm_array_type,
                              symbol->value, "array_value");
    }

    if (expression && expression->type == NODE_FUNCTION_CALL) {
        FunctionCallNode *call = (FunctionCallNode *)expression;
        FunctionNode *function = find_function(context, call->name);
        const VarTypeNode *return_type = function_return_var_type(function);
        if (!return_type || !return_type->is_array ||
            !var_type_equal(return_type, expected_type)) {
            fprintf(stderr, "error: array return type mismatch\n"); // 中文：数组返回类型不匹配
            exit(1);
        }
        return generate_function_call(context, call);
    }

    if (expression && expression->type == NODE_INDEX_EXPRESSION) {
        const VarTypeNode *element_type = NULL;
        LLVMValueRef address = generate_index_address(
            context, (IndexExpressionNode *)expression, &element_type);
        if (!element_type || !element_type->is_array ||
            !var_type_equal(element_type, expected_type)) {
            fprintf(stderr, "error: array type mismatch\n"); // 中文：数组类型不匹配
            exit(1);
        }
        return LLVMBuildLoad2(context->builder, llvm_array_type,
                              address, "array_slice_value");
    }

    fprintf(stderr, "error: expression does not produce an array value\n"); // 中文：表达式不产生数组值
    exit(1);
}


static const char *index_base_name(ASTNode *expression) {
    if (!expression) return NULL;
    if (expression->type == NODE_IDENTIFIER) {
        return ((IdentifierNode *)expression)->name;
    }
    if (expression->type == NODE_INDEX_EXPRESSION) {
        return index_base_name(((IndexExpressionNode *)expression)->array);
    }
    return NULL;
}

static void generate_index_assignment(
    CodeGenContext *context, IndexAssignmentNode *assignment) {
    const char *base_name = index_base_name((ASTNode *)assignment->target);
    Symbol *symbol = base_name ? find_symbol(context, base_name) : NULL;
    if (symbol && symbol->is_const) {
        fprintf(stderr, "error: cannot assign to constant '%s'\n", base_name); // 中文：不能给常量赋值
        exit(1);
    }

    const VarTypeNode *element_type = NULL;
    LLVMValueRef address = generate_index_address(
        context, assignment->target, &element_type);
    if (element_type->is_array) {
        fprintf(stderr, "error: assigning an entire subarray is not supported yet\n"); // 中文：暂不支持整个子数组赋值
        exit(1);
    }
    enum LiteralType actual_type = expression_type(context, assignment->expression);
    if (!array_element_type_compatible(element_type->type, actual_type)) {
        fprintf(stderr, "error: array element assignment type mismatch\n"); // 中文：数组元素赋值类型不匹配
        exit(1);
    }
    LLVMValueRef value = generate_expression_as(
        context, assignment->expression, element_type->type);
    LLVMBuildStore(context->builder, value, address);
}

static LLVMValueRef condition_value(CodeGenContext *context, ASTNode *condition) {
    LLVMValueRef value = generate_expression(context, condition);
    enum LiteralType type = expression_type(context, condition);
    if (type == LITERAL_BOOL) return value;
    if (is_integer_type(type)) {
        LLVMValueRef zero = LLVMConstInt(get_llvm_type(context, type), 0, 0);
        return LLVMBuildICmp(context->builder, LLVMIntNE, value, zero, "if_condition");
    }
    fprintf(stderr, "error: if condition must be an integer or boolean expression\n"); // 中文：if 条件必须是整数或布尔表达式
    exit(1);
}

static void generate_assert(CodeGenContext *context, FunctionCallNode *call) {
    unsigned count = 0;
    for (ASTNode *argument = call->arguments; argument; argument = argument->next) count++;

    if (count < 1 || count > 2) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "assert expects one or two arguments"); // 中文：assert 需要一到两个参数
        exit(1);
    }

    const char *message = "condition is false";
    if (count == 2) {
        ASTNode *message_node = call->arguments->next;
        if (message_node->type != NODE_LITERAL ||
            ((LiteralNode *)message_node)->literal_type != LITERAL_STRING) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "assert message must be a string literal"); // 中文：assert 消息必须是字符串字面量
            exit(1);
        }
        message = ((LiteralNode *)message_node)->value.string_value;
    }

    LLVMValueRef function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(context->builder));
    LLVMBasicBlockRef pass_block =
        LLVMAppendBasicBlockInContext(context->context, function, "assert_pass");
    LLVMBasicBlockRef fail_block =
        LLVMAppendBasicBlockInContext(context->context, function, "assert_fail");

    LLVMBuildCondBr(context->builder, condition_value(context, call->arguments),
                    pass_block, fail_block);

    LLVMPositionBuilderAtEnd(context->builder, fail_block);
    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
    LLVMValueRef printf_arguments[5] = {
        LLVMBuildGlobalStringPtr(context->builder,
            "Assertion failed at %s:%d:%d: %s\n", "assert_format"), // 中文：断言失败：文件、行、列、消息
        LLVMBuildGlobalStringPtr(context->builder,
            call->filename ? call->filename : "<unknown>", "assert_filename"),
        LLVMConstInt(int32_type, (unsigned)call->line, 0),
        LLVMConstInt(int32_type, (unsigned)call->column, 0),
        LLVMBuildGlobalStringPtr(context->builder, message, "assert_message")
    };
    LLVMBuildCall2(context->builder, context->printf_type, context->printf_func,
                   printf_arguments, 5, "assert_printf");

    LLVMValueRef exit_argument = LLVMConstInt(int32_type, 1, 0);
    LLVMBuildCall2(context->builder, context->exit_type, context->exit_func,
                   &exit_argument, 1, "");
    LLVMBuildUnreachable(context->builder);

    LLVMPositionBuilderAtEnd(context->builder, pass_block);
}

static void generate_if_statement(CodeGenContext *context, IfStatementNode *if_node) {
    LLVMValueRef function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(context->builder));
    LLVMBasicBlockRef then_block = LLVMAppendBasicBlockInContext(context->context, function, "if_then");
    LLVMBasicBlockRef else_block = LLVMAppendBasicBlockInContext(context->context, function, "if_else");
    LLVMBasicBlockRef merge_block = LLVMAppendBasicBlockInContext(context->context, function, "if_end");

    LLVMBuildCondBr(context->builder, condition_value(context, if_node->condition),
                    then_block, else_block);

    LLVMPositionBuilderAtEnd(context->builder, then_block);
    generate_statement_list(context, if_node->consequence);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(context->builder))) {
        LLVMBuildBr(context->builder, merge_block);
    }

    LLVMPositionBuilderAtEnd(context->builder, else_block);
    if (if_node->alternative) {
        if (if_node->alternative->type == NODE_IF_STATEMENT) {
            generate_if_statement(context, (IfStatementNode *)if_node->alternative);
        } else {
            generate_statement_list(context, if_node->alternative);
        }
    }
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(context->builder))) {
        LLVMBuildBr(context->builder, merge_block);
    }

    LLVMPositionBuilderAtEnd(context->builder, merge_block);
}

static void generate_for_statement(CodeGenContext *context, ForStatementNode *for_node) {
    if (for_node->initializer) {
        generate_statement_list(context, for_node->initializer);
    }

    LLVMValueRef function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(context->builder));
    LLVMBasicBlockRef condition_block =
        LLVMAppendBasicBlockInContext(context->context, function, "for_condition");
    LLVMBasicBlockRef body_block =
        LLVMAppendBasicBlockInContext(context->context, function, "for_body");
    LLVMBasicBlockRef update_block =
        LLVMAppendBasicBlockInContext(context->context, function, "for_update");
    LLVMBasicBlockRef end_block =
        LLVMAppendBasicBlockInContext(context->context, function, "for_end");

    LLVMBuildBr(context->builder, condition_block);

    LLVMPositionBuilderAtEnd(context->builder, condition_block);
    LLVMValueRef condition = for_node->condition
        ? condition_value(context, for_node->condition)
        : LLVMConstInt(LLVMInt1TypeInContext(context->context), 1, 0);
    LLVMBuildCondBr(context->builder, condition, body_block, end_block);

    LLVMPositionBuilderAtEnd(context->builder, body_block);
    LoopContext loop_context = {
        .continue_block = update_block,
        .break_block = end_block,
        .parent = context->current_loop
    };
    context->current_loop = &loop_context;
    generate_statement_list(context, for_node->body);
    context->current_loop = loop_context.parent;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(context->builder))) {
        LLVMBuildBr(context->builder, update_block);
    }

    LLVMPositionBuilderAtEnd(context->builder, update_block);
    if (for_node->update) {
        generate_statement_list(context, for_node->update);
    }
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(context->builder))) {
        LLVMBuildBr(context->builder, condition_block);
    }

    LLVMPositionBuilderAtEnd(context->builder, end_block);
}

static void generate_statement_list(CodeGenContext *context, ASTNode *statement) {
    for (; statement; statement = statement->next) {
        LLVMBasicBlockRef block = LLVMGetInsertBlock(context->builder);
        if (block && LLVMGetBasicBlockTerminator(block)) break;

        switch (statement->type) {
            case NODE_PRINT:
                generate_print(context, (PrintNode *)statement);
                break;
            case NODE_VAR_DECL: {
                VarDeclNode *declaration = (VarDeclNode *)statement;
                if (declaration->type && declaration->type->is_array) {
                    if (!declaration->expression) {
                        fprintf(stderr, "error: array variables must be initialized\n"); // 中文：数组变量必须初始化
                        exit(1);
                    }
                    LLVMTypeRef array_type = get_llvm_var_type(
                        context, declaration->type);
                    LLVMValueRef storage = create_entry_alloca(
                        context, array_type, declaration->name);
                    insert_array_symbol(
                        context, declaration->name, storage, declaration->type,
                        declaration->is_const);
                    if (declaration->expression->type == NODE_ARRAY_LITERAL) {
                        ArrayLiteralNode *literal =
                            (ArrayLiteralNode *)declaration->expression;
                        // Initialization follows the same recursive shape as the declared type.
                        generate_array_initializer(
                            context, storage, declaration->type, literal);
                    } else {
                        LLVMValueRef value = generate_array_value(
                            context, declaration->expression, declaration->type);
                        LLVMBuildStore(context->builder, value, storage);
                    }
                    break;
                }
                if (declaration->expression &&
                    declaration->expression->type == NODE_ARRAY_LITERAL) {
                    fprintf(stderr, "error: array declarations must explicitly specify [element type; length]\n"); // 中文：数组声明必须显式指定 [元素类型; 长度]
                    exit(1);
                }
                enum LiteralType type = declaration->type
                    ? declaration->type->type
                    : expression_type(context, declaration->expression);
                LLVMTypeRef llvm_type = get_llvm_type(context, type);
                LLVMValueRef storage = create_entry_alloca(
                    context, llvm_type, declaration->name);
                insert_symbol(context, declaration->name, storage, type, declaration->is_const);
                if (declaration->expression) {
                    LLVMValueRef value = generate_expression_as(
                        context, declaration->expression, type);
                    LLVMBuildStore(context->builder, value, storage);
                }
                break;
            }
            case NODE_ASSIGNMENT:
                generate_assignment(context, (AssignmentNode *)statement);
                break;
            case NODE_INDEX_ASSIGNMENT:
                generate_index_assignment(context, (IndexAssignmentNode *)statement);
                break;
            case NODE_RETURN: {
                ReturnNode *return_node = (ReturnNode *)statement;
                LLVMValueRef value = context->current_return_var_type &&
                                     context->current_return_var_type->is_array
                    ? generate_array_value(context, return_node->expression,
                                           context->current_return_var_type)
                    : generate_expression_as(context, return_node->expression,
                                             context->current_return_type);
                LLVMBuildRet(context->builder, value);
                break;
            }
            case NODE_FUNCTION_CALL: {
                FunctionCallNode *call = (FunctionCallNode *)statement;
                if (strcmp(call->name, "assert") == 0) {
                    generate_assert(context, call);
                } else {
                    generate_function_call(context, call);
                }
                break;
            }
            case NODE_IF_STATEMENT:
                generate_if_statement(context, (IfStatementNode *)statement);
                break;
            case NODE_FOR_STATEMENT:
                generate_for_statement(context, (ForStatementNode *)statement);
                break;
            case NODE_BREAK_STATEMENT:
                if (!context->current_loop) {
                    fprintf(stderr, "error: break can only be used inside a for loop\n"); // 中文：break 只能在 for 循环中使用
                    exit(1);
                }
                LLVMBuildBr(context->builder, context->current_loop->break_block);
                break;
            case NODE_CONTINUE_STATEMENT:
                if (!context->current_loop) {
                    fprintf(stderr, "error: continue can only be used inside a for loop\n"); // 中文：continue 只能在 for 循环中使用
                    exit(1);
                }
                LLVMBuildBr(context->builder, context->current_loop->continue_block);
                break;
            default:
                fprintf(stderr, "error: unsupported statement type %d\n", statement->type); // 中文：不支持的语句类型
                exit(1);
        }
    }
}

static unsigned function_param_count(FunctionNode *function) {
    unsigned count = 0;
    for (ASTNode *param = function->params; param; param = param->next) count++;
    return count;
}

static LLVMTypeRef create_function_type(CodeGenContext *context, FunctionNode *function) {
    for (ASTNode *type = function->param_types; type; type = type->next) {
        if (((VarTypeNode *)type)->is_array) {
            fprintf(stderr, "error: arrays are not supported as function parameters yet\n"); // 中文：第一版数组暂不支持作为函数参数
            exit(1);
        }
    }
    unsigned count = function_param_count(function);
    LLVMTypeRef *params = count ? malloc(sizeof(LLVMTypeRef) * count) : NULL;
    for (unsigned i = 0; i < count; i++) {
        params[i] = get_llvm_type(context, function_param_type(function, i));
    }
    LLVMTypeRef return_type = function->return_type && function->return_type->is_array
        ? get_llvm_var_type(context, function->return_type)
        : get_llvm_type(context, function_return_type(function));
    LLVMTypeRef type = LLVMFunctionType(return_type, params, count, 0);
    free(params);
    return type;
}

static LLVMValueRef zero_value(CodeGenContext *context, enum LiteralType type) {
    LLVMTypeRef llvm_type = get_llvm_type(context, type);
    if (is_integer_type(type)) return LLVMConstInt(llvm_type, 0, 0);
    if (type == LITERAL_FLOAT || type == LITERAL_F32 || type == LITERAL_F64) {
        return LLVMConstReal(llvm_type, 0.0);
    }
    return LLVMConstNull(llvm_type);
}

static LLVMValueRef zero_var_value(
    CodeGenContext *context, const VarTypeNode *type, enum LiteralType scalar_type) {
    if (type && type->is_array) return LLVMConstNull(get_llvm_var_type(context, type));
    return zero_value(context, type ? type->type : scalar_type);
}

// Remove internal functions that cannot be reached from externally visible entry points.
static void eliminate_unreachable_functions(CodeGenContext *context) {
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMPassBuilderOptionsSetVerifyEach(options, 1);

    // Remove unreachable definitions first, then discard their unused extern declarations.
    LLVMErrorRef error = LLVMRunPasses(
        context->module, "globaldce,strip-dead-prototypes", NULL, options);
    LLVMDisposePassBuilderOptions(options);
    if (error) {
        char *message = LLVMGetErrorMessage(error);
        fprintf(stderr, "LLVM GlobalDCE failed: %s\n", message); // 中文：LLVM GlobalDCE 执行失败
        LLVMDisposeErrorMessage(message);
        exit(1);
    }
}

CodeGenContext *create_codegen_context(const char *module_name) {
    CodeGenContext *context = calloc(1, sizeof(CodeGenContext));
    if (!context) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    context->context = LLVMContextCreate();
    context->module = LLVMModuleCreateWithNameInContext(module_name, context->context);
    context->builder = LLVMCreateBuilderInContext(context->context);
    context->current_return_type = default_integer_type();

    char *target_triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(context->module, target_triple);
    LLVMDisposeMessage(target_triple);

    return context;
}

static FunctionNode *find_user_main(ProgramNode *program) {
    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type == NODE_FUNCTION) {
            FunctionNode *function = (FunctionNode *)node;
            if (!function->is_extern && strcmp(function->name, "main") == 0) {
                return function;
            }
        }
    }
    return NULL;
}

static void generate_entry_point(CodeGenContext *context, FunctionNode *user_main) {
    if (!user_main) return;

    if (function_param_count(user_main) != 0) {
        fprintf(stderr, "error: main function cannot declare parameters; use std.env.args() instead\n"); // 中文：main 不能声明参数，请使用 std.env.args()
        exit(1);
    }
    if (user_main->return_type && user_main->return_type->is_array) {
        fprintf(stderr, "error: main function cannot return an array\n"); // 中文：main 不能返回数组
        exit(1);
    }

    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
    LLVMTypeRef char_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(context->context), 0);
    LLVMTypeRef argv_type = LLVMPointerType(char_ptr_type, 0);
    LLVMTypeRef init_args_params[2] = {int32_type, argv_type};
    LLVMTypeRef init_args_type = LLVMFunctionType(
        LLVMVoidTypeInContext(context->context), init_args_params, 2, 0);
    LLVMValueRef init_args = LLVMGetNamedFunction(context->module, "__4yue_init_args");
    if (!init_args) {
        init_args = LLVMAddFunction(context->module, "__4yue_init_args", init_args_type);
    }

    LLVMTypeRef main_params[2] = {int32_type, argv_type};
    LLVMTypeRef main_type = LLVMFunctionType(int32_type, main_params, 2, 0);
    LLVMValueRef main_function = LLVMAddFunction(context->module, "main", main_type);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(
        context->context, main_function, "entry");
    LLVMPositionBuilderAtEnd(context->builder, entry);

    LLVMValueRef init_args_values[2] = {
        LLVMGetParam(main_function, 0),
        LLVMGetParam(main_function, 1)
    };
    LLVMBuildCall2(context->builder, init_args_type, init_args,
                   init_args_values, 2, "");

    LLVMValueRef user_main_function = LLVMGetNamedFunction(
        context->module, llvm_function_name(user_main));
    LLVMTypeRef user_main_type = LLVMGlobalGetValueType(user_main_function);
    LLVMValueRef result = LLVMBuildCall2(
        context->builder, user_main_type, user_main_function, NULL, 0,
        "user_main_result");

    enum LiteralType return_type = function_return_type(user_main);
    if (is_integer_type(return_type)) {
        result = cast_integer(context, result, return_type, LITERAL_I32);
        LLVMBuildRet(context->builder, result);
    } else {
        LLVMBuildRet(context->builder, LLVMConstInt(int32_type, 0, 0));
    }
}

void generate_code(CodeGenContext *context, ProgramNode *program) {
    context->program = program;
    FunctionNode *user_main = find_user_main(program);

    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
    LLVMTypeRef char_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(context->context), 0);
    context->printf_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 1);
    context->printf_func = LLVMAddFunction(context->module, "printf", context->printf_type);
    context->exit_type = LLVMFunctionType(LLVMVoidTypeInContext(context->context), &int32_type, 1, 0);
    context->exit_func = LLVMAddFunction(context->module, "exit", context->exit_type);

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        const char *name = llvm_function_name(function);
        LLVMValueRef llvm_function = LLVMAddFunction(
            context->module, name, create_function_type(context, function));
        // Executables expose only main; GlobalDCE may remove every unreachable helper.
        if (!function->is_extern) {
            LLVMSetLinkage(llvm_function, LLVMInternalLinkage);
        }
    }

    generate_entry_point(context, user_main);

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        // Runtime functions already have native implementations and need no LLVM body.
        if (function->is_extern) continue;
        LLVMValueRef llvm_function = LLVMGetNamedFunction(
            context->module, llvm_function_name(function));

        free_symbols(context->symbols);
        context->symbols = NULL;
        context->current_return_type = function_return_type(function);
        context->current_return_var_type = function_return_var_type(function);

        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context->context, llvm_function, "entry");
        LLVMPositionBuilderAtEnd(context->builder, entry);

        ASTNode *param = function->params;
        unsigned param_index = 0;
        while (param) {
            IdentifierNode *identifier = (IdentifierNode *)param;
            enum LiteralType type = function_param_type(function, param_index);
            const VarTypeNode *var_type = function_param_var_type(function, param_index);
            LLVMValueRef storage = create_entry_alloca(
                context,
                var_type && var_type->is_array
                    ? get_llvm_var_type(context, var_type)
                    : get_llvm_type(context, type),
                identifier->name);
            LLVMValueRef value = LLVMGetParam(llvm_function, param_index);
            LLVMSetValueName2(value, identifier->name, strlen(identifier->name));
            LLVMBuildStore(context->builder, value, storage);
            if (var_type && var_type->is_array) {
                insert_array_symbol(context, identifier->name, storage, var_type, 0);
            } else {
                insert_symbol(context, identifier->name, storage, type, 0);
            }
            param = param->next;
            param_index++;
        }

        generate_statement_list(context, function->body);
        LLVMBasicBlockRef current = LLVMGetInsertBlock(context->builder);
        if (current && !LLVMGetBasicBlockTerminator(current)) {
            LLVMBuildRet(context->builder,
                         zero_var_value(context, context->current_return_var_type,
                                        context->current_return_type));
        }
    }

    char *error = NULL;
    if (LLVMVerifyModule(context->module, LLVMReturnStatusAction, &error) != 0) {
        fprintf(stderr, "LLVM IR verification failed: %s\n", error); // 中文：LLVM IR 验证失败
        LLVMDisposeMessage(error);
        exit(1);
    }

    // Run DCE only after full generation and validation so unreachable code still reports errors.
    eliminate_unreachable_functions(context);
}

static int initialize_execution_engine(CodeGenContext *context) {
    char *error = NULL;
    if (LLVMCreateExecutionEngineForModule(&context->engine, context->module, &error) != 0) {
        fprintf(stderr, "failed to create execution engine: %s\n", error); // 中文：创建执行引擎失败
        LLVMDisposeMessage(error);
        return -1;
    }
    return 0;
}

void free_codegen_context(CodeGenContext *context) {
    if (!context) return;
    free_symbols(context->symbols);
    if (context->builder) LLVMDisposeBuilder(context->builder);
    if (context->engine) {
        LLVMDisposeExecutionEngine(context->engine);
    } else if (context->module) {
        LLVMDisposeModule(context->module);
    }
    if (context->context) LLVMContextDispose(context->context);
    free(context);
}

int execute_code(CodeGenContext *context, const char *function_name) {
    if (initialize_execution_engine(context) != 0) return -1;
    LLVMValueRef function = LLVMGetNamedFunction(context->module, function_name);
    if (!function) {
        fprintf(stderr, "function not found: %s\n", function_name); // 中文：未找到函数
        return -1;
    }
    LLVMGenericValueRef result_ref = LLVMRunFunction(context->engine, function, 0, NULL);
    int result = result_ref ? (int)LLVMGenericValueToInt(result_ref, 0) : 0;
    if (result_ref) LLVMDisposeGenericValue(result_ref);
    return result;
}

int write_ir_to_file(CodeGenContext *context, const char *filename) {
    char *error = NULL;
    if (LLVMPrintModuleToFile(context->module, filename, &error) != 0) {
        fprintf(stderr, "failed to write IR file: %s\n", error); // 中文：写入 IR 文件失败
        LLVMDisposeMessage(error);
        return -1;
    }
    return 0;
}

static int write_object_for_triple(
    CodeGenContext *context, const char *filename,
    const char *target_triple_override, const char *output_kind) {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers();

    char *error = NULL;
    LLVMTargetRef target = NULL;
    char *target_triple = target_triple_override
        ? strdup(target_triple_override)
        : LLVMGetDefaultTargetTriple();
    if (!target_triple) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return -1;
    }
    if (LLVMGetTargetFromTriple(target_triple, &target, &error) != 0) {
        fprintf(stderr, "failed to get target machine: %s\n", error); // 中文：获取目标机器失败
        if (error) LLVMDisposeMessage(error);
        if (target_triple_override) {
            free(target_triple);
        } else {
            LLVMDisposeMessage(target_triple);
        }
        return -1;
    }

    LLVMTargetMachineRef target_machine = LLVMCreateTargetMachine(
        target, target_triple, "", "", LLVMCodeGenLevelDefault,
        LLVMRelocPIC, LLVMCodeModelDefault);
    if (!target_machine) {
        fprintf(stderr, "failed to create target machine\n"); // 中文：创建目标机器失败
        if (target_triple_override) {
            free(target_triple);
        } else {
            LLVMDisposeMessage(target_triple);
        }
        return -1;
    }

    LLVMSetTarget(context->module, target_triple);
    LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(target_machine);
    char *data_layout = LLVMCopyStringRepOfTargetData(target_data);
    LLVMSetDataLayout(context->module, data_layout);

    char *output_path = strdup(filename);
    if (!output_path) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        LLVMDisposeMessage(data_layout);
        LLVMDisposeTargetData(target_data);
        LLVMDisposeTargetMachine(target_machine);
        if (target_triple_override) {
            free(target_triple);
        } else {
            LLVMDisposeMessage(target_triple);
        }
        return -1;
    }

    int result = LLVMTargetMachineEmitToFile(
        target_machine, context->module, output_path, LLVMObjectFile, &error);
    if (result != 0) {
        fprintf(stderr, "failed to write %s file: %s\n", output_kind, error); // 中文：写入输出文件失败
        if (error) LLVMDisposeMessage(error);
        result = -1;
    } else {
        result = 0;
    }

    free(output_path);
    LLVMDisposeMessage(data_layout);
    LLVMDisposeTargetData(target_data);
    LLVMDisposeTargetMachine(target_machine);
    if (target_triple_override) {
        free(target_triple);
    } else {
        LLVMDisposeMessage(target_triple);
    }
    return result;
}

int write_object_to_file(CodeGenContext *context, const char *filename) {
    return write_object_for_triple(context, filename, NULL, "object");
}

int write_wasm_to_file(CodeGenContext *context, const char *filename) {
    return write_object_for_triple(
        context, filename, "wasm32-unknown-unknown", "WebAssembly");
}
