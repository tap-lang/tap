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

static char *type_key(const VarTypeNode *type);
static VarTypeNode *clone_type(const VarTypeNode *source);

// 复制具名类型携带的泛型实参。
static void clone_type_arguments(VarTypeNode *destination, ASTNode *arguments) {
    for (ASTNode *argument = arguments; argument; argument = argument->next) {
        add_var_type_argument(
            destination, clone_type((VarTypeNode *)argument));
    }
}

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
    if (source->struct_name) {
        VarTypeNode *copy = create_struct_type(source->struct_name);
        clone_type_arguments(copy, source->type_arguments);
        return copy;
    }
    if (source->enum_name) {
        VarTypeNode *copy = create_enum_type(source->enum_name);
        clone_type_arguments(copy, source->type_arguments);
        return copy;
    }
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
        if (!left_name || !right_name || strcmp(left_name, right_name) != 0) return 0;
        ASTNode *left_argument = left->type_arguments;
        ASTNode *right_argument = right->type_arguments;
        while (left_argument && right_argument) {
            if (!type_equal((VarTypeNode *)left_argument,
                            (VarTypeNode *)right_argument)) return 0;
            left_argument = left_argument->next;
            right_argument = right_argument->next;
        }
        return !left_argument && !right_argument;
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

// 判断名称是否在给定的类型参数列表里。
static int has_type_parameter(ASTNode *type_params, const char *name) {
    for (ASTNode *node = type_params; node; node = node->next) {
        if (strcmp(((IdentifierNode *)node)->name, name) == 0) return 1;
    }
    return 0;
}

// 判断类型节点是否直接表示类型参数（按给定的类型参数列表判断）。
//
// 泛型函数和泛型结构体用的是同一套类型参数机制，所以这一层收在「类型参数列表」上，
// 两边共用 —— 泛型结构体字面量的类型实参推断就是靠它复用 unify_type 的。
static const char *type_parameter_name_in(
    ASTNode *type_params, const VarTypeNode *type) {
    if (!type || type->is_pointer || type->is_array || type->struct_name ||
        !type->enum_name) {
        return NULL;
    }
    return has_type_parameter(type_params, type->enum_name) ? type->enum_name : NULL;
}

// 判断类型节点是否直接表示当前泛型函数的类型参数。
static const char *type_parameter_name(
    FunctionNode *function, const VarTypeNode *type) {
    return type_parameter_name_in(function ? function->type_params : NULL, type);
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

// 用给定的类型参数列表把形参类型与实际类型递归统一并收集泛型绑定。
//
// 泛型函数和泛型结构体共用这一份实现（`type_params` 就是各自的类型参数链表）。
static int unify_type_params(ASTNode *type_params, const VarTypeNode *pattern,
                             const VarTypeNode *actual, TypeBinding **bindings,
                             int only_unbound) {
    if (!pattern || !actual) return 0;
    const char *parameter = type_parameter_name_in(type_params, pattern);
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
        return unify_type_params(type_params, pattern->element_type,
                                 actual->element_type, bindings, only_unbound);
    }
    if (pattern->is_array) {
        return pattern->array_length == actual->array_length &&
               unify_type_params(type_params, pattern->element_type,
                                 actual->element_type, bindings, only_unbound);
    }
    const char *pattern_name = pattern->struct_name
        ? pattern->struct_name : pattern->enum_name;
    const char *actual_name = actual->struct_name
        ? actual->struct_name : actual->enum_name;
    if (pattern_name || actual_name) {
        int same_name = pattern_name && actual_name &&
            strcmp(pattern_name, actual_name) == 0;
        int actual_is_specialization = pattern_name && actual_name &&
            actual->type_arguments && actual->struct_name &&
            strncmp(actual_name, pattern_name, strlen(pattern_name)) == 0 &&
            actual_name[strlen(pattern_name)] == '$';
        if (!same_name && !actual_is_specialization) {
            return 0;
        }
        ASTNode *pattern_argument = pattern->type_arguments;
        ASTNode *actual_argument = actual->type_arguments;
        while (pattern_argument && actual_argument) {
            if (!unify_type_params(type_params, (VarTypeNode *)pattern_argument,
                                   (VarTypeNode *)actual_argument, bindings,
                                   only_unbound)) return 0;
            pattern_argument = pattern_argument->next;
            actual_argument = actual_argument->next;
        }
        return !pattern_argument && !actual_argument;
    }
    // 不含类型参数的具体类型仍交给现有类型检查器处理转换和诊断。
    return 1;
}

