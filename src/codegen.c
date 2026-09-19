#include "codegen.h"
#include "helpers.h"

#include <limits.h>

// 返回未显式标注整数时使用的默认整数类型。
static enum LiteralType default_integer_type(void) {
    return LITERAL_I32;
}

// 判断字面量类型是否按整数类型处理。
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

// 判断整数类型是否按无符号规则处理。
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

// 判断是否为浮点类型，用于浮点常量和浮点类型转换。
static int is_float_type(enum LiteralType type) {
    return type == LITERAL_FLOAT || type == LITERAL_F32 || type == LITERAL_F64;
}

// 判断标量类型之间是否允许隐式转换：整数之间、浮点之间互相兼容，另外允许 int -> float。
// bool 虽然按整数处理，但不参与隐式数值转换，避免 `let f: f64 = true;` 这类写法通过。
static int scalar_types_compatible(enum LiteralType target, enum LiteralType source) {
    if (target == source) return 1;
    if (is_integer_type(target) && is_integer_type(source)) return 1;
    if (is_float_type(target) && is_float_type(source)) return 1;
    if (is_float_type(target) && is_integer_type(source) && source != LITERAL_BOOL) return 1;
    return 0;
}

// 计算两个浮点操作数共同提升后的类型；任一侧为 f64 时整体提升到 f64。
static enum LiteralType common_float_type(enum LiteralType left, enum LiteralType right) {
    if (left == LITERAL_F64 || right == LITERAL_F64) return LITERAL_F64;
    return LITERAL_F32;
}

static LLVMTypeRef get_llvm_var_type(CodeGenContext *context, const VarTypeNode *type);
static LLVMTypeRef get_llvm_struct_type_by_name(CodeGenContext *context, const char *name);

// 返回整数类型对应的位宽。
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

// 根据位宽和符号信息选择最小可承载的整数类型。
static enum LiteralType integer_type_for(unsigned bits, int is_unsigned) {
    if (bits <= 8) return is_unsigned ? LITERAL_U8 : LITERAL_I8;
    if (bits <= 16) return is_unsigned ? LITERAL_U16 : LITERAL_I16;
    if (bits <= 32) return is_unsigned ? LITERAL_U32 : LITERAL_I32;
    if (bits <= 64) return is_unsigned ? LITERAL_U64 : LITERAL_I64;
    return is_unsigned ? LITERAL_U128 : LITERAL_I128;
}

// 计算两个整数操作数共同提升后的整数类型。
static enum LiteralType common_integer_type(enum LiteralType left, enum LiteralType right) {
    unsigned left_bits = integer_type_bits(left);
    unsigned right_bits = integer_type_bits(right);
    unsigned bits = left_bits > right_bits ? left_bits : right_bits;
    int use_unsigned = is_unsigned_type(left) || is_unsigned_type(right);
    return integer_type_for(bits, use_unsigned);
}

// 将源语言标量类型降低为 LLVM 类型。
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

// 递归地将标量、结构体和多维数组类型降低为 LLVM 类型。
static LLVMTypeRef get_llvm_var_type(CodeGenContext *context, const VarTypeNode *type) {
    if (type->is_pointer) {
        return LLVMPointerType(get_llvm_var_type(context, type->element_type), 0);
    }
    if (type->is_array) {
        return LLVMArrayType2(
            get_llvm_var_type(context, type->element_type), type->array_length);
    }
    if (type->struct_name) {
        return get_llvm_struct_type_by_name(context, type->struct_name);
    }
    return get_llvm_type(context, type->type);
}

// 在当前局部符号表中按名称查找变量符号。
static Symbol *find_symbol(CodeGenContext *context, const char *name) {
    for (Symbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) return symbol;
    }
    return NULL;
}

// 插入普通标量符号到当前局部符号表。
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
    symbol->declared_type = NULL;
    symbol->array_type = NULL;
    symbol->is_const = is_const;
    symbol->next = context->symbols;
    context->symbols = symbol;
}

// 按完整声明类型登记符号，保留枚举和数组等高层类型信息。
static void insert_typed_symbol(CodeGenContext *context, const char *name,
                                LLVMValueRef value, const VarTypeNode *type,
                                int is_const) {
    insert_symbol(context, name, value, type->type, is_const);
    // The declaration AST outlives Codegen, so no type copy is required here.
    context->symbols->declared_type = type;
    if (type->is_array) context->symbols->array_type = type;
}

// 插入数组符号，并记录完整数组类型信息。
static void insert_array_symbol(CodeGenContext *context, const char *name,
                                LLVMValueRef value, const VarTypeNode *array_type,
                                int is_const) {
    insert_typed_symbol(context, name, value, array_type, is_const);
}

// 释放当前函数生成过程中使用的局部符号表。
static void free_symbols(Symbol *symbols) {
    while (symbols) {
        Symbol *next = symbols->next;
        free(symbols->name);
        free(symbols);
        symbols = next;
    }
}

// 在函数入口块分配局部槽位，保证分支和循环内都能支配使用点。
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

// 在当前程序中按源语言函数名查找函数声明。
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

// 在当前程序中按名称查找枚举声明。
static EnumNode *find_enum(CodeGenContext *context, const char *name) {
    if (!context->program) return NULL;
    for (ASTNode *node = context->program->enums; node; node = node->next) {
        if (node->type == NODE_ENUM) {
            EnumNode *enum_node = (EnumNode *)node;
            if (strcmp(enum_node->name, name) == 0) return enum_node;
        }
    }
    return NULL;
}

// 在当前程序中按名称查找结构体声明。
static StructNode *find_struct(CodeGenContext *context, const char *name) {
    if (!context->program) return NULL;
    for (ASTNode *node = context->program->structs; node; node = node->next) {
        if (node->type == NODE_STRUCT) {
            StructNode *struct_node = (StructNode *)node;
            if (strcmp(struct_node->name, name) == 0) return struct_node;
        }
    }
    return NULL;
}

// 由结构体名反查它降级自哪个载荷枚举。泛型实例化后名字形如 Option$i32，靠
// tagged_enum_name 才能映射回 Option 枚举声明。普通结构体返回 NULL。
static EnumNode *struct_tagged_enum(CodeGenContext *context, const char *struct_name) {
    if (!struct_name) return NULL;
    StructNode *structure = find_struct(context, struct_name);
    if (!structure || !structure->is_tagged_enum || !structure->tagged_enum_name) {
        return NULL;
    }
    return find_enum(context, structure->tagged_enum_name);
}

// 在结构体声明中查找字段，并返回字段下标。
static StructFieldNode *find_struct_field(
    StructNode *struct_node, const char *name, unsigned *index_out) {
    unsigned index = 0;
    for (ASTNode *field = struct_node ? struct_node->fields : NULL;
         field; field = field->next, index++) {
        StructFieldNode *struct_field = (StructFieldNode *)field;
        if (strcmp(struct_field->name, name) == 0) {
            if (index_out) *index_out = index;
            return struct_field;
        }
    }
    return NULL;
}

// 按字段下标取出结构体字段声明。
static StructFieldNode *struct_field_at(StructNode *struct_node, unsigned index) {
    unsigned current = 0;
    for (ASTNode *field = struct_node ? struct_node->fields : NULL;
         field; field = field->next, current++) {
        if (current == index) return (StructFieldNode *)field;
    }
    return NULL;
}

// 获取结构体对应的 LLVM 具名类型。
static LLVMTypeRef get_llvm_struct_type_by_name(CodeGenContext *context, const char *name) {
    LLVMTypeRef type = LLVMGetTypeByName2(context->context, name);
    if (type) return type;

    if (!find_struct(context, name)) {
        fprintf(stderr, "error: undefined struct type '%s'\n",
                name); // 中文：未定义的结构体类型
        exit(1);
    }
    return LLVMStructCreateNamed(context->context, name);
}

