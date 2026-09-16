#include "generics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "helpers.h"

typedef struct TypeBinding {
    char *name;
    VarTypeNode *type;
    struct TypeBinding *next;
} TypeBinding;

typedef struct TypeSymbol {
    char *name;
    VarTypeNode *type;
    struct TypeSymbol *next;
} TypeSymbol;

typedef struct {
    ProgramNode *program;
    TypeSymbol *symbols;
} GenericContext;

// 深拷贝一个完整类型节点。
static VarTypeNode *clone_type(const VarTypeNode *source) {
    if (!source) return NULL;
    if (source->is_pointer) {
        return create_pointer_type(clone_type(source->element_type));
    }
    if (source->is_array) {
        return create_array_type(
            clone_type(source->element_type), source->array_length);
    }
    if (source->struct_name) return create_struct_type(source->struct_name);
    if (source->enum_name) return create_enum_type(source->enum_name);
    return create_var_type(source->type);
}

// 比较两个尚未降低到 LLVM 的完整类型。
static int type_equal(const VarTypeNode *left, const VarTypeNode *right) {
    if (!left || !right) return left == right;
    if (left->is_pointer != right->is_pointer ||
        left->is_array != right->is_array) {
        return 0;
    }
    if (left->is_pointer) {
        return type_equal(left->element_type, right->element_type);
    }
    if (left->is_array) {
        return left->array_length == right->array_length &&
               type_equal(left->element_type, right->element_type);
    }
    const char *left_name = left->struct_name ? left->struct_name : left->enum_name;
    const char *right_name = right->struct_name ? right->struct_name : right->enum_name;
    if (left_name || right_name) {
        return left_name && right_name && strcmp(left_name, right_name) == 0;
    }
    return left->type == right->type;
}

// 返回链表中的节点数量。
static unsigned node_count(ASTNode *node) {
    unsigned count = 0;
    for (; node; node = node->next) count++;
    return count;
}

// 在程序中按内部名称查找函数。
static FunctionNode *find_function(ProgramNode *program, const char *name) {
    for (ASTNode *node = program ? program->functions : NULL;
         node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        if (strcmp(function->name, name) == 0) return function;
    }
    return NULL;
}

// 在程序中查找具名结构体。
static StructNode *find_struct(ProgramNode *program, const char *name) {
    for (ASTNode *node = program ? program->structs : NULL;
         node; node = node->next) {
        StructNode *struct_node = (StructNode *)node;
        if (strcmp(struct_node->name, name) == 0) return struct_node;
    }
    return NULL;
}

// 在程序中查找具名枚举。
static EnumNode *find_enum(ProgramNode *program, const char *name) {
    for (ASTNode *node = program ? program->enums : NULL;
         node; node = node->next) {
        EnumNode *enum_node = (EnumNode *)node;
        if (strcmp(enum_node->name, name) == 0) return enum_node;
    }
    return NULL;
}

// 在结构体中查找字段声明。
static StructFieldNode *find_field(StructNode *struct_node, const char *name) {
    for (ASTNode *node = struct_node ? struct_node->fields : NULL;
         node; node = node->next) {
        StructFieldNode *field = (StructFieldNode *)node;
        if (strcmp(field->name, name) == 0) return field;
    }
    return NULL;
}

// 判断名称是否是当前泛型函数的类型参数。
static int is_type_parameter(FunctionNode *function, const char *name) {
    for (ASTNode *node = function ? function->type_params : NULL;
         node; node = node->next) {
        if (strcmp(((IdentifierNode *)node)->name, name) == 0) return 1;
    }
    return 0;
}

// 判断类型节点是否直接表示泛型类型参数。
static const char *type_parameter_name(
    FunctionNode *function, const VarTypeNode *type) {
    if (!type || type->is_pointer || type->is_array || type->struct_name ||
        !type->enum_name) {
        return NULL;
    }
    return is_type_parameter(function, type->enum_name) ? type->enum_name : NULL;
}

