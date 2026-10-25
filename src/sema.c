#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "helpers.h"
#include "types.h"

// Sema 的符号表。
//
// 和 Codegen 的 Symbol 表不同：那个表要存 LLVM 槽位，这个表**只需要类型**。
// 名字借用 AST（AST 活到编译结束），类型由符号表拥有，离开作用域时释放。
typedef struct SemaSymbol {
    const char *name;
    VarTypeNode *type;
    struct SemaSymbol *next;
} SemaSymbol;

typedef struct {
    ProgramNode *program;
    SemaSymbol *symbols;
} SemaContext;

// 覆盖率统计：只在 TAP_SEMA_STATS=1 时输出。
static unsigned sema_filled = 0;
static unsigned sema_missed = 0;

static SemaSymbol *find_symbol(SemaContext *context, const char *name) {
    for (SemaSymbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) return symbol;
    }
    return NULL;
}

// 接管 type 的所有权；离开作用域时由 pop_symbols_to 释放。
static void insert_symbol(SemaContext *context, const char *name, VarTypeNode *type) {
    if (!name || !type) return;
    SemaSymbol *symbol = (SemaSymbol *)malloc(sizeof(SemaSymbol));
    if (!symbol) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    symbol->name = name;
    symbol->type = type;
    symbol->next = context->symbols;
    context->symbols = symbol;
}

static SemaSymbol *symbol_scope_mark(SemaContext *context) {
    return context->symbols;
}

static void pop_symbols_to(SemaContext *context, SemaSymbol *mark) {
    while (context->symbols != mark) {
        SemaSymbol *next = context->symbols->next;
        free_ast((ASTNode *)context->symbols->type);
        free(context->symbols);
        context->symbols = next;
    }
}

// 纯标量：没有结构体 / 枚举 / 数组 / 指针外壳。
static int is_scalar_type(const VarTypeNode *type) {
    return type && !type->struct_name && !type->enum_name &&
           !type->is_array && !type->is_pointer;
}

static FunctionNode *find_function(ProgramNode *program, const char *name) {
    if (!program || !name) return NULL;
    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        if (function->name && strcmp(function->name, name) == 0) return function;
    }
    return NULL;
}

static VarTypeNode *analyze_expression(SemaContext *context, ASTNode *expression);

// 只求值、不关心类型结果时的便捷包装。
static void analyze_expression_only(SemaContext *context, ASTNode *expression) {
    free_ast((ASTNode *)analyze_expression(context, expression));
}

// 真正做推断的部分。返回新分配的完整类型，调用方负责 free_ast；推不出来返回 NULL，
// 由 Codegen 自己兜底 —— 这样 Sema 猜错也绝不会影响生成结果。
static VarTypeNode *compute_expression_type(SemaContext *context, ASTNode *expression) {
    switch (expression->type) {
        case NODE_LITERAL:
            return create_var_type(((LiteralNode *)expression)->literal_type);

        case NODE_IDENTIFIER: {
            const char *name = ((IdentifierNode *)expression)->name;
            SemaSymbol *symbol = find_symbol(context, name);
            if (symbol) return tap_type_clone(symbol->type);

            // 顶层常量不在局部符号表里。
            for (ASTNode *node = context->program->constants; node; node = node->next) {
                VarDeclNode *constant = (VarDeclNode *)node;
                if (constant->type && constant->name &&
                    strcmp(constant->name, name) == 0) {
                    return tap_type_clone(constant->type);
                }
            }
            // 枚举成员、多级字段访问、字符串方法接收者等留给 Codegen。
            return NULL;
        }

        case NODE_CAST: {
            CastNode *cast = (CastNode *)expression;
            return is_scalar_type(cast->target_type)
                ? tap_type_clone(cast->target_type) : NULL;
        }

        case NODE_SIZEOF:
            return create_var_type(LITERAL_UINT);

        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            if (binary->op_type >= OP_EQUAL &&
                binary->op_type <= OP_GREATER_THAN_OR_EQUAL) {
                return create_var_type(LITERAL_BOOL);
            }
            if (binary->op_type == OP_AND || binary->op_type == OP_OR) {
                return create_var_type(LITERAL_BOOL);
            }

            VarTypeNode *left = analyze_expression(context, binary->left);
            VarTypeNode *right = analyze_expression(context, binary->right);
            VarTypeNode *result = NULL;
            // 只在两边类型完全相同时给出结果。有符号/无符号混合和隐式提升的规则留在
            // Codegen（common_integer_type），这里猜错会让缓存和实际生成对不上。
            if (left && right && is_scalar_type(left) && is_scalar_type(right) &&
                left->type == right->type) {
                result = create_var_type(left->type);
            }
            free_ast((ASTNode *)left);
            free_ast((ASTNode *)right);
            return result;
        }

        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            // 方法调用、字符串内建、枚举构造都要接收者类型或目标类型，留给 Codegen。
            FunctionNode *function = find_function(context->program, call->name);
            if (!function || !function->return_type) return NULL;
            return tap_type_clone(function->return_type);
        }

        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)expression;
            VarTypeNode *container = analyze_expression(context, index->array);
            analyze_expression_only(context, index->index);
            if (!container || (!container->is_array && !container->is_pointer)) {
                free_ast((ASTNode *)container);
                return NULL;
            }
            VarTypeNode *element = tap_type_clone(container->element_type);
            free_ast((ASTNode *)container);
            return element;
        }

        case NODE_STRUCT_LITERAL:
            return create_struct_type(((StructLiteralNode *)expression)->struct_name);

        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)expression;
            if (!array->elements) return NULL;
            VarTypeNode *element = analyze_expression(context, array->elements);
            if (!element) return NULL;
            uint64_t length = array->is_repeat ? array->repeat_count : array->count;
            return create_array_type(element, length);
        }

        case NODE_FIELD_ACCESS: {
            FieldAccessNode *access = (FieldAccessNode *)expression;
            VarTypeNode *object = analyze_expression(context, access->object);
            if (!object || !object->struct_name) {
                free_ast((ASTNode *)object);
                return NULL;
            }
            StructNode *structure =
                tap_find_struct(context->program, object->struct_name);
            StructFieldNode *field = structure
                ? tap_find_struct_field(structure, access->field, NULL) : NULL;
            free_ast((ASTNode *)object);
            return field ? tap_type_clone(field->field_type) : NULL;
        }

        case NODE_REFERENCE: {
            ReferenceNode *reference = (ReferenceNode *)expression;
            VarTypeNode *target = analyze_expression(context, reference->target);
            if (!target) return NULL;
            return create_pointer_type(target); // 接管 target 的所有权
        }

        default:
            // match / try 需要目标类型才能定结果，其余节点同理 —— 一律交给 Codegen。
            return NULL;
    }
}