// 解析 Enum.Member 形式的名称，返回枚举声明，并通过 variant_out 返回成员声明。
static EnumNode *variant_reference(
    CodeGenContext *context, const char *name, EnumVariantNode **variant_out) {
    const char *dot = strchr(name, '.');
    if (!dot || dot == name || strchr(dot + 1, '.')) return NULL;

    size_t enum_name_length = (size_t)(dot - name);
    char *enum_name = malloc(enum_name_length + 1);
    if (!enum_name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    memcpy(enum_name, name, enum_name_length);
    enum_name[enum_name_length] = '\0';

    EnumNode *enum_node = find_enum(context, enum_name);
    free(enum_name);
    if (!enum_node) return NULL;

    const char *variant_name = dot + 1;
    for (ASTNode *node = enum_node->variants; node; node = node->next) {
        EnumVariantNode *variant = (EnumVariantNode *)node;
        if (strcmp(variant->name, variant_name) == 0) {
            if (variant_out) *variant_out = variant;
            return enum_node;
        }
    }
    return NULL;
}

// 统计一个枚举成员声明的载荷数量。
static unsigned variant_payload_count(const EnumVariantNode *variant) {
    unsigned count = 0;
    for (ASTNode *type = variant->payload_types; type; type = type->next) count++;
    return count;
}

// 判断标识符是否是某个载荷枚举的无载荷成员，例如 Shape.Empty。
// 这类成员不需要括号，直接写成 Enum.Member。
static EnumNode *bare_variant_reference(
    CodeGenContext *context, const char *name, EnumVariantNode **variant_out) {
    EnumVariantNode *variant = NULL;
    EnumNode *enum_node = variant_reference(context, name, &variant);
    if (!enum_node || !enum_node->has_payload) return NULL;
    if (variant_payload_count(variant) != 0) return NULL;
    if (variant_out) *variant_out = variant;
    return enum_node;
}

// 解析 Enum.Member 形式的枚举成员，并返回成员的判别序号。
// 只对无载荷枚举成立：带载荷的成员必须写成 Enum.Member(...) 构造。
static int enum_variant_value(CodeGenContext *context, const char *name, uint64_t *value_out) {
    EnumVariantNode *variant = NULL;
    EnumNode *enum_node = variant_reference(context, name, &variant);
    if (!enum_node || enum_node->has_payload) return 0;
    if (value_out) *value_out = variant->tag;
    return 1;
}

// 在程序的顶层常量列表中查找已经解析出的全局常量。
static VarDeclNode *find_global_constant(CodeGenContext *context, const char *name) {
    if (!context->program) return NULL;
    for (ASTNode *node = context->program->constants; node; node = node->next) {
        if (node->type == NODE_VAR_DECL) {
            VarDeclNode *constant = (VarDeclNode *)node;
            if (strcmp(constant->name, name) == 0) return constant;
        }
    }
    return NULL;
}

typedef enum {
    STRING_METHOD_NONE,
    STRING_METHOD_LEN,
    STRING_METHOD_BYTE_AT,
    STRING_METHOD_SLICE
} StringMethodKind;

static const char *string_method_name(StringMethodKind method) {
    switch (method) {
        case STRING_METHOD_LEN: return "len";
        case STRING_METHOD_BYTE_AT: return "byte_at";
        case STRING_METHOD_SLICE: return "slice";
        default: return "";
    }
}

// 非标识符接收者由 Parser 降低为保留的内部调用名称。
static StringMethodKind internal_string_method(const FunctionCallNode *call) {
    if (strcmp(call->name, "__tap_builtin_string_len") == 0) {
        return STRING_METHOD_LEN;
    }
    if (strcmp(call->name, "__tap_builtin_string_byte_at") == 0) {
        return STRING_METHOD_BYTE_AT;
    }
    if (strcmp(call->name, "__tap_builtin_string_slice") == 0) {
        return STRING_METHOD_SLICE;
    }
    return STRING_METHOD_NONE;
}

// 标识符接收者沿用限定调用表示，同时返回点号前的变量名范围。
static StringMethodKind named_string_method(
    const FunctionCallNode *call, const char **receiver_name, size_t *receiver_length) {
    const char *dot = strchr(call->name, '.');
    if (!dot || dot == call->name || strchr(dot + 1, '.')) return STRING_METHOD_NONE;

    StringMethodKind method = STRING_METHOD_NONE;
    if (strcmp(dot + 1, "len") == 0) method = STRING_METHOD_LEN;
    else if (strcmp(dot + 1, "byte_at") == 0) method = STRING_METHOD_BYTE_AT;
    else if (strcmp(dot + 1, "slice") == 0) method = STRING_METHOD_SLICE;
    if (method == STRING_METHOD_NONE) return method;

    *receiver_name = call->name;
    *receiver_length = (size_t)(dot - call->name);
    return method;
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

// 返回函数的标量返回类型，未声明时使用默认整数类型。
static enum LiteralType function_return_type(FunctionNode *function) {
    return function && function->return_type ? function->return_type->type : default_integer_type();
}

// 返回函数的完整返回类型节点，用于数组、枚举和结构体。
static const VarTypeNode *function_return_var_type(FunctionNode *function) {
    return function ? function->return_type : NULL;
}

// 返回指定下标参数的标量类型，未声明时使用默认整数类型。
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

// 返回指定下标参数的完整类型节点。
static const VarTypeNode *function_param_var_type(FunctionNode *function, unsigned index) {
    ASTNode *type = function ? function->param_types : NULL;
    while (type && index > 0) {
        type = type->next;
        index--;
    }
    return type && type->type == NODE_VAR_TYPE ? (VarTypeNode *)type : NULL;
}

// 返回 LLVM 中使用的函数名；用户 main 会重命名为内部入口。
static const char *llvm_function_name(const FunctionNode *function) {
    return function && !function->is_extern && strcmp(function->name, "main") == 0
        ? "__tap_user_main"
        : function->name;
}

// 将源语言调用名转换为 LLVM 调用名。
static const char *llvm_call_name(const char *name) {
    return strcmp(name, "main") == 0 ? "__tap_user_main" : name;
}

static enum LiteralType expression_type(CodeGenContext *context, ASTNode *expression);
static LLVMValueRef generate_expression_for_type(
    CodeGenContext *context, ASTNode *expression, const VarTypeNode *target_type);
static const VarTypeNode *indexed_value_type(
    CodeGenContext *context, ASTNode *expression);
static const char *index_base_name(ASTNode *expression);

// 比较两个完整类型节点是否等价。
static int var_type_equal(const VarTypeNode *left, const VarTypeNode *right) {
    if (!left || !right) return left == right;
    if (left->is_array != right->is_array) return 0;
    if (left->is_pointer != right->is_pointer) return 0;
    if (left->is_pointer) {
        return var_type_equal(left->element_type, right->element_type);
    }
    if (!left->is_array) {
        if (left->struct_name || right->struct_name) {
            return left->struct_name && right->struct_name &&
                   strcmp(left->struct_name, right->struct_name) == 0;
        }
        if (left->enum_name || right->enum_name) {
            return left->enum_name && right->enum_name &&
                   strcmp(left->enum_name, right->enum_name) == 0;
        }
        return left->type == right->type;
    }
    if (left->enum_name || right->enum_name) {
        if (!left->enum_name || !right->enum_name ||
            strcmp(left->enum_name, right->enum_name) != 0) {
            return 0;
        }
    }
    if (left->struct_name || right->struct_name) {
        if (!left->struct_name || !right->struct_name ||
            strcmp(left->struct_name, right->struct_name) != 0) {
            return 0;
        }
    }
    return left->array_length == right->array_length &&
           var_type_equal(left->element_type, right->element_type);
}

// 将形如 base.field 的限定名拆成两个部分；调用方负责释放输出字符串。
static int split_field_access_name(
    const char *name, char **base_out, char **field_out) {
    const char *dot = strchr(name, '.');
    if (!dot || dot == name || !dot[1] || strchr(dot + 1, '.')) return 0;

    size_t base_length = (size_t)(dot - name);
    char *base = malloc(base_length + 1);
    char *field = strdup(dot + 1);
    if (!base || !field) {
        free(base);
        free(field);
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    memcpy(base, name, base_length);
    base[base_length] = '\0';
    *base_out = base;
    *field_out = field;
    return 1;
}

// 解析结构体字段访问，返回基础符号、字段声明和字段下标。
static StructFieldNode *field_access_info(
    CodeGenContext *context, const char *name, Symbol **symbol_out,
    StructNode **struct_out, unsigned *index_out) {
    char *base_name = NULL;
    char *field_name = NULL;
    if (!split_field_access_name(name, &base_name, &field_name)) return NULL;

    Symbol *symbol = find_symbol(context, base_name);
    if (!symbol) {
        free(base_name);
        free(field_name);
        return NULL;
    }
    if (!symbol->declared_type || !symbol->declared_type->struct_name) {
        fprintf(stderr, "error: variable '%s' is not a struct\n",
                base_name); // 中文：变量不是结构体
        free(base_name);
        free(field_name);
        exit(1);
    }

    StructNode *struct_node = find_struct(context, symbol->declared_type->struct_name);
    StructFieldNode *field = find_struct_field(struct_node, field_name, index_out);
    if (!field) {
        fprintf(stderr, "error: struct '%s' has no field '%s'\n",
                symbol->declared_type->struct_name, field_name); // 中文：结构体没有该字段
        free(base_name);
        free(field_name);
        exit(1);
    }

    if (symbol_out) *symbol_out = symbol;
    if (struct_out) *struct_out = struct_node;
    free(base_name);
    free(field_name);
    return field;
}

// 生成结构体字段地址，用于字段读取和字段赋值。
static LLVMValueRef generate_field_address(
    CodeGenContext *context, const char *name, StructFieldNode **field_out,
    Symbol **symbol_out) {
    Symbol *symbol = NULL;
    StructNode *struct_node = NULL;
    unsigned index = 0;
    StructFieldNode *field =
        field_access_info(context, name, &symbol, &struct_node, &index);
    if (!field) return NULL;

    LLVMTypeRef struct_type = get_llvm_struct_type_by_name(context, struct_node->name);
    if (field_out) *field_out = field;
    if (symbol_out) *symbol_out = symbol;
    return LLVMBuildStructGEP2(
        context->builder, struct_type, symbol->value, index, "struct_field_ptr");
}

// 取函数名最后一段，模块函数内部名如 __tap_module_0.push 会得到 push。
static const char *function_base_name(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : name;
}

// 泛型函数单态化后带有 `$...` 后缀，方法查找只比较源语言中的基础名称。
static int method_name_matches(const char *function_name, const char *method_name) {
    const char *base = function_base_name(function_name);
    const char *suffix = strchr(base, '$');
    size_t length = suffix ? (size_t)(suffix - base) : strlen(base);
    return strlen(method_name) == length && strncmp(base, method_name, length) == 0;
}

// 根据接收者完整类型和方法名查找可作为方法调用的函数。
static FunctionNode *find_method_function(
    CodeGenContext *context, const VarTypeNode *receiver_type,
    const char *method_name) {
    if (!context->program || !receiver_type) return NULL;
    for (ASTNode *node = context->program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        const VarTypeNode *first_param = function_param_var_type(function, 0);
        if (first_param && var_type_equal(first_param, receiver_type) &&
            method_name_matches(function->name, method_name)) {
            return function;
        }
    }
    return NULL;
}

// 解析 receiver.method(...) 调用，只有接收者是结构体变量时才参与方法匹配。
static FunctionNode *resolve_method_call(
    CodeGenContext *context, const FunctionCallNode *call,
    Symbol **receiver_symbol_out, char **receiver_name_out,
    char **method_name_out) {
    char *receiver_name = NULL;
    char *method_name = NULL;
    if (!split_field_access_name(call->name, &receiver_name, &method_name)) {
        return NULL;
    }

    Symbol *receiver_symbol = find_symbol(context, receiver_name);
    if (!receiver_symbol ||
        !receiver_symbol->declared_type ||
        !receiver_symbol->declared_type->struct_name) {
        free(receiver_name);
        free(method_name);
        return NULL;
    }

    FunctionNode *function =
        find_method_function(context, receiver_symbol->declared_type, method_name);
    if (!function) {
        free(receiver_name);
        free(method_name);
        return NULL;
    }

    if (receiver_symbol_out) *receiver_symbol_out = receiver_symbol;
    if (receiver_name_out) {
        *receiver_name_out = receiver_name;
    } else {
        free(receiver_name);
    }
    if (method_name_out) {
        *method_name_out = method_name;
    } else {
        free(method_name);
    }
    return function;
}

// 判断调用是否返回使用 LLVM opaque pointer 的底层内存地址。
static int is_runtime_memory_pointer_result(const FunctionCallNode *call) {
    return call &&
        (strcmp(call->name, "__tap_malloc") == 0 ||
         strcmp(call->name, "__tap_realloc") == 0);
}

// 判断底层内存 ABI 的参数是否允许接收任意元素类型的裸指针。
static int is_runtime_memory_pointer_argument(
    const FunctionCallNode *call, unsigned index) {
    if (!call || index != 0) return 0;
    return strcmp(call->name, "__tap_realloc") == 0 ||
           strcmp(call->name, "__tap_free") == 0;
}

// 检查取地址表达式的目标类型是否与指针元素类型一致。
static int reference_assignable_to(
    CodeGenContext *context, const ReferenceNode *reference,
    const VarTypeNode *pointer_type) {
    if (!reference || !pointer_type || !pointer_type->is_pointer ||
        !reference->target) {
        return 0;
    }

    if (reference->target->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)reference->target)->name;
        Symbol *symbol = find_symbol(context, name);
        if (symbol) {
            if (symbol->declared_type) {
                return var_type_equal(symbol->declared_type, pointer_type->element_type);
            }
            const VarTypeNode *element_type = pointer_type->element_type;
            return !element_type->is_array && !element_type->is_pointer &&
                   !element_type->enum_name && !element_type->struct_name &&
                   element_type->type == symbol->type;
        }

        StructFieldNode *field = field_access_info(context, name, NULL, NULL, NULL);
        return field && var_type_equal(field->field_type, pointer_type->element_type);
    }

    if (reference->target->type == NODE_INDEX_EXPRESSION) {
        const VarTypeNode *element_type =
            indexed_value_type(context, reference->target);
        return element_type && var_type_equal(element_type, pointer_type->element_type);
    }

    return 0;
}

// 判断源表达式是否可以写入目标声明类型。
static int expression_assignable_to(CodeGenContext *context, ASTNode *expression,
                                    const VarTypeNode *target_type) {
    if (!target_type || target_type->is_array) return 0;
    if (target_type->is_pointer) {
        if (expression && expression->type == NODE_FUNCTION_CALL &&
            is_runtime_memory_pointer_result((FunctionCallNode *)expression)) {
            return 1;
        }
        if (expression && expression->type == NODE_REFERENCE) {
            return reference_assignable_to(
                context, (ReferenceNode *)expression, target_type);
        }
        if (expression && expression->type == NODE_FUNCTION_CALL) {
            FunctionNode *function =
                find_function(context, ((FunctionCallNode *)expression)->name);
            if (!function) {
                function = resolve_method_call(
                    context, (FunctionCallNode *)expression, NULL, NULL, NULL);
            }
            const VarTypeNode *return_type = function_return_var_type(function);
            return return_type && var_type_equal(return_type, target_type);
        }
        if (expression && expression->type == NODE_IDENTIFIER) {
            const char *name = ((IdentifierNode *)expression)->name;
            Symbol *symbol = find_symbol(context, name);
            if (symbol && symbol->declared_type) {
                return var_type_equal(symbol->declared_type, target_type);
            }
            StructFieldNode *field =
                field_access_info(context, name, NULL, NULL, NULL);
            return field && var_type_equal(field->field_type, target_type);
        }
        if (expression && expression->type == NODE_INDEX_EXPRESSION) {
            const VarTypeNode *element_type =
                indexed_value_type(context, expression);
            return element_type && var_type_equal(element_type, target_type);
        }
        return 0;
    }
    if (target_type->struct_name) {
        if (expression && expression->type == NODE_STRUCT_LITERAL) {
            StructLiteralNode *literal = (StructLiteralNode *)expression;
            return strcmp(literal->struct_name, target_type->struct_name) == 0;
        }
        if (expression && expression->type == NODE_IDENTIFIER) {
            IdentifierNode *identifier = (IdentifierNode *)expression;
            // 载荷枚举的无载荷成员，例如 Shape.Empty。
            EnumVariantNode *bare = NULL;
            EnumNode *bare_enum =
                bare_variant_reference(context, identifier->name, &bare);
            if (bare_enum) {
                EnumNode *target_enum =
                    struct_tagged_enum(context, target_type->struct_name);
                return target_enum && strcmp(target_enum->name, bare_enum->name) == 0;
            }
            Symbol *symbol = find_symbol(context, identifier->name);
            if (symbol && symbol->declared_type && symbol->declared_type->struct_name) {
                return strcmp(symbol->declared_type->struct_name,
                              target_type->struct_name) == 0;
            }
            StructFieldNode *field =
                field_access_info(context, identifier->name, NULL, NULL, NULL);
            return field && field->field_type->struct_name &&
                   strcmp(field->field_type->struct_name, target_type->struct_name) == 0;
        }
        if (expression && expression->type == NODE_FUNCTION_CALL) {
            // 载荷枚举的构造表达式产生该枚举（同名结构体）类型的值。
            EnumVariantNode *variant = NULL;
            EnumNode *enum_node = variant_reference(
                context, ((FunctionCallNode *)expression)->name, &variant);
            if (enum_node && enum_node->has_payload) {
                EnumNode *target_enum =
                    struct_tagged_enum(context, target_type->struct_name);
                return target_enum && strcmp(target_enum->name, enum_node->name) == 0;
            }
            FunctionNode *function =
                find_function(context, ((FunctionCallNode *)expression)->name);
            if (!function) {
                function = resolve_method_call(
                    context, (FunctionCallNode *)expression, NULL, NULL, NULL);
            }
            const VarTypeNode *return_type = function_return_var_type(function);
            return return_type && return_type->struct_name &&
                   strcmp(return_type->struct_name, target_type->struct_name) == 0;
        }
        if (expression && expression->type == NODE_INDEX_EXPRESSION) {
            const VarTypeNode *element_type =
                indexed_value_type(context, expression);
            return element_type && var_type_equal(element_type, target_type);
        }
        return 0;
    }
    if (target_type->enum_name) {
        if (expression && expression->type == NODE_IDENTIFIER) {
            IdentifierNode *identifier = (IdentifierNode *)expression;
            uint64_t ignored = 0;
            if (enum_variant_value(context, identifier->name, &ignored)) {
                const char *dot = strchr(identifier->name, '.');
                return dot && strlen(target_type->enum_name) == (size_t)(dot - identifier->name) &&
                       strncmp(identifier->name, target_type->enum_name,
                               (size_t)(dot - identifier->name)) == 0;
            }
            Symbol *symbol = find_symbol(context, identifier->name);
            if (symbol && symbol->declared_type && symbol->declared_type->enum_name) {
                return strcmp(symbol->declared_type->enum_name, target_type->enum_name) == 0;
            }
        }
        if (expression && expression->type == NODE_FUNCTION_CALL) {
            FunctionNode *function =
                find_function(context, ((FunctionCallNode *)expression)->name);
            if (!function) {
                function = resolve_method_call(
                    context, (FunctionCallNode *)expression, NULL, NULL, NULL);
            }
            const VarTypeNode *return_type = function_return_var_type(function);
            return return_type && return_type->enum_name &&
                   strcmp(return_type->enum_name, target_type->enum_name) == 0;
        }
        if (expression && expression->type == NODE_INDEX_EXPRESSION) {
            const VarTypeNode *element_type =
                indexed_value_type(context, expression);
            return element_type && var_type_equal(element_type, target_type);
        }
        return 0;
    }
    return scalar_types_compatible(target_type->type,
                                   expression_type(context, expression));
}

// 解析标识符或连续下标表达式最终指向的数组/指针元素类型。
static const VarTypeNode *indexed_value_type(CodeGenContext *context, ASTNode *expression) {
    if (expression && expression->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)expression)->name;
        Symbol *symbol = find_symbol(context, name);
        if (!symbol) {
            StructFieldNode *field =
                field_access_info(context, name, NULL, NULL, NULL);
            if (field && field->field_type->is_pointer) {
                return field->field_type->element_type;
            }
            if (field && field->field_type->is_array) {
                return field->field_type;
            }
            fprintf(stderr, "error: undefined variable '%s'\n", name); // 中文：未定义的变量
            exit(1);
        }
        if (symbol->declared_type && symbol->declared_type->is_pointer) {
            return symbol->declared_type->element_type;
        }
        if (!symbol->array_type) {
            fprintf(stderr, "error: variable '%s' is not an array or pointer\n", name); // 中文：变量不是数组或指针
            exit(1);
        }
        return symbol->array_type;
    }

    if (expression && expression->type == NODE_INDEX_EXPRESSION) {
        IndexExpressionNode *index = (IndexExpressionNode *)expression;
        const VarTypeNode *container_type = indexed_value_type(context, index->array);
        if (container_type->is_pointer) {
            return container_type->element_type;
        }
        if (!container_type->is_array) {
            return container_type;
        }
        return container_type->element_type;
    }

    if (expression && expression->type == NODE_FUNCTION_CALL) {
        FunctionCallNode *call = (FunctionCallNode *)expression;
        FunctionNode *function = find_function(context, call->name);
        const VarTypeNode *return_type = function_return_var_type(function);
        if (return_type && return_type->is_pointer) {
            return return_type->element_type;
        }
        if (!return_type || !return_type->is_array) {
            fprintf(stderr, "error: function '%s' does not return an array or pointer\n", call->name); // 中文：函数不返回数组或指针
            exit(1);
        }
        return return_type;
    }

    fprintf(stderr, "error: invalid array or pointer index target\n"); // 中文：数组或指针索引目标无效
    exit(1);
}

