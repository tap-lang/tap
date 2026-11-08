#include "names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 名字归一：把「根确定是当前函数局部变量」的点号标识符改写成字段访问链。
//
// 背景：parser 看到 `a.b.c` 时把整串拼成一个 IdentifierNode 的 name。这在当时是唯一
// 省事的做法，但下游每个阶段都得再用 strchr 拆一遍（全仓 50 多处），而且「点」在
// 这里同时表示三件事：字段访问、模块成员、类型成员（枚举分支）。同一个字符串在
// 不同位置被当成不同的东西，是这套语法里最贵的一处历史包袱。
//
// 为什么不能直接在 parser 里分：三个语言事实决定了解析期无法判断 `a.b` 里的 `a`
// 是什么 ——
//   1. 类型可以先用后声明（`fn f(): Point` 写在 `struct Point` 之前是合法的）；
//   2. import 可以写在文件末尾，模块别名在解析到某一处时可能还没出现；
//   3. 变量可以和模块别名重名（`import std.vec as vec;` 之后 `let vec: i32` 合法）。
//
// 所以这里在**解析完成、模块装载完成之后**跑一趟，用「这一层已知的局部名字」做
// **正向判定**：只有根确实是局部变量时才改写；拿不准就原样保留，继续走原来的字符串
// 路径。方向是安全的 —— 最坏情况是「少改」，不会把模块名误当字段。
//
// 改写之后：
//   - 读：`p.inner.value` → FIELD_ACCESS(FIELD_ACCESS(p, inner), value)；
//   - 写：`p.x = v` 仍然是 AssignmentNode 的字符串形式（它要一个新节点，
//     和这一步分开做，见 docs/todo/language.md）；
//   - 模块/类型成员（`math.PI`、`Color.Red`）不动 —— 它们本来就不是字段链。

typedef struct {
    const char **items;
    int count;
    int capacity;
} NameSet;

typedef struct {
    NameSet locals;       // 当前函数的局部名字：参数、let/const、match 绑定
    NameSet namespaces;   // 模块别名 / 类型名 / 顶层函数与常量：这些绝不当字段链
} NameContext;

static void name_set_add(NameSet *set, const char *name) {
    if (!name || !name[0]) return;
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->items[i], name) == 0) return;
    }
    if (set->count == set->capacity) {
        int capacity = set->capacity ? set->capacity * 2 : 16;
        const char **items =
            (const char **)realloc(set->items, sizeof(char *) * (size_t)capacity);
        if (!items) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        set->items = items;
        set->capacity = capacity;
    }
    set->items[set->count++] = name;
}

// 名字集合里是否有恰好长度为 length 的 name 前缀项。
static int name_set_contains(const NameSet *set, const char *name, size_t length) {
    for (int i = 0; i < set->count; i++) {
        const char *item = set->items[i];
        if (strlen(item) == length && strncmp(item, name, length) == 0) return 1;
    }
    return 0;
}

