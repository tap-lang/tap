#include "codegen.h"

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
            fprintf(stderr, "不支持的类型: %d\n", type);
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
                          enum LiteralType type) {
    Symbol *symbol = malloc(sizeof(Symbol));
    if (!symbol) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    symbol->name = strdup(name);
    symbol->value = value;
    symbol->type = type;
    symbol->array_type = NULL;
    symbol->next = context->symbols;
    context->symbols = symbol;
}

static void insert_array_symbol(CodeGenContext *context, const char *name,
                                LLVMValueRef value, const VarTypeNode *array_type) {
    insert_symbol(context, name, value, array_type->type);
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

static enum LiteralType function_return_type(FunctionNode *function) {
    return function && function->return_type ? function->return_type->type : default_integer_type();
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

// Resolve the recursive type produced by an identifier or a chain of indexes.
static const VarTypeNode *indexed_value_type(CodeGenContext *context, ASTNode *expression) {
    if (expression && expression->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)expression)->name;
        Symbol *symbol = find_symbol(context, name);
        if (!symbol) {
            fprintf(stderr, "错误：未定义的变量 '%s'\n", name);
            exit(1);
        }
        if (!symbol->array_type) {
            fprintf(stderr, "错误：变量 '%s' 不是数组\n", name);
            exit(1);
        }
        return symbol->array_type;
    }

    if (expression && expression->type == NODE_INDEX_EXPRESSION) {
        IndexExpressionNode *index = (IndexExpressionNode *)expression;
        const VarTypeNode *container_type = indexed_value_type(context, index->array);
        if (!container_type->is_array) {
            fprintf(stderr, "错误：索引目标不是数组\n");
            exit(1);
        }
        return container_type->element_type;
    }

    fprintf(stderr, "错误：数组索引目标无效\n");
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
                fprintf(stderr, "错误：多维数组必须索引到标量元素\n");
                exit(1);
            }
            return type->type;
        }
        case NODE_FUNCTION_CALL: {
            FunctionNode *function = find_function(context, ((FunctionCallNode *)expression)->name);
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
static LLVMValueRef generate_integer_binary(CodeGenContext *context, BinaryOpNode *binary,
                                            enum LiteralType operand_type);

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
            fprintf(stderr, "错误：未定义的变量 '%s'\n", name);
            exit(1);
        }
        if (!symbol->array_type) {
            fprintf(stderr, "错误：变量 '%s' 不是数组\n", name);
            exit(1);
        }
        array_address = symbol->value;
        array_type = symbol->array_type;
    } else if (index_expression->array &&
               index_expression->array->type == NODE_INDEX_EXPRESSION) {
        array_address = generate_index_address(
            context, (IndexExpressionNode *)index_expression->array, &array_type);
        if (!array_type->is_array) {
            fprintf(stderr, "错误：索引目标不是数组\n");
            exit(1);
        }
    } else {
        fprintf(stderr, "错误：数组索引目标无效\n");
        exit(1);
    }

    // Every dimension performs its own signed/unsigned bounds check.
    enum LiteralType index_type = expression_type(context, index_expression->index);
    if (!is_integer_type(index_type) || integer_type_bits(index_type) > 64) {
        fprintf(stderr, "错误：数组下标必须是最多 64 位的整数\n");
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
            "Array index out of bounds: index=%lld, length=%llu\n", "array_bounds_format"),
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
            fprintf(stderr, "错误：不支持的二元操作符\n");
            exit(1);
    }
}