// 深拷贝完整类型节点，供局部引用类型推断后由 AST 独立持有。
static VarTypeNode *copy_var_type(const VarTypeNode *source) {
    if (!source) return NULL;
    if (source->is_pointer) {
        return create_pointer_type(copy_var_type(source->element_type));
    }
    if (source->is_array) {
        return create_array_type(
            copy_var_type(source->element_type), source->array_length);
    }
    if (source->struct_name) return create_struct_type(source->struct_name);
    if (source->enum_name) return create_enum_type(source->enum_name);
    return create_var_type(source->type);
}

// 根据可寻址目标推断 `&target` 的完整 *T 类型。
static VarTypeNode *infer_reference_type(
    CodeGenContext *context, const ReferenceNode *reference) {
    ASTNode *target = reference ? reference->target : NULL;
    const VarTypeNode *target_type = NULL;
    VarTypeNode *inferred_scalar = NULL;

    if (target && target->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)target)->name;
        Symbol *symbol = find_symbol(context, name);
        if (symbol) {
            if (symbol->declared_type) {
                target_type = symbol->declared_type;
            } else {
                inferred_scalar = create_var_type(symbol->type);
                target_type = inferred_scalar;
            }
        } else {
            StructFieldNode *field =
                field_access_info(context, name, NULL, NULL, NULL);
            if (field) target_type = field->field_type;
        }
    } else if (target && target->type == NODE_INDEX_EXPRESSION) {
        target_type = indexed_value_type(context, target);
    }

    if (!target_type) {
        fprintf(stderr, "error: reference target is not addressable\n"); // 中文：引用目标不可寻址
        exit(1);
    }

    VarTypeNode *element_type = copy_var_type(target_type);
    free_ast((ASTNode *)inferred_scalar);
    return create_pointer_type(element_type);
}

// 推断表达式在当前上下文中的标量类型。
static enum LiteralType expression_type(CodeGenContext *context, ASTNode *expression) {
    if (!expression) return default_integer_type();

    switch (expression->type) {
        case NODE_LITERAL:
            return ((LiteralNode *)expression)->literal_type;
        case NODE_IDENTIFIER: {
            const char *name = ((IdentifierNode *)expression)->name;
            Symbol *symbol = find_symbol(context, name);
            if (!symbol) {
                uint64_t enum_value = 0;
                if (enum_variant_value(context, name, &enum_value)) return LITERAL_I32;
                StructFieldNode *field =
                    field_access_info(context, name, NULL, NULL, NULL);
                if (field) return field->field_type->type;
                VarDeclNode *constant = find_global_constant(context, name);
                return constant && constant->type
                    ? constant->type->type
                    : default_integer_type();
            }
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
        case NODE_SIZEOF:
            return LITERAL_UINT;
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            const char *receiver_name = NULL;
            size_t receiver_length = 0;
            FunctionNode *function = find_function(context, call->name);
            if (!function) {
                function = resolve_method_call(context, call, NULL, NULL, NULL);
            }
            StringMethodKind method = internal_string_method(call);
            if (method == STRING_METHOD_NONE) {
                method = named_string_method(
                    call, &receiver_name, &receiver_length);
            }
            if (!function) {
                if (method == STRING_METHOD_LEN) return LITERAL_UINT;
                if (method == STRING_METHOD_BYTE_AT) return LITERAL_U8;
                if (method == STRING_METHOD_SLICE) return LITERAL_STRING;
            }
            return function_return_type(function);
        }
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            if (binary->op_type >= OP_EQUAL && binary->op_type <= OP_GREATER_THAN_OR_EQUAL) {
                return LITERAL_BOOL;
            }
            if (binary->op_type == OP_AND || binary->op_type == OP_OR) {
                return LITERAL_BOOL;
            }
            enum LiteralType left_type = expression_type(context, binary->left);
            enum LiteralType right_type = expression_type(context, binary->right);
            if (is_float_type(left_type) || is_float_type(right_type)) {
                return common_float_type(left_type, right_type);
            }
            return common_integer_type(left_type, right_type);
        }
        default:
            return default_integer_type();
    }
}

// 在整数类型之间执行扩展、截断或转 bool。
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

// 根据目标类型执行标量转换；支持整数扩展/截断、f32/f64 互转和 int -> float。
static LLVMValueRef cast_value(CodeGenContext *context, LLVMValueRef value,
                               enum LiteralType source, enum LiteralType target) {
    if (is_integer_type(source) && is_integer_type(target)) {
        return cast_integer(context, value, source, target);
    }
    if (is_integer_type(source) && is_float_type(target)) {
        LLVMTypeRef target_type = get_llvm_type(context, target);
        return is_unsigned_type(source)
            ? LLVMBuildUIToFP(context->builder, value, target_type, "uint_to_float")
            : LLVMBuildSIToFP(context->builder, value, target_type, "int_to_float");
    }
    if (is_float_type(source) && is_float_type(target)) {
        if (source == target) return value;
        if (target == LITERAL_F64) {
            return LLVMBuildFPExt(context->builder, value, get_llvm_type(context, target),
                                  "float_ext");
        }
        if (target == LITERAL_F32 || target == LITERAL_FLOAT) {
            return LLVMBuildFPTrunc(context->builder, value, get_llvm_type(context, target),
                                    "float_trunc");
        }
    }
    return value;
}

static LLVMValueRef generate_expression(CodeGenContext *context, ASTNode *expression);
static LLVMValueRef generate_function_call(CodeGenContext *context, FunctionCallNode *call);
static LLVMValueRef generate_variant_value(
    CodeGenContext *context, const char *struct_name,
    EnumNode *enum_node, EnumVariantNode *variant,
    ASTNode *arguments, const char *filename, int line, int column);
static LLVMValueRef generate_logical_binary(CodeGenContext *context, BinaryOpNode *binary);
static LLVMValueRef condition_value(CodeGenContext *context, ASTNode *condition);
static LLVMValueRef generate_integer_binary(CodeGenContext *context, BinaryOpNode *binary,
                                            enum LiteralType operand_type);
static LLVMValueRef generate_float_binary(CodeGenContext *context, BinaryOpNode *binary,
                                          enum LiteralType operand_type);
static LLVMValueRef generate_array_value(
    CodeGenContext *context, ASTNode *expression, const VarTypeNode *expected_type);
static unsigned function_param_count(FunctionNode *function);
static void validate_var_type(CodeGenContext *context, VarTypeNode *type);

// 取出整数字面量的数值。bool 字面量写入的是 union 的 bool_value（4 字节），
// 直接按 int_value（8 字节）读会带进未初始化的高位字节，因此按类型选正确的成员。
static uint64_t literal_integer_value(const LiteralNode *literal) {
    if (literal->literal_type == LITERAL_BOOL) {
        return literal->value.bool_value != 0 ? 1 : 0;
    }
    return literal->value.int_value;
}

// 根据整数字面量原文或数值构造指定整数类型的 LLVM 常量。
static LLVMValueRef integer_constant(CodeGenContext *context, LiteralNode *literal,
                                     enum LiteralType type) {
    LLVMTypeRef llvm_type = get_llvm_type(context, type);
    if (literal->integer_text) {
        return LLVMConstIntOfStringAndSize(llvm_type, literal->integer_text,
                                          (unsigned)strlen(literal->integer_text), 10);
    }
    return LLVMConstInt(llvm_type, literal_integer_value(literal), 0);
}

// 查找结构体字面量中的初始化字段。
static StructInitFieldNode *find_struct_init_field(
    StructLiteralNode *literal, const char *name) {
    for (ASTNode *field = literal->fields; field; field = field->next) {
        StructInitFieldNode *init = (StructInitFieldNode *)field;
        if (strcmp(init->name, name) == 0) return init;
    }
    return NULL;
}

// 校验结构体字面量字段是否完整、无重复且无未知字段。
static void validate_struct_literal_fields(
    CodeGenContext *context, StructNode *struct_node, StructLiteralNode *literal) {
    for (ASTNode *init_node = literal->fields; init_node; init_node = init_node->next) {
        StructInitFieldNode *init = (StructInitFieldNode *)init_node;
        if (!find_struct_field(struct_node, init->name, NULL)) {
            fprintf(stderr, "error: struct '%s' has no field '%s'\n",
                    struct_node->name, init->name); // 中文：结构体没有该字段
            exit(1);
        }
        for (ASTNode *other = init_node->next; other; other = other->next) {
            if (strcmp(init->name, ((StructInitFieldNode *)other)->name) == 0) {
                fprintf(stderr, "error: duplicate struct initializer field '%s'\n",
                        init->name); // 中文：重复的结构体初始化字段
                exit(1);
            }
        }
    }

    for (ASTNode *field_node = struct_node->fields; field_node; field_node = field_node->next) {
        StructFieldNode *field = (StructFieldNode *)field_node;
        if (!find_struct_init_field(literal, field->name)) {
            fprintf(stderr, "error: missing initializer for field '%s.%s'\n",
                    struct_node->name, field->name); // 中文：缺少结构体字段初始化
            exit(1);
        }
        validate_var_type(context, field->field_type);
    }
}

// 生成结构体字面量的 LLVM 聚合值。
static LLVMValueRef generate_struct_literal_value(
    CodeGenContext *context, StructLiteralNode *literal, const VarTypeNode *target_type) {
    if (!target_type || !target_type->struct_name ||
        strcmp(literal->struct_name, target_type->struct_name) != 0) {
        fprintf(stderr, "error: struct initializer type mismatch\n"); // 中文：结构体初始化类型不匹配
        exit(1);
    }

    StructNode *struct_node = find_struct(context, literal->struct_name);
    if (!struct_node) {
        fprintf(stderr, "error: undefined struct type '%s'\n",
                literal->struct_name); // 中文：未定义的结构体类型
        exit(1);
    }
    validate_struct_literal_fields(context, struct_node, literal);

    LLVMTypeRef struct_type = get_llvm_struct_type_by_name(context, literal->struct_name);
    LLVMValueRef value = LLVMGetUndef(struct_type);
    unsigned index = 0;
    for (ASTNode *field_node = struct_node->fields;
         field_node; field_node = field_node->next, index++) {
        StructFieldNode *field = (StructFieldNode *)field_node;
        StructInitFieldNode *init = find_struct_init_field(literal, field->name);
        LLVMValueRef field_value =
            generate_expression_for_type(context, init->expression, field->field_type);
        value = LLVMBuildInsertValue(
            context->builder, value, field_value, index, "struct_insert");
    }
    return value;
}