static void name_set_free(NameSet *set) {
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

// 把 `a.b.c` 拆成 IdentifierNode(a) + 两层 FIELD_ACCESS。
//
// 段太长（>255）时放弃改写，返回原节点：正常代码里不会有这种名字，真出现了也
// 不应该因为「太长」而崩掉编译。**不释放旧节点** —— 调用方负责在摘掉链表后继之后
// 释放它（free_ast 会递归释放 next，先摘再放才不会连带释放整条链）。
static ASTNode *build_field_chain(ASTNode *source, const char *name,
                                  size_t root_length) {
    char buffer[256];
    if (root_length >= sizeof(buffer)) return source;
    memcpy(buffer, name, root_length);
    buffer[root_length] = '\0';

    ASTNode *chain = (ASTNode *)create_identifier(buffer);
    chain->loc = source->loc;

    const char *rest = name + root_length + 1;
    while (rest && rest[0]) {
        const char *dot = strchr(rest, '.');
        size_t length = dot ? (size_t)(dot - rest) : strlen(rest);
        if (length == 0 || length >= sizeof(buffer)) {
            // 段名不合法：整条链作废，保持原样，交给旧路径报错。
            free_ast(chain);
            return source;
        }
        memcpy(buffer, rest, length);
        buffer[length] = '\0';
        FieldAccessNode *access = create_field_access(chain, buffer);
        access->base.loc = source->loc;
        chain = (ASTNode *)access;
        rest = dot ? dot + 1 : NULL;
    }

    return chain;
}

static ASTNode *rewrite_expression(NameContext *context, ASTNode *expression);
static void rewrite_statements(NameContext *context, ASTNode *statements);

// 改写一个子节点槽位：需要替换时把旧节点的 next 摘给新节点、再单独释放旧节点。
static void rewrite_slot(NameContext *context, ASTNode **slot) {
    ASTNode *original = *slot;
    if (!original) return;
    ASTNode *rewritten = rewrite_expression(context, original);
    if (rewritten == original) return;

    rewritten->next = original->next;
    original->next = NULL;
    free_ast(original);
    *slot = rewritten;
}

static void rewrite_expression_list(NameContext *context, ASTNode **head) {
    ASTNode **slot = head;
    while (*slot) {
        rewrite_slot(context, slot);
        slot = &(*slot)->next;
    }
}

// match 的两种形态（语句 / 表达式）共用：分支绑定的名字先登记，再走分支体和值。
static void rewrite_match(NameContext *context, MatchStatementNode *match_node) {
    rewrite_slot(context, &match_node->expression);
    for (ASTNode *node = match_node->arms; node; node = node->next) {
        MatchArmNode *arm = (MatchArmNode *)node;
        for (ASTNode *binding = arm->bindings; binding; binding = binding->next) {
            name_set_add(&context->locals, ((IdentifierNode *)binding)->name);
        }
        rewrite_statements(context, arm->body);
        rewrite_slot(context, &arm->value);
    }
}

static ASTNode *rewrite_expression(NameContext *context, ASTNode *expression) {
    if (!expression) return NULL;

    switch (expression->type) {
        case NODE_IDENTIFIER: {
            const char *name = ((IdentifierNode *)expression)->name;
            const char *dot = name ? strchr(name, '.') : NULL;
            if (!dot || dot == name) return expression;
            size_t root_length = (size_t)(dot - name);
            if (!name_set_contains(&context->locals, name, root_length)) {
                return expression; // 根不是局部变量：模块/类型成员，保持字符串
            }
            if (name_set_contains(&context->namespaces, name, root_length)) {
                return expression; // 和模块别名/类型名同名：不敢当字段，保持原样
            }
            return build_field_chain(expression, name, root_length);
        }
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            rewrite_slot(context, &binary->left);
            rewrite_slot(context, &binary->right);
            return expression;
        }
        case NODE_REFERENCE: {
            ReferenceNode *reference = (ReferenceNode *)expression;
            rewrite_slot(context, &reference->target);
            return expression;
        }
        case NODE_DEREFERENCE: {
            DereferenceNode *dereference = (DereferenceNode *)expression;
            rewrite_slot(context, &dereference->operand);
            return expression;
        }
        case NODE_CAST: {
            CastNode *cast = (CastNode *)expression;
            rewrite_slot(context, &cast->expression);
            return expression;
        }
        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)expression;
            rewrite_slot(context, &index->array);
            rewrite_slot(context, &index->index);
            return expression;
        }
        case NODE_FIELD_ACCESS: {
            FieldAccessNode *access = (FieldAccessNode *)expression;
            rewrite_slot(context, &access->object);
            return expression;
        }
        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)expression;
            rewrite_expression_list(context, &array->elements);
            return expression;
        }
        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)expression;
            for (ASTNode *node = literal->fields; node; node = node->next) {
                StructInitFieldNode *field = (StructInitFieldNode *)node;
                rewrite_slot(context, &field->expression);
            }
            return expression;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            rewrite_expression_list(context, &call->arguments);
            return expression;
        }
        case NODE_TRY: {
            TryNode *try_node = (TryNode *)expression;
            rewrite_slot(context, &try_node->inner);
            return expression;
        }
        case NODE_MATCH_STATEMENT:
            rewrite_match(context, (MatchStatementNode *)expression);
            return expression;
        default:
            return expression;
    }
}

