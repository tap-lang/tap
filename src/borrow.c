#include "borrow.h"

#include <stdlib.h>
#include <string.h>

#include "helpers.h"
#include "types.h"

// 借用检查用的符号表。
//
// 和 Sema / Codegen 的符号表都不同：这里关心的不是类型，而是**寿命**（storage_rank）
// 和**借用状态**。名字借用 AST 里的字符串，AST 活到编译结束，所以不需要复制。
typedef struct BorrowSymbol {
    const char *name;
    // 这个名字自己的存储所在的作用域秩。参数是 0，函数体顶层是 1，每进一层块 +1。
    int storage_rank;
    // 它是引用时，指向的存储的秩；-1 表示指向函数外（引用型参数、静态数据）。
    int pointee_rank;
    int is_reference;
    int reference_mutable;
    // 被 `&mut` 借出时记下借用所在的作用域秩，-1 表示当前没有独占借用。
    int mutable_borrow_rank;
    // 被 `&` 借出时同理。多次共享借用取**最外层**的秩，保证内层块退出时不会误清。
    int shared_borrow_rank;
    // 当前**语句**里已经发生的临时借用（`f(&mut x, &mut x)` 的实参位置）。
    // 语句边界清空：它们不像 `let r = &mut x` 那样活到作用域末尾，所以不能记在
    // 上面两个秩字段里 —— 那样会把「同一语句内的冲突」和「跨语句的冲突」混为一谈。
    int temp_mutable_borrow;
    int temp_shared_borrow;
    // 声明的完整类型，用来判断 `&a.b[i]` 到底指向栈帧还是堆。未知时为 NULL，
    // 此时一律按「还在栈帧里」处理（保守方向：宁可报错也不放过悬垂）。
    const VarTypeNode *type;
    struct BorrowSymbol *next;
} BorrowSymbol;

typedef struct {
    ProgramNode *program;
    BorrowSymbol *symbols;
    int rank; // 当前作用域秩
} BorrowContext;

// 一个可寻址表达式的解析结果。
//
// base / rank 只对「存储在本函数栈帧里」的目标有意义：一旦路径穿过指针或引用，
// 指向的东西就不归本函数管，base 置空、rank 置 -1。
typedef struct {
    BorrowSymbol *base;   // 可跟踪的本地槽位；穿过指针后为 NULL
    int rank;             // 存储所在作用域秩；-1 表示函数外
    const VarTypeNode *type; // 这个左值本身的类型；未知为 NULL
    int crossed_shared;   // 路径上穿过了共享引用 `&T`
    int crossed_mutable;  // 路径上穿过了独占引用 `&mut T`
} LValueInfo;

static void walk_expression(BorrowContext *context, ASTNode *expression);
static void walk_statement_list(BorrowContext *context, ASTNode *statement);

static BorrowSymbol *find_symbol(BorrowContext *context, const char *name) {
    for (BorrowSymbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        if (strcmp(symbol->name, name) == 0) return symbol;
    }
    return NULL;
}

// 查名字对应的本地符号。
//
// `point.x` 这种只有一层字段访问的写法在 AST 里是**一个带点的标识符**（不是
// FieldAccessNode），存储属于点号前面的那个对象，所以要退回去找基名。
static BorrowSymbol *lookup_symbol(BorrowContext *context, const char *name) {
    BorrowSymbol *symbol = find_symbol(context, name);
    if (symbol) return symbol;

    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return NULL;
    size_t length = (size_t)(dot - name);
    if (length >= 256) return NULL;
    char base[256];
    memcpy(base, name, length);
    base[length] = '\0';
    return find_symbol(context, base);
}

static BorrowSymbol *insert_symbol(BorrowContext *context, const char *name,
                                   int storage_rank, int is_reference,
                                   int pointee_rank, int reference_mutable,
                                   const VarTypeNode *type) {
    if (!name) return NULL;
    BorrowSymbol *symbol = (BorrowSymbol *)malloc(sizeof(BorrowSymbol));
    if (!symbol) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    symbol->name = name;
    symbol->storage_rank = storage_rank;
    symbol->pointee_rank = pointee_rank;
    symbol->is_reference = is_reference;
    symbol->reference_mutable = reference_mutable;
    symbol->mutable_borrow_rank = -1;
    symbol->shared_borrow_rank = -1;
    symbol->temp_mutable_borrow = 0;
    symbol->temp_shared_borrow = 0;
    symbol->type = type;
    symbol->next = context->symbols;
    context->symbols = symbol;
    return symbol;
}