// 按目标标量类型生成表达式，并在必要时执行类型转换。
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
        if (binary->op_type >= OP_ADD && binary->op_type <= OP_MODULO) {
            // 窄目标类型不能改变表达式本身的求值宽度：先把表达式按它自己的自然类型求值，
            // 再截断到目标类型。只有目标更宽时才直接用目标类型，这样 i128 这类宽字面量
            // 仍能在宽类型下参与运算而不丢失精度。
            enum LiteralType natural = expression_type(context, expression);
            enum LiteralType operand =
                integer_type_bits(target) > integer_type_bits(natural) ? target : natural;
            LLVMValueRef value = generate_integer_binary(context, binary, operand);
            return cast_value(context, value, operand, target);
        }
    }

    enum LiteralType source = expression_type(context, expression);
    return cast_value(context, generate_expression(context, expression), source, target);
}

// 校验构造上显式写的类型实参（`Enum<T>.Member(...)`）。
// 实例化始终由目标类型驱动，显式实参起的是断言作用：数量要和枚举声明一致，每一项也要和
// 推断出的实参相符。这样写错了会直接报错，而不是被静默忽略。
// 类型实参在泛型单态化阶段已经解析过，这里可以直接比较。
static void validate_variant_type_arguments(
    CodeGenContext *context, const EnumNode *enum_node,
    ASTNode *explicit_arguments, const VarTypeNode *target_type) {
    (void)context;
    if (!explicit_arguments) return;

    unsigned declared = 0;
    for (ASTNode *node = enum_node->type_params; node; node = node->next) declared++;
    unsigned given = 0;
    for (ASTNode *node = explicit_arguments; node; node = node->next) given++;
    if (declared != given) {
        fprintf(stderr,
            "error: enum '%s' takes %u type argument(s), but %u were given\n",
            enum_node->name, declared, given); // 中文：枚举类型实参数量不匹配
        exit(1);
    }

    // 目标类型不是同一个泛型枚举的实例化时无从比较，到此为止。
    if (!target_type || !target_type->type_arguments) return;

    ASTNode *explicit_node = explicit_arguments;
    ASTNode *inferred_node = target_type->type_arguments;
    unsigned index = 1;
    for (; explicit_node && inferred_node;
         explicit_node = explicit_node->next, inferred_node = inferred_node->next) {
        if (!var_type_equal((VarTypeNode *)explicit_node,
                            (VarTypeNode *)inferred_node)) {
            fprintf(stderr,
                "error: type argument %u of enum '%s' does not match the target type\n",
                index, enum_node->name); // 中文：显式类型实参与目标类型不符
            exit(1);
        }
        index++;
    }
}

// 根据完整目标类型生成表达式，结构体和数组保留高层类型信息。
static LLVMValueRef generate_expression_for_type(
    CodeGenContext *context, ASTNode *expression, const VarTypeNode *target_type) {
    if (!target_type) return generate_expression(context, expression);
    if (target_type->is_array) {
        return generate_array_value(context, expression, target_type);
    }
    if (target_type->is_pointer) {
        if (!expression_assignable_to(context, expression, target_type)) {
            fprintf(stderr, "error: pointer expression type mismatch\n"); // 中文：指针表达式类型不匹配
            exit(1);
        }
        return generate_expression(context, expression);
    }
    if (target_type->struct_name) {
        if (!expression_assignable_to(context, expression, target_type)) {
            fprintf(stderr, "error: struct expression type mismatch\n"); // 中文：结构体表达式类型不匹配
            exit(1);
        }
        if (expression && expression->type == NODE_STRUCT_LITERAL) {
            return generate_struct_literal_value(
                context, (StructLiteralNode *)expression, target_type);
        }
        if (expression && expression->type == NODE_FUNCTION_CALL) {
            // 载荷枚举构造只在有目标类型时才成立，这里负责真正生成值。
            FunctionCallNode *call = (FunctionCallNode *)expression;
            EnumVariantNode *variant = NULL;
            EnumNode *enum_node = variant_reference(context, call->name, &variant);
            if (enum_node && enum_node->has_payload) {
                validate_variant_type_arguments(
                    context, enum_node, call->enum_type_arguments, target_type);
                return generate_variant_value(context, target_type->struct_name,
                    enum_node, variant,
                    call->arguments, call->filename, call->line, call->column);
            }
        }
        if (expression && expression->type == NODE_IDENTIFIER) {
            // 载荷枚举的无载荷成员：Shape.Empty 直接构造只带标签的值。
            IdentifierNode *identifier = (IdentifierNode *)expression;
            EnumVariantNode *bare = NULL;
            EnumNode *bare_enum = bare_variant_reference(
                context, identifier->name, &bare);
            if (bare_enum) {
                validate_variant_type_arguments(
                    context, bare_enum, identifier->enum_type_arguments, target_type);
                return generate_variant_value(context, target_type->struct_name,
                    bare_enum, bare, NULL, NULL, 0, 0);
            }
            StructFieldNode *field = NULL;
            LLVMValueRef field_address =
                generate_field_address(context, identifier->name, &field, NULL);
            if (field_address) {
                return LLVMBuildLoad2(
                    context->builder, get_llvm_var_type(context, field->field_type),
                    field_address, "struct_field_value");
            }
        }
        return generate_expression(context, expression);
    }
    return generate_expression_as(context, expression, target_type->type);
}

// 生成数组元素地址，同时执行运行时越界检查。
static LLVMValueRef generate_index_address(
    CodeGenContext *context, IndexExpressionNode *index_expression,
    const VarTypeNode **element_type_out) {
    LLVMValueRef array_address = NULL;
    LLVMValueRef pointer_value = NULL;
    const VarTypeNode *array_type = NULL;
    int is_pointer_index = 0;

    // 第一层下标从数组存储或指针值开始；后续下标从子数组地址继续。
    if (index_expression->array &&
        index_expression->array->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)index_expression->array)->name;
        Symbol *symbol = find_symbol(context, name);
        if (!symbol) {
            StructFieldNode *field = NULL;
            LLVMValueRef field_address =
                generate_field_address(context, name, &field, NULL);
            if (field_address && field->field_type->is_pointer) {
                array_type = field->field_type;
                pointer_value = LLVMBuildLoad2(
                    context->builder, get_llvm_var_type(context, array_type),
                    field_address, "loaded_field_pointer");
                is_pointer_index = 1;
            } else if (field_address && field->field_type->is_array) {
                array_address = field_address;
                array_type = field->field_type;
            } else {
                fprintf(stderr, "error: undefined variable '%s'\n", name); // 中文：未定义的变量
                exit(1);
            }
        } else if (symbol->declared_type && symbol->declared_type->is_pointer) {
            array_type = symbol->declared_type;
            pointer_value = LLVMBuildLoad2(
                context->builder, get_llvm_var_type(context, array_type),
                symbol->value, "loaded_pointer");
            is_pointer_index = 1;
        } else if (symbol->array_type) {
            array_address = symbol->value;
            array_type = symbol->array_type;
        } else {
            fprintf(stderr, "error: variable '%s' is not an array or pointer\n", name); // 中文：变量不是数组或指针
            exit(1);
        }
    } else if (index_expression->array &&
               index_expression->array->type == NODE_INDEX_EXPRESSION) {
        array_address = generate_index_address(
            context, (IndexExpressionNode *)index_expression->array, &array_type);
        if (array_type->is_pointer) {
            pointer_value = LLVMBuildLoad2(
                context->builder, get_llvm_var_type(context, array_type),
                array_address, "loaded_nested_pointer");
            is_pointer_index = 1;
        } else if (!array_type->is_array) {
            fprintf(stderr, "error: index target is not an array or pointer\n"); // 中文：索引目标不是数组或指针
            exit(1);
        }
    } else if (index_expression->array &&
               index_expression->array->type == NODE_FUNCTION_CALL) {
        FunctionCallNode *call = (FunctionCallNode *)index_expression->array;
        FunctionNode *function = find_function(context, call->name);
        array_type = function_return_var_type(function);
        if (array_type && array_type->is_pointer) {
            pointer_value = generate_function_call(context, call);
            is_pointer_index = 1;
        } else if (array_type && array_type->is_array) {
            LLVMTypeRef llvm_array_type = get_llvm_var_type(context, array_type);
            array_address = create_entry_alloca(context, llvm_array_type, "array_return_tmp");
            LLVMBuildStore(context->builder, generate_function_call(context, call), array_address);
        } else {
            fprintf(stderr, "error: function '%s' does not return an array or pointer\n", call->name); // 中文：函数不返回数组或指针
            exit(1);
        }
    } else {
        fprintf(stderr, "error: invalid array or pointer index target\n"); // 中文：数组或指针索引目标无效
        exit(1);
    }

    // 固定长度数组的每一维都会根据下标有无符号执行上下界检查。
    enum LiteralType index_type = expression_type(context, index_expression->index);
    if (!is_integer_type(index_type) || integer_type_bits(index_type) > 64) {
        fprintf(stderr, "error: index must be an integer of at most 64 bits\n"); // 中文：下标必须是最多 64 位的整数
        exit(1);
    }

    LLVMValueRef raw_index = generate_expression(context, index_expression->index);
    enum LiteralType check_type = is_unsigned_type(index_type) ? LITERAL_U64 : LITERAL_I64;
    LLVMValueRef index = cast_integer(context, raw_index, index_type, check_type);
    LLVMTypeRef index_llvm_type = get_llvm_type(context, check_type);

    if (is_pointer_index) {
        if (element_type_out) *element_type_out = array_type->element_type;
        LLVMTypeRef element_llvm_type = get_llvm_var_type(context, array_type->element_type);
        return LLVMBuildGEP2(context->builder, element_llvm_type, pointer_value,
                             &index, 1, "pointer_element_ptr");
    }

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

// 生成可寻址表达式的地址，并阻止通过引用修改常量。
static LLVMValueRef generate_reference(
    CodeGenContext *context, ReferenceNode *reference) {
    ASTNode *target = reference ? reference->target : NULL;
    if (target && target->type == NODE_IDENTIFIER) {
        const char *name = ((IdentifierNode *)target)->name;
        Symbol *symbol = find_symbol(context, name);
        if (symbol) {
            if (symbol->is_const) {
                fprintf(stderr, "error: cannot take mutable reference to constant '%s'\n",
                        name); // 中文：不能取得常量的可写引用
                exit(1);
            }
            return symbol->value;
        }

        StructFieldNode *field = NULL;
        Symbol *base_symbol = NULL;
        LLVMValueRef address = generate_field_address(
            context, name, &field, &base_symbol);
        if (address) {
            if (base_symbol->is_const) {
                fprintf(stderr, "error: cannot take mutable reference to constant '%s'\n",
                        base_symbol->name); // 中文：不能取得常量字段的可写引用
                exit(1);
            }
            return address;
        }
    }

    if (target && target->type == NODE_INDEX_EXPRESSION) {
        const char *base_name = index_base_name(target);
        Symbol *symbol = base_name ? find_symbol(context, base_name) : NULL;
        if (symbol && symbol->is_const) {
            fprintf(stderr, "error: cannot take mutable reference to constant '%s'\n",
                    base_name); // 中文：不能取得常量元素的可写引用
            exit(1);
        }
        return generate_index_address(
            context, (IndexExpressionNode *)target, NULL);
    }

    fprintf(stderr, "error: reference target is not addressable\n"); // 中文：引用目标不可寻址
    exit(1);
}

// 生成整数二元运算或比较表达式。
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
        case OP_MODULO:
            return is_unsigned_type(operand_type)
                ? LLVMBuildURem(context->builder, left, right, "urem_result")
                : LLVMBuildSRem(context->builder, left, right, "srem_result");
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

// 生成浮点二元运算与比较。比较使用 ordered 谓词，NaN 参与比较时结果为 false。
static LLVMValueRef generate_float_binary(CodeGenContext *context, BinaryOpNode *binary,
                                          enum LiteralType operand_type) {
    LLVMValueRef left = generate_expression_as(context, binary->left, operand_type);
    LLVMValueRef right = generate_expression_as(context, binary->right, operand_type);

    switch (binary->op_type) {
        case OP_ADD: return LLVMBuildFAdd(context->builder, left, right, "fadd_result");
        case OP_SUBTRACT: return LLVMBuildFSub(context->builder, left, right, "fsub_result");
        case OP_MULTIPLY: return LLVMBuildFMul(context->builder, left, right, "fmul_result");
        case OP_DIVIDE: return LLVMBuildFDiv(context->builder, left, right, "fdiv_result");
        case OP_MODULO: return LLVMBuildFRem(context->builder, left, right, "frem_result");
        case OP_EQUAL:
            return LLVMBuildFCmp(context->builder, LLVMRealOEQ, left, right, "feq_result");
        case OP_NOT_EQUAL:
            return LLVMBuildFCmp(context->builder, LLVMRealONE, left, right, "fne_result");
        case OP_LESS_THAN:
            return LLVMBuildFCmp(context->builder, LLVMRealOLT, left, right, "flt_result");
        case OP_GREATER_THAN:
            return LLVMBuildFCmp(context->builder, LLVMRealOGT, left, right, "fgt_result");
        case OP_LESS_THAN_OR_EQUAL:
            return LLVMBuildFCmp(context->builder, LLVMRealOLE, left, right, "fle_result");
        case OP_GREATER_THAN_OR_EQUAL:
            return LLVMBuildFCmp(context->builder, LLVMRealOGE, left, right, "fge_result");
        default:
            fprintf(stderr, "error: unsupported binary operator\n"); // 中文：不支持的二元操作符
            exit(1);
    }
}