// 在泛型类型绑定中按名称查找具体类型。
static TypeBinding *find_binding(TypeBinding *bindings, const char *name) {
    for (; bindings; bindings = bindings->next) {
        if (strcmp(bindings->name, name) == 0) return bindings;
    }
    return NULL;
}

// 判断两个具体标量是否可沿用现有 Codegen 的数值转换规则。
static int scalar_types_compatible(const VarTypeNode *left,
                                   const VarTypeNode *right) {
    if (!left || !right || left->is_pointer || right->is_pointer ||
        left->is_array || right->is_array || left->enum_name || right->enum_name ||
        left->struct_name || right->struct_name) {
        return 0;
    }
    int left_integer = left->type <= LITERAL_U128 || left->type == LITERAL_BOOL;
    int right_integer = right->type <= LITERAL_U128 || right->type == LITERAL_BOOL;
    int left_float = left->type >= LITERAL_FLOAT && left->type <= LITERAL_F64;
    int right_float = right->type >= LITERAL_FLOAT && right->type <= LITERAL_F64;
    return (left_integer && right_integer) || (left_float && right_float);
}

// 添加泛型类型绑定，并拒绝同一参数推断为不兼容类型。
static int bind_type(TypeBinding **bindings, const char *name,
                     const VarTypeNode *type) {
    TypeBinding *binding = find_binding(*bindings, name);
    if (binding) {
        return type_equal(binding->type, type) ||
               scalar_types_compatible(binding->type, type);
    }

    binding = (TypeBinding *)calloc(1, sizeof(TypeBinding));
    if (!binding) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    binding->name = strdup(name);
    binding->type = clone_type(type);
    binding->next = *bindings;
    *bindings = binding;
    return 1;
}

// 释放泛型类型绑定链表。
static void free_bindings(TypeBinding *bindings) {
    while (bindings) {
        TypeBinding *next = bindings->next;
        free(bindings->name);
        free_ast((ASTNode *)bindings->type);
        free(bindings);
        bindings = next;
    }
}

// 将形参类型与实际类型递归统一并收集泛型绑定。
static int unify_type(FunctionNode *function, const VarTypeNode *pattern,
                      const VarTypeNode *actual, TypeBinding **bindings,
                      int only_unbound) {
    if (!pattern || !actual) return 0;
    const char *parameter = type_parameter_name(function, pattern);
    if (parameter) {
        TypeBinding *existing = find_binding(*bindings, parameter);
        if (only_unbound && existing) return 1;
        return bind_type(bindings, parameter, actual);
    }
    if (pattern->is_pointer != actual->is_pointer ||
        pattern->is_array != actual->is_array) {
        return 0;
    }
    if (pattern->is_pointer) {
        return unify_type(function, pattern->element_type, actual->element_type,
                          bindings, only_unbound);
    }
    if (pattern->is_array) {
        return pattern->array_length == actual->array_length &&
               unify_type(function, pattern->element_type, actual->element_type,
                          bindings, only_unbound);
    }
    // 不含类型参数的具体类型仍交给现有类型检查器处理转换和诊断。
    return 1;
}

// 使用泛型绑定复制并替换类型节点中的类型参数。
static VarTypeNode *substitute_type(FunctionNode *function,
                                    const VarTypeNode *source,
                                    TypeBinding *bindings) {
    if (!source) return NULL;
    const char *parameter = type_parameter_name(function, source);
    if (parameter) {
        TypeBinding *binding = find_binding(bindings, parameter);
        return binding ? clone_type(binding->type) : clone_type(source);
    }
    if (source->is_pointer) {
        return create_pointer_type(
            substitute_type(function, source->element_type, bindings));
    }
    if (source->is_array) {
        return create_array_type(
            substitute_type(function, source->element_type, bindings),
            source->array_length);
    }
    return clone_type(source);
}

// 声明泛型 AST 克隆入口，供表达式链表和语句克隆相互调用。
static ASTNode *clone_expression(ASTNode *source, FunctionNode *template,
                                 TypeBinding *bindings);