// 退出作用域：弹出符号，并把**在这个作用域里建立的**借用标记清掉。
//
// 只清 rank 完全相等的标记：内层块退出时不能把外层的共享借用一起清掉，否则
// `let a = &x; { let b = &x; } x = 1;` 会被误判成合法。
static void pop_symbols_to(BorrowContext *context, BorrowSymbol *mark, int rank) {
    while (context->symbols != mark) {
        BorrowSymbol *next = context->symbols->next;
        free(context->symbols);
        context->symbols = next;
    }
    for (BorrowSymbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        if (symbol->mutable_borrow_rank == rank) symbol->mutable_borrow_rank = -1;
        if (symbol->shared_borrow_rank == rank) symbol->shared_borrow_rank = -1;
    }
}

// 语句边界：清掉上一句留下的临时借用标记。
//
// 直接遍历当前符号表就够了 —— 语句内的临时借用一定落在**当前作用域里已登记**的
// 名字上，不需要额外维护一个链表。
static void clear_temp_borrows(BorrowContext *context) {
    for (BorrowSymbol *symbol = context->symbols; symbol; symbol = symbol->next) {
        symbol->temp_mutable_borrow = 0;
        symbol->temp_shared_borrow = 0;
    }
}

// ---------------------------------------------------------------------------
// 类型解析：判断一个左值的存储到底在不在本函数栈帧里
// ---------------------------------------------------------------------------

static const VarTypeNode *struct_field_type(BorrowContext *context,
                                            const VarTypeNode *type,
                                            const char *field) {
    if (!type || !type->struct_name || !field) return NULL;
    StructNode *structure = tap_find_struct(context->program, type->struct_name);
    if (!structure) return NULL;
    StructFieldNode *declared = tap_find_struct_field(structure, field, NULL);
    return declared ? declared->field_type : NULL;
}

// 穿过一层指针/引用。
//
// 这一步是 `&v.field` 和 `&local.field` 的分水岭：v 是 `&Struct` 时字段在别处，
// 本函数退出后依然有效；local 是结构体值时字段就在栈帧里。前者可以返回，后者不行。
static void apply_indirection(LValueInfo *info) {
    if (!info->type || !info->type->is_pointer) return;
    if (info->type->is_reference && info->type->reference_mutable) {
        info->crossed_mutable = 1;
    } else if (info->type->is_reference) {
        info->crossed_shared = 1;
    }
    info->type = info->type->element_type;
    info->rank = -1; // 指向的存储不归本帧管
    info->base = NULL;
}

static void apply_field(BorrowContext *context, LValueInfo *info,
                        const char *field) {
    if (info->type && info->type->is_pointer) apply_indirection(info);
    info->type = struct_field_type(context, info->type, field);
}

// 把 `a.b.c` 的第一个点前面的部分取出来。成功返回 1。
static int split_dotted(const char *name, char *buffer, size_t size,
                        const char **rest) {
    const char *dot = strchr(name, '.');
    if (!dot || dot == name) return 0;
    size_t length = (size_t)(dot - name);
    if (length >= size) return 0;
    memcpy(buffer, name, length);
    buffer[length] = '\0';
    *rest = dot + 1;
    return 1;
}

// 取下一段字段名，并把 rest 前移。没有下一段时返回 0。
static int next_segment(const char **rest, char *buffer, size_t size) {
    if (!*rest || !**rest) return 0;
    const char *dot = strchr(*rest, '.');
    size_t length = dot ? (size_t)(dot - *rest) : strlen(*rest);
    if (length == 0 || length >= size) return 0;
    memcpy(buffer, *rest, length);
    buffer[length] = '\0';
    *rest = dot ? dot + 1 : NULL;
    return 1;
}

static void resolve_lvalue(BorrowContext *context, ASTNode *expression,
                           LValueInfo *info) {
    info->base = NULL;
    info->rank = -1;
    info->type = NULL;
    info->crossed_shared = 0;
    info->crossed_mutable = 0;
    if (!expression) return;

    switch (expression->type) {
        case NODE_IDENTIFIER: {
            const char *name = ((IdentifierNode *)expression)->name;
            BorrowSymbol *symbol = find_symbol(context, name);
            if (symbol) {
                info->base = symbol;
                info->rank = symbol->storage_rank;
                info->type = symbol->type;
                return;
            }

            char base_name[256];
            const char *rest = NULL;
            if (!split_dotted(name, base_name, sizeof(base_name), &rest)) return;
            symbol = find_symbol(context, base_name);
            if (!symbol) return; // 模块名、常量、枚举成员等，不归借用检查管

            // 取的是变量本身的地址，所以起点还在栈帧里；字段链后面可能穿指针。
            info->base = symbol;
            info->rank = symbol->storage_rank;
            info->type = symbol->type;

            char field[256];
            while (next_segment(&rest, field, sizeof(field))) {
                apply_field(context, info, field);
            }
            return;
        }

        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)expression;
            resolve_lvalue(context, index->array, info);
            if (info->type && info->type->is_pointer) {
                // 指针下标 = 解引用：元素在被指向的地方，不在本帧。
                apply_indirection(info);
            } else if (info->type && info->type->is_array) {
                info->type = info->type->element_type; // 数组元素和数组同一块存储
            } else {
                info->type = NULL;
            }
            return;
        }

        case NODE_FIELD_ACCESS: {
            FieldAccessNode *access = (FieldAccessNode *)expression;
            resolve_lvalue(context, access->object, info);
            apply_field(context, info, access->field);
            return;
        }

        default:
            return;
    }
}

