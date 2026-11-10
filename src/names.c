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
//   - 写：`p.x = v` → FIELD_ASSIGNMENT(FIELD_ACCESS(p, x), v)；
//   - 模块/类型成员（`math.PI`、`Color.Red`）不动 —— 它们本来就不是字段链。
//
// 写路径必须一起改写：只要 `p.x = v` 还是「带点名字的 AssignmentNode」，Codegen 就得
// 再拆一遍点号（`generate_field_address`），那条分支和 FIELD_ACCESS 的读路径是两套实现，
// 多级字段、指针接收者、常量子字段这些规则都要各写一遍。改写成同一种表示之后，
// 写只是「先求地址再 store」，读只是「先求地址再 load」。

typedef struct {
    const char **items;
    int count;
    int capacity;
} NameSet;

typedef struct {
    NameSet locals;       // 当前函数的局部名字：参数、let/const、match 绑定
    NameSet constants;    // 顶层常量名：`CONST.field` / `CONST.method()` 的根
    int trace;            // TAP_TRACE_NAMES=1：打印每一处改写
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

// 把 `name[0..end)` 里的点号链拆成 IdentifierNode(首段) + 若干层 FIELD_ACCESS。
// `end` 传 0 表示整串。只有一段时返回裸 IdentifierNode。
//
// 不适用（段为空、段太长）时返回 NULL，调用方保持原节点。**不释放旧节点** —— 调用方
// 负责在摘掉链表后继之后释放它（free_ast 会递归释放 next，先摘再放才不会连带释放整条链）。
static ASTNode *build_field_chain(ASTNode *source, const char *name,
                                  size_t root_length, size_t end) {
    if (end == 0) end = strlen(name);
    char buffer[256];
    if (root_length >= sizeof(buffer) || root_length >= end) return NULL;
    memcpy(buffer, name, root_length);
    buffer[root_length] = '\0';

    ASTNode *chain = (ASTNode *)create_identifier(buffer);
    chain->loc = source->loc;

    const char *rest = name + root_length + 1;
    const char *limit = name + end;
    while (rest < limit && rest[0]) {
        const char *dot = memchr(rest, '.', (size_t)(limit - rest));
        size_t length = dot ? (size_t)(dot - rest) : (size_t)(limit - rest);
        if (length == 0 || length >= sizeof(buffer)) {
            // 段名不合法：整条链作废，保持原样，交给旧路径报错。
            chain->next = NULL;
            free_ast(chain);
            return NULL;
        }
        memcpy(buffer, rest, length);
        buffer[length] = '\0';
        FieldAccessNode *access = create_field_access(chain, buffer);
        // 空指针守卫和诊断要用；names 造出来的链和 parser 造的一样带上位置。
        //
        // 列号按段在整串里的偏移算：整串是一个标识符 token，不可能跨行，所以
        // `源列 + (rest - name)` 就是这一段自己的列。不这么算的话，`p.y` 报
        // 「没有字段 y」时会指到 `p` 上，多级链里每一环也都指向同一个列。
        access->base.loc = source->loc;
        access->base.loc.column = source->loc.column + (unsigned)(rest - name);
        access->filename = source->loc.file ? strdup(source->loc.file) : NULL;
        access->line = (int)access->base.loc.line;
        access->column = (int)access->base.loc.column;
        chain = (ASTNode *)access;
        rest = dot ? dot + 1 : limit;
    }

    return chain;
}

// `name` 的点号链是否该当字段访问处理：根必须是**局部名或顶层常量**。
//
// 顶层常量也要算：`const EMPTY: String = ""; EMPTY.len()` 和 `CONFIG.port` 都是
// 「常量上的字段/方法」，它们的根不在局部符号表里，但同样不是命名空间。
// 模块成员的引用在更早的模块装载阶段就被改名成了 `__tap_module_N.name`，根是
// `__tap_module_N` 而不是别名，所以这里不会把 `math.PI` 误当字段链。
//
// 不需要额外排除「和模块别名 / 类型名同名」的情况：局部名遮蔽别名是合法的
// （`import std.vec as vec;` 之后 `let vec: i32 = 1;` 能编译），所以只要根是局部变量，
// `vec.data` 里的 `vec` 就一定是那个变量，不可能是模块。早先这里有一道按全局别名表
// 的否决，把 std 里所有 `vec.data` / `builder.buffer` 全挡在门外（它们恰好和调用方的
// import 别名同名），白白让几百处字段访问继续走字符串路径。
static int qualified_name_is_field(const NameContext *context, const char *name,
                                   size_t *root_length) {
    const char *dot = name ? strchr(name, '.') : NULL;
    if (!dot || dot == name) return 0;
    size_t length = (size_t)(dot - name);
    if (name_set_contains(&context->locals, name, length) ||
        name_set_contains(&context->constants, name, length)) {
        *root_length = length;
        return 1;
    }

    // 模块限定的顶层常量：`c.LABEL.len()` 装载后是 `__tap_module_0.LABEL.len`，
    // 根是**前两段**。只在这一段确实是常量时才认 —— 同样的形状也可能是
    // `__tap_module_0.EnumType.Member`（模块私有枚举的成员），那个不是字段链。
    const char *second = dot + 1;
    const char *next = strchr(second, '.');
    if (!next) return 0;
    size_t two = (size_t)(next - name);
    if (name_set_contains(&context->constants, name, two)) {
        *root_length = two;
        return 1;
    }
    return 0;
}

static ASTNode *rewrite_expression(NameContext *context, ASTNode *expression);
static void rewrite_statements(NameContext *context, ASTNode **head);

// `p.shift(1)` / `p.inner.value()`：把接收者从调用名里拆出来，改成显式 receiver。
//
// 判据和字段访问完全一致 —— 根是当前函数的局部变量。模块函数（`math.abs`）的根不是
// 局部变量，名字原样保留，继续走模块装载阶段改名后的查表。
//
// 方法名是**最后一段**（`a.b.method` 的 method），接收者是它前面的整条链。
static void rewrite_method_receiver(NameContext *context, FunctionCallNode *call) {
    const char *name = call->name;
    if (!name) return;
    const char *last_dot = strrchr(name, '.');
    if (!last_dot || last_dot == name) return;

    size_t root_length = 0;
    if (!qualified_name_is_field(context, name, &root_length)) return;

    size_t receiver_end = (size_t)(last_dot - name);
    if (root_length > receiver_end) return; // 名字畸形（根跨越了方法名）

    ASTNode *receiver = NULL;
    if (root_length == receiver_end) {
        // 接收者就是根变量本身：`p.shift(1)`。
        char buffer[256];
        if (root_length >= sizeof(buffer)) return;
        memcpy(buffer, name, root_length);
        buffer[root_length] = '\0';
        receiver = (ASTNode *)create_identifier(buffer);
        receiver->loc = ((ASTNode *)call)->loc;
    } else {
        // 接收者本身是字段链：`p.inner.value()`。
        receiver = build_field_chain((ASTNode *)call, name, root_length, receiver_end);
    }
    if (!receiver) return;

    char *method = strdup(last_dot + 1);
    free(call->name);
    call->name = method;
    call->receiver = receiver;
}

static void trace_rewrite(const NameContext *context, const char *name) {
    if (context->trace) {
        fprintf(stderr, "names: %s -> field chain\n", name);
    }
}

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
        rewrite_statements(context, &arm->body);
        rewrite_slot(context, &arm->value);
    }
}