// 泛型函数版本：类型参数来自函数的 type_params。
static int unify_type(FunctionNode *function, const VarTypeNode *pattern,
                      const VarTypeNode *actual, TypeBinding **bindings,
                      int only_unbound) {
    return unify_type_params(function ? function->type_params : NULL,
                             pattern, actual, bindings, only_unbound);
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
    VarTypeNode *copy = source->struct_name
        ? create_struct_type(source->struct_name)
        : source->enum_name
            ? create_enum_type(source->enum_name)
            : create_var_type(source->type);
    for (ASTNode *argument = source->type_arguments; argument;
         argument = argument->next) {
        add_var_type_argument(copy, substitute_type(
            function, (VarTypeNode *)argument, bindings));
    }
    return copy;
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
        case NODE_SIZEOF:
            return (ASTNode *)create_sizeof(substitute_type(
                template, ((SizeofNode *)source)->operand_type, bindings));
        case NODE_CAST: {
            CastNode *cast = (CastNode *)source;
            CastNode *copy = create_cast(
                clone_expression(cast->expression, template, bindings),
                substitute_type(template, cast->target_type, bindings));
            copy->filename = cast->filename ? strdup(cast->filename) : NULL;
            copy->line = cast->line;
            copy->column = cast->column;
            return (ASTNode *)copy;
        }
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
        case NODE_FIELD_ACCESS: {
            FieldAccessNode *access = (FieldAccessNode *)source;
            FieldAccessNode *copy = create_field_access(
                clone_expression(access->object, template, bindings),
                access->field);
            copy->filename = access->filename ? strdup(access->filename) : NULL;
            copy->line = access->line;
            copy->column = access->column;
            return (ASTNode *)copy;
        }
        case NODE_TRY: {
            TryNode *try_node = (TryNode *)source;
            TryNode *copy = create_try(
                clone_expression(try_node->inner, template, bindings));
            copy->filename = try_node->filename ? strdup(try_node->filename) : NULL;
            copy->line = try_node->line;
            copy->column = try_node->column;
            return (ASTNode *)copy;
        }
        case NODE_MATCH_STATEMENT: {
            MatchStatementNode *match_node = (MatchStatementNode *)source;
            MatchStatementNode *copy = create_match_statement(
                clone_expression(match_node->expression, template, bindings));
            for (ASTNode *node = match_node->arms; node; node = node->next) {
                MatchArmNode *source_arm = (MatchArmNode *)node;
                MatchArmNode *copy_arm = create_match_arm(
                    source_arm->enum_name, source_arm->variant_name);
                for (ASTNode *binding = source_arm->bindings;
                     binding; binding = binding->next) {
                    add_match_binding(copy_arm,
                        (IdentifierNode *)clone_expression(binding, template, bindings));
                }
                copy_arm->body =
                    clone_statement_list(source_arm->body, template, bindings);
                copy_arm->value =
                    clone_expression(source_arm->value, template, bindings);
                copy_arm->filename =
                    source_arm->filename ? strdup(source_arm->filename) : NULL;
                copy_arm->line = source_arm->line;
                copy_arm->column = source_arm->column;
                add_match_arm(copy, copy_arm);
            }
            return (ASTNode *)copy;
        }
        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)source;
            StructLiteralNode *copy = create_struct_literal(literal->struct_name);
            for (ASTNode *type = literal->type_arguments; type; type = type->next) {
                add_struct_literal_type_argument(copy, substitute_type(
                    template, (VarTypeNode *)type, bindings));
            }
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
            copy->forwards_variadic = call->forwards_variadic;
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
        case NODE_FUNCTION_CALL:
            return clone_expression(source, template, bindings);
        case NODE_MATCH_STATEMENT:
            // 语句形式的 match 走 clone_expression 里那个分支就够了。
            return clone_expression(source, template, bindings);
        case NODE_IF_STATEMENT: {
            IfStatementNode *statement = (IfStatementNode *)source;
            // alternative 一律按语句链表克隆：`else if` 是长度 1 的链表。
            // 按节点类型特判会把「表头恰好是 if 的 else 块」截断成一条语句。
            return (ASTNode *)create_if_statement(
                clone_expression(statement->condition, template, bindings),
                clone_statement_list(statement->consequence, template, bindings),
                clone_statement_list(statement->alternative, template, bindings));
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
        for (ASTNode *argument = type->type_arguments; argument;
             argument = argument->next) {
            char *argument_key = type_key((VarTypeNode *)argument);
            size += strlen(argument_key) + 1;
            free(argument_key);
        }
        char *key = (char *)malloc(size);
        if (!key) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        snprintf(key, size, "n_%s", name);
        for (ASTNode *argument = type->type_arguments; argument;
             argument = argument->next) {
            char *argument_key = type_key((VarTypeNode *)argument);
            strcat(key, "_");
            strcat(key, argument_key);
            free(argument_key);
        }
        return key;
    }
    return strdup(scalar_names[type->type]);
}

static void materialize_type(GenericContext *context, VarTypeNode *type);