// 生成 && / || 的短路求值：左侧结果决定是否需要求值右侧。
// && 左侧为假、|| 左侧为真时，右侧完全不求值。
static LLVMValueRef generate_logical_binary(CodeGenContext *context, BinaryOpNode *binary) {
    int is_and = binary->op_type == OP_AND;
    LLVMValueRef function = LLVMGetBasicBlockParent(LLVMGetInsertBlock(context->builder));

    LLVMBasicBlockRef rhs_block = LLVMAppendBasicBlockInContext(
        context->context, function, is_and ? "and_rhs" : "or_rhs");
    LLVMBasicBlockRef merge_block = LLVMAppendBasicBlockInContext(
        context->context, function, is_and ? "and_end" : "or_end");

    // 左侧本身可能含短路运算，求值后必须重新取当前基本块作为 phi 的前驱。
    LLVMValueRef left = condition_value(context, binary->left);
    LLVMBasicBlockRef left_block = LLVMGetInsertBlock(context->builder);

    if (is_and) {
        LLVMBuildCondBr(context->builder, left, rhs_block, merge_block);
    } else {
        LLVMBuildCondBr(context->builder, left, merge_block, rhs_block);
    }

    LLVMPositionBuilderAtEnd(context->builder, rhs_block);
    LLVMValueRef right = condition_value(context, binary->right);
    LLVMBasicBlockRef right_block = LLVMGetInsertBlock(context->builder);
    LLVMBuildBr(context->builder, merge_block);

    LLVMPositionBuilderAtEnd(context->builder, merge_block);
    LLVMTypeRef bool_type = LLVMInt1TypeInContext(context->context);
    LLVMValueRef phi = LLVMBuildPhi(context->builder, bool_type, "logical_result");
    LLVMValueRef incoming_values[2] = {
        LLVMConstInt(bool_type, is_and ? 0 : 1, 0),
        right
    };
    LLVMBasicBlockRef incoming_blocks[2] = {left_block, right_block};
    LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
    return phi;
}

// 解析字符串方法的接收者和用户参数。
static LLVMValueRef generate_string_receiver(
    CodeGenContext *context, FunctionCallNode *call, StringMethodKind method,
    const char *receiver_name, size_t receiver_length,
    unsigned expected_argument_count, ASTNode **arguments_out) {
    unsigned argument_count = 0;
    for (ASTNode *argument = call->arguments; argument; argument = argument->next) {
        argument_count++;
    }

    unsigned user_argument_count = receiver_name
        ? argument_count
        : argument_count > 0 ? argument_count - 1 : 0;
    if (user_argument_count != expected_argument_count) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "method '%s' expects %u arguments, but got %u",
                         string_method_name(method), expected_argument_count,
                         user_argument_count);
        exit(1);
    }

    if (receiver_name) {
        Symbol *symbol = find_symbol_with_length(context, receiver_name, receiver_length);
        if (!symbol) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "undefined variable '%.*s'",
                             (int)receiver_length, receiver_name);
            exit(1);
        }
        if (symbol->array_type || symbol->type != LITERAL_STRING) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "method '%s' is only available on string values",
                             string_method_name(method));
            exit(1);
        }
        *arguments_out = call->arguments;
        return LLVMBuildLoad2(
            context->builder, get_llvm_type(context, LITERAL_STRING),
            symbol->value, "string_method_receiver");
    }

    ASTNode *receiver_expression = call->arguments;
    if (!receiver_expression || expression_type(context, receiver_expression) != LITERAL_STRING) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "method '%s' is only available on string values",
                         string_method_name(method));
        exit(1);
    }
    *arguments_out = receiver_expression->next;
    return generate_expression(context, receiver_expression);
}

// 生成字符串 len() 计算，返回 UTF-8 字节长度。
static LLVMValueRef generate_string_length(
    CodeGenContext *context, LLVMValueRef string_value) {
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

static LLVMValueRef runtime_string_function(
    CodeGenContext *context, const char *name, LLVMTypeRef return_type,
    LLVMTypeRef *parameter_types, unsigned parameter_count) {
    LLVMValueRef function = LLVMGetNamedFunction(context->module, name);
    if (function) return function;
    LLVMTypeRef function_type = LLVMFunctionType(
        return_type, parameter_types, parameter_count, 0);
    return LLVMAddFunction(context->module, name, function_type);
}

// 按 UTF-8 编码字节序生成字符串内容比较。
static LLVMValueRef generate_string_comparison(
    CodeGenContext *context, BinaryOpNode *binary) {
    LLVMTypeRef string_type = get_llvm_type(context, LITERAL_STRING);
    LLVMTypeRef parameter_types[2] = {string_type, string_type};
    LLVMValueRef function = runtime_string_function(
        context, "__tap_string_compare", get_llvm_type(context, LITERAL_I32),
        parameter_types, 2);
    LLVMValueRef left = generate_expression(context, binary->left);
    LLVMValueRef right = generate_expression(context, binary->right);
    LLVMValueRef arguments[2] = {left, right};
    LLVMValueRef result = LLVMBuildCall2(
        context->builder, LLVMGlobalGetValueType(function), function,
        arguments, 2, "string_compare");
    LLVMValueRef zero = LLVMConstInt(get_llvm_type(context, LITERAL_I32), 0, 0);

    LLVMIntPredicate predicate;
    switch (binary->op_type) {
        case OP_EQUAL: predicate = LLVMIntEQ; break;
        case OP_NOT_EQUAL: predicate = LLVMIntNE; break;
        case OP_LESS_THAN: predicate = LLVMIntSLT; break;
        case OP_GREATER_THAN: predicate = LLVMIntSGT; break;
        case OP_LESS_THAN_OR_EQUAL: predicate = LLVMIntSLE; break;
        case OP_GREATER_THAN_OR_EQUAL: predicate = LLVMIntSGE; break;
        default:
            fprintf(stderr, "error: strings only support comparison operators\n");
            exit(1);
    }
    return LLVMBuildICmp(
        context->builder, predicate, result, zero, "string_compare_result");
}

static LLVMValueRef generate_string_integer_argument(
    CodeGenContext *context, FunctionCallNode *call, ASTNode *argument,
    unsigned argument_index) {
    enum LiteralType type = expression_type(context, argument);
    if (!is_integer_type(type) || integer_type_bits(type) > 64) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "method '%s' argument %u must be an integer of at most 64 bits",
                         strchr(call->name, '.') ? strrchr(call->name, '.') + 1 :
                         string_method_name(internal_string_method(call)),
                         argument_index);
        exit(1);
    }
    return generate_expression_as(context, argument, LITERAL_I64);
}

// 生成内建字符串方法。
static LLVMValueRef generate_string_method(
    CodeGenContext *context, FunctionCallNode *call, StringMethodKind method,
    const char *receiver_name, size_t receiver_length) {
    unsigned expected_count = method == STRING_METHOD_LEN
        ? 0
        : method == STRING_METHOD_BYTE_AT ? 1 : 2;
    ASTNode *arguments = NULL;
    LLVMValueRef receiver = generate_string_receiver(
        context, call, method, receiver_name, receiver_length,
        expected_count, &arguments);

    if (method == STRING_METHOD_LEN) {
        return generate_string_length(context, receiver);
    }

    LLVMTypeRef string_type = get_llvm_type(context, LITERAL_STRING);
    LLVMTypeRef index_type = get_llvm_type(context, LITERAL_I64);
    if (method == STRING_METHOD_BYTE_AT) {
        LLVMTypeRef parameter_types[2] = {string_type, index_type};
        LLVMValueRef function = runtime_string_function(
            context, "__tap_string_byte_at", get_llvm_type(context, LITERAL_U8),
            parameter_types, 2);
        LLVMValueRef call_arguments[2] = {
            receiver,
            generate_string_integer_argument(context, call, arguments, 1)
        };
        return LLVMBuildCall2(
            context->builder, LLVMGlobalGetValueType(function), function,
            call_arguments, 2, "string_byte");
    }

    LLVMTypeRef parameter_types[3] = {string_type, index_type, index_type};
    LLVMValueRef function = runtime_string_function(
        context, "__tap_string_slice", string_type, parameter_types, 3);
    LLVMValueRef start = generate_string_integer_argument(
        context, call, arguments, 1);
    LLVMValueRef end = generate_string_integer_argument(
        context, call, arguments->next, 2);
    LLVMValueRef call_arguments[3] = {receiver, start, end};
    return LLVMBuildCall2(
        context->builder, LLVMGlobalGetValueType(function), function,
        call_arguments, 3, "string_slice");
}

// 生成结构体方法调用；语句形式可在返回同类型结构体时自动写回接收者。
static LLVMValueRef generate_method_call(
    CodeGenContext *context, FunctionCallNode *call, FunctionNode *function,
    Symbol *receiver_symbol, int write_back_receiver) {
    unsigned user_count = 0;
    for (ASTNode *argument = call->arguments; argument; argument = argument->next) {
        user_count++;
    }

    unsigned expected_count = function_param_count(function);
    if (user_count + 1 != expected_count) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "method '%s' expects %u arguments, but got %u",
                         function_base_name(function->name),
                         expected_count > 0 ? expected_count - 1 : 0,
                         user_count); // 中文：方法参数数量不匹配
        exit(1);
    }

    const VarTypeNode *receiver_type = receiver_symbol->declared_type;
    const VarTypeNode *first_param = function_param_var_type(function, 0);
    if (!first_param || !var_type_equal(first_param, receiver_type)) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "method '%s' receiver type mismatch",
                         function_base_name(function->name)); // 中文：方法接收者类型不匹配
        exit(1);
    }

    LLVMValueRef llvm_function = LLVMGetNamedFunction(
        context->module, llvm_call_name(function->name));
    if (!llvm_function) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "undefined function '%s'", function->name); // 中文：未定义的函数
        exit(1);
    }

    LLVMValueRef *arguments = expected_count
        ? malloc(sizeof(LLVMValueRef) * expected_count)
        : NULL;
    if (expected_count && !arguments) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }

    arguments[0] = LLVMBuildLoad2(
        context->builder, get_llvm_var_type(context, receiver_type),
        receiver_symbol->value, "method_receiver");

    ASTNode *argument = call->arguments;
    for (unsigned i = 1; i < expected_count; i++, argument = argument->next) {
        const VarTypeNode *param_type = function_param_var_type(function, i);
        if (param_type &&
            (param_type->enum_name || param_type->struct_name || param_type->is_pointer) &&
            !expression_assignable_to(context, argument, param_type)) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "method '%s' argument %u type mismatch",
                             function_base_name(function->name), i); // 中文：方法参数类型不匹配
            free(arguments);
            exit(1);
        }
        arguments[i] = param_type
            ? generate_expression_for_type(context, argument, param_type)
            : generate_expression_as(context, argument, function_param_type(function, i));
    }

    LLVMTypeRef function_type = LLVMGlobalGetValueType(llvm_function);
    LLVMValueRef value = LLVMBuildCall2(context->builder, function_type, llvm_function,
                                        arguments, expected_count, "method_call_result");
    free(arguments);

    const VarTypeNode *return_type = function_return_var_type(function);
    if (write_back_receiver && return_type && var_type_equal(return_type, receiver_type)) {
        if (receiver_symbol->is_const) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "cannot call mutating method '%s' on constant '%s'",
                             function_base_name(function->name),
                             receiver_symbol->name); // 中文：不能对常量调用可变方法
            exit(1);
        }
        LLVMBuildStore(context->builder, value, receiver_symbol->value);
    }

    return value;
}

// 生成函数调用表达式，包括内建字符串方法和参数类型转换。
// 生成载荷枚举的构造值：写入判别标签，再把实参逐个放进该成员对应的载荷字段。
// struct_name 是具体结构体名（泛型实例化后形如 Option$i32），字段类型从它取；
// 判别标签和字段下标来自枚举声明，各次实例化都相同。
// 无载荷成员传入空的 arguments，只写标签。
static LLVMValueRef generate_variant_value(
    CodeGenContext *context, const char *struct_name,
    EnumNode *enum_node, EnumVariantNode *variant,
    ASTNode *arguments, const char *filename, int line, int column) {
    unsigned expected = variant_payload_count(variant);
    unsigned actual = 0;
    for (ASTNode *argument = arguments; argument; argument = argument->next) actual++;
    if (expected != actual) {
        print_diagnostic(stderr, "error", filename, line, column,
                         "variant '%s.%s' expects %u value(s), but got %u",
                         enum_node->name, variant->name, expected, actual); // 中文：成员载荷数量不匹配
        exit(1);
    }

    StructNode *struct_node = find_struct(context, struct_name);
    LLVMTypeRef struct_type = get_llvm_struct_type_by_name(context, struct_name);
    LLVMValueRef value = LLVMGetUndef(struct_type);
    value = LLVMBuildInsertValue(context->builder, value,
        LLVMConstInt(LLVMInt32TypeInContext(context->context), variant->tag, 0),
        0, "variant_tag");

    ASTNode *argument = arguments;
    for (unsigned index = 0; index < expected; index++, argument = argument->next) {
        StructFieldNode *field =
            struct_field_at(struct_node, variant->field_index + index);
        if (!field) {
            fprintf(stderr, "error: malformed payload enum '%s'\n",
                    enum_node->name); // 中文：载荷枚举的内部结构异常
            exit(1);
        }
        LLVMValueRef field_value =
            generate_expression_for_type(context, argument, field->field_type);
        value = LLVMBuildInsertValue(context->builder, value, field_value,
                                     variant->field_index + index, "variant_payload");
    }
    return value;
}