static ASTNode *rewrite_expression(NameContext *context, ASTNode *expression) {
    if (!expression) return NULL;

    switch (expression->type) {
        case NODE_IDENTIFIER: {
            const char *name = ((IdentifierNode *)expression)->name;
            size_t root_length = 0;
            if (!qualified_name_is_field(context, name, &root_length)) {
                return expression;
            }
            ASTNode *chain = build_field_chain(expression, name, root_length, 0);
            if (chain) trace_rewrite(context, name);
            return chain ? chain : expression;
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
            if (call->receiver) {
                rewrite_slot(context, &call->receiver);
            } else {
                rewrite_method_receiver(context, call);
            }
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

// 语句列表按**槽位**遍历：`p.x = v` 要整条换成 FIELD_ASSIGNMENT，光改字段不够。
static void rewrite_statements(NameContext *context, ASTNode **head) {
    ASTNode **slot = head;
    while (*slot) {
        ASTNode *statement = *slot;
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

                size_t root_length = 0;
                if (!qualified_name_is_field(context, assignment->name,
                                             &root_length)) {
                    break; // 根不是局部变量：模块/类型成员，交给旧路径
                }
                ASTNode *chain = build_field_chain(
                    statement, assignment->name, root_length, 0);
                if (!chain || chain->type != NODE_FIELD_ACCESS) {
                    if (chain) free_ast(chain);
                    break;
                }

                trace_rewrite(context, assignment->name);
                FieldAssignmentNode *replacement = create_field_assignment(
                    (FieldAccessNode *)chain, assignment->expression);
                replacement->base.loc = statement->loc;
                // 所有权转移：expression 归新节点，链表后继接过去，旧节点清空后释放。
                assignment->expression = NULL;
                replacement->base.next = statement->next;
                statement->next = NULL;
                free_ast(statement);
                *slot = (ASTNode *)replacement;
                statement = *slot;
                break;
            }
            case NODE_FIELD_ASSIGNMENT: {
                FieldAssignmentNode *assignment = (FieldAssignmentNode *)statement;
                rewrite_slot(context, &assignment->target->object);
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
                rewrite_statements(context, &if_node->consequence);
                rewrite_statements(context, &if_node->alternative);
                break;
            }
            case NODE_FOR_STATEMENT: {
                ForStatementNode *for_node = (ForStatementNode *)statement;
                rewrite_statements(context, &for_node->initializer);
                rewrite_slot(context, &for_node->condition);
                rewrite_statements(context, &for_node->update);
                rewrite_statements(context, &for_node->body);
                break;
            }
            case NODE_MATCH_STATEMENT:
                rewrite_match(context, (MatchStatementNode *)statement);
                break;
            case NODE_FUNCTION_CALL:
                rewrite_slot(context, slot);
                break;
            default:
                break;
        }
        slot = &(*slot)->next;
    }
}

static void rewrite_function(NameContext *context, FunctionNode *function) {
    if (!function || function->is_extern) return;
    name_set_free(&context->locals);
    for (ASTNode *param = function->params; param; param = param->next) {
        name_set_add(&context->locals, ((IdentifierNode *)param)->name);
    }
    rewrite_statements(context, &function->body);
}

void normalize_qualified_names(ProgramNode *program) {
    if (!program) return;

    NameContext context;
    memset(&context, 0, sizeof(context));
    for (ASTNode *node = program->constants; node; node = node->next) {
        name_set_add(&context.constants, ((VarDeclNode *)node)->name);
    }
    const char *trace = getenv("TAP_TRACE_NAMES");
    context.trace = trace && trace[0];

    for (ASTNode *node = program->functions; node; node = node->next) {
        rewrite_function(&context, (FunctionNode *)node);
    }

    name_set_free(&context.locals);
    name_set_free(&context.constants);
}