static ASTNode *clone_statement_list(ASTNode *source, FunctionNode *template,
                                     TypeBinding *bindings);

// 克隆表达式链表，并在类型实参中执行泛型替换。
static ASTNode *clone_expression_list(ASTNode *source, FunctionNode *template,
                                      TypeBinding *bindings) {
    ASTNode *head = NULL;
    ASTNode **tail = &head;
    for (; source; source = source->next) {
        ASTNode *copy = clone_expression(source, template, bindings);
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

// 克隆单个表达式，并替换其中出现的泛型类型参数。
static ASTNode *clone_expression(ASTNode *source, FunctionNode *template,
                                 TypeBinding *bindings) {
    if (!source) return NULL;
    switch (source->type) {
        case NODE_IDENTIFIER:
            return (ASTNode *)create_identifier(((IdentifierNode *)source)->name);
        case NODE_LITERAL: {
            LiteralNode *literal = (LiteralNode *)source;
            LiteralNode *copy = NULL;
            if (literal->literal_type == LITERAL_STRING) {
                copy = create_string_literal(literal->value.string_value);
            } else if (literal->literal_type == LITERAL_FLOAT ||
                       literal->literal_type == LITERAL_F32 ||
                       literal->literal_type == LITERAL_F64) {
                copy = create_float_literal(literal->value.float_value);
            } else if (literal->literal_type == LITERAL_BOOL) {
                copy = create_bool_literal(literal->value.bool_value);
            } else if (literal->integer_text) {
                copy = create_int_literal_text(literal->integer_text);
            } else {
                copy = create_int_literal(literal->value.int_value);
            }
            copy->literal_type = literal->literal_type;
            return (ASTNode *)copy;
        }
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)source;
            return (ASTNode *)create_binary_op(
                binary->op_type,
                clone_expression(binary->left, template, bindings),
                clone_expression(binary->right, template, bindings));
        }
        case NODE_REFERENCE:
            return (ASTNode *)create_reference(clone_expression(
                ((ReferenceNode *)source)->target, template, bindings));
        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)source;
            ArrayLiteralNode *copy = create_array_literal();
            for (ASTNode *element = array->elements; element; element = element->next) {
                add_array_element(copy, clone_expression(element, template, bindings));
            }
            if (array->is_repeat) set_array_repeat(copy, array->repeat_count);
            return (ASTNode *)copy;
        }
        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)source;
            return (ASTNode *)create_index_expression(
                clone_expression(index->array, template, bindings),
                clone_expression(index->index, template, bindings));
        }
        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)source;
            StructLiteralNode *copy = create_struct_literal(literal->struct_name);
            for (ASTNode *node = literal->fields; node; node = node->next) {
                StructInitFieldNode *field = (StructInitFieldNode *)node;
                add_struct_init_field(copy, create_struct_init_field(
                    field->name,
                    clone_expression(field->expression, template, bindings)));
            }
            return (ASTNode *)copy;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)source;
            FunctionCallNode *copy = create_function_call(call->name);
            copy->filename = call->filename ? strdup(call->filename) : NULL;
            copy->line = call->line;
            copy->column = call->column;
            for (ASTNode *type = call->type_arguments; type; type = type->next) {
                add_type_argument(copy, substitute_type(
                    template, (VarTypeNode *)type, bindings));
            }
            copy->arguments = clone_expression_list(
                call->arguments, template, bindings);
            return (ASTNode *)copy;
        }
        default:
            fprintf(stderr, "error: cannot clone generic expression type %d\n",
                    source->type); // 中文：无法克隆泛型表达式
            exit(1);
    }
}