static LLVMValueRef generate_function_call(CodeGenContext *context, FunctionCallNode *call) {
    const char *receiver_name = NULL;
    size_t receiver_length = 0;
    StringMethodKind string_method = internal_string_method(call);
    if (string_method == STRING_METHOD_NONE) {
        string_method = named_string_method(
            call, &receiver_name, &receiver_length);
    }
    FunctionNode *function = find_function(context, call->name);

    if (!function) {
        Symbol *method_receiver = NULL;
        FunctionNode *method =
            resolve_method_call(context, call, &method_receiver, NULL, NULL);
        if (method) {
            return generate_method_call(context, call, method, method_receiver, 0);
        }
    }

    // 已解析到真实函数的 module.method() 优先按模块调用处理。
    if (receiver_name && function) string_method = STRING_METHOD_NONE;
    if (string_method != STRING_METHOD_NONE) {
        return generate_string_method(
            context, call, string_method, receiver_name, receiver_length);
    }

    if (strcmp(call->name, "assert") == 0) {
        print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                         "assert can only be used as a statement"); // 中文：assert 只能作为语句使用
        exit(1);
    }

    const char *callee_name = function ? llvm_call_name(call->name) : call->name;
    LLVMValueRef llvm_function = LLVMGetNamedFunction(context->module, callee_name);
    if (!llvm_function || !function) {
        char *base_name = NULL;
        char *method_name = NULL;
        if (split_field_access_name(call->name, &base_name, &method_name)) {
            Symbol *receiver_symbol = find_symbol(context, base_name);
            if (!receiver_symbol) {
                print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                                 "namespace '%s' was not imported",
                                 base_name); // 中文：未导入名称空间
            } else {
                print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                                 "type has no method '%s'",
                                 method_name); // 中文：类型没有该方法
            }
            free(base_name);
            free(method_name);
            exit(1);
        }
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
        const VarTypeNode *param_type = function_param_var_type(function, i);
        int erased_pointer_argument =
            is_runtime_memory_pointer_argument(call, i) &&
            param_type && param_type->is_pointer;
        if (!erased_pointer_argument && param_type &&
            (param_type->enum_name || param_type->struct_name || param_type->is_pointer) &&
            !expression_assignable_to(context, argument, param_type)) {
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "function '%s' argument %u type mismatch",
                             call->name, i + 1); // 中文：函数参数类型不匹配
            free(arguments);
            exit(1);
        }
        arguments[i] = erased_pointer_argument
            ? generate_expression(context, argument)
            : param_type
            ? generate_expression_for_type(context, argument, param_type)
            : generate_expression_as(context, argument, function_param_type(function, i));
    }

    LLVMTypeRef function_type = LLVMGlobalGetValueType(llvm_function);
    LLVMValueRef value = LLVMBuildCall2(context->builder, function_type, llvm_function,
                                        arguments, count, "call_result");
    free(arguments);
    return value;
}

// 生成普通表达式的 LLVM 值。
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
                uint64_t enum_value = 0;
                if (enum_variant_value(context, identifier->name, &enum_value)) {
                    return LLVMConstInt(get_llvm_type(context, LITERAL_I32), enum_value, 0);
                }
                // 载荷枚举的成员需要目标类型才能确定结果类型，不能单独作为值使用。
                EnumVariantNode *variant = NULL;
                EnumNode *enum_node =
                    variant_reference(context, identifier->name, &variant);
                if (enum_node && enum_node->has_payload) {
                    fprintf(stderr,
                        "error: variant '%s' must be used where an enum type is expected\n",
                        identifier->name); // 中文：载荷枚举成员必须用于期望枚举类型的场合
                    exit(1);
                }
                StructFieldNode *field = NULL;
                LLVMValueRef field_address =
                    generate_field_address(context, identifier->name, &field, NULL);
                if (field_address) {
                    return LLVMBuildLoad2(
                        context->builder, get_llvm_var_type(context, field->field_type),
                        field_address, "loaded_struct_field");
                }
                VarDeclNode *constant = find_global_constant(context, identifier->name);
                LLVMValueRef global = LLVMGetNamedGlobal(context->module, identifier->name);
                if (!constant || !constant->type || !global) {
                    fprintf(stderr, "error: undefined variable '%s'\n", identifier->name); // 中文：未定义的变量
                    exit(1);
                }
                if (constant->type->is_array) {
                    fprintf(stderr, "error: array constant '%s' must be accessed with an index\n", identifier->name); // 中文：数组常量必须通过下标访问
                    exit(1);
                }
                return LLVMBuildLoad2(
                    context->builder, get_llvm_type(context, constant->type->type),
                    global, "loaded_global_const");
            }
            if (symbol->array_type) {
                fprintf(stderr, "error: array '%s' must be accessed with an index\n", identifier->name); // 中文：数组必须通过下标访问
                exit(1);
            }
            LLVMTypeRef llvm_type = symbol->declared_type
                ? get_llvm_var_type(context, symbol->declared_type)
                : get_llvm_type(context, symbol->type);
            return LLVMBuildLoad2(context->builder, llvm_type,
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
        case NODE_REFERENCE:
            return generate_reference(context, (ReferenceNode *)expression);
        case NODE_SIZEOF: {
            SizeofNode *size_expression = (SizeofNode *)expression;
            validate_var_type(context, size_expression->operand_type);
            return LLVMSizeOf(get_llvm_var_type(
                context, size_expression->operand_type));
        }
        case NODE_ARRAY_LITERAL:
            fprintf(stderr, "error: array literals can only be used to initialize array variables\n"); // 中文：数组字面量只能用于数组变量初始化
            exit(1);
        case NODE_STRUCT_LITERAL:
            fprintf(stderr, "error: struct literals must be used with a struct target type\n"); // 中文：结构体字面量必须用于结构体目标类型
            exit(1);
        case NODE_FUNCTION_CALL:
            {
                FunctionCallNode *call = (FunctionCallNode *)expression;
                // 载荷枚举构造需要目标类型才能确定结果类型，与结构体字面量一致。
                EnumVariantNode *variant = NULL;
                EnumNode *enum_node = variant_reference(context, call->name, &variant);
                if (enum_node && enum_node->has_payload) {
                    print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                        "variant construction must be used where an enum type is expected"); // 中文：载荷枚举构造必须用于期望枚举类型的场合
                    exit(1);
                }
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
            if (binary->op_type == OP_AND || binary->op_type == OP_OR) {
                return generate_logical_binary(context, binary);
            }
            enum LiteralType left_type = expression_type(context, binary->left);
            enum LiteralType right_type = expression_type(context, binary->right);
            if (left_type == LITERAL_STRING || right_type == LITERAL_STRING) {
                if (left_type != LITERAL_STRING || right_type != LITERAL_STRING) {
                    fprintf(stderr, "error: string comparison requires two string values\n");
                    exit(1);
                }
                return generate_string_comparison(context, binary);
            }
            if (is_float_type(left_type) || is_float_type(right_type)) {
                return generate_float_binary(
                    context, binary, common_float_type(left_type, right_type));
            }
            enum LiteralType operand_type = common_integer_type(left_type, right_type);
            return generate_integer_binary(context, binary, operand_type);
        }
        default:
            break;
    }

    fprintf(stderr, "error: unsupported expression type\n"); // 中文：不支持的表达式类型
    exit(1);
}

// 把 printf 实参提升到 C 可变参数 ABI 要求的宽度：窄整数提升到 i32，浮点提升到 double。
static LLVMValueRef promote_printf_argument(CodeGenContext *context, ASTNode *expression) {
    enum LiteralType type = expression_type(context, expression);
    LLVMValueRef value = generate_expression(context, expression);
    if (is_float_type(type)) {
        return cast_value(context, value, type, LITERAL_F64);
    }
    if (!is_integer_type(type)) return value;
    if (integer_type_bits(type) < 32) {
        return cast_integer(context, value, type, LITERAL_I32);
    }
    return value;
}

// 生成 print 语句，对接到底层 printf 调用。
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
            arguments[i] = promote_printf_argument(context, argument);
        }
        LLVMBuildCall2(context->builder, context->printf_type, context->printf_func,
                       arguments, count, "printf_result");
        free(arguments);
        return;
    }

    enum LiteralType type = expression_type(context, first);
    const char *format = "%d";
    if (is_float_type(type)) {
        format = "%f";
    } else if (is_integer_type(type) && integer_type_bits(type) > 32) {
        format = is_unsigned_type(type) ? "%llu" : "%lld";
    } else if (is_unsigned_type(type)) {
        format = "%u";
    }

    LLVMValueRef arguments[2] = {
        LLVMBuildGlobalStringPtr(context->builder, format, "format_string"),
        promote_printf_argument(context, first)
    };
    LLVMBuildCall2(context->builder, context->printf_type, context->printf_func,
                   arguments, 2, "printf_result");
}

static void generate_statement_list(CodeGenContext *context, ASTNode *statement);

// 生成变量或结构体字段赋值语句。
static void generate_assignment(CodeGenContext *context, AssignmentNode *assignment) {
    Symbol *symbol = find_symbol(context, assignment->name);
    if (!symbol) {
        StructFieldNode *field = NULL;
        Symbol *base_symbol = NULL;
        LLVMValueRef field_address =
            generate_field_address(context, assignment->name, &field, &base_symbol);
        if (field_address) {
            if (base_symbol->is_const) {
                fprintf(stderr, "error: cannot assign to field of constant '%s'\n",
                        base_symbol->name); // 中文：不能修改常量结构体字段
                exit(1);
            }
            if (!expression_assignable_to(context, assignment->expression, field->field_type)) {
                fprintf(stderr, "error: assignment type mismatch for '%s'\n",
                        assignment->name); // 中文：赋值类型不匹配
                exit(1);
            }
            LLVMValueRef value = generate_expression_for_type(
                context, assignment->expression, field->field_type);
            LLVMBuildStore(context->builder, value, field_address);
            return;
        }
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
    if (symbol->declared_type &&
        !expression_assignable_to(context, assignment->expression, symbol->declared_type)) {
        fprintf(stderr, "error: assignment type mismatch for '%s'\n",
                assignment->name); // 中文：赋值类型不匹配
        exit(1);
    }
    LLVMValueRef value = symbol->declared_type
        ? generate_expression_for_type(context, assignment->expression, symbol->declared_type)
        : generate_expression_as(context, assignment->expression, symbol->type);
    LLVMBuildStore(context->builder, value, symbol->value);
}

// 判断数组元素的标量类型是否允许写入目标元素类型。
static int array_element_type_compatible(
    enum LiteralType expected, enum LiteralType actual) {
    return scalar_types_compatible(expected, actual);
}

// 递归校验嵌套数组字面量，并把每个标量叶子写入数组存储。
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
        if (element_type->is_pointer) {
            if (!expression_assignable_to(context, current_element, element_type)) {
                fprintf(stderr, "error: array element type mismatch\n"); // 中文：数组元素类型不匹配
                exit(1);
            }
            LLVMValueRef value = generate_expression_for_type(
                context, current_element, element_type);
            LLVMBuildStore(context->builder, value, element_address);
            if (!literal->is_repeat) element = element->next;
            continue;
        }
        enum LiteralType actual_type = expression_type(context, current_element);
        if (element_type->enum_name
                ? !expression_assignable_to(context, current_element, element_type)
                : !array_element_type_compatible(element_type->type, actual_type)) {
            fprintf(stderr, "error: array element type mismatch\n"); // 中文：数组元素类型不匹配
            exit(1);
        }
        LLVMValueRef value = generate_expression_as(
            context, current_element, element_type->type);
        LLVMBuildStore(context->builder, value, element_address);
        if (!literal->is_repeat) element = element->next;
    }
}

// 按期望数组类型生成数组值。
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


// 找到下标赋值目标最外层数组变量名。
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

// 生成数组元素赋值语句。
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
    if (element_type->is_pointer) {
        if (!expression_assignable_to(context, assignment->expression, element_type)) {
            fprintf(stderr, "error: pointer element assignment type mismatch\n"); // 中文：指针元素赋值类型不匹配
            exit(1);
        }
        LLVMValueRef value = generate_expression_for_type(
            context, assignment->expression, element_type);
        LLVMBuildStore(context->builder, value, address);
        return;
    }
    enum LiteralType actual_type = expression_type(context, assignment->expression);
    if (element_type->enum_name
            ? !expression_assignable_to(context, assignment->expression, element_type)
            : !array_element_type_compatible(element_type->type, actual_type)) {
        fprintf(stderr, "error: array element assignment type mismatch\n"); // 中文：数组元素赋值类型不匹配
        exit(1);
    }
    LLVMValueRef value = generate_expression_as(
        context, assignment->expression, element_type->type);
    LLVMBuildStore(context->builder, value, address);
}

// 将 if/for 条件表达式规范化为 LLVM i1 值。
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

// 生成 assert 语句的运行时检查和失败输出。
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

// 生成 if/elseif/else 条件控制流。
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

// 生成 for/while 循环控制流，包括 break/continue 目标块。
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

// 取出表达式的完整声明类型；标量表达式返回 NULL。
static const VarTypeNode *expression_var_type(
    CodeGenContext *context, ASTNode *expression) {
    if (!expression) return NULL;
    if (expression->type == NODE_IDENTIFIER) {
        Symbol *symbol =
            find_symbol(context, ((IdentifierNode *)expression)->name);
        return symbol ? symbol->declared_type : NULL;
    }
    if (expression->type == NODE_FUNCTION_CALL) {
        FunctionCallNode *call = (FunctionCallNode *)expression;
        FunctionNode *function = find_function(context, call->name);
        if (!function) {
            function = resolve_method_call(context, call, NULL, NULL, NULL);
        }
        return function_return_var_type(function);
    }
    if (expression->type == NODE_INDEX_EXPRESSION) {
        return indexed_value_type(context, expression);
    }
    return NULL;
}