static LLVMValueRef generate_function_call(CodeGenContext *context, FunctionCallNode *call) {
    if (strcmp(call->name, "assert") == 0) {
        fprintf(stderr, "error: assert can only be used as a statement\n");
        exit(1);
    }

    LLVMValueRef llvm_function = LLVMGetNamedFunction(context->module, call->name);
    FunctionNode *function = find_function(context, call->name);
    if (!llvm_function || !function) {
        fprintf(stderr, "错误：未定义的函数 '%s'\n", call->name);
        exit(1);
    }

    unsigned count = 0;
    for (ASTNode *argument = call->arguments; argument; argument = argument->next) count++;

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
                fprintf(stderr, "错误：未定义的变量 '%s'\n", identifier->name);
                exit(1);
            }
            if (symbol->array_type) {
                fprintf(stderr, "错误：数组 '%s' 必须通过下标访问\n", identifier->name);
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
                fprintf(stderr, "错误：多维数组必须索引到标量元素\n");
                exit(1);
            }
            return LLVMBuildLoad2(context->builder, get_llvm_var_type(context, element_type),
                                  address, "array_element");
        }
        case NODE_ARRAY_LITERAL:
            fprintf(stderr, "错误：数组字面量只能用于数组变量初始化\n");
            exit(1);
        case NODE_FUNCTION_CALL:
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

    fprintf(stderr, "错误：不支持的表达式类型\n");
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
        fprintf(stderr, "错误：未定义的变量 '%s'\n", assignment->name);
        exit(1);
    }
    if (symbol->array_type) {
        fprintf(stderr, "错误：第一版数组暂不支持整个数组赋值\n");
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
    if (literal->count != array_type->array_length) {
        fprintf(stderr,
            "错误：数组初始化元素数量为 %llu，但声明长度为 %llu\n",
            (unsigned long long)literal->count,
            (unsigned long long)array_type->array_length);
        exit(1);
    }

    LLVMTypeRef llvm_array_type = get_llvm_var_type(context, array_type);
    const VarTypeNode *element_type = array_type->element_type;
    ASTNode *element = literal->elements;
    for (uint64_t index = 0; index < literal->count; index++, element = element->next) {
        LLVMValueRef indexes[2] = {
            LLVMConstInt(LLVMInt64TypeInContext(context->context), 0, 0),
            LLVMConstInt(LLVMInt64TypeInContext(context->context), index, 0)
        };
        LLVMValueRef element_address = LLVMBuildGEP2(
            context->builder, llvm_array_type, storage, indexes, 2,
            "array_init_element_ptr");

        if (element_type->is_array) {
            if (!element || element->type != NODE_ARRAY_LITERAL) {
                fprintf(stderr, "错误：多维数组初始化需要嵌套数组字面量\n");
                exit(1);
            }
            generate_array_initializer(
                context, element_address, element_type, (ArrayLiteralNode *)element);
            continue;
        }

        if (!element || element->type == NODE_ARRAY_LITERAL) {
            fprintf(stderr, "错误：数组元素类型不匹配\n");
            exit(1);
        }
        enum LiteralType actual_type = expression_type(context, element);
        if (!array_element_type_compatible(element_type->type, actual_type)) {
            fprintf(stderr, "错误：数组元素类型不匹配\n");
            exit(1);
        }
        LLVMValueRef value = generate_expression_as(
            context, element, element_type->type);
        LLVMBuildStore(context->builder, value, element_address);
    }
}