static VarTypeNode *analyze_expression(SemaContext *context, ASTNode *expression) {
    if (!expression) return NULL;
    if (expression->resolved_type) return tap_type_clone(expression->resolved_type);

    VarTypeNode *type = compute_expression_type(context, expression);
    if (type) {
        expression->resolved_type = tap_type_clone(type);
        sema_filled++;
    } else {
        sema_missed++;
    }
    return type;
}

static void analyze_statement_list(SemaContext *context, ASTNode *statement);

static void analyze_statement(SemaContext *context, ASTNode *statement) {
    switch (statement->type) {
        case NODE_VAR_DECL: {
            VarDeclNode *declaration = (VarDeclNode *)statement;
            VarTypeNode *inferred = analyze_expression(context, declaration->expression);

            // 有标注以标注为准，否则用推断结果。推不出来就不登记 —— 后续引用会返回
            // NULL 并落到 Codegen 的推断路径，比在这里猜一个默认类型安全。
            VarTypeNode *type = declaration->type
                ? tap_type_clone(declaration->type)
                : (inferred ? tap_type_clone(inferred) : NULL);
            free_ast((ASTNode *)inferred);
            if (type) insert_symbol(context, declaration->name, type);
            break;
        }

        case NODE_ASSIGNMENT:
            analyze_expression_only(context, ((AssignmentNode *)statement)->expression);
            break;

        case NODE_IF_STATEMENT: {
            IfStatementNode *if_node = (IfStatementNode *)statement;
            analyze_expression_only(context, if_node->condition);

            SemaSymbol *mark = symbol_scope_mark(context);
            analyze_statement_list(context, if_node->consequence);
            pop_symbols_to(context, mark);

            if (if_node->alternative) {
                mark = symbol_scope_mark(context);
                analyze_statement_list(context, if_node->alternative);
                pop_symbols_to(context, mark);
            }
            break;
        }

        case NODE_FOR_STATEMENT: {
            ForStatementNode *for_node = (ForStatementNode *)statement;
            SemaSymbol *mark = symbol_scope_mark(context);
            analyze_statement_list(context, for_node->initializer);
            analyze_expression_only(context, for_node->condition);
            analyze_statement_list(context, for_node->update);
            analyze_statement_list(context, for_node->body);
            pop_symbols_to(context, mark);
            break;
        }

        case NODE_RETURN:
            analyze_expression_only(context, ((ReturnNode *)statement)->expression);
            break;

        case NODE_MATCH_STATEMENT: {
            MatchStatementNode *match_node = (MatchStatementNode *)statement;
            analyze_expression_only(context, match_node->expression);
            for (ASTNode *node = match_node->arms; node; node = node->next) {
                MatchArmNode *arm = (MatchArmNode *)node;
                SemaSymbol *mark = symbol_scope_mark(context);
                // 载荷绑定的类型要先知道枚举变体，暂不登记：arm 里引用绑定变量时
                // 会返回 NULL 并落到 Codegen。
                analyze_statement_list(context, arm->body);
                analyze_expression_only(context, arm->value);
                pop_symbols_to(context, mark);
            }
            break;
        }

        default:
            // 表达式语句（函数调用等）。
            analyze_expression_only(context, statement);
            break;
    }
}

static void analyze_statement_list(SemaContext *context, ASTNode *statement) {
    for (; statement; statement = statement->next) {
        analyze_statement(context, statement);
    }
}

static void analyze_function(SemaContext *context, FunctionNode *function) {
    if (!function || function->is_extern) return;

    context->symbols = NULL;

    ASTNode *param = function->params;
    ASTNode *param_type = function->param_types;
    while (param && param_type) {
        insert_symbol(context, ((IdentifierNode *)param)->name,
                      tap_type_clone((VarTypeNode *)param_type));
        param = param->next;
        param_type = param_type->next;
    }

    analyze_statement_list(context, function->body);
    pop_symbols_to(context, NULL);
}

int analyze_program(ProgramNode *program) {
    if (!program) return 0;

    SemaContext context;
    context.program = program;
    context.symbols = NULL;

    sema_filled = 0;
    sema_missed = 0;

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        analyze_function(&context, (FunctionNode *)node);
    }

    const char *stats = getenv("TAP_SEMA_STATS");
    if (stats && stats[0] != '\0') {
        unsigned total = sema_filled + sema_missed;
        fprintf(stderr, "sema: %u/%u expressions typed (%.0f%%)\n",
                sema_filled, total,
                total ? (100.0 * (double)sema_filled / (double)total) : 100.0);
    }
    return 0;
}