// 克隆单条语句，并替换其中出现的泛型类型参数。
static ASTNode *clone_statement(ASTNode *source, FunctionNode *template,
                                TypeBinding *bindings) {
    switch (source->type) {
        case NODE_VAR_DECL: {
            VarDeclNode *declaration = (VarDeclNode *)source;
            return (ASTNode *)create_var_decl(
                declaration->name,
                substitute_type(template, declaration->type, bindings),
                clone_expression(declaration->expression, template, bindings),
                declaration->is_const);
        }
        case NODE_ASSIGNMENT: {
            AssignmentNode *assignment = (AssignmentNode *)source;
            return (ASTNode *)create_assignment(
                assignment->name,
                clone_expression(assignment->expression, template, bindings));
        }
        case NODE_INDEX_ASSIGNMENT: {
            IndexAssignmentNode *assignment = (IndexAssignmentNode *)source;
            return (ASTNode *)create_index_assignment(
                (IndexExpressionNode *)clone_expression(
                    (ASTNode *)assignment->target, template, bindings),
                clone_expression(assignment->expression, template, bindings));
        }
        case NODE_RETURN:
            return (ASTNode *)create_return(clone_expression(
                ((ReturnNode *)source)->expression, template, bindings));
        case NODE_PRINT: {
            PrintNode *copy = create_print();
            for (ASTNode *argument = ((PrintNode *)source)->arguments;
                 argument; argument = argument->next) {
                add_print_argument(copy, clone_expression(argument, template, bindings));
            }
            return (ASTNode *)copy;
        }
        case NODE_FUNCTION_CALL:
            return clone_expression(source, template, bindings);
        case NODE_IF_STATEMENT: {
            IfStatementNode *statement = (IfStatementNode *)source;
            return (ASTNode *)create_if_statement(
                clone_expression(statement->condition, template, bindings),
                clone_statement_list(statement->consequence, template, bindings),
                statement->alternative &&
                        statement->alternative->type == NODE_IF_STATEMENT
                    ? clone_statement(statement->alternative, template, bindings)
                    : clone_statement_list(statement->alternative, template, bindings));
        }
        case NODE_FOR_STATEMENT: {
            ForStatementNode *statement = (ForStatementNode *)source;
            return (ASTNode *)create_for_statement(
                clone_statement_list(statement->initializer, template, bindings),
                clone_expression(statement->condition, template, bindings),
                clone_statement_list(statement->update, template, bindings),
                clone_statement_list(statement->body, template, bindings));
        }
        case NODE_BREAK_STATEMENT:
            return create_break_statement();
        case NODE_CONTINUE_STATEMENT:
            return create_continue_statement();
        default:
            fprintf(stderr, "error: cannot clone generic statement type %d\n",
                    source->type); // 中文：无法克隆泛型语句
            exit(1);
    }
}