// ---------------------------------------------------------------------------
// 函数解析：借用怎么跨函数传递
// ---------------------------------------------------------------------------

static const char *function_base_name(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : name;
}

static FunctionNode *find_named_function(ProgramNode *program, const char *name) {
    if (!program || !name) return NULL;
    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        if (function->name && strcmp(function->name, name) == 0) return function;
    }
    return NULL;
}

static const VarTypeNode *function_parameter_type(const FunctionNode *function,
                                                  unsigned index) {
    ASTNode *type = function ? function->param_types : NULL;
    while (type && index > 0) {
        type = type->next;
        index--;
    }
    return type && type->type == NODE_VAR_TYPE ? (VarTypeNode *)type : NULL;
}

// impl 里的方法被改名成 `method$Type`，这里按「基名 + 首个参数类型」反查。
static FunctionNode *find_method_function_for(BorrowContext *context,
                                              const VarTypeNode *receiver_type,
                                              const char *method_name) {
    if (!context->program || !receiver_type || !method_name) return NULL;
    for (ASTNode *node = context->program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *function = (FunctionNode *)node;
        if (function->type_params) continue; // 泛型模板本身不可调用
        const VarTypeNode *first = function_parameter_type(function, 0);
        if (!first || !tap_receiver_type_matches(first, receiver_type)) continue;

        const char *base = function_base_name(function->name);
        const char *dollar = strchr(base, '$');
        size_t length = dollar ? (size_t)(dollar - base) : strlen(base);
        if (strlen(method_name) == length &&
            strncmp(base, method_name, length) == 0) {
            return function;
        }
    }
    return NULL;
}

// 解析一次调用。receiver_out 非空表示这是方法调用，接收者符号已解析出来
//（方法名写在调用名的点号前面，实参列表里不含接收者）。
static FunctionNode *resolve_called_function(BorrowContext *context,
                                             FunctionCallNode *call,
                                             BorrowSymbol **receiver_out) {
    if (receiver_out) *receiver_out = NULL;
    if (!call || !call->name) return NULL;

    FunctionNode *function = find_named_function(context->program, call->name);
    if (function) return function;

    const char *dot = strrchr(call->name, '.');
    if (!dot || dot == call->name) return NULL;

    size_t length = (size_t)(dot - call->name);
    if (length >= 256) return NULL;
    char base[256];
    memcpy(base, call->name, length);
    base[length] = '\0';

    BorrowSymbol *receiver = lookup_symbol(context, base);
    if (!receiver || !receiver->type) return NULL;

    // 和 Codegen 的 resolve_method_call 一致：用接收者**声明的完整类型**去匹配首参，
    // 不先解引用。方法签名写成 `fn m(v: &Counter)` 时，接收者也得是 `&Counter`。
    const VarTypeNode *receiver_type = receiver->type;
    if (!receiver_type->struct_name) return NULL;

    function = find_method_function_for(context, receiver_type, dot + 1);
    if (!function) return NULL;
    if (receiver_out) *receiver_out = receiver;
    return function;
}

// 按省略规则确定「返回的借用来自第几个参数」。
//
// 返回 -1：这个函数不返回借用，没有借用关系要传播。
// 返回 -2：返回借用但引用型参数不是恰好一个，无法确定来源（调用方会报错）。
static int borrow_source_parameter(const FunctionNode *function, int *count_out) {
    if (count_out) *count_out = 0;
    if (!function || !function->return_type || !function->return_type->is_reference) {
        return -1;
    }
    int index = -1;
    int count = 0;
    unsigned position = 0;
    for (ASTNode *node = function->param_types; node; node = node->next, position++) {
        VarTypeNode *type = (VarTypeNode *)node;
        if (node->type != NODE_VAR_TYPE || !type->is_reference) continue;
        index = (int)position;
        count++;
    }
    if (count_out) *count_out = count;
    return count == 1 ? index : -2;
}

static ASTNode *nth_argument(ASTNode *arguments, unsigned index) {
    ASTNode *argument = arguments;
    while (argument && index > 0) {
        argument = argument->next;
        index--;
    }
    return argument;
}