// 在枚举声明中按名字查找成员。
static EnumVariantNode *find_enum_variant(EnumNode *enum_node, const char *name) {
    for (ASTNode *node = enum_node->variants; node; node = node->next) {
        EnumVariantNode *variant = (EnumVariantNode *)node;
        if (strcmp(variant->name, name) == 0) return variant;
    }
    return NULL;
}

// 取出 match 被匹配值所属的载荷枚举；不满足条件时给出诊断并退出。
static EnumNode *matched_payload_enum(
    CodeGenContext *context, MatchStatementNode *match_node,
    const VarTypeNode **matched_type_out) {
    const VarTypeNode *matched =
        expression_var_type(context, match_node->expression);
    EnumNode *enum_node =
        matched ? struct_tagged_enum(context, matched->struct_name) : NULL;
    if (!enum_node || !enum_node->has_payload) {
        fprintf(stderr,
            "error: match requires a value of a payload enum type\n"); // 中文：match 需要载荷枚举类型的值
        exit(1);
    }
    if (matched_type_out) *matched_type_out = matched;
    return enum_node;
}

// 校验 match：分支必须覆盖全部成员或包含通配分支，绑定数量必须与载荷数量一致。
static void validate_match_statement(
    CodeGenContext *context, MatchStatementNode *match_node) {
    EnumNode *enum_node = matched_payload_enum(context, match_node, NULL);

    int has_wildcard = 0;
    for (ASTNode *node = match_node->arms; node; node = node->next) {
        MatchArmNode *arm = (MatchArmNode *)node;
        if (!arm->variant_name) {
            if (arm->bindings) {
                print_diagnostic(stderr, "error", arm->filename, arm->line, arm->column,
                    "wildcard arm cannot bind payload values"); // 中文：通配分支不能绑定载荷
                exit(1);
            }
            has_wildcard = 1;
            continue;
        }
        if (strcmp(arm->enum_name, enum_node->name) != 0) {
            print_diagnostic(stderr, "error", arm->filename, arm->line, arm->column,
                "arm pattern '%s.%s' does not belong to enum '%s'",
                arm->enum_name, arm->variant_name, enum_node->name); // 中文：分支模式不属于该枚举
            exit(1);
        }
        EnumVariantNode *variant = find_enum_variant(enum_node, arm->variant_name);
        if (!variant) {
            print_diagnostic(stderr, "error", arm->filename, arm->line, arm->column,
                "enum '%s' has no variant '%s'",
                enum_node->name, arm->variant_name); // 中文：枚举没有该成员
            exit(1);
        }
        unsigned bindings = 0;
        for (ASTNode *binding = arm->bindings; binding; binding = binding->next) {
            bindings++;
        }
        unsigned payload = variant_payload_count(variant);
        if (bindings != payload) {
            print_diagnostic(stderr, "error", arm->filename, arm->line, arm->column,
                "variant '%s.%s' has %u payload value(s), but the arm binds %u",
                enum_node->name, variant->name, payload, bindings); // 中文：绑定数量与载荷数量不一致
            exit(1);
        }
    }

    // 逐个成员统计覆盖次数：重复分支和遗漏分支都在这里报出来。
    for (ASTNode *node = enum_node->variants; node; node = node->next) {
        EnumVariantNode *variant = (EnumVariantNode *)node;
        unsigned matches = 0;
        MatchArmNode *first_arm = NULL;
        for (ASTNode *arm_node = match_node->arms;
             arm_node; arm_node = arm_node->next) {
            MatchArmNode *arm = (MatchArmNode *)arm_node;
            if (arm->variant_name && strcmp(arm->variant_name, variant->name) == 0) {
                if (matches == 0) first_arm = arm;
                matches++;
            }
        }
        if (matches > 1) {
            print_diagnostic(stderr, "error", first_arm->filename, first_arm->line,
                first_arm->column, "variant '%s.%s' is matched more than once",
                enum_node->name, variant->name); // 中文：成员被重复匹配
            exit(1);
        }
        if (matches == 0 && !has_wildcard) {
            fprintf(stderr,
                "error: match is not exhaustive: variant '%s.%s' is not covered\n",
                enum_node->name, variant->name); // 中文：match 未穷尽
            exit(1);
        }
    }
}

// 生成 match 语句：按判别标签分派到各分支，并把该分支的载荷绑定到局部变量。
static void generate_match_statement(
    CodeGenContext *context, MatchStatementNode *match_node) {
    validate_match_statement(context, match_node);

    const VarTypeNode *matched = NULL;
    EnumNode *enum_node = matched_payload_enum(context, match_node, &matched);
    StructNode *struct_node = find_struct(context, matched->struct_name);
    LLVMTypeRef struct_type = get_llvm_struct_type_by_name(context, matched->struct_name);
    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);

    // 被匹配值是变量时直接复用它的存储槽，否则求值后放进临时槽。
    LLVMValueRef storage = NULL;
    if (match_node->expression->type == NODE_IDENTIFIER) {
        Symbol *symbol = find_symbol(
            context, ((IdentifierNode *)match_node->expression)->name);
        if (symbol && symbol->declared_type && symbol->declared_type->struct_name &&
            strcmp(symbol->declared_type->struct_name, matched->struct_name) == 0) {
            storage = symbol->value;
        }
    }
    if (!storage) {
        LLVMValueRef value =
            generate_expression_for_type(context, match_node->expression, matched);
        storage = create_entry_alloca(context, struct_type, "match_value");
        LLVMBuildStore(context->builder, value, storage);
    }

    LLVMValueRef tag_pointer = LLVMBuildStructGEP2(
        context->builder, struct_type, storage, 0, "match_tag_pointer");
    LLVMValueRef tag =
        LLVMBuildLoad2(context->builder, int32_type, tag_pointer, "match_tag");

    LLVMValueRef function =
        LLVMGetBasicBlockParent(LLVMGetInsertBlock(context->builder));
    LLVMBasicBlockRef end_block =
        LLVMAppendBasicBlockInContext(context->context, function, "match_end");

    unsigned arm_count = 0;
    for (ASTNode *node = match_node->arms; node; node = node->next) arm_count++;
    LLVMBasicBlockRef *arm_blocks = arm_count
        ? malloc(sizeof(LLVMBasicBlockRef) * arm_count) : NULL;
    if (arm_count && !arm_blocks) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    unsigned index = 0;
    for (ASTNode *node = match_node->arms; node; node = node->next, index++) {
        arm_blocks[index] =
            LLVMAppendBasicBlockInContext(context->context, function, "match_arm");
    }

    // 通配分支承担 default；没有通配时由穷尽性检查保证 default 不可达。
    LLVMBasicBlockRef default_block = end_block;
    index = 0;
    for (ASTNode *node = match_node->arms; node; node = node->next, index++) {
        if (!((MatchArmNode *)node)->variant_name) default_block = arm_blocks[index];
    }

    LLVMValueRef dispatch =
        LLVMBuildSwitch(context->builder, tag, default_block, arm_count);
    index = 0;
    for (ASTNode *node = match_node->arms; node; node = node->next, index++) {
        MatchArmNode *arm = (MatchArmNode *)node;
        if (!arm->variant_name) continue;
        EnumVariantNode *variant = find_enum_variant(enum_node, arm->variant_name);
        LLVMAddCase(dispatch, LLVMConstInt(int32_type, variant->tag, 0),
                    arm_blocks[index]);
    }

    index = 0;
    for (ASTNode *node = match_node->arms; node; node = node->next, index++) {
        MatchArmNode *arm = (MatchArmNode *)node;
        LLVMPositionBuilderAtEnd(context->builder, arm_blocks[index]);

        if (arm->variant_name) {
            EnumVariantNode *variant = find_enum_variant(enum_node, arm->variant_name);
            unsigned offset = 0;
            for (ASTNode *binding = arm->bindings;
                 binding; binding = binding->next, offset++) {
                unsigned field_index = variant->field_index + offset;
                StructFieldNode *field = struct_field_at(struct_node, field_index);
                LLVMTypeRef field_type = get_llvm_var_type(context, field->field_type);
                LLVMValueRef pointer = LLVMBuildStructGEP2(context->builder,
                    struct_type, storage, field_index, "payload_pointer");
                const char *name = ((IdentifierNode *)binding)->name;
                LLVMValueRef slot = create_entry_alloca(context, field_type, name);
                LLVMBuildStore(context->builder,
                    LLVMBuildLoad2(context->builder, field_type, pointer, "payload"),
                    slot);
                insert_typed_symbol(context, name, slot, field->field_type, 0);
            }
        }

        generate_statement_list(context, arm->body);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(context->builder))) {
            LLVMBuildBr(context->builder, end_block);
        }
    }

    free(arm_blocks);
    LLVMPositionBuilderAtEnd(context->builder, end_block);
}

// 逐条生成语句列表，遇到已终结的基本块时停止。
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
                if (!declaration->type && declaration->expression &&
                    declaration->expression->type == NODE_REFERENCE) {
                    declaration->type = infer_reference_type(
                        context, (ReferenceNode *)declaration->expression);
                }
                validate_var_type(context, declaration->type);
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
                        // 初始化过程与声明类型保持相同递归形状。
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
                if (declaration->type &&
                    (declaration->type->struct_name || declaration->type->is_pointer)) {
                    LLVMTypeRef llvm_type = get_llvm_var_type(context, declaration->type);
                    LLVMValueRef storage = create_entry_alloca(
                        context, llvm_type, declaration->name);
                    insert_typed_symbol(context, declaration->name, storage,
                                        declaration->type, declaration->is_const);
                    if (declaration->expression) {
                        LLVMValueRef value = generate_expression_for_type(
                            context, declaration->expression, declaration->type);
                        LLVMBuildStore(context->builder, value, storage);
                    }
                    break;
                }
                enum LiteralType type = declaration->type
                    ? declaration->type->type
                    : expression_type(context, declaration->expression);
                LLVMTypeRef llvm_type = get_llvm_type(context, type);
                LLVMValueRef storage = create_entry_alloca(
                    context, llvm_type, declaration->name);
                if (declaration->type) {
                    insert_typed_symbol(context, declaration->name, storage,
                                        declaration->type, declaration->is_const);
                } else {
                    insert_symbol(context, declaration->name, storage, type,
                                  declaration->is_const);
                }
                if (declaration->expression) {
                    if (declaration->type &&
                        !expression_assignable_to(context, declaration->expression,
                                                  declaration->type)) {
                        fprintf(stderr, "error: initializer type mismatch for '%s'\n",
                                declaration->name); // 中文：初始化表达式类型不匹配
                        exit(1);
                    }
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
                if (context->current_return_var_type &&
                    (context->current_return_var_type->enum_name ||
                     context->current_return_var_type->struct_name ||
                     context->current_return_var_type->is_pointer) &&
                    !expression_assignable_to(context, return_node->expression,
                                              context->current_return_var_type)) {
                    fprintf(stderr, "error: return type mismatch\n"); // 中文：返回值类型不匹配
                    exit(1);
                }
                LLVMValueRef value = context->current_return_var_type
                    ? generate_expression_for_type(
                        context, return_node->expression, context->current_return_var_type)
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
                    Symbol *method_receiver = NULL;
                    FunctionNode *method =
                        resolve_method_call(context, call, &method_receiver, NULL, NULL);
                    if (method) {
                        generate_method_call(context, call, method, method_receiver, 1);
                    } else {
                        generate_function_call(context, call);
                    }
                }
                break;
            }
            case NODE_IF_STATEMENT:
                generate_if_statement(context, (IfStatementNode *)statement);
                break;
            case NODE_FOR_STATEMENT:
                generate_for_statement(context, (ForStatementNode *)statement);
                break;
            case NODE_MATCH_STATEMENT:
                generate_match_statement(context, (MatchStatementNode *)statement);
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

// 统计函数参数数量。
static unsigned function_param_count(FunctionNode *function) {
    unsigned count = 0;
    for (ASTNode *param = function->params; param; param = param->next) count++;
    return count;
}

// 根据函数签名创建 LLVM 函数类型。
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
        const VarTypeNode *param_type = function_param_var_type(function, i);
        params[i] = param_type
            ? get_llvm_var_type(context, param_type)
            : get_llvm_type(context, function_param_type(function, i));
    }
    LLVMTypeRef return_type = function->return_type &&
                              (function->return_type->is_array ||
                               function->return_type->struct_name ||
                               function->return_type->is_pointer)
        ? get_llvm_var_type(context, function->return_type)
        : get_llvm_type(context, function_return_type(function));
    LLVMTypeRef type = LLVMFunctionType(return_type, params, count, 0);
    free(params);
    return type;
}

// 生成指定标量类型的零值。
static LLVMValueRef zero_value(CodeGenContext *context, enum LiteralType type) {
    LLVMTypeRef llvm_type = get_llvm_type(context, type);
    if (is_integer_type(type)) return LLVMConstInt(llvm_type, 0, 0);
    if (type == LITERAL_FLOAT || type == LITERAL_F32 || type == LITERAL_F64) {
        return LLVMConstReal(llvm_type, 0.0);
    }
    return LLVMConstNull(llvm_type);
}

// 生成完整类型的零值，支持数组和结构体。
static LLVMValueRef zero_var_value(
    CodeGenContext *context, const VarTypeNode *type, enum LiteralType scalar_type) {
    if (type && (type->is_array || type->struct_name)) {
        return LLVMConstNull(get_llvm_var_type(context, type));
    }
    return zero_value(context, type ? type->type : scalar_type);
}