// 克隆语句链表，并保持原有语句顺序。
static ASTNode *clone_statement_list(ASTNode *source, FunctionNode *template,
                                     TypeBinding *bindings) {
    ASTNode *head = NULL;
    ASTNode **tail = &head;
    for (; source; source = source->next) {
        ASTNode *copy = clone_statement(source, template, bindings);
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

// 为具体类型组合生成稳定的 LLVM 前函数名片段。
static char *type_key(const VarTypeNode *type) {
    const char *scalar_names[] = {
        "int", "uint", "i8", "u8", "i16", "u16", "i32", "u32",
        "i64", "u64", "i128", "u128", "float", "f32", "f64",
        "string", "bool"
    };
    if (type->is_pointer) {
        char *element = type_key(type->element_type);
        size_t size = strlen(element) + 3;
        char *key = (char *)malloc(size);
        if (!key) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        snprintf(key, size, "p_%s", element);
        free(element);
        return key;
    }
    if (type->is_array) {
        char *element = type_key(type->element_type);
        size_t size = strlen(element) + 40;
        char *key = (char *)malloc(size);
        if (!key) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        snprintf(key, size, "a_%llu_%s",
                 (unsigned long long)type->array_length, element);
        free(element);
        return key;
    }
    const char *name = type->struct_name ? type->struct_name : type->enum_name;
    if (name) {
        size_t size = strlen(name) + 3;
        char *key = (char *)malloc(size);
        if (!key) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        snprintf(key, size, "n_%s", name);
        return key;
    }
    return strdup(scalar_names[type->type]);
}

// 根据模板名称和类型参数生成单态化函数名。
static char *specialized_name(FunctionNode *function, TypeBinding *bindings) {
    size_t size = strlen(function->name) + 1;
    for (ASTNode *node = function->type_params; node; node = node->next) {
        TypeBinding *binding = find_binding(
            bindings, ((IdentifierNode *)node)->name);
        char *key = type_key(binding->type);
        size += strlen(key) + 1;
        free(key);
    }
    char *name = (char *)malloc(size);
    if (!name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    strcpy(name, function->name);
    for (ASTNode *node = function->type_params; node; node = node->next) {
        TypeBinding *binding = find_binding(
            bindings, ((IdentifierNode *)node)->name);
        char *key = type_key(binding->type);
        strcat(name, "$");
        strcat(name, key);
        free(key);
    }
    return name;
}

// 克隆泛型模板并生成不再含类型参数的具体函数。
static FunctionNode *instantiate_function(FunctionNode *template,
                                          TypeBinding *bindings,
                                          const char *name) {
    FunctionNode *copy = create_function((char *)name);
    copy->filename = template->filename ? strdup(template->filename) : NULL;
    copy->line = template->line;
    copy->column = template->column;
    copy->is_extern = template->is_extern;

    ASTNode *param = template->params;
    ASTNode *param_type = template->param_types;
    while (param) {
        add_param(copy, create_identifier(((IdentifierNode *)param)->name));
        add_param_type(copy, substitute_type(
            template, (VarTypeNode *)param_type, bindings));
        param = param->next;
        param_type = param_type ? param_type->next : NULL;
    }
    copy->return_type = substitute_type(
        template, template->return_type, bindings);
    copy->body = clone_statement_list(template->body, template, bindings);
    return copy;
}

// 在当前函数类型环境中查找局部变量。
static TypeSymbol *find_symbol(GenericContext *context, const char *name) {
    for (TypeSymbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) return symbol;
    }
    return NULL;
}

// 向当前函数类型环境登记局部变量。
static void add_symbol(GenericContext *context, const char *name,
                       const VarTypeNode *type) {
    if (!type) return;
    TypeSymbol *symbol = find_symbol(context, name);
    if (symbol) {
        free_ast((ASTNode *)symbol->type);
        symbol->type = clone_type(type);
        return;
    }
    symbol = (TypeSymbol *)calloc(1, sizeof(TypeSymbol));
    if (!symbol) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    symbol->name = strdup(name);
    symbol->type = clone_type(type);
    symbol->next = context->symbols;
    context->symbols = symbol;
}

// 释放当前函数的类型环境。
static void free_symbols(TypeSymbol *symbols) {
    while (symbols) {
        TypeSymbol *next = symbols->next;
        free(symbols->name);
        free_ast((ASTNode *)symbols->type);
        free(symbols);
        symbols = next;
    }
}

// 声明表达式处理入口，供泛型调用和索引类型推断递归使用。
static VarTypeNode *process_expression(GenericContext *context,
                                       ASTNode *expression,
                                       const VarTypeNode *expected);

// 报告带调用位置的泛型错误并终止编译。
static void generic_call_error(FunctionCallNode *call, const char *message,
                               const char *detail) {
    print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                     message, detail);
    exit(1);
}

// 解析标识符、结构体字段或枚举成员的完整类型。
static VarTypeNode *identifier_type(GenericContext *context, const char *name) {
    TypeSymbol *symbol = find_symbol(context, name);
    if (symbol) return clone_type(symbol->type);

    const char *dot = strchr(name, '.');
    if (dot && !strchr(dot + 1, '.')) {
        size_t base_size = (size_t)(dot - name);
        char *base = (char *)malloc(base_size + 1);
        if (!base) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        memcpy(base, name, base_size);
        base[base_size] = '\0';
        TypeSymbol *base_symbol = find_symbol(context, base);
        if (base_symbol) {
            const char *struct_name = base_symbol->type->struct_name
                ? base_symbol->type->struct_name
                : base_symbol->type->enum_name;
            StructFieldNode *field = find_field(
                find_struct(context->program, struct_name), dot + 1);
            free(base);
            if (field) return clone_type(field->field_type);
        } else {
            if (find_enum(context->program, base)) {
                VarTypeNode *type = create_enum_type(base);
                free(base);
                return type;
            }
            free(base);
            return create_enum_type(name);
        }
    }

    for (ASTNode *node = context->program->constants; node; node = node->next) {
        VarDeclNode *constant = (VarDeclNode *)node;
        if (strcmp(constant->name, name) == 0) return clone_type(constant->type);
    }
    return NULL;
}

// 返回函数指定位置的完整参数类型。
static const VarTypeNode *function_param_type(FunctionNode *function,
                                               unsigned index) {
    ASTNode *node = function ? function->param_types : NULL;
    while (node && index > 0) {
        node = node->next;
        index--;
    }
    return node ? (VarTypeNode *)node : NULL;
}

// 根据调用点信息实例化泛型函数并改写调用名称。
static FunctionNode *specialize_call(GenericContext *context,
                                     FunctionCallNode *call,
                                     FunctionNode *template,
                                     const VarTypeNode *expected) {
    unsigned type_param_count = node_count(template->type_params);
    unsigned explicit_count = node_count(call->type_arguments);
    if (explicit_count > 0 && explicit_count != type_param_count) {
        generic_call_error(call,
            "generic function type argument count mismatch for '%s'", call->name);
    }
    if (node_count(call->arguments) != node_count(template->params)) {
        generic_call_error(call,
            "generic function argument count mismatch for '%s'", call->name);
    }

    TypeBinding *bindings = NULL;
    ASTNode *type_param = template->type_params;
    ASTNode *type_argument = call->type_arguments;
    while (type_param && type_argument) {
        if (!bind_type(&bindings, ((IdentifierNode *)type_param)->name,
                       (VarTypeNode *)type_argument)) {
            generic_call_error(call,
                "conflicting generic type arguments for '%s'", call->name);
        }
        type_param = type_param->next;
        type_argument = type_argument->next;
    }

    ASTNode *argument = call->arguments;
    ASTNode *parameter_type = template->param_types;
    while (argument && parameter_type) {
        VarTypeNode *actual = process_expression(context, argument, NULL);
        if (!actual || !unify_type(template, (VarTypeNode *)parameter_type,
                                   actual, &bindings, 0)) {
            free_ast((ASTNode *)actual);
            generic_call_error(call,
                "cannot infer compatible generic arguments for '%s'", call->name);
        }
        free_ast((ASTNode *)actual);
        argument = argument->next;
        parameter_type = parameter_type->next;
    }

    if (expected && template->return_type) {
        unify_type(template, template->return_type, expected, &bindings, 1);
    }

    for (ASTNode *node = template->type_params; node; node = node->next) {
        const char *name = ((IdentifierNode *)node)->name;
        if (!find_binding(bindings, name)) {
            generic_call_error(call,
                "cannot infer generic type parameter '%s'", name);
        }
    }

    char *name = specialized_name(template, bindings);
    FunctionNode *specialized = find_function(context->program, name);
    if (!specialized) {
        specialized = instantiate_function(template, bindings, name);
        add_function(context->program, specialized);
    }
    free(call->name);
    call->name = strdup(name);
    free(name);
    free_ast(call->type_arguments);
    call->type_arguments = NULL;
    free_bindings(bindings);
    return specialized;
}

// 推断索引表达式最终元素的类型。
static VarTypeNode *index_type(GenericContext *context,
                               IndexExpressionNode *index) {
    VarTypeNode *container = process_expression(context, index->array, NULL);
    VarTypeNode *index_value = process_expression(context, index->index, NULL);
    free_ast((ASTNode *)index_value);
    if (!container || (!container->is_array && !container->is_pointer)) {
        free_ast((ASTNode *)container);
        return NULL;
    }
    VarTypeNode *element = clone_type(container->element_type);
    free_ast((ASTNode *)container);
    return element;
}

// 处理调用表达式中的嵌套泛型并返回具体返回类型。
static VarTypeNode *process_call(GenericContext *context,
                                 FunctionCallNode *call,
                                 const VarTypeNode *expected) {
    FunctionNode *function = find_function(context->program, call->name);
    if (function && function->type_params) {
        function = specialize_call(context, call, function, expected);
        return clone_type(function->return_type);
    }
    if (call->type_arguments) {
        generic_call_error(call, "function '%s' is not generic", call->name);
    }

    unsigned index = 0;
    for (ASTNode *argument = call->arguments; argument;
         argument = argument->next, index++) {
        VarTypeNode *ignored = process_expression(
            context, argument, function_param_type(function, index));
        free_ast((ASTNode *)ignored);
    }
    if (function && function->return_type) return clone_type(function->return_type);
    if (strcmp(call->name, "__tap_builtin_string_len") == 0 ||
        strstr(call->name, ".len")) {
        return create_var_type(LITERAL_UINT);
    }
    if (strcmp(call->name, "__tap_builtin_string_byte_at") == 0 ||
        strstr(call->name, ".byte_at")) {
        return create_var_type(LITERAL_U8);
    }
    if (strcmp(call->name, "__tap_builtin_string_slice") == 0 ||
        strstr(call->name, ".slice")) {
        return create_var_type(LITERAL_STRING);
    }
    return expected ? clone_type(expected) : create_var_type(LITERAL_I32);
}

// 递归处理表达式，完成泛型调用改写并推断结果类型。
static VarTypeNode *process_expression(GenericContext *context,
                                       ASTNode *expression,
                                       const VarTypeNode *expected) {
    if (!expression) return NULL;
    switch (expression->type) {
        case NODE_LITERAL:
            return create_var_type(((LiteralNode *)expression)->literal_type);
        case NODE_IDENTIFIER:
            return identifier_type(
                context, ((IdentifierNode *)expression)->name);
        case NODE_FUNCTION_CALL:
            return process_call(
                context, (FunctionCallNode *)expression, expected);
        case NODE_INDEX_EXPRESSION:
            return index_type(context, (IndexExpressionNode *)expression);
        case NODE_REFERENCE: {
            VarTypeNode *target = process_expression(
                context, ((ReferenceNode *)expression)->target, NULL);
            return target ? create_pointer_type(target) : NULL;
        }
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            VarTypeNode *left = process_expression(context, binary->left, NULL);
            VarTypeNode *right = process_expression(context, binary->right, NULL);
            free_ast((ASTNode *)right);
            if (binary->op_type >= OP_EQUAL &&
                binary->op_type <= OP_GREATER_THAN_OR_EQUAL) {
                free_ast((ASTNode *)left);
                return create_var_type(LITERAL_BOOL);
            }
            return left;
        }
        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)expression;
            for (ASTNode *element = array->elements; element; element = element->next) {
                VarTypeNode *ignored = process_expression(
                    context, element, expected ? expected->element_type : NULL);
                free_ast((ASTNode *)ignored);
            }
            return expected ? clone_type(expected) : NULL;
        }
        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)expression;
            StructNode *struct_node = find_struct(
                context->program, literal->struct_name);
            for (ASTNode *node = literal->fields; node; node = node->next) {
                StructInitFieldNode *field = (StructInitFieldNode *)node;
                StructFieldNode *definition = find_field(struct_node, field->name);
                VarTypeNode *ignored = process_expression(
                    context, field->expression,
                    definition ? definition->field_type : NULL);
                free_ast((ASTNode *)ignored);
            }
            return create_enum_type(literal->struct_name);
        }
        default:
            return expected ? clone_type(expected) : NULL;
    }
}