static int reference_rank_of(BorrowContext *context, ASTNode *expression,
                             int *pointee_rank);

// 一个实参作为借用来源时的秩。
static int argument_source_rank(BorrowContext *context, ASTNode *argument) {
    int rank = -1;
    if (!argument) return -1;
    if (!reference_rank_of(context, argument, &rank)) return -1;
    return rank;
}

// 表达式是不是一个借用；是的话用 *pointee_rank 带回它指向的存储的秩。
//
// 三种来源：`&x` 取地址、引用类型的变量、**返回借用的函数调用**。最后一种是 L1 的
// 核心 —— 没有它，`return first(&local)` 这种写法就能把栈上地址带出去。
static int reference_rank_of(BorrowContext *context, ASTNode *expression,
                             int *pointee_rank) {
    if (!expression) return 0;

    if (expression->type == NODE_REFERENCE) {
        LValueInfo info;
        resolve_lvalue(context, ((ReferenceNode *)expression)->target, &info);
        *pointee_rank = info.rank;
        return 1;
    }

    if (expression->type == NODE_IDENTIFIER) {
        BorrowSymbol *symbol =
            lookup_symbol(context, ((IdentifierNode *)expression)->name);
        if (symbol && symbol->is_reference) {
            *pointee_rank = symbol->pointee_rank;
            return 1;
        }
        return 0;
    }

    if (expression->type == NODE_FUNCTION_CALL) {
        FunctionCallNode *call = (FunctionCallNode *)expression;
        BorrowSymbol *receiver = NULL;
        FunctionNode *function = resolve_called_function(context, call, &receiver);
        if (!function) return 0;
        int source = borrow_source_parameter(function, NULL);
        if (source < 0) return 0;

        if (receiver) {
            // 方法：参数 0 是接收者，其余实参从 1 开始对位。
            if (source == 0) {
                // 借的是接收者。接收者本身是引用时，借的是它指向的存储；否则
                // 调用点会为 `&self` 自动取地址，借的就是接收者自己所在的槽 ——
                // 所以这里要区分，直接用 pointee_rank 会把局部接收者当成函数外。
                *pointee_rank = receiver->is_reference
                    ? receiver->pointee_rank
                    : receiver->storage_rank;
                return 1;
            }
            *pointee_rank = argument_source_rank(
                context, nth_argument(call->arguments, (unsigned)source - 1));
            return 1;
        }

        *pointee_rank = argument_source_rank(
            context, nth_argument(call->arguments, (unsigned)source));
        return 1;
    }

    return 0;
}

static int reference_is_mutable(ASTNode *expression) {
    return expression && expression->type == NODE_REFERENCE &&
           ((ReferenceNode *)expression)->is_mutable;
}

// ---------------------------------------------------------------------------
// 借用规则
// ---------------------------------------------------------------------------

// 借用冲突检查的核心：source 这个符号要被借出 is_mutable。
//
// mark 只在借用**会持续**的地方为真（`let r = ...`）：临时借用（`f(&mut x)` 这种）
// 在语句结束时就不存在了，不记进秩字段，只记一个语句级的临时标记。
//
// 反过来，检查临时借用时**必须**把同一语句里已经发生的临时借用也算进来，
// 否则 `f(&mut x, &mut x)` 这种写法会溜过去 —— 两次借用都没留下持久标记。
static void check_symbol_borrow_conflict(BorrowContext *context,
                                         BorrowSymbol *source, int is_mutable,
                                         int mark,
                                         const ASTNode *diagnostic_node) {
    if (!source) return;
    int check_temp = !mark;

    // 每条冲突只报一条然后返回：报完之后继续改状态会让后面的语句再报一遍，
    // 一个错误刷出三四条诊断，反而看不清。返回也意味着不再登记这次借用 ——
    // 反正已经有错，编译不会走到生成代码那一步。
    if (is_mutable) {
        if (source->mutable_borrow_rank >= 0 ||
            (check_temp && source->temp_mutable_borrow)) {
            report_error_soft(diagnostic_node, "'%s' is already mutably borrowed",
                              source->name); // 中文：已经被独占借用
            return;
        }
        if (source->shared_borrow_rank >= 0 ||
            (check_temp && source->temp_shared_borrow)) {
            report_error_soft(diagnostic_node,
                              "'%s' is already borrowed, so it cannot be borrowed mutably",
                              source->name); // 中文：已被共享借用，不能再独占借用
            return;
        }
        if (mark) source->mutable_borrow_rank = context->rank;
        else source->temp_mutable_borrow = 1;
        return;
    }

    if (source->mutable_borrow_rank >= 0 ||
        (check_temp && source->temp_mutable_borrow)) {
        report_error_soft(diagnostic_node,
                          "'%s' is mutably borrowed, so it cannot be borrowed again",
                          source->name); // 中文：已被独占借用，不能再借用
        return;
    }
    if (mark) {
        if (source->shared_borrow_rank < 0 ||
            context->rank < source->shared_borrow_rank) {
            source->shared_borrow_rank = context->rank;
        }
    } else {
        source->temp_shared_borrow = 1;
    }
}