// 为泛型结构体的具体实参组合生成稳定名称。
static char *specialized_struct_name(
    StructNode *template, ASTNode *type_arguments) {
    size_t size = strlen(template->name) + 1;
    for (ASTNode *argument = type_arguments; argument; argument = argument->next) {
        char *key = type_key((VarTypeNode *)argument);
        size += strlen(key) + 1;
        free(key);
    }
    char *name = malloc(size);
    if (!name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    strcpy(name, template->name);
    for (ASTNode *argument = type_arguments; argument; argument = argument->next) {
        char *key = type_key((VarTypeNode *)argument);
        strcat(name, "$");
        strcat(name, key);
        free(key);
    }
    return name;
}

// 载荷枚举降级生成的结构体，在诊断里仍然按「枚举」称呼。
static const char *generic_type_noun(const StructNode *structure) {
    return structure && structure->is_tagged_enum ? "enum" : "struct";
}

// 按具体类型实参克隆结构体字段，并把嵌套泛型类型一并实例化。
static char *instantiate_struct(
    GenericContext *context, StructNode *template, ASTNode *type_arguments) {
    if (node_count(template->type_params) != node_count(type_arguments)) {
        fprintf(stderr, "error: generic %s type argument count mismatch for '%s'\n",
                generic_type_noun(template), template->name); // 中文：泛型类型实参数量不匹配
        exit(1);
    }

    char *name = specialized_struct_name(template, type_arguments);
    StructNode *existing = find_struct(context->program, name);
    if (existing) return name;

    StructNode *copy = create_struct(name);
    // 载荷枚举降级生成的结构体要保留来源枚举名，实例化后靠它映射回枚举声明。
    copy->is_tagged_enum = template->is_tagged_enum;
    copy->is_pub = template->is_pub;
    if (template->tagged_enum_name) {
        copy->tagged_enum_name = strdup(template->tagged_enum_name);
        if (!copy->tagged_enum_name) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
    }
    add_struct(context->program, copy);

    TypeBinding *bindings = NULL;
    ASTNode *parameter = template->type_params;
    ASTNode *argument = type_arguments;
    while (parameter && argument) {
        bind_type(&bindings, ((IdentifierNode *)parameter)->name,
                  (VarTypeNode *)argument);
        parameter = parameter->next;
        argument = argument->next;
    }

    FunctionNode substitution_scope = {0};
    substitution_scope.type_params = template->type_params;
    for (ASTNode *field = template->fields; field; field = field->next) {
        StructFieldNode *source = (StructFieldNode *)field;
        VarTypeNode *field_type = substitute_type(
            &substitution_scope, source->field_type, bindings);
        materialize_type(context, field_type);
        add_struct_field(copy, create_struct_field(source->name, field_type));
    }
    free_bindings(bindings);
    return name;
}

// 把 Vec<i32> 形式的类型引用改写为后端可直接使用的具体结构体名。
static void materialize_type(GenericContext *context, VarTypeNode *type) {
    if (!type) return;
    if (type->is_pointer || type->is_array) {
        materialize_type(context, type->element_type);
        // create_pointer_type 会把元素的名字复制到指针节点上，但不复制类型实参。
        // 元素单态化之后必须把结果同步回来，否则指针节点上留的是已经删掉的模板名，
        // codegen 的 validate_var_type 会报 undefined enum type（`*Vec<string>` 就是这样）。
        VarTypeNode *element = type->element_type;
        if (element) {
            if (element->struct_name) {
                free(type->struct_name);
                type->struct_name = strdup(element->struct_name);
                free(type->enum_name);
                type->enum_name = NULL;
            } else if (element->enum_name) {
                free(type->enum_name);
                type->enum_name = strdup(element->enum_name);
            }
        }
        return;
    }
    for (ASTNode *argument = type->type_arguments; argument;
         argument = argument->next) {
        materialize_type(context, (VarTypeNode *)argument);
    }

    const char *name = type->struct_name ? type->struct_name : type->enum_name;
    if (!name) return;
    StructNode *structure = find_struct(context->program, name);
    if (!structure) {
        if (type->type_arguments) {
            fprintf(stderr, "error: undefined generic struct type '%s'\n", name);
            exit(1);
        }
        return;
    }
    if (!structure->type_params) {
        if (type->type_arguments && !type->struct_name) {
            fprintf(stderr, "error: %s '%s' is not generic\n",
                    generic_type_noun(structure), name);
            exit(1);
        }
        if (!type->struct_name) {
            type->struct_name = type->enum_name;
            type->enum_name = NULL;
        }
        return;
    }
    if (!type->type_arguments) {
        fprintf(stderr, "error: generic %s '%s' requires type arguments\n",
                generic_type_noun(structure), name);
        exit(1);
    }

    char *concrete_name = instantiate_struct(
        context, structure, type->type_arguments);
    free(type->enum_name);
    free(type->struct_name);
    type->enum_name = NULL;
    type->struct_name = concrete_name;
}

static void materialize_expression(GenericContext *context, ASTNode *expression);

// 递归实例化语句中出现的泛型结构体类型。
static void materialize_statements(GenericContext *context, ASTNode *statement) {
    for (; statement; statement = statement->next) {
        switch (statement->type) {
            case NODE_VAR_DECL: {
                VarDeclNode *declaration = (VarDeclNode *)statement;
                materialize_type(context, declaration->type);
                materialize_expression(context, declaration->expression);
                break;
            }
            case NODE_ASSIGNMENT:
                materialize_expression(
                    context, ((AssignmentNode *)statement)->expression);
                break;
            case NODE_INDEX_ASSIGNMENT: {
                IndexAssignmentNode *assignment = (IndexAssignmentNode *)statement;
                materialize_expression(context, (ASTNode *)assignment->target);
                materialize_expression(context, assignment->expression);
                break;
            }
            case NODE_RETURN:
                materialize_expression(
                    context, ((ReturnNode *)statement)->expression);
                break;
            case NODE_FUNCTION_CALL:
                materialize_expression(context, statement);
                break;
            case NODE_IF_STATEMENT: {
                IfStatementNode *node = (IfStatementNode *)statement;
                materialize_expression(context, node->condition);
                materialize_statements(context, node->consequence);
                materialize_statements(context, node->alternative);
                break;
            }
            case NODE_FOR_STATEMENT: {
                ForStatementNode *node = (ForStatementNode *)statement;
                materialize_statements(context, node->initializer);
                materialize_expression(context, node->condition);
                materialize_statements(context, node->update);
                materialize_statements(context, node->body);
                break;
            }
            case NODE_MATCH_STATEMENT: {
                MatchStatementNode *node = (MatchStatementNode *)statement;
                materialize_expression(context, node->expression);
                for (ASTNode *arm = node->arms; arm; arm = arm->next) {
                    materialize_statements(context, ((MatchArmNode *)arm)->body);
                    // match 作表达式时分支值在 value 里，别漏。
                    materialize_expression(context, ((MatchArmNode *)arm)->value);
                }
                break;
            }
            default:
                break;
        }
    }
}

// 递归实例化表达式中的类型实参和结构体字面量。
static void materialize_expression(GenericContext *context, ASTNode *expression) {
    if (!expression) return;
    switch (expression->type) {
        case NODE_BINARY_OP: {
            BinaryOpNode *node = (BinaryOpNode *)expression;
            materialize_expression(context, node->left);
            materialize_expression(context, node->right);
            break;
        }
        case NODE_REFERENCE:
            materialize_expression(
                context, ((ReferenceNode *)expression)->target);
            break;
        case NODE_SIZEOF:
            materialize_type(context, ((SizeofNode *)expression)->operand_type);
            break;
        case NODE_CAST: {
            CastNode *node = (CastNode *)expression;
            materialize_expression(context, node->expression);
            materialize_type(context, node->target_type);
            break;
        }
        case NODE_ARRAY_LITERAL:
            for (ASTNode *element = ((ArrayLiteralNode *)expression)->elements;
                 element; element = element->next) {
                materialize_expression(context, element);
            }
            break;
        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *node = (IndexExpressionNode *)expression;
            materialize_expression(context, node->array);
            materialize_expression(context, node->index);
            break;
        }
        case NODE_FIELD_ACCESS:
            materialize_expression(
                context, ((FieldAccessNode *)expression)->object);
            break;
        case NODE_TRY:
            materialize_expression(context, ((TryNode *)expression)->inner);
            break;
        case NODE_MATCH_STATEMENT: {
            // match 作表达式：被匹配值和各分支的值都要走一遍。
            MatchStatementNode *node = (MatchStatementNode *)expression;
            materialize_expression(context, node->expression);
            for (ASTNode *arm = node->arms; arm; arm = arm->next) {
                materialize_expression(context, ((MatchArmNode *)arm)->value);
            }
            break;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            for (ASTNode *type = call->type_arguments; type; type = type->next) {
                materialize_type(context, (VarTypeNode *)type);
            }
            for (ASTNode *type = call->enum_type_arguments; type; type = type->next) {
                materialize_type(context, (VarTypeNode *)type);
            }
            for (ASTNode *argument = call->arguments; argument;
                 argument = argument->next) {
                materialize_expression(context, argument);
            }
            break;
        }
        case NODE_IDENTIFIER: {
            // Enum<T>.Member 这种裸成员写法也带显式类型实参。
            IdentifierNode *identifier = (IdentifierNode *)expression;
            for (ASTNode *type = identifier->enum_type_arguments; type; type = type->next) {
                materialize_type(context, (VarTypeNode *)type);
            }
            break;
        }
        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)expression;
            VarTypeNode *type = create_enum_type(literal->struct_name);
            for (ASTNode *argument = literal->type_arguments; argument;
                 argument = argument->next) {
                add_var_type_argument(type, clone_type((VarTypeNode *)argument));
            }
            materialize_type(context, type);
            free(literal->struct_name);
            literal->struct_name = strdup(
                type->struct_name ? type->struct_name : type->enum_name);
            free_ast(literal->type_arguments);
            literal->type_arguments = NULL;
            free_ast((ASTNode *)type);
            for (ASTNode *field = literal->fields; field; field = field->next) {
                materialize_expression(
                    context, ((StructInitFieldNode *)field)->expression);
            }
            break;
        }
        default:
            break;
    }
}