// 处理语句链中的泛型调用并维护局部类型环境。
static void process_statements(GenericContext *context, ASTNode *statement,
                               const VarTypeNode *return_type) {
    for (; statement; statement = statement->next) {
        switch (statement->type) {
            case NODE_VAR_DECL: {
                VarDeclNode *declaration = (VarDeclNode *)statement;
                VarTypeNode *inferred = process_expression(
                    context, declaration->expression, declaration->type);
                add_symbol(context, declaration->name,
                           declaration->type ? declaration->type : inferred);
                free_ast((ASTNode *)inferred);
                break;
            }
            case NODE_ASSIGNMENT: {
                AssignmentNode *assignment = (AssignmentNode *)statement;
                TypeSymbol *symbol = find_symbol(context, assignment->name);
                VarTypeNode *ignored = process_expression(
                    context, assignment->expression,
                    symbol ? symbol->type : NULL);
                free_ast((ASTNode *)ignored);
                break;
            }
            case NODE_INDEX_ASSIGNMENT: {
                IndexAssignmentNode *assignment = (IndexAssignmentNode *)statement;
                VarTypeNode *element = index_type(context, assignment->target);
                VarTypeNode *ignored = process_expression(
                    context, assignment->expression, element);
                free_ast((ASTNode *)ignored);
                free_ast((ASTNode *)element);
                break;
            }
            case NODE_RETURN: {
                VarTypeNode *ignored = process_expression(
                    context, ((ReturnNode *)statement)->expression, return_type);
                free_ast((ASTNode *)ignored);
                break;
            }
            case NODE_PRINT:
                for (ASTNode *argument = ((PrintNode *)statement)->arguments;
                     argument; argument = argument->next) {
                    VarTypeNode *ignored = process_expression(context, argument, NULL);
                    free_ast((ASTNode *)ignored);
                }
                break;
            case NODE_FUNCTION_CALL: {
                VarTypeNode *ignored = process_expression(context, statement, NULL);
                free_ast((ASTNode *)ignored);
                break;
            }
            case NODE_IF_STATEMENT: {
                IfStatementNode *if_node = (IfStatementNode *)statement;
                VarTypeNode *ignored = process_expression(
                    context, if_node->condition, NULL);
                free_ast((ASTNode *)ignored);
                process_statements(context, if_node->consequence, return_type);
                process_statements(context, if_node->alternative, return_type);
                break;
            }
            case NODE_FOR_STATEMENT: {
                ForStatementNode *for_node = (ForStatementNode *)statement;
                process_statements(context, for_node->initializer, return_type);
                VarTypeNode *ignored = process_expression(
                    context, for_node->condition, NULL);
                free_ast((ASTNode *)ignored);
                process_statements(context, for_node->update, return_type);
                process_statements(context, for_node->body, return_type);
                break;
            }
            default:
                break;
        }
    }
}