// 只登记「这次借用持续到作用域末尾」，不报诊断。
//
// 用在 `let r = &mut x;` 上：冲突检查在遍历表达式时就做过了（那里拿得到 `&mut x`
// 自己的位置，比整条语句精确），这里再检查一遍会为同一个错误报两条。
static void mark_persistent_borrow(BorrowContext *context, BorrowSymbol *source,
                                   int is_mutable) {
    if (!source) return;
    if (is_mutable) {
        source->mutable_borrow_rank = context->rank;
        return;
    }
    if (source->shared_borrow_rank < 0 ||
        context->rank < source->shared_borrow_rank) {
        source->shared_borrow_rank = context->rank;
    }
}

// 检查一次借用是否与已有借用冲突。
static void check_borrow_conflict(BorrowContext *context, ASTNode *target,
                                  int is_mutable, int mark,
                                  const ASTNode *diagnostic_node) {
    LValueInfo info;
    resolve_lvalue(context, target, &info);

    // `&mut v.field` 里 v 是共享引用时，等于透过共享引用拿独占权限。
    if (is_mutable && info.crossed_shared) {
        report_error_soft(diagnostic_node,
                          "cannot take a mutable borrow through a shared reference");
        // 中文：不能透过共享引用取独占借用
    }

    check_symbol_borrow_conflict(context, info.base, is_mutable, mark,
                                 diagnostic_node);
}

// 方法返回借用时，把这次借用登记到接收者身上。
//
// `let r = point.first();` 之后 r 就别名了 point，这时再改 point 必须报错 ——
// 但方法调用不像 `&point` 那样有个显式的取地址节点，不登记就会漏过去。
//
// 只处理**返回值是引用**的方法：返回普通值的调用不产生持续借用，登记反而会误报
// （`let n = vec.len();` 之后 `vec.data[i] = x` 会全部被拦下）。
// 接收者本身是引用时也不登记：借的是它指向的存储，不是这个槽。
static void mark_method_receiver_borrow(BorrowContext *context,
                                        ASTNode *expression,
                                        const ASTNode *diagnostic_node) {
    if (!expression || expression->type != NODE_FUNCTION_CALL) return;

    BorrowSymbol *receiver = NULL;
    FunctionNode *function = resolve_called_function(
        context, (FunctionCallNode *)expression, &receiver);
    if (!function || !receiver || receiver->is_reference) return;
    if (!function->return_type || !function->return_type->is_reference) return;
    if (borrow_source_parameter(function, NULL) != 0) return;

    const VarTypeNode *self_type = function_parameter_type(function, 0);
    if (!self_type || !self_type->is_reference) return;

    check_symbol_borrow_conflict(context, receiver,
                                 self_type->reference_mutable, 1,
                                 diagnostic_node);
}

// 方法调用本身就是一次借用，接收者当前的状态必须允许这次调用。
//
// 三种接收者要求不同：
//   `&self`      共享借用 —— 和已有的共享借用可以共存，但已被独占借走就不行
//   `&mut self`  临时独占借用 —— 接收者不能还被任何借用占着
//   按值接收      等于把接收者整个拿走（`values.free()`）—— 同样不能还被借走，
//                 否则调用方手里那个引用立刻悬垂
//
// 都不登记标记（mark = 0）：方法调用结束借用就结束，不像 `let r = &p` 那样持续到
// 作用域末尾。接收者本身是引用时也跳过 —— 借的是它指向的存储，不是这个槽。
static void check_method_receiver_borrow(BorrowContext *context,
                                         ASTNode *expression) {
    if (!expression || expression->type != NODE_FUNCTION_CALL) return;

    FunctionCallNode *call = (FunctionCallNode *)expression;
    BorrowSymbol *receiver = NULL;
    FunctionNode *function = resolve_called_function(context, call, &receiver);
    if (!function || !receiver || receiver->is_reference) return;

    const VarTypeNode *self_type = function_parameter_type(function, 0);
    if (!self_type) return;

    // 按值接收：单独一条诊断。说「借用了就不能再借」会让人以为只是借用冲突，
    // 实际是「这个值被拿走了，你手上那个引用要悬垂」。
    if (!self_type->is_reference) {
        if (receiver->mutable_borrow_rank >= 0 ||
            receiver->shared_borrow_rank >= 0) {
            const char *dot = strrchr(call->name, '.');
            report_error_soft(expression,
                              "cannot call method '%s' on '%s' while it is borrowed",
                              dot ? dot + 1 : call->name, receiver->name);
            // 中文：接收者还被借用着，不能调用按值接收的方法
        }
        return;
    }

    // `&mut self` 是独占要求；`&self` 只要接收者没被独占借走就行。
    check_symbol_borrow_conflict(context, receiver,
                                 self_type->reference_mutable, 0, expression);
}