static void generate_index_assignment(
    CodeGenContext *context, IndexAssignmentNode *assignment) {
    const VarTypeNode *element_type = NULL;
    LLVMValueRef address = generate_index_address(
        context, assignment->target, &element_type);
    if (element_type->is_array) {
        fprintf(stderr, "错误：暂不支持整个子数组赋值\n");
        exit(1);
    }
    enum LiteralType actual_type = expression_type(context, assignment->expression);
    if (!array_element_type_compatible(element_type->type, actual_type)) {
        fprintf(stderr, "错误：数组元素赋值类型不匹配\n");
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
    fprintf(stderr, "错误：if 条件必须是整数或布尔表达式\n");
    exit(1);
}

static void generate_assert(CodeGenContext *context, FunctionCallNode *call) {
    unsigned count = 0;
    for (ASTNode *argument = call->arguments; argument; argument = argument->next) count++;

    if (count < 1 || count > 2) {
        fprintf(stderr, "%s:%d:%d: error: assert expects one or two arguments\n",
                call->filename ? call->filename : "<unknown>", call->line, call->column);
        exit(1);
    }

    const char *message = "condition is false";
    if (count == 2) {
        ASTNode *message_node = call->arguments->next;
        if (message_node->type != NODE_LITERAL ||
            ((LiteralNode *)message_node)->literal_type != LITERAL_STRING) {
            fprintf(stderr, "%s:%d:%d: error: assert message must be a string literal\n",
                    call->filename ? call->filename : "<unknown>", call->line, call->column);
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
            "Assertion failed at %s:%d:%d: %s\n", "assert_format"),
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
                    if (!declaration->expression ||
                        declaration->expression->type != NODE_ARRAY_LITERAL) {
                        fprintf(stderr, "错误：数组变量必须使用数组字面量初始化\n");
                        exit(1);
                    }
                    ArrayLiteralNode *literal =
                        (ArrayLiteralNode *)declaration->expression;
                    LLVMTypeRef array_type = get_llvm_var_type(
                        context, declaration->type);
                    LLVMValueRef storage = create_entry_alloca(
                        context, array_type, declaration->name);
                    insert_array_symbol(
                        context, declaration->name, storage, declaration->type);
                    // Initialization follows the same recursive shape as the declared type.
                    generate_array_initializer(
                        context, storage, declaration->type, literal);
                    break;
                }
                if (declaration->expression &&
                    declaration->expression->type == NODE_ARRAY_LITERAL) {
                    fprintf(stderr, "错误：数组声明必须显式指定 array[元素类型, 长度]\n");
                    exit(1);
                }
                enum LiteralType type = declaration->type
                    ? declaration->type->type
                    : expression_type(context, declaration->expression);
                LLVMTypeRef llvm_type = get_llvm_type(context, type);
                LLVMValueRef storage = create_entry_alloca(
                    context, llvm_type, declaration->name);
                insert_symbol(context, declaration->name, storage, type);
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
                LLVMValueRef value = generate_expression_as(context, return_node->expression,
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
                    fprintf(stderr, "错误：break 只能在 for 循环中使用\n");
                    exit(1);
                }
                LLVMBuildBr(context->builder, context->current_loop->break_block);
                break;
            case NODE_CONTINUE_STATEMENT:
                if (!context->current_loop) {
                    fprintf(stderr, "错误：continue 只能在 for 循环中使用\n");
                    exit(1);
                }
                LLVMBuildBr(context->builder, context->current_loop->continue_block);
                break;
            default:
                fprintf(stderr, "错误：不支持的语句类型 %d\n", statement->type);
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
    if (function->return_type && function->return_type->is_array) {
        fprintf(stderr, "错误：第一版数组暂不支持作为函数返回类型\n");
        exit(1);
    }
    for (ASTNode *type = function->param_types; type; type = type->next) {
        if (((VarTypeNode *)type)->is_array) {
            fprintf(stderr, "错误：第一版数组暂不支持作为函数参数\n");
            exit(1);
        }
    }
    unsigned count = function_param_count(function);
    LLVMTypeRef *params = count ? malloc(sizeof(LLVMTypeRef) * count) : NULL;
    for (unsigned i = 0; i < count; i++) {
        params[i] = get_llvm_type(context, function_param_type(function, i));
    }
    LLVMTypeRef type = LLVMFunctionType(get_llvm_type(context, function_return_type(function)),
                                        params, count, 0);
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
        fprintf(stderr, "LLVM GlobalDCE 执行失败: %s\n", message);
        LLVMDisposeErrorMessage(message);
        exit(1);
    }
}

CodeGenContext *create_codegen_context(const char *module_name) {
    CodeGenContext *context = calloc(1, sizeof(CodeGenContext));
    if (!context) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    context->context = LLVMContextCreate();
    context->module = LLVMModuleCreateWithNameInContext(module_name, context->context);
    context->builder = LLVMCreateBuilderInContext(context->context);
    context->current_return_type = default_integer_type();

#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
    LLVMSetTarget(context->module, "x86_64-pc-windows-cygnus");
#elif defined(__APPLE__) && defined(__MACH__)
    LLVMSetTarget(context->module, "arm64-apple-macosx15.0.0");
#endif

    return context;
}