static void rewrite_statements(NameContext *context, ASTNode *statements) {
    for (ASTNode *statement = statements; statement; statement = statement->next) {
        switch (statement->type) {
            case NODE_VAR_DECL: {
                VarDeclNode *declaration = (VarDeclNode *)statement;
                rewrite_slot(context, &declaration->expression);
                // 声明本身在初始化式之后登记：`let p: T = p;` 里的 p 还是外层那个。
                name_set_add(&context->locals, declaration->name);
                break;
            }
            case NODE_ASSIGNMENT: {
                AssignmentNode *assignment = (AssignmentNode *)statement;
                rewrite_slot(context, &assignment->expression);
                break;
            }
            case NODE_INDEX_ASSIGNMENT: {
                IndexAssignmentNode *assignment = (IndexAssignmentNode *)statement;
                rewrite_slot(context, &assignment->target->array);
                rewrite_slot(context, &assignment->target->index);
                rewrite_slot(context, &assignment->expression);
                break;
            }
            case NODE_RETURN: {
                ReturnNode *return_node = (ReturnNode *)statement;
                rewrite_slot(context, &return_node->expression);
                break;
            }
            case NODE_IF_STATEMENT: {
                IfStatementNode *if_node = (IfStatementNode *)statement;
                rewrite_slot(context, &if_node->condition);
                rewrite_statements(context, if_node->consequence);
                rewrite_statements(context, if_node->alternative);
                break;
            }
            case NODE_FOR_STATEMENT: {
                ForStatementNode *for_node = (ForStatementNode *)statement;
                rewrite_statements(context, for_node->initializer);
                rewrite_slot(context, &for_node->condition);
                rewrite_statements(context, for_node->update);
                rewrite_statements(context, for_node->body);
                break;
            }
            case NODE_MATCH_STATEMENT:
                rewrite_match(context, (MatchStatementNode *)statement);
                break;
            case NODE_FUNCTION_CALL:
                rewrite_expression(context, statement);
                break;
            default:
                break;
        }
    }
}

// 顶层声明里的名字：模块别名、类型名、函数名、常量名。出现在这些位置上的点号
// 一律不是字段访问，所以整体登记进 namespaces 集合，用于「宁可少改」的否决判断。
static void collect_namespaces(ProgramNode *program, NameSet *namespaces) {
    for (ASTNode *node = program->imports; node; node = node->next) {
        ImportNode *import_node = (ImportNode *)node;
        name_set_add(namespaces, import_node->alias);
    }
    for (ASTNode *node = program->structs; node; node = node->next) {
        name_set_add(namespaces, ((StructNode *)node)->name);
    }
    for (ASTNode *node = program->enums; node; node = node->next) {
        name_set_add(namespaces, ((EnumNode *)node)->name);
    }
    for (ASTNode *node = program->constants; node; node = node->next) {
        name_set_add(namespaces, ((VarDeclNode *)node)->name);
    }
    for (ASTNode *node = program->functions; node; node = node->next) {
        name_set_add(namespaces, ((FunctionNode *)node)->name);
    }
    for (ASTNode *node = program->traits; node; node = node->next) {
        name_set_add(namespaces, ((TraitNode *)node)->name);
    }
}

static void rewrite_function(NameContext *context, FunctionNode *function) {
    if (!function || function->is_extern) return;
    name_set_free(&context->locals);
    for (ASTNode *param = function->params; param; param = param->next) {
        name_set_add(&context->locals, ((IdentifierNode *)param)->name);
    }
    rewrite_statements(context, function->body);
}

void normalize_qualified_names(ProgramNode *program) {
    if (!program) return;

    NameContext context;
    memset(&context, 0, sizeof(context));
    collect_namespaces(program, &context.namespaces);

    for (ASTNode *node = program->functions; node; node = node->next) {
        rewrite_function(&context, (FunctionNode *)node);
    }

    name_set_free(&context.locals);
    name_set_free(&context.namespaces);
}