// 普通使用（读取）一个已经被 `&mut` 借出的变量。
static void check_plain_use(BorrowContext *context, ASTNode *expression) {
    if (!expression || expression->type != NODE_IDENTIFIER) return;
    BorrowSymbol *symbol =
        lookup_symbol(context, ((IdentifierNode *)expression)->name);
    if (!symbol) return;
    // 临时借用也算：`f(&mut x, x)` 里第二个实参读的就是被独占借走的 x。
    if (symbol->mutable_borrow_rank < 0 && !symbol->temp_mutable_borrow) return;
    report_error_soft(expression,
                      "'%s' is mutably borrowed and cannot be used here",
                      symbol->name); // 中文：变量已被独占借用
}

// 写入一个变量（`x = v`、`x.f = v`、`x[i] = v`）时的检查。
//
// 两件事：不能写正在被借用的变量；不能通过共享引用 `&T` 写 —— 后者正是 `&T`
// 唯一的承诺（借给你只是看），破了它整个借用体系就没意义了。
//
// through_reference 用来区分「给引用变量本身重新赋值」和「穿过引用写它指向的东西」：
// `held = &other;` 改的是 held 这个槽，合法；`held.field = v` / `held[0] = v` 改的是
// 被借的对象，只有 `&mut` 才允许。
static void check_write_target(BorrowSymbol *symbol, int through_reference,
                               const ASTNode *diagnostic_node) {
    if (!symbol) return;

    // 一条语句只报一条：`&x` 和 `&mut x` 同时挂着的时候，两条都报会让人以为
    // 有两处不同的错误。
    if (through_reference && symbol->is_reference &&
        !symbol->reference_mutable) {
        report_error_soft(diagnostic_node,
                          "cannot write through a shared reference '%s'",
                          symbol->name); // 中文：不能通过共享引用写入
        return;
    }
    if (symbol->mutable_borrow_rank >= 0) {
        report_error_soft(diagnostic_node,
                          "'%s' is mutably borrowed and cannot be assigned here",
                          symbol->name); // 中文：变量已被独占借用
        return;
    }
    if (symbol->shared_borrow_rank >= 0) {
        report_error_soft(diagnostic_node,
                          "'%s' is borrowed and cannot be assigned here",
                          symbol->name); // 中文：变量已被借用，不能赋值
    }
}

// ---------------------------------------------------------------------------
// 遍历
// ---------------------------------------------------------------------------

static void walk_expression(BorrowContext *context, ASTNode *expression) {
    if (!expression) return;

    switch (expression->type) {
        case NODE_LITERAL:
        case NODE_SIZEOF:
            break;

        case NODE_IDENTIFIER:
            check_plain_use(context, expression);
            break;

        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            walk_expression(context, binary->left);
            walk_expression(context, binary->right);
            break;
        }

        case NODE_REFERENCE: {
            ReferenceNode *reference = (ReferenceNode *)expression;
            // 目标不做普通使用检查：`&x` 里对 x 的「读取」正是借用本身。
            check_borrow_conflict(context, reference->target,
                                  reference->is_mutable, 0, expression);
            break;
        }

        case NODE_DEREFERENCE:
            walk_expression(context, ((DereferenceNode *)expression)->operand);
            break;

        case NODE_CAST:
            walk_expression(context, ((CastNode *)expression)->expression);
            break;

        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            for (ASTNode *argument = call->arguments; argument;
                 argument = argument->next) {
                walk_expression(context, argument);
            }
            check_method_receiver_borrow(context, expression);
            break;
        }

        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)expression;
            walk_expression(context, index->array);
            walk_expression(context, index->index);
            break;
        }

        case NODE_FIELD_ACCESS:
            walk_expression(context, ((FieldAccessNode *)expression)->object);
            break;

        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)expression;
            for (ASTNode *node = literal->fields; node; node = node->next) {
                walk_expression(context, ((StructInitFieldNode *)node)->expression);
            }
            break;
        }

        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)expression;
            for (ASTNode *node = array->elements; node; node = node->next) {
                walk_expression(context, node);
            }
            break;
        }

        case NODE_TRY:
            walk_expression(context, ((TryNode *)expression)->inner);
            break;

        case NODE_MATCH_STATEMENT: {
            MatchStatementNode *match_node = (MatchStatementNode *)expression;
            walk_expression(context, match_node->expression);
            for (ASTNode *node = match_node->arms; node; node = node->next) {
                MatchArmNode *arm = (MatchArmNode *)node;
                BorrowSymbol *mark = context->symbols;
                int rank = context->rank;
                context->rank = rank + 1;
                // 载荷绑定是解构出来的值，存储就在当前栈帧。
                for (ASTNode *binding = arm->bindings; binding;
                     binding = binding->next) {
                    insert_symbol(context, ((IdentifierNode *)binding)->name,
                                  context->rank, 0, -1, 0, NULL);
                }
                walk_statement_list(context, arm->body);
                walk_expression(context, arm->value);
                pop_symbols_to(context, mark, context->rank);
                context->rank = rank;
            }
            break;
        }

        default:
            break;
    }
}