void generate_code(CodeGenContext *context, ProgramNode *program) {
    context->program = program;

    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
    LLVMTypeRef char_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(context->context), 0);
    context->printf_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 1);
    context->printf_func = LLVMAddFunction(context->module, "printf", context->printf_type);
    context->exit_type = LLVMFunctionType(LLVMVoidTypeInContext(context->context), &int32_type, 1, 0);
    context->exit_func = LLVMAddFunction(context->module, "exit", context->exit_type);

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        LLVMValueRef llvm_function = LLVMAddFunction(
            context->module, function->name, create_function_type(context, function));
        // Executables expose only main; GlobalDCE may remove every unreachable helper.
        if (!function->is_extern && strcmp(function->name, "main") != 0) {
            LLVMSetLinkage(llvm_function, LLVMInternalLinkage);
        }
    }

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        // Runtime functions already have native implementations and need no LLVM body.
        if (function->is_extern) continue;
        LLVMValueRef llvm_function = LLVMGetNamedFunction(context->module, function->name);

        free_symbols(context->symbols);
        context->symbols = NULL;
        context->current_return_type = function_return_type(function);

        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context->context, llvm_function, "entry");
        LLVMPositionBuilderAtEnd(context->builder, entry);

        ASTNode *param = function->params;
        unsigned param_index = 0;
        while (param) {
            IdentifierNode *identifier = (IdentifierNode *)param;
            enum LiteralType type = function_param_type(function, param_index);
            LLVMValueRef storage = create_entry_alloca(
                context, get_llvm_type(context, type), identifier->name);
            LLVMValueRef value = LLVMGetParam(llvm_function, param_index);
            LLVMSetValueName2(value, identifier->name, strlen(identifier->name));
            LLVMBuildStore(context->builder, value, storage);
            insert_symbol(context, identifier->name, storage, type);
            param = param->next;
            param_index++;
        }

        generate_statement_list(context, function->body);
        LLVMBasicBlockRef current = LLVMGetInsertBlock(context->builder);
        if (current && !LLVMGetBasicBlockTerminator(current)) {
            LLVMBuildRet(context->builder, zero_value(context, context->current_return_type));
        }
    }

    char *error = NULL;
    if (LLVMVerifyModule(context->module, LLVMReturnStatusAction, &error) != 0) {
        fprintf(stderr, "LLVM IR 验证失败: %s\n", error);
        LLVMDisposeMessage(error);
        exit(1);
    }

    // Run DCE only after full generation and validation so unreachable code still reports errors.
    eliminate_unreachable_functions(context);
}

static int initialize_execution_engine(CodeGenContext *context) {
    char *error = NULL;
    if (LLVMCreateExecutionEngineForModule(&context->engine, context->module, &error) != 0) {
        fprintf(stderr, "创建执行引擎失败: %s\n", error);
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
        fprintf(stderr, "未找到函数: %s\n", function_name);
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
        fprintf(stderr, "写入IR文件失败: %s\n", error);
        LLVMDisposeMessage(error);
        return -1;
    }
    return 0;
}

int write_object_to_file(CodeGenContext *context, const char *filename) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    char *error = NULL;
    LLVMTargetRef target = NULL;
    char *target_triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(target_triple, &target, &error) != 0) {
        fprintf(stderr, "获取目标机器失败: %s\n", error);
        LLVMDisposeMessage(error);
        LLVMDisposeMessage(target_triple);
        return -1;
    }

    LLVMTargetMachineRef target_machine = LLVMCreateTargetMachine(
        target, target_triple, "", "", LLVMCodeGenLevelDefault,
        LLVMRelocPIC, LLVMCodeModelDefault);
    if (!target_machine) {
        fprintf(stderr, "创建目标机器失败\n");
        LLVMDisposeMessage(target_triple);
        return -1;
    }

    LLVMSetTarget(context->module, target_triple);
    LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(target_machine);
    char *data_layout = LLVMCopyStringRepOfTargetData(target_data);
    LLVMSetDataLayout(context->module, data_layout);

    char *output_path = strdup(filename);
    if (!output_path) {
        fprintf(stderr, "内存分配失败\n");
        LLVMDisposeMessage(data_layout);
        LLVMDisposeTargetData(target_data);
        LLVMDisposeTargetMachine(target_machine);
        LLVMDisposeMessage(target_triple);
        return -1;
    }

    int result = LLVMTargetMachineEmitToFile(
        target_machine, context->module, output_path, LLVMObjectFile, &error);
    if (result != 0) {
        fprintf(stderr, "写入目标文件失败: %s\n", error);
        LLVMDisposeMessage(error);
        result = -1;
    } else {
        result = 0;
    }

    free(output_path);
    LLVMDisposeMessage(data_layout);
    LLVMDisposeTargetData(target_data);
    LLVMDisposeTargetMachine(target_machine);
    LLVMDisposeMessage(target_triple);
    return result;
}