// 实例化函数签名和函数体中出现的泛型结构体。
static void materialize_function(
    GenericContext *context, FunctionNode *function) {
    for (ASTNode *type = function->param_types; type; type = type->next) {
        materialize_type(context, (VarTypeNode *)type);
    }
    materialize_type(context, function->return_type);
    materialize_statements(context, function->body);
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
    copy->is_variadic = template->is_variadic;
    copy->is_pub = template->is_pub;
    // 单态化后的名字带 `$...` 后缀且可能含模块前缀；诊断优先用模板的原名。
    copy->original_name = template->original_name
        ? strdup(template->original_name)
        : NULL;

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

// 把 match 分支的载荷绑定登记进类型环境。定义在 process_statements 附近，
// 但 match 的**表达式**形式在 process_expression 里处理，所以要先声明。
static void bind_match_payloads(
    GenericContext *context, const VarTypeNode *matched, MatchArmNode *arm);

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

    // 模块私有枚举被改名成 `__tap_module_0.E` 后，成员引用变成 `__tap_module_0.E.B`
    // （两个点）。按**最后一个**点切分，才能取出正确的前缀；用 strchr 取第一个点会切错。
    const char *dot = strrchr(name, '.');
    if (dot) {
        size_t base_size = (size_t)(dot - name);
        char *base = (char *)malloc(base_size + 1);
        if (!base) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        memcpy(base, name, base_size);
        base[base_size] = '\0';
        TypeSymbol *base_symbol = find_symbol(context, base);
        if (base_symbol && base_symbol->type) {
            const char *struct_name = base_symbol->type->struct_name
                ? base_symbol->type->struct_name
                : base_symbol->type->enum_name;
            // 基础符号不是结构体 / 枚举时（标量，或类型还没推断出来的局部变量），这里
            // 没有字段可查 —— 当查不到处理，别把 NULL 名字喂给 find_struct（会段错误）。
            StructFieldNode *field = struct_name
                ? find_field(find_struct(context->program, struct_name), dot + 1)
                : NULL;
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
// 一个具体类型是否实现了某个 trait：impl 表里有没有 (trait, type) 这条记录。
static int type_implements_trait(
    ProgramNode *program, const char *trait_name, const char *type_name) {
    for (ASTNode *node = program ? program->impls : NULL; node; node = node->next) {
        ImplNode *impl_node = (ImplNode *)node;
        if (strcmp(impl_node->trait_name, trait_name) == 0 &&
            strcmp(impl_node->type_name, type_name) == 0) {
            return 1;
        }
    }
    return 0;
}

// 检查一次实例化里每个类型参数是否满足自己的约束。
//
// 约束在 parser 里存成 trait 名（VarTypeNode.struct_name）。还没落到具体类型的类型参数
// （嵌套泛型）先跳过，等外层实例化到具体类型时这里会再查一次。
static void check_type_bounds(
    GenericContext *context, FunctionCallNode *call, FunctionNode *template,
    TypeBinding *bindings) {
    for (ASTNode *node = template->type_params; node; node = node->next) {
        IdentifierNode *type_param = (IdentifierNode *)node;
        if (!type_param->type_bounds) continue;

        TypeBinding *binding = find_binding(bindings, type_param->name);
        if (!binding) continue;
        const VarTypeNode *actual = binding->type;
        const char *type_name =
            actual->struct_name ? actual->struct_name : actual->enum_name;

        // 名字既不是结构体也不是枚举 —— 多半是还没实例化的类型参数，跳过。
        if (type_name && !find_struct(context->program, type_name) &&
            !find_enum(context->program, type_name)) {
            continue;
        }

        for (ASTNode *bound = type_param->type_bounds; bound; bound = bound->next) {
            const char *trait_name = ((VarTypeNode *)bound)->struct_name;
            if (!trait_name) continue;
            if (type_name &&
                type_implements_trait(context->program, trait_name, type_name)) {
                continue;
            }
            char *key = type_key(actual);
            // type_key 给具名类型加了 `n_` 前缀（单态化用），诊断里用源语言的名字更好读。
            const char *display = type_name ? type_name : key;
            print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                             "type '%s' does not implement trait '%s' required by '%s'",
                             display, trait_name, type_param->name); // 中文：类型没有实现约束要求的 trait
            free(key);
            exit(1);
        }
    }
}

// 取具名类型的名字（结构体或枚举）；标量、数组、指针返回 NULL。
static const char *var_type_named(const VarTypeNode *type) {
    if (!type || type->is_array || type->is_pointer) return NULL;
    return type->struct_name ? type->struct_name : type->enum_name;
}

// 剥掉模块前缀，诊断里只显示源语言名字（`__tap_module_0.Point` → `Point`）。
// 按**最后一个**点切分：模块私有枚举的成员形式带两个点（`__tap_module_0.E.B`）。
static const char *strip_module_prefix(const char *name) {
    if (!name) return "(none)";
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : name;
}

// 诊断用的类型名：标量给名字，具名类型剥掉模块前缀，指针/数组/泛型实参递归展开。
static char *describe_type(const VarTypeNode *type) {
    if (!type) return strdup("nothing");
    if (type->is_pointer) {
        char *element = describe_type(type->element_type);
        size_t size = strlen(element) + 2;
        char *result = (char *)malloc(size);
        if (!result) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        snprintf(result, size, "*%s", element);
        free(element);
        return result;
    }
    if (type->is_array) {
        char *element = describe_type(type->element_type);
        size_t size = strlen(element) + 40;
        char *result = (char *)malloc(size);
        if (!result) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        snprintf(result, size, "[%s; %llu]",
                 element, (unsigned long long)type->array_length);
        free(element);
        return result;
    }
    const char *named = var_type_named(type);
    if (!named) return type_key(type); // 标量：type_key 直接给出源语言名字
    const char *base = strip_module_prefix(named);
    if (!type->type_arguments) return strdup(base);

    size_t size = strlen(base) + 3;
    for (ASTNode *argument = type->type_arguments; argument; argument = argument->next) {
        char *text = describe_type((VarTypeNode *)argument);
        size += strlen(text) + 2;
        free(text);
    }
    char *result = (char *)malloc(size);
    if (!result) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    size_t used = (size_t)snprintf(result, size, "%s<", base);
    for (ASTNode *argument = type->type_arguments; argument; argument = argument->next) {
        char *text = describe_type((VarTypeNode *)argument);
        used += (size_t)snprintf(result + used, size - used, "%s%s",
                                 argument == type->type_arguments ? "" : ", ", text);
        free(text);
    }
    snprintf(result + used, size - used, ">");
    return result;
}

// 在程序里按名字查 trait 声明。
static TraitNode *find_trait(ProgramNode *program, const char *name) {
    for (ASTNode *node = program ? program->traits : NULL; node; node = node->next) {
        TraitNode *trait_node = (TraitNode *)node;
        if (strcmp(trait_node->name, name) == 0) return trait_node;
    }
    return NULL;
}

// 在 trait 里按名字查方法签名。
static TraitMethodNode *find_trait_method(TraitNode *trait_node, const char *name) {
    for (ASTNode *node = trait_node ? trait_node->methods : NULL;
         node; node = node->next) {
        TraitMethodNode *method = (TraitMethodNode *)node;
        if (strcmp(method->name, name) == 0) return method;
    }
    return NULL;
}

// 取回 impl 方法在源语言里的名字。
//
// 方法在解析时就被改名成 `<方法名>$<类型名>`，进了模块还会再套一层 `__tap_module_N.`。
// 所以先按**最后一个点**切掉模块前缀，再按**第一个 `$`** 切掉类型后缀 —— 和 Codegen 的
// 方法查找（method_name_matches）用的是同一套切分规则。
static char *impl_method_source_name(const FunctionNode *method) {
    const char *dot = strrchr(method->name, '.');
    const char *base = dot ? dot + 1 : method->name;
    const char *dollar = strchr(base, '$');
    size_t length = dollar ? (size_t)(dollar - base) : strlen(base);
    char *result = (char *)malloc(length + 1);
    if (!result) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    memcpy(result, base, length);
    result[length] = '\0';
    return result;
}

// 校验每个 impl 块里的方法和 trait 声明的签名一致。
//
// 必须在**所有模块加载完、类型改名之后**跑：trait 和 impl 可能来自不同模块，而且两边
// 引用的类型名要先改写完才能直接比对。比较项是「接收者 + 参数个数 + 每个参数类型 +
// 返回类型」；关联类型和默认方法 v1 都没有，所以不涉及。
//
// trait 名找不到时这里不报错（那属于「用了没定义的 trait」，是另一回事），直接跳过。
int validate_impl_signatures(ProgramNode *program) {
    for (ASTNode *node = program ? program->impls : NULL; node; node = node->next) {
        ImplNode *impl_node = (ImplNode *)node;
        TraitNode *trait_node = find_trait(program, impl_node->trait_name);
        if (!trait_node) continue;

        for (ImplMethodLink *link = impl_node->methods; link; link = link->next) {
            FunctionNode *method = link->function;
            char *method_name = impl_method_source_name(method);
            TraitMethodNode *signature = find_trait_method(trait_node, method_name);
            if (!signature) {
                print_diagnostic(stderr, "error", method->filename, method->line,
                                 method->column,
                                 "method '%s' is not a member of trait '%s'",
                                 method_name, impl_node->trait_name); // 中文：该方法不属于这个 trait
                free(method_name);
                return 1;
            }

            unsigned declared = node_count(signature->param_types) +
                                (signature->has_self ? 1u : 0u);
            unsigned actual = node_count(method->params);
            if (declared != actual) {
                print_diagnostic(stderr, "error", method->filename, method->line,
                                 method->column,
                                 "method '%s' has %u parameters, but trait '%s' declares %u",
                                 method_name, actual, impl_node->trait_name, declared); // 中文：参数个数与 trait 声明不一致
                free(method_name);
                return 1;
            }

            VarTypeNode *impl_param = (VarTypeNode *)method->param_types;
            ASTNode *trait_param = signature->param_types;
            unsigned index = 1;

            // 接收者：trait 里写的是 `self`（不带类型），impl 里必须写成目标类型本身，
            // 否则方法查找（首参类型 == 接收者类型）根本找不到它。
            if (signature->has_self && impl_param) {
                const char *named = var_type_named(impl_param);
                if (!named || strcmp(named, impl_node->type_name) != 0) {
                    char *actual_type = describe_type(impl_param);
                    print_diagnostic(stderr, "error", method->filename, method->line,
                                     method->column,
                                     "method '%s' takes '%s' as its receiver, but impl is for '%s'",
                                     method_name, actual_type,
                                     strip_module_prefix(impl_node->type_name)); // 中文：接收者类型与 impl 的目标类型不一致
                    free(actual_type);
                    free(method_name);
                    return 1;
                }
                impl_param = (VarTypeNode *)impl_param->base.next;
                index++;
            }

            while (trait_param && impl_param) {
                if (!type_equal((VarTypeNode *)trait_param, impl_param)) {
                    char *expected = describe_type((VarTypeNode *)trait_param);
                    char *actual_type = describe_type(impl_param);
                    print_diagnostic(stderr, "error", method->filename, method->line,
                                     method->column,
                                     "method '%s' parameter %u has type '%s', but trait '%s' declares '%s'",
                                     method_name, index, actual_type,
                                     impl_node->trait_name, expected); // 中文：参数类型与 trait 声明不一致
                    free(expected);
                    free(actual_type);
                    free(method_name);
                    return 1;
                }
                trait_param = trait_param->next;
                impl_param = (VarTypeNode *)impl_param->base.next;
                index++;
            }

            if (!type_equal(method->return_type, signature->return_type)) {
                char *expected = describe_type(signature->return_type);
                char *actual_type = describe_type(method->return_type);
                print_diagnostic(stderr, "error", method->filename, method->line,
                                 method->column,
                                 "method '%s' returns '%s', but trait '%s' declares '%s'",
                                 method_name, actual_type,
                                 impl_node->trait_name, expected); // 中文：返回类型与 trait 声明不一致
                free(expected);
                free(actual_type);
                free(method_name);
                return 1;
            }
            free(method_name);
        }

        // 完整性：trait 里声明的每个方法，impl 里都得有 —— 缺一个就不算实现了这个 trait。
        // （约束检查只看 impl 记录在不在，所以这里必须兜住。）放在逐方法比对之后，
        // 这样「写了 trait 里没有的方法」会先报到更贴切的那条诊断上。
        for (ASTNode *declared = trait_node->methods; declared; declared = declared->next) {
            TraitMethodNode *signature = (TraitMethodNode *)declared;
            int implemented = 0;
            for (ImplMethodLink *link = impl_node->methods; link; link = link->next) {
                char *name = impl_method_source_name(link->function);
                if (strcmp(name, signature->name) == 0) implemented = 1;
                free(name);
                if (implemented) break;
            }
            if (!implemented) {
                print_diagnostic(stderr, "error", impl_node->filename, impl_node->line,
                                 impl_node->column,
                                 "impl of trait '%s' for '%s' is missing method '%s'",
                                 impl_node->trait_name,
                                 strip_module_prefix(impl_node->type_name),
                                 signature->name); // 中文：impl 缺少 trait 要求的方法
                return 1;
            }
        }
    }
    return 0;
}

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

    // 类型实参推断完了，检查它们是否满足 `T: Trait` 这类约束。
    check_type_bounds(context, call, template, bindings);

    char *name = specialized_name(template, bindings);
    FunctionNode *specialized = find_function(context->program, name);
    if (!specialized) {
        specialized = instantiate_function(template, bindings, name);
        materialize_function(context, specialized);
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

// 返回模块内部函数名的最后一段，并忽略单态化后缀。
static int function_base_matches(const char *function_name, const char *method_name) {
    const char *base = strrchr(function_name, '.');
    base = base ? base + 1 : function_name;
    const char *suffix = strchr(base, '$');
    size_t length = suffix ? (size_t)(suffix - base) : strlen(base);
    return strlen(method_name) == length && strncmp(base, method_name, length) == 0;
}

// 查找 receiver.method(...) 对应的泛型函数模板。
static FunctionNode *find_generic_method_template(
    GenericContext *context, const char *method_name,
    const VarTypeNode *receiver_type) {
    for (ASTNode *node = context->program->functions; node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        if (function->type_params && function->param_types &&
            function_base_matches(function->name, method_name)) {
            TypeBinding *bindings = NULL;
            int matches = unify_type(
                function, (VarTypeNode *)function->param_types,
                receiver_type, &bindings, 0);
            free_bindings(bindings);
            if (matches) return function;
        }
    }
    return NULL;
}

// 根据结构体接收者和用户参数实例化泛型方法，调用名保留为 receiver.method。
static FunctionNode *specialize_method_call(
    GenericContext *context, FunctionCallNode *call,
    const VarTypeNode *expected) {
    // 接收者类型若是模块私有、被改名成 `__tap_module_N.Type`，方法名会变成
    // 包含多个点的形状。按**最后一个**点切分，前缀才是正确的接收者类型名；
    // 拒绝多点的写法会让泛型方法在模块私有类型上无法实例化。
    const char *dot = strrchr(call->name, '.');
    if (!dot || dot == call->name) return NULL;

    size_t receiver_length = (size_t)(dot - call->name);
    char *receiver_name = malloc(receiver_length + 1);
    if (!receiver_name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    memcpy(receiver_name, call->name, receiver_length);
    receiver_name[receiver_length] = '\0';
    TypeSymbol *receiver = find_symbol(context, receiver_name);
    free(receiver_name);
    if (!receiver) return NULL;

    FunctionNode *template = find_generic_method_template(
        context, dot + 1, receiver->type);
    if (!template) return NULL;
    if (node_count(call->arguments) + 1 != node_count(template->params)) {
        generic_call_error(call,
            "generic method argument count mismatch for '%s'", dot + 1);
    }

    unsigned type_param_count = node_count(template->type_params);
    unsigned explicit_count = node_count(call->type_arguments);
    if (explicit_count > 0 && explicit_count != type_param_count) {
        generic_call_error(call,
            "generic function type argument count mismatch for '%s'", dot + 1);
    }

    TypeBinding *bindings = NULL;
    ASTNode *type_parameter = template->type_params;
    ASTNode *type_argument = call->type_arguments;
    while (type_parameter && type_argument) {
        if (!bind_type(&bindings, ((IdentifierNode *)type_parameter)->name,
                       (VarTypeNode *)type_argument)) {
            generic_call_error(call,
                "conflicting generic type arguments for '%s'", dot + 1);
        }
        type_parameter = type_parameter->next;
        type_argument = type_argument->next;
    }

    if (!unify_type(template, (VarTypeNode *)template->param_types,
                    receiver->type, &bindings, 0)) {
        generic_call_error(call,
            "cannot infer compatible generic receiver for '%s'", dot + 1);
    }

    ASTNode *argument = call->arguments;
    ASTNode *parameter_type = template->param_types->next;
    while (argument && parameter_type) {
        VarTypeNode *actual = process_expression(context, argument, NULL);
        if (!actual || !unify_type(template, (VarTypeNode *)parameter_type,
                                   actual, &bindings, 0)) {
            free_ast((ASTNode *)actual);
            generic_call_error(call,
                "cannot infer compatible generic arguments for '%s'", dot + 1);
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
        materialize_function(context, specialized);
        add_function(context->program, specialized);
    }
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
// 查找 receiver.method(...) 对应的普通（非泛型）方法：首参类型和接收者一致的顶层函数。
//
// 少了这一步，非泛型方法调用会一路落到 process_call 末尾、被当成默认的 i32，
// 和 Codegen 侧推断出的真实类型对不上（例如 `let q = p.shift(4);` 之后的泛型调用）。
static FunctionNode *find_non_generic_method(
    GenericContext *context, const char *call_name) {
    const char *dot = strrchr(call_name, '.');
    if (!dot || dot == call_name) return NULL;

    size_t receiver_length = (size_t)(dot - call_name);
    char *receiver_name = (char *)malloc(receiver_length + 1);
    if (!receiver_name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    memcpy(receiver_name, call_name, receiver_length);
    receiver_name[receiver_length] = '\0';
    TypeSymbol *receiver = find_symbol(context, receiver_name);
    const VarTypeNode *receiver_type = receiver ? receiver->type : NULL;
    free(receiver_name);
    if (!receiver_type) return NULL;

    const char *method_name = dot + 1;
    for (ASTNode *node = context->program->functions; node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        if (function->type_params || !function->param_types) continue;
        if (!function_base_matches(function->name, method_name)) continue;
        if (type_equal((VarTypeNode *)function->param_types, receiver_type)) {
            return function;
        }
    }
    return NULL;
}

// 泛型结构体字面量没写类型实参时，用字段值反推。
//
// `Box { value: 42 }` 里 `value` 的声明类型是 `T`、初始化表达式是 i32，unify 得到 T = i32；
// 结果写回 literal->type_arguments，和显式写 `Box<i32> { value: 42 }` 完全等价。
// 推不出来的类型参数不写 —— Codegen 会照常报「requires type arguments」。
static void infer_struct_literal_arguments(
    GenericContext *context, StructNode *struct_node, StructLiteralNode *literal) {
    TypeBinding *bindings = NULL;
    for (ASTNode *node = literal->fields; node; node = node->next) {
        StructInitFieldNode *field = (StructInitFieldNode *)node;
        StructFieldNode *definition = find_field(struct_node, field->name);
        if (!definition) continue;
        VarTypeNode *actual = process_expression(
            context, field->expression, definition->field_type);
        if (actual) {
            unify_type_params(struct_node->type_params, definition->field_type,
                              actual, &bindings, 0);
        }
        free_ast((ASTNode *)actual);
    }
    for (ASTNode *node = struct_node->type_params; node; node = node->next) {
        const char *name = ((IdentifierNode *)node)->name;
        TypeBinding *binding = find_binding(bindings, name);
        if (!binding) continue;
        add_struct_literal_type_argument(literal, clone_type(binding->type));
    }
    free_bindings(bindings);
}

// 无目标类型时从元素反推数组类型（`[1, 2, 3]` → `[i32; 3]`）。
//
// 元素类型取第一个元素的完整类型，其余元素按它逐项处理 —— 结构体字面量、泛型调用这些
// 需要目标类型的东西就靠这一步拿到类型。空数组推不出来，返回 NULL。
static VarTypeNode *infer_array_literal_type(
    GenericContext *context, ArrayLiteralNode *array) {
    if (!array->elements) return NULL;

    VarTypeNode *element_type = process_expression(context, array->elements, NULL);
    if (!element_type) return NULL;

    for (ASTNode *element = array->elements->next; element; element = element->next) {
        VarTypeNode *ignored = process_expression(context, element, element_type);
        free_ast((ASTNode *)ignored);
    }
    // 长度和 Codegen 用同一套口径：`[v; N]` 取重复次数，其余取元素个数。
    uint64_t length = array->is_repeat ? array->repeat_count : array->count;
    return create_array_type(element_type, length);
}

static VarTypeNode *process_call(GenericContext *context,
                                 FunctionCallNode *call,
                                 const VarTypeNode *expected) {
    FunctionNode *function = find_function(context->program, call->name);
    if (function && function->type_params) {
        function = specialize_call(context, call, function, expected);
        return clone_type(function->return_type);
    }
    if (!function) {
        function = specialize_method_call(context, call, expected);
        if (function) return clone_type(function->return_type);
        function = find_non_generic_method(context, call->name);
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
    if (strcmp(call->name, "__tap_builtin_string_char_count") == 0 ||
        strstr(call->name, ".char_count")) {
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
//
// **注意：这里和 Codegen 的 `expression_type` / `infer_expression_var_type` 是两份并行的
// 推断实现。** 单态化按这里的结论挑实例，Codegen 按那边的结论生成代码 —— 同一个表达式
// 两边必须给出一致的类型，否则**不报错，而是静默选错单态化实例**（2026-10-11 修过一次：
// 非泛型方法调用在这边落到默认 i32，那边推出来是真实的 Point，于是 `identity(q)` 被单态化
// 成 i32 版本）。改动任一侧的覆盖范围时，务必同步另一侧。
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
        case NODE_FIELD_ACCESS: {
            // `接收者.字段`：先推接收者的类型，再按名字查字段。
            FieldAccessNode *access = (FieldAccessNode *)expression;
            VarTypeNode *object_type = process_expression(
                context, access->object, NULL);
            VarTypeNode *result = NULL;
            if (object_type && object_type->struct_name) {
                StructNode *struct_node =
                    find_struct(context->program, object_type->struct_name);
                StructFieldNode *field = struct_node
                    ? find_field(struct_node, access->field)
                    : NULL;
                if (field) result = clone_type(field->field_type);
            }
            free_ast((ASTNode *)object_type);
            return result;
        }
        case NODE_TRY: {
            // 内层按它自己的签名推断，不要把 `?` 的目标类型传下去：内层是 Result<T, string>，
            // 而目标类型是 T，硬统一会失败。
            VarTypeNode *inner = process_expression(
                context, ((TryNode *)expression)->inner, NULL);
            free_ast((ASTNode *)inner);
            return expected ? clone_type(expected) : NULL;
        }
        case NODE_MATCH_STATEMENT: {
            // match 作表达式：分支值里的泛型调用要在这里特化。
            MatchStatementNode *node = (MatchStatementNode *)expression;
            VarTypeNode *matched = process_expression(
                context, node->expression, NULL);
            for (ASTNode *arm = node->arms; arm; arm = arm->next) {
                MatchArmNode *arm_node = (MatchArmNode *)arm;
                // 和语句形式一样：**先**登记载荷绑定，分支值里才能解析出接收者类型。
                bind_match_payloads(context, matched, arm_node);
                VarTypeNode *arm_value = process_expression(
                    context, arm_node->value, NULL);
                free_ast((ASTNode *)arm_value);
            }
            free_ast((ASTNode *)matched);
            return expected ? clone_type(expected) : NULL;
        }
        case NODE_REFERENCE: {
            VarTypeNode *target = process_expression(
                context, ((ReferenceNode *)expression)->target, NULL);
            return target ? create_pointer_type(target) : NULL;
        }
        case NODE_SIZEOF:
            return create_var_type(LITERAL_UINT);
        case NODE_CAST: {
            CastNode *cast = (CastNode *)expression;
            VarTypeNode *inner = process_expression(context, cast->expression, NULL);
            free_ast((ASTNode *)inner);
            return cast->target_type ? clone_type(cast->target_type) : NULL;
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
            // 有目标类型时按元素类型逐项处理；没有就从元素反推（`[1, 2, 3]` → [i32; 3]）。
            if (expected) {
                for (ASTNode *element = array->elements; element;
                     element = element->next) {
                    VarTypeNode *ignored = process_expression(
                        context, element, expected->element_type);
                    free_ast((ASTNode *)ignored);
                }
                return clone_type(expected);
            }
            return infer_array_literal_type(context, array);
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
            // 泛型结构体没写类型实参时从字段值反推（`Box { value: 42 }` → Box<i32>），
            // 写回 literal->type_arguments，后面的物化阶段就能按实例化后的类型改名。
            if (struct_node && struct_node->type_params && !literal->type_arguments) {
                infer_struct_literal_arguments(context, struct_node, literal);
            }
            VarTypeNode *type = create_struct_type(literal->struct_name);
            for (ASTNode *argument = literal->type_arguments; argument;
                 argument = argument->next) {
                add_var_type_argument(type, clone_type((VarTypeNode *)argument));
            }
            return type;
        }
        default:
            return expected ? clone_type(expected) : NULL;
    }
}

// 处理语句链中的泛型调用并维护局部类型环境。
// 按名字在枚举里找成员。
static EnumVariantNode *find_variant(EnumNode *enum_node, const char *name) {
    for (ASTNode *node = enum_node ? enum_node->variants : NULL;
         node; node = node->next) {
        if (strcmp(((EnumVariantNode *)node)->name, name) == 0) {
            return (EnumVariantNode *)node;
        }
    }
    return NULL;
}

// 按下标取结构体字段。
static StructFieldNode *struct_field_at(StructNode *structure, unsigned index) {
    unsigned current = 0;
    for (ASTNode *node = structure ? structure->fields : NULL;
         node; node = node->next, current++) {
        if (current == index) return (StructFieldNode *)node;
    }
    return NULL;
}

// 把 match 分支的载荷绑定（`Holder.Item(inner)` 里的 inner）登记进类型环境。
//
// 不登记的话，分支体里的 `inner.len()` 找不到接收者类型，泛型方法 `len` 不会被特化，
// Codegen 的 find_method_function 就匹配不上，最后误报成
// 「method 'len' is only available on string values」。
//
// 载荷类型取自**降级后的内部结构体**：变体首个载荷字段的下标是 variant->field_index，
// 第 offset 个绑定对应 field_index + offset（和 Codegen 的绑定逻辑一致）。
//
// 这里拿到的是**尚未 materialize** 的字段类型（process_statements 跑在 specialize_generics
// 的第一阶段，结构体字段要到第二阶段才 materialize）。这是可行的：函数参数走的是同一条
// 路——process_function 登记的也是未 materialize 的 param_types，而
// `fn f(v: Vec<i32>) { v.len(); }` 一直是正常的。
static void bind_match_payloads(
    GenericContext *context, const VarTypeNode *matched, MatchArmNode *arm) {
    if (!matched) return;
    if (!arm->variant_name || !arm->bindings) return; // 通配分支没有绑定

    // 被匹配值的类型有两种记法：泛型阶段按**枚举**名记（`enum_name = "Holder"`），
    // Codegen 阶段按**降级后的结构体**名记（`struct_name`）。两种都要认，否则
    // 这里会静默 return，绑定登记不上，症状和没修一样。
    EnumNode *enum_node = NULL;
    StructNode *structure = NULL;
    if (matched->enum_name) {
        enum_node = find_enum(context->program, matched->enum_name);
        structure = find_struct(context->program, matched->enum_name);
    } else if (matched->struct_name) {
        structure = find_struct(context->program, matched->struct_name);
        if (structure && structure->is_tagged_enum) {
            const char *enum_name = structure->tagged_enum_name
                ? structure->tagged_enum_name : structure->name;
            enum_node = find_enum(context->program, enum_name);
        }
    }
    if (!enum_node || !structure) return;

    EnumVariantNode *variant = find_variant(enum_node, arm->variant_name);
    if (!variant) return;

    // 泛型枚举（`Result<Vec<i32>, string>`）的载荷字段类型是**类型参数**（`T`/`E`），
    // 直接登记进去等于登记了个 `T`，分支体里照样解析不出接收者类型。必须按被匹配值的
    // 类型实参替换成具体类型。非泛型枚举时 bindings 为空，substitute_type 就是 clone。
    TypeBinding *type_bindings = NULL;
    ASTNode *parameter = enum_node->type_params;
    ASTNode *argument = matched->type_arguments;
    while (parameter && argument) {
        bind_type(&type_bindings, ((IdentifierNode *)parameter)->name,
                  (VarTypeNode *)argument);
        parameter = parameter->next;
        argument = argument->next;
    }
    FunctionNode substitution_scope = {0};
    substitution_scope.type_params = enum_node->type_params;

    unsigned offset = 0;
    for (ASTNode *binding = arm->bindings; binding; binding = binding->next, offset++) {
        StructFieldNode *field =
            struct_field_at(structure, variant->field_index + offset);
        if (!field) break;
        VarTypeNode *payload_type = substitute_type(
            &substitution_scope, field->field_type, type_bindings);
        add_symbol(context, ((IdentifierNode *)binding)->name, payload_type);
        free_ast((ASTNode *)payload_type);
    }
    free_bindings(type_bindings);
}

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
            case NODE_MATCH_STATEMENT: {
                // 分支体是函数体的一部分，泛型方法调用同样需要在这里特化。
                MatchStatementNode *match_node = (MatchStatementNode *)statement;
                VarTypeNode *matched = process_expression(
                    context, match_node->expression, NULL);
                for (ASTNode *arm = match_node->arms; arm; arm = arm->next) {
                    MatchArmNode *arm_node = (MatchArmNode *)arm;
                    // **先**登记载荷绑定再处理分支体，顺序不能反：分支体里的
                    // `inner.len()` 要靠这个类型才能找到并特化泛型方法。
                    bind_match_payloads(context, matched, arm_node);
                    process_statements(context, arm_node->body, return_type);
                    // match 作表达式时分支值在 value 里：里面的泛型调用同样要特化，
                    // 漏了会让 Codegen 找不到那个函数（报 struct expression type mismatch）。
                    VarTypeNode *arm_value_type = process_expression(
                        context, arm_node->value, NULL);
                    free_ast((ASTNode *)arm_value_type);
                }
                free_ast((ASTNode *)matched);
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

// 从程序中移除仅供单态化使用的泛型结构体模板。
static void remove_struct_templates(ProgramNode *program) {
    ASTNode **link = &program->structs;
    while (*link) {
        StructNode *structure = (StructNode *)*link;
        if (!structure->type_params) {
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
    for (ASTNode *node = program ? program->structs : NULL;
         node; node = node->next) {
        StructNode *structure = (StructNode *)node;
        if (!structure->type_params) {
            for (ASTNode *field = structure->fields; field; field = field->next) {
                materialize_type(
                    &context, ((StructFieldNode *)field)->field_type);
            }
        }
    }
    for (ASTNode *node = program ? program->constants : NULL;
         node; node = node->next) {
        VarDeclNode *constant = (VarDeclNode *)node;
        materialize_type(&context, constant->type);
        materialize_expression(&context, constant->expression);
    }
    for (ASTNode *node = program ? program->functions : NULL;
         node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        if (!function->type_params) materialize_function(&context, function);
    }
    free_symbols(context.symbols);
    remove_templates(program);
    remove_struct_templates(program);
    return 0;
}