// 删除无法从外部可见入口触达的内部函数。
static void eliminate_unreachable_functions(CodeGenContext *context) {
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMPassBuilderOptionsSetVerifyEach(options, 1);

    // 先删除不可达定义，再丢弃未使用的 extern 声明。
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

// 创建并初始化代码生成上下文。
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

// 在程序中查找用户声明的 main 函数。
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

// 将顶层常量的字面量初始化表达式转换为 LLVM 常量初始值。
static LLVMValueRef constant_initializer(
    CodeGenContext *context, VarDeclNode *constant) {
    if (!constant->type || constant->type->is_array) {
        fprintf(stderr, "error: global constants must have a scalar type\n"); // 中文：全局常量必须是标量类型
        exit(1);
    }
    if (!constant->expression || constant->expression->type != NODE_LITERAL) {
        fprintf(stderr, "error: global constant '%s' must be initialized with a literal\n",
                constant->name); // 中文：全局常量必须使用字面量初始化
        exit(1);
    }

    LiteralNode *literal = (LiteralNode *)constant->expression;
    LLVMTypeRef target_type = get_llvm_type(context, constant->type->type);
    if (is_integer_type(constant->type->type) && is_integer_type(literal->literal_type)) {
        if (literal->integer_text) {
            return LLVMConstIntOfStringAndSize(
                target_type, literal->integer_text,
                (unsigned)strlen(literal->integer_text), 10);
        }
        return LLVMConstInt(target_type, literal_integer_value(literal), 0);
    }
    if (is_float_type(constant->type->type) && is_float_type(literal->literal_type)) {
        return LLVMConstReal(target_type, literal->value.float_value);
    }
    fprintf(stderr, "error: global constant '%s' initializer type mismatch\n",
            constant->name); // 中文：全局常量初始化类型不匹配
    exit(1);
}

// 为所有顶层常量生成 LLVM 全局常量，并设置为内部链接。
static void generate_global_constants(CodeGenContext *context, ProgramNode *program) {
    for (ASTNode *node = program->constants; node; node = node->next) {
        VarDeclNode *constant = (VarDeclNode *)node;
        LLVMValueRef global = LLVMAddGlobal(
            context->module, get_llvm_type(context, constant->type->type),
            constant->name);
        LLVMSetInitializer(global, constant_initializer(context, constant));
        LLVMSetGlobalConstant(global, 1);
        LLVMSetLinkage(global, LLVMInternalLinkage);
    }
}

// 校验命名类型引用；解析阶段先把标识符类型暂存为 enum_name。
// 带载荷的枚举在降级后就是同名结构体，因此这里要把它改写成 struct_name。
static void validate_var_type(CodeGenContext *context, VarTypeNode *type) {
    if (!type) return;
    if (type->enum_name) {
        EnumNode *enum_node = find_enum(context, type->enum_name);
        if (!enum_node || enum_node->has_payload) {
            StructNode *struct_node = find_struct(context, type->enum_name);
            if (struct_node) {
                type->struct_name = type->enum_name;
                type->enum_name = NULL;
            }
        }
    }
    if (type->enum_name && !find_enum(context, type->enum_name)) {
        fprintf(stderr, "error: undefined enum type '%s'\n",
                type->enum_name); // 中文：未定义的枚举类型
        exit(1);
    }
    if (type->struct_name && !find_struct(context, type->struct_name)) {
        fprintf(stderr, "error: undefined struct type '%s'\n",
                type->struct_name); // 中文：未定义的结构体类型
        exit(1);
    }
    if (type->is_array) validate_var_type(context, type->element_type);
    if (type->is_pointer) validate_var_type(context, type->element_type);
}

// 递归校验语句中的枚举类型引用，覆盖嵌套 if/for/while 代码块。
static void validate_statement_types(CodeGenContext *context, ASTNode *statement) {
    for (; statement; statement = statement->next) {
        switch (statement->type) {
            case NODE_VAR_DECL:
                validate_var_type(context, ((VarDeclNode *)statement)->type);
                break;
            case NODE_IF_STATEMENT: {
                IfStatementNode *if_node = (IfStatementNode *)statement;
                validate_statement_types(context, if_node->consequence);
                validate_statement_types(context, if_node->alternative);
                break;
            }
            case NODE_FOR_STATEMENT: {
                ForStatementNode *for_node = (ForStatementNode *)statement;
                validate_statement_types(context, for_node->initializer);
                validate_statement_types(context, for_node->update);
                validate_statement_types(context, for_node->body);
                break;
            }
            case NODE_MATCH_STATEMENT: {
                // match 本身的校验需要局部符号表，放在生成阶段做（见 generate_match_statement）。
                MatchStatementNode *match_node = (MatchStatementNode *)statement;
                for (ASTNode *arm = match_node->arms; arm; arm = arm->next) {
                    validate_statement_types(context, ((MatchArmNode *)arm)->body);
                }
                break;
            }
            default:
                break;
        }
    }
}

// 逐个检查结构体声明、枚举声明和函数/变量中引用的命名类型。
static void validate_user_types(CodeGenContext *context, ProgramNode *program) {
    for (ASTNode *node = program->enums; node; node = node->next) {
        EnumNode *enum_node = (EnumNode *)node;
        for (ASTNode *other = node->next; other; other = other->next) {
            EnumNode *other_enum = (EnumNode *)other;
            if (strcmp(enum_node->name, other_enum->name) == 0) {
                fprintf(stderr, "error: duplicate enum '%s'\n",
                        enum_node->name); // 中文：重复的枚举声明
                exit(1);
            }
        }
        for (ASTNode *variant = enum_node->variants; variant; variant = variant->next) {
            EnumVariantNode *variant_node = (EnumVariantNode *)variant;
            for (ASTNode *other = variant->next; other; other = other->next) {
                if (strcmp(variant_node->name, ((EnumVariantNode *)other)->name) == 0) {
                    fprintf(stderr, "error: duplicate enum variant '%s.%s'\n",
                            enum_node->name, variant_node->name); // 中文：重复的枚举成员
                    exit(1);
                }
            }
        }
    }

    for (ASTNode *node = program->structs; node; node = node->next) {
        StructNode *struct_node = (StructNode *)node;
        for (ASTNode *other = node->next; other; other = other->next) {
            StructNode *other_struct = (StructNode *)other;
            if (strcmp(struct_node->name, other_struct->name) == 0) {
                fprintf(stderr, "error: duplicate struct '%s'\n",
                        struct_node->name); // 中文：重复的结构体声明
                exit(1);
            }
        }
        if (!struct_node->is_tagged_enum && find_enum(context, struct_node->name)) {
            fprintf(stderr, "error: type '%s' is already declared as enum\n",
                    struct_node->name); // 中文：类型名已声明为枚举
            exit(1);
        }
        for (ASTNode *field = struct_node->fields; field; field = field->next) {
            StructFieldNode *struct_field = (StructFieldNode *)field;
            validate_var_type(context, struct_field->field_type);
            for (ASTNode *other = field->next; other; other = other->next) {
                if (strcmp(struct_field->name, ((StructFieldNode *)other)->name) == 0) {
                    fprintf(stderr, "error: duplicate struct field '%s.%s'\n",
                            struct_node->name, struct_field->name); // 中文：重复的结构体字段
                    exit(1);
                }
            }
        }
    }

    for (ASTNode *constant = program->constants; constant; constant = constant->next) {
        validate_var_type(context, ((VarDeclNode *)constant)->type);
    }
    for (ASTNode *node = program->functions; node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        validate_var_type(context, function->return_type);
        for (ASTNode *type = function->param_types; type; type = type->next) {
            validate_var_type(context, (VarTypeNode *)type);
        }
        validate_statement_types(context, function->body);
    }
}

// 提前声明所有 LLVM 结构体类型，再填充字段类型，支持字段引用后声明的结构体。
static void declare_struct_types(CodeGenContext *context, ProgramNode *program) {
    for (ASTNode *node = program->structs; node; node = node->next) {
        StructNode *struct_node = (StructNode *)node;
        if (!LLVMGetTypeByName2(context->context, struct_node->name)) {
            LLVMStructCreateNamed(context->context, struct_node->name);
        }
    }

    for (ASTNode *node = program->structs; node; node = node->next) {
        StructNode *struct_node = (StructNode *)node;
        unsigned count = 0;
        for (ASTNode *field = struct_node->fields; field; field = field->next) count++;
        LLVMTypeRef *field_types = count ? malloc(sizeof(LLVMTypeRef) * count) : NULL;
        if (count && !field_types) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        unsigned index = 0;
        for (ASTNode *field = struct_node->fields; field; field = field->next, index++) {
            field_types[index] = get_llvm_var_type(
                context, ((StructFieldNode *)field)->field_type);
        }
        LLVMStructSetBody(
            get_llvm_struct_type_by_name(context, struct_node->name),
            field_types, count, 0);
        free(field_types);
    }
}

// 生成包装入口 main，初始化运行时参数后调用用户 main。
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
    if (user_main->return_type && user_main->return_type->struct_name) {
        fprintf(stderr, "error: main function cannot return a struct\n"); // 中文：main 不能返回结构体
        exit(1);
    }

    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
    LLVMTypeRef char_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(context->context), 0);
    LLVMTypeRef argv_type = LLVMPointerType(char_ptr_type, 0);
    LLVMTypeRef init_args_params[2] = {int32_type, argv_type};
    LLVMTypeRef init_args_type = LLVMFunctionType(
        LLVMVoidTypeInContext(context->context), init_args_params, 2, 0);
    LLVMValueRef init_args = LLVMGetNamedFunction(context->module, "__tap_init_args");
    if (!init_args) {
        init_args = LLVMAddFunction(context->module, "__tap_init_args", init_args_type);
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

    // // 把控制台切到 UTF-8 代码页，否则 Windows 下程序输出的中文会乱码。
    // LLVMTypeRef init_console_type = LLVMFunctionType(
    //     LLVMVoidTypeInContext(context->context), NULL, 0, 0);
    // LLVMValueRef init_console = LLVMGetNamedFunction(
    //     context->module, "__tap_init_console");
    // if (!init_console) {
    //     init_console = LLVMAddFunction(
    //         context->module, "__tap_init_console", init_console_type);
    // }
    // LLVMBuildCall2(context->builder, init_console_type, init_console,
    //                NULL, 0, "");

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

// 生成整个程序的 LLVM IR。
void generate_code(CodeGenContext *context, ProgramNode *program) {
    context->program = program;
    FunctionNode *user_main = find_user_main(program);
    validate_user_types(context, program);
    declare_struct_types(context, program);

    LLVMTypeRef int32_type = LLVMInt32TypeInContext(context->context);
    LLVMTypeRef char_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(context->context), 0);
    context->printf_type = LLVMFunctionType(int32_type, &char_ptr_type, 1, 1);
    context->printf_func = LLVMAddFunction(context->module, "printf", context->printf_type);
    context->exit_type = LLVMFunctionType(LLVMVoidTypeInContext(context->context), &int32_type, 1, 0);
    context->exit_func = LLVMAddFunction(context->module, "exit", context->exit_type);

    generate_global_constants(context, program);

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        const char *name = llvm_function_name(function);
        LLVMValueRef llvm_function = LLVMAddFunction(
            context->module, name, create_function_type(context, function));
        // 可执行产物只暴露 main；GlobalDCE 可能移除所有不可达 helper。
        if (!function->is_extern) {
            LLVMSetLinkage(llvm_function, LLVMInternalLinkage);
        }
    }

    generate_entry_point(context, user_main);

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        // 运行时函数已有原生实现，不需要生成 LLVM 函数体。
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
                var_type
                    ? get_llvm_var_type(context, var_type)
                    : get_llvm_type(context, type),
                identifier->name);
            LLVMValueRef value = LLVMGetParam(llvm_function, param_index);
            LLVMSetValueName2(value, identifier->name, strlen(identifier->name));
            LLVMBuildStore(context->builder, value, storage);
            if (var_type) {
                insert_typed_symbol(context, identifier->name, storage, var_type, 0);
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

    // 完整生成和校验之后再运行 DCE，确保不可达代码中的错误仍能报告。
    eliminate_unreachable_functions(context);
}

// 懒初始化 JIT 执行引擎。
static int initialize_execution_engine(CodeGenContext *context) {
    char *error = NULL;
    if (LLVMCreateExecutionEngineForModule(&context->engine, context->module, &error) != 0) {
        fprintf(stderr, "failed to create execution engine: %s\n", error); // 中文：创建执行引擎失败
        LLVMDisposeMessage(error);
        return -1;
    }
    return 0;
}

// 释放代码生成上下文及其 LLVM 资源。
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

// 使用 JIT 执行指定函数并返回整数结果。
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

// 将当前 LLVM 模块写出为 IR 文本文件。
int write_ir_to_file(CodeGenContext *context, const char *filename) {
    char *error = NULL;
    if (LLVMPrintModuleToFile(context->module, filename, &error) != 0) {
        fprintf(stderr, "failed to write IR file: %s\n", error); // 中文：写入 IR 文件失败
        LLVMDisposeMessage(error);
        return -1;
    }
    return 0;
}

// 按指定目标三元组输出目标文件。
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

// 使用当前宿主目标输出目标文件。
int write_object_to_file(CodeGenContext *context, const char *filename) {
    return write_object_for_triple(context, filename, NULL, "object");
}

// 使用 wasm32 目标输出 WebAssembly 对象文件。
int write_wasm_to_file(CodeGenContext *context, const char *filename) {
    return write_object_for_triple(
        context, filename, "wasm32-unknown-unknown", "WebAssembly");
}