static void walk_statement(BorrowContext *context, ASTNode *statement) {
    // 临时借用是**语句级**的：上一句的 `&mut x` 到这一句就不存在了。
    clear_temp_borrows(context);
    switch (statement->type) {
        case NODE_VAR_DECL: {
            VarDeclNode *declaration = (VarDeclNode *)statement;
            walk_expression(context, declaration->expression);

            // 显式标注成裸指针 `*T` 就是「遗忘借用」：不跟踪寿命，也不登记别名。
            // 这是和 C 打交道、以及需要长期持有指针时唯一的出口。
            if (declaration->type && declaration->type->is_pointer &&
                !declaration->type->is_reference) {
                insert_symbol(context, declaration->name, context->rank, 0, -1, 0,
                              declaration->type);
                break;
            }

            int pointee_rank = -1;
            if (!reference_rank_of(context, declaration->expression,
                                   &pointee_rank)) {
                insert_symbol(context, declaration->name, context->rank, 0, -1, 0,
                              declaration->type);
                break;
            }

            // 借用的寿命必须覆盖接收它的变量。
            if (pointee_rank > context->rank) {
                report_error_soft(
                    statement,
                    "borrowed value does not live long enough: '%s' would outlive it",
                    declaration->name); // 中文：被借用的值活不过这个引用
            }

            // 绑定成变量之后借用才算「持续」，这时才登记别名。诊断不在这里出 ——
            // 上面的 walk_expression 已经检查过（位置是 `&mut x` 本身，比整条语句准）。
            if (declaration->expression &&
                declaration->expression->type == NODE_REFERENCE) {
                ReferenceNode *reference =
                    (ReferenceNode *)declaration->expression;
                LValueInfo info;
                resolve_lvalue(context, reference->target, &info);
                mark_persistent_borrow(context, info.base, reference->is_mutable);
            }
            mark_method_receiver_borrow(context, declaration->expression,
                                        statement);

            int is_mutable = declaration->type && declaration->type->is_reference
                ? declaration->type->reference_mutable
                : reference_is_mutable(declaration->expression);
            insert_symbol(context, declaration->name, context->rank, 1,
                          pointee_rank, is_mutable, declaration->type);
            break;
        }

        case NODE_ASSIGNMENT: {
            AssignmentNode *assignment = (AssignmentNode *)statement;
            walk_expression(context, assignment->expression);

            BorrowSymbol *target = lookup_symbol(context, assignment->name);
            // 带点的名字（`point.x`）是穿过结构体写字段，算「通过引用写」。
            check_write_target(target,
                               strchr(assignment->name, '.') != NULL, statement);

            int pointee_rank = -1;
            if (target && target->is_reference &&
                reference_rank_of(context, assignment->expression,
                                  &pointee_rank)) {
                if (pointee_rank > target->storage_rank) {
                    report_error_soft(
                        statement,
                        "borrowed value does not live long enough: '%s' would outlive it",
                        target->name);
                }
                target->pointee_rank = pointee_rank;
                target->reference_mutable =
                    reference_is_mutable(assignment->expression);
                mark_method_receiver_borrow(context, assignment->expression,
                                            statement);
            }
            break;
        }

        case NODE_INDEX_ASSIGNMENT: {
            IndexAssignmentNode *assignment = (IndexAssignmentNode *)statement;
            LValueInfo info;
            resolve_lvalue(context, assignment->target->array, &info);
            if (info.crossed_shared) {
                report_error_soft(statement,
                                  "cannot write through a shared reference");
            }
            check_write_target(info.base, 1, statement);
            walk_expression(context, (ASTNode *)assignment->target);
            walk_expression(context, assignment->expression);
            break;
        }

        case NODE_IF_STATEMENT: {
            IfStatementNode *if_node = (IfStatementNode *)statement;
            walk_expression(context, if_node->condition);

            int rank = context->rank;
            BorrowSymbol *mark = context->symbols;
            context->rank = rank + 1;
            walk_statement_list(context, if_node->consequence);
            pop_symbols_to(context, mark, context->rank);

            if (if_node->alternative) {
                mark = context->symbols;
                context->rank = rank + 1;
                walk_statement_list(context, if_node->alternative);
                pop_symbols_to(context, mark, context->rank);
            }
            context->rank = rank;
            break;
        }

        case NODE_FOR_STATEMENT: {
            ForStatementNode *for_node = (ForStatementNode *)statement;
            int rank = context->rank;
            BorrowSymbol *mark = context->symbols;
            context->rank = rank + 1;
            walk_statement_list(context, for_node->initializer);
            walk_expression(context, for_node->condition);
            walk_statement_list(context, for_node->update);
            walk_statement_list(context, for_node->body);
            pop_symbols_to(context, mark, context->rank);
            context->rank = rank;
            break;
        }

        case NODE_RETURN: {
            ASTNode *expression = ((ReturnNode *)statement)->expression;
            walk_expression(context, expression);
            int pointee_rank = -1;
            if (reference_rank_of(context, expression, &pointee_rank) &&
                pointee_rank >= 0) {
                report_error_soft(
                    statement,
                    "cannot return a reference to a value that lives in this function");
                // 中文：不能返回指向本函数内局部值的引用
            }
            break;
        }

        case NODE_MATCH_STATEMENT:
            walk_expression(context, statement);
            break;

        default:
            walk_expression(context, statement);
            break;
    }
}