// 处理一个具体函数中的全部泛型调用。
static void process_function(GenericContext *context, FunctionNode *function) {
    free_symbols(context->symbols);
    context->symbols = NULL;
    ASTNode *param = function->params;
    ASTNode *type = function->param_types;
    while (param && type) {
        add_symbol(context, ((IdentifierNode *)param)->name,
                   (VarTypeNode *)type);
        param = param->next;
        type = type->next;
    }
    process_statements(context, function->body, function->return_type);
}

// 从程序函数链表中移除已经实例化完毕的泛型模板。
static void remove_templates(ProgramNode *program) {
    ASTNode **link = &program->functions;
    while (*link) {
        FunctionNode *function = (FunctionNode *)*link;
        if (!function->type_params) {
            link = &(*link)->next;
            continue;
        }
        ASTNode *removed = *link;
        *link = removed->next;
        removed->next = NULL;
        free_ast(removed);
    }
}

// 在 LLVM 代码生成前实例化所有被调用的泛型函数。
int specialize_generics(ProgramNode *program) {
    GenericContext context = {.program = program, .symbols = NULL};
    for (ASTNode *node = program ? program->functions : NULL;
         node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        if (!function->type_params) process_function(&context, function);
    }
    free_symbols(context.symbols);
    remove_templates(program);
    return 0;
}