static void walk_statement_list(BorrowContext *context, ASTNode *statement) {
    for (; statement; statement = statement->next) {
        walk_statement(context, statement);
    }
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

// 返回借用的函数必须能唯一确定「借的是哪个参数」。tap 还没有生命周期参数，
// 所以用 Rust 省略规则的子集：恰好一个引用型参数时，返回值就借自它。
static void check_borrow_signature(const FunctionNode *function) {
    if (!function->return_type || !function->return_type->is_reference) return;

    int count = 0;
    int source = borrow_source_parameter(function, &count);
    if (source != -2) return;

    const char *name = function->original_name ? function->original_name
                                               : function->name;
    // 有位置就打位置，没有就只打一句话 —— 两个分支互斥。以前 `report_error_at`
    // 是 noreturn，靠 exit 保证第二条不会执行；换成 soft 之后必须显式 else，
    // 否则一条签名错误会被报两遍。
    if (function->filename) {
        report_error_at_soft(
            function->filename, function->line, function->column,
            "function '%s' returns a reference but has %d reference parameters; "
            "exactly one is required to tell which value is borrowed",
            name, count); // 中文：返回引用的函数必须恰好有一个引用参数
    } else {
        report_error_soft(NULL,
                          "function '%s' returns a reference but has %d reference parameters; "
                          "exactly one is required to tell which value is borrowed",
                          name, count);
    }
}

static void check_function(BorrowContext *context, FunctionNode *function) {
    if (!function || function->is_extern) return;

    context->symbols = NULL;
    context->rank = 0;

    // 参数存储在自己的栈帧里（秩 0），但引用型参数指向的是函数外，所以
    // pointee_rank 是 -1 —— 这类引用可以原样返回。
    ASTNode *param = function->params;
    ASTNode *param_type = function->param_types;
    while (param && param_type) {
        VarTypeNode *type = (VarTypeNode *)param_type;
        insert_symbol(context, ((IdentifierNode *)param)->name, 0,
                      type->is_reference, -1, type->reference_mutable, type);
        param = param->next;
        param_type = param_type->next;
    }

    // 函数体顶层是秩 1：参数的存储比任何局部变量活得久。
    context->rank = 1;
    walk_statement_list(context, function->body);
    pop_symbols_to(context, NULL, context->rank);

    // 签名检查放在函数体之后：函数体里的错误更具体（「返回了局部值的引用」比
    // 「引用参数不是恰好一个」更能说明问题），先报更有用的那条。
    check_borrow_signature(function);
}

int check_borrows(ProgramNode *program) {
    if (!program) return 0;

    BorrowContext context;
    context.program = program;
    context.symbols = NULL;
    context.rank = 0;

    // 只统计本次借用检查报了几条：调用方可能已经跑过别的阶段（现在不会，但
    // 计数是全局的，用差值才不会把别人的错误算到自己头上）。
    int before = tap_error_count();

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        check_function(&context, (FunctionNode *)node);
    }

    return tap_error_count() > before;
}
