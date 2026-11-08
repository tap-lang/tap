#include "borrow.h"

#include <limits.h>
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
    // 声明的完整类型，用来判断 `&a.b[i]` 到底指向栈帧还是堆。未知时为 NULL，
    // 此时一律按「还在栈帧里」处理（保守方向：宁可报错也不放过悬垂）。
    const VarTypeNode *type;
    struct BorrowSymbol *next;
} BorrowSymbol;

// 借用路径的最大长度。路径是按字段/下标逐段拼出来的，正常代码很难超过这个长度；
// 真超了就退化成「整个对象」，方向是保守的。
#define BORROW_PATH_MAX 128

// 一条借用记录。
//
// 借用的粒度是**路径**而不是整个变量：`&mut p.left` 只占住 `p.left`，不影响
// `p.right`。路径写成 '/' 分隔的段：`/left`、`/left/next`（字段链）、
// `/items/#0`（常量下标）、`/items/*`（下标不是常量，通配）。空串表示整个对象。
//
// 记录挂在 context 的链表里而不是挂在符号上：一个变量同时被借出多条互不相交的路径
// 是常态（`&mut p.left` + `&mut p.right`），链天然支持，也不用给符号预留槽位。
typedef struct BorrowRecord {
    BorrowSymbol *base;   // 路径的起点变量
    char path[BORROW_PATH_MAX];
    int is_mutable;       // &mut 借出
    int rank;             // 建立这条记录时的作用域秩，用来在块退出时清掉
    int is_temp;          // 语句级临时借用：语句结束就不存在了
    // NLL 的失效点：借用活到第几条语句之后。`INT_MAX` 表示活到作用域结束（保守，
    // 也是没开 NLL 时的行为）。
    //
    // 值来自预扫描算出的「接收这次借用的引用变量最后一次被使用的语句」；算不出来
    // （名字会逃出这条语句、预扫描遇到不认识的节点、没开 NLL）就保持 INT_MAX。
    int dead_at;
    struct BorrowRecord *next;
} BorrowRecord;

// 一个名字在函数体里的活跃信息，由 NLL 预扫描填。
typedef struct LastUseEntry {
    char *name;           // strdup 出来的副本，函数检查完统一释放
    int last_stmt;        // 最后一次出现在表达式里的语句编号（从 1 开始）
    int leaks;            // 出现在「指针可能活过这条语句」的位置
    struct LastUseEntry *next;
} LastUseEntry;

// `let to = from;` 这种别名复制。from 的活跃期必须覆盖 to 的活跃期，否则
// `let r = &mut x; let s = r; use(s); x = 1;` 里 r 的借用会在第二句就结束。
typedef struct AliasEdge {
    const char *from;
    const char *to;
    struct AliasEdge *next;
} AliasEdge;

typedef struct {
    ProgramNode *program;
    BorrowSymbol *symbols;
    BorrowRecord *borrows;
    int rank; // 当前作用域秩
    // 当前语句编号。预扫描和正式遍历按同样的顺序编号，两边才能对上。
    int statement_index;
    int nll_enabled;      // 0 表示本函数退回纯词法语义
    LastUseEntry *last_uses;
} BorrowContext;

// 一个可寻址表达式的解析结果。
//
// base / rank 只对「存储在本函数栈帧里」的目标有意义：一旦路径穿过指针或引用，
// 指向的东西就不归本函数管，base 置空、rank 置 -1。
typedef struct {
    BorrowSymbol *base;   // 可跟踪的本地槽位；穿过指针后为 NULL
    // 路径起点的变量：`base` 穿过指针后会被清空，这个不清，只用来在诊断里报名字。
    BorrowSymbol *root;
    int rank;             // 存储所在作用域秩；-1 表示函数外
    const VarTypeNode *type; // 这个左值本身的类型；未知为 NULL
    int crossed_shared;   // 路径上穿过了共享引用 `&T`
    int crossed_mutable;  // 路径上穿过了独占引用 `&mut T`
    // 从 base 走到这个左值经过的字段/下标链，格式见 BorrowRecord::path。
    // 空串表示就是 base 本身。base 为 NULL 时无意义。
    char path[BORROW_PATH_MAX];
    int path_overflow;    // 路径太长被截断，已退化成「整个对象」
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
    symbol->type = type;
    symbol->next = context->symbols;
    context->symbols = symbol;
    return symbol;
}

// 退出作用域：弹出符号，并把**在这个作用域里建立的**借用记录清掉。
//
// 只清 rank 完全相等的记录：内层块退出时不能把外层的共享借用一起清掉，否则
// `let a = &x; { let b = &x; } x = 1;` 会被误判成合法。
//
// 顺带清掉「base 就是这一批被弹出的符号」的记录 —— 它们不可能还有效，而且 base
// 指针马上就要被 free，留着就是悬垂。
static void pop_symbols_to(BorrowContext *context, BorrowSymbol *mark, int rank) {
    BorrowRecord **link = &context->borrows;
    while (*link) {
        BorrowRecord *record = *link;
        int popped = record->rank == rank;
        for (BorrowSymbol *symbol = context->symbols;
             !popped && symbol != mark; symbol = symbol->next) {
            if (symbol == record->base) popped = 1;
        }
        if (popped) {
            *link = record->next;
            free(record);
            continue;
        }
        link = &record->next;
    }

    while (context->symbols != mark) {
        BorrowSymbol *next = context->symbols->next;
        free(context->symbols);
        context->symbols = next;
    }
}

// 语句边界：清掉上一句留下的临时借用记录。
static void clear_temp_borrows(BorrowContext *context) {
    BorrowRecord **link = &context->borrows;
    while (*link) {
        BorrowRecord *record = *link;
        if (record->is_temp) {
            *link = record->next;
            free(record);
            continue;
        }
        link = &record->next;
    }
}

// 把整个借用记录链清空（一个函数检查完时用）。
static void clear_all_borrows(BorrowContext *context) {
    while (context->borrows) {
        BorrowRecord *next = context->borrows->next;
        free(context->borrows);
        context->borrows = next;
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

// 往路径末尾追加一段。路径超出 BORROW_PATH_MAX 时退化成「整个对象」——
// 保守方向，宁可多报也不漏。
static void path_push(LValueInfo *info, const char *segment) {
    if (!info->base || info->path_overflow) return;
    size_t used = strlen(info->path);
    size_t length = strlen(segment);
    if (used + 1 + length + 1 > BORROW_PATH_MAX) {
        info->path[0] = '\0';
        info->path_overflow = 1;
        return;
    }
    info->path[used] = '/';
    memcpy(info->path + used + 1, segment, length + 1);
}

// 路径整体是不是「前缀」关系（含相等）。逐段比较，段边界是 '/'；
// 段 `*` 是下标通配：非常量下标的元素和任何下标都算重叠。
static int path_is_prefix(const char *whole, const char *prefix) {
    const char *left = whole;
    const char *right = prefix;
    for (;;) {
        if (*right == '\0') return 1;
        if (*left == '\0') return 0;
        left++; // 跳过 '/'
        right++;
        const char *left_end = strchr(left, '/');
        const char *right_end = strchr(right, '/');
        size_t left_length = left_end ? (size_t)(left_end - left) : strlen(left);
        size_t right_length =
            right_end ? (size_t)(right_end - right) : strlen(right);
        int wildcard = (left_length == 1 && *left == '*') ||
                       (right_length == 1 && *right == '*');
        if (!wildcard &&
            (left_length != right_length ||
             memcmp(left, right, left_length) != 0)) {
            return 0;
        }
        left += left_length;
        right += right_length;
    }
}

// 两个左值是否可能指向同一块存储：只要有前缀关系就算（保守）。
static int paths_overlap(const char *left, const char *right) {
    return path_is_prefix(left, right) || path_is_prefix(right, left);
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

static void reset_lvalue(LValueInfo *info) {
    info->base = NULL;
    info->root = NULL;
    info->rank = -1;
    info->type = NULL;
    info->crossed_shared = 0;
    info->crossed_mutable = 0;
    info->path[0] = '\0';
    info->path_overflow = 0;
}

// 名字 → 左值。`point.x` 这种只有字段链的写法在 AST 里是一个带点的标识符，
// 所以路径要从名字里拆。
static void resolve_identifier(BorrowContext *context, const char *name,
                               LValueInfo *info) {
    reset_lvalue(info);
    BorrowSymbol *symbol = find_symbol(context, name);
    if (symbol) {
        info->base = symbol;
        info->root = symbol;
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
    info->root = symbol;
    info->rank = symbol->storage_rank;
    info->type = symbol->type;

    char field[256];
    while (next_segment(&rest, field, sizeof(field))) {
        apply_field(context, info, field);
        path_push(info, field);
    }
}

static void resolve_lvalue(BorrowContext *context, ASTNode *expression,
                           LValueInfo *info) {
    reset_lvalue(info);
    if (!expression) return;

    switch (expression->type) {
        case NODE_IDENTIFIER: {
            resolve_identifier(context, ((IdentifierNode *)expression)->name,
                               info);
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
            // 常量下标按具体元素记（`&v[0]` 和 `v[1] = x` 互不相干），
            // 非常量只能用通配 —— 判断不了是不是同一个元素，按重叠处理。
            if (index->index && index->index->type == NODE_LITERAL) {
                LiteralNode *literal = (LiteralNode *)index->index;
                if (literal->literal_type >= LITERAL_INT &&
                    literal->literal_type <= LITERAL_U128) {
                    char segment[32];
                    snprintf(segment, sizeof(segment), "#%llu",
                             (unsigned long long)literal->value.int_value);
                    path_push(info, segment);
                    return;
                }
            }
            path_push(info, "*");
            return;
        }

        case NODE_FIELD_ACCESS: {
            FieldAccessNode *access = (FieldAccessNode *)expression;
            resolve_lvalue(context, access->object, info);
            apply_field(context, info, access->field);
            path_push(info, access->field);
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

// 追加一条借用记录。path 是相对 base 的路径，空串表示借走整个对象。
static void add_borrow_record(BorrowContext *context, BorrowSymbol *base,
                              const char *path, int is_mutable, int is_temp) {
    BorrowRecord *record = (BorrowRecord *)malloc(sizeof(BorrowRecord));
    if (!record) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    record->base = base;
    snprintf(record->path, sizeof(record->path), "%s", path ? path : "");
    record->is_mutable = is_mutable;
    record->rank = context->rank;
    record->is_temp = is_temp;
    record->dead_at = INT_MAX; // 默认活到作用域结束，NLL 在 let 处再收窄
    record->next = context->borrows;
    context->borrows = record;
}

// 统计 base 上与被查路径重叠的借用。
//
// only_mutable 为真时只关心独占借用（`&self` 接收、普通读取用得上）；
// 为假时两类都算。
static void collect_overlapping_borrows(BorrowContext *context,
                                        BorrowSymbol *base, const char *path,
                                        int only_mutable, int include_temp,
                                        int *has_mutable, int *has_shared) {
    if (has_mutable) *has_mutable = 0;
    if (has_shared) *has_shared = 0;
    if (!base) return;
    for (BorrowRecord *record = context->borrows; record;
         record = record->next) {
        if (record->base != base) continue;
        // NLL：已经过了失效点的记录不再参与冲突判定。`dead_at` 是「最后一次使用」
        // 所在的语句，所以那一句本身还算活着（`dead_at < 当前` 才失效）。
        if (record->dead_at < context->statement_index) continue;
        if (record->is_temp && !include_temp) continue;
        if (only_mutable && !record->is_mutable) continue;
        if (!paths_overlap(record->path, path)) continue;
        if (record->is_mutable) {
            if (has_mutable) *has_mutable = 1;
        } else if (has_shared) {
            *has_shared = 1;
        }
    }
}

// 借用冲突检查的核心：把 base 上的 path 借出去（is_mutable 表示独占）。
//
// mark 只在借用**会持续**的地方为真（`let r = ...`）：临时借用（`f(&mut x)` 这种）
// 在语句结束时就不存在了，只留一条 is_temp 记录，语句边界清掉。
//
// 反过来，检查临时借用时**必须**把同一语句里已经发生的临时借用也算进来，
// 否则 `f(&mut x, &mut x)` 这种写法会溜过去 —— 两次借用都没留下持久记录。
static void check_symbol_borrow_conflict(BorrowContext *context,
                                         BorrowSymbol *source, const char *path,
                                         int is_mutable, int mark,
                                         const ASTNode *diagnostic_node) {
    if (!source) return;
    int has_mutable = 0;
    int has_shared = 0;
    collect_overlapping_borrows(context, source, path, 0, !mark, &has_mutable,
                                &has_shared);

    // 每条冲突只报一条然后返回：报完之后继续登记会让后面的语句再报一遍，
    // 一个错误刷出三四条诊断，反而看不清。返回也意味着不再登记这次借用 ——
    // 反正已经有错，编译不会走到生成代码那一步。
    if (is_mutable) {
        if (has_mutable) {
            report_error_soft(diagnostic_node, "'%s' is already mutably borrowed",
                              source->name); // 中文：已经被独占借用
            return;
        }
        if (has_shared) {
            report_error_soft(diagnostic_node,
                              "'%s' is already borrowed, so it cannot be borrowed mutably",
                              source->name); // 中文：已被共享借用，不能再独占借用
            return;
        }
        add_borrow_record(context, source, path, 1, !mark);
        return;
    }

    if (has_mutable) {
        report_error_soft(diagnostic_node,
                          "'%s' is mutably borrowed, so it cannot be borrowed again",
                          source->name); // 中文：已被独占借用，不能再借用
        return;
    }
    add_borrow_record(context, source, path, 0, !mark);
}

// 只登记「这次借用持续到作用域末尾」，不报诊断。
//
// 用在 `let r = &mut x;` 上：冲突检查在遍历表达式时就做过了（那里拿得到 `&mut x`
// 自己的位置，比整条语句精确），这里再检查一遍会为同一个错误报两条。
static void mark_persistent_borrow(BorrowContext *context, BorrowSymbol *source,
                                   const char *path, int is_mutable) {
    if (!source) return;
    add_borrow_record(context, source, path, is_mutable, 0);
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

    check_symbol_borrow_conflict(context, info.base, info.path, is_mutable, mark,
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

    check_symbol_borrow_conflict(context, receiver, "",
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
        int has_mutable = 0;
        int has_shared = 0;
        collect_overlapping_borrows(context, receiver, "", 0, 1, &has_mutable,
                                    &has_shared);
        if (has_mutable || has_shared) {
            const char *dot = strrchr(call->name, '.');
            report_error_soft(expression,
                              "cannot call method '%s' on '%s' while it is borrowed",
                              dot ? dot + 1 : call->name, receiver->name);
            // 中文：接收者还被借用着，不能调用按值接收的方法
        }
        return;
    }

    // `&mut self` 是独占要求；`&self` 只要接收者没被独占借走就行。
    check_symbol_borrow_conflict(context, receiver, "",
                                 self_type->reference_mutable, 0, expression);
}

// 普通使用（读取）一个已经被 `&mut` 借出的左值。
//
// 路径同样细化到字段：握着 `&mut p.left` 去读 `p.right` 不该报错，读 `p` 整个
// 或者读 `p.left` 才报。
static void check_plain_use(BorrowContext *context, BorrowSymbol *symbol,
                            const char *path, const ASTNode *diagnostic_node) {
    if (!symbol) return;
    int has_mutable = 0;
    // 临时借用也算：`f(&mut x, x)` 里第二个实参读的就是被独占借走的 x。
    collect_overlapping_borrows(context, symbol, path, 1, 1, &has_mutable, NULL);
    if (!has_mutable) return;
    report_error_soft(diagnostic_node,
                      "'%s' is mutably borrowed and cannot be used here",
                      symbol->name); // 中文：变量已被独占借用
}

// 写入一个左值（`x = v`、`x.f = v`、`x[i] = v`）时的检查。
//
// 两件事：不能写正在被借用的地方；不能通过共享引用 `&T` 写 —— 后者正是 `&T`
// 唯一的承诺（借给你只是看），破了它整个借用体系就没意义了。
//
// 写入也按路径判：握着 `&mut p.left` 时写 `p.right` 合法，写 `p` 或 `p.left`
// 不合法。`crossed_shared` 区分「给引用变量本身重新赋值」和「穿过共享引用写它
// 指向的东西」：`held = &other;` 改的是 held 这个槽，合法；`held.field = v` 改的
// 是被借的对象，只有 `&mut` 才允许。
static void check_write_target(BorrowContext *context, const LValueInfo *info,
                               const ASTNode *diagnostic_node) {
    if (!info->base && !info->crossed_shared) return;

    if (info->crossed_shared) {
        report_error_soft(diagnostic_node,
                          "cannot write through a shared reference '%s'",
                          info->root ? info->root->name : "?");
        // 中文：不能通过共享引用写入
        return;
    }

    // 一条语句只报一条：`&x` 和 `&mut x` 同时挂着的时候，两条都报会让人以为
    // 有两处不同的错误。
    int has_mutable = 0;
    int has_shared = 0;
    collect_overlapping_borrows(context, info->base, info->path, 0, 0,
                                &has_mutable, &has_shared);
    if (has_mutable) {
        report_error_soft(diagnostic_node,
                          "'%s' is mutably borrowed and cannot be assigned here",
                          info->base->name); // 中文：变量已被独占借用
        return;
    }
    if (has_shared) {
        report_error_soft(diagnostic_node,
                          "'%s' is borrowed and cannot be assigned here",
                          info->base->name); // 中文：变量已被借用，不能赋值
    }
}

// 结构体 / 数组字面量把引用藏进字段之后，类型里就没有任何「谁借了谁」的信息了：
// `Holder` 和 `i32` 在类型系统里是两个毫不相干的类型，调用方无从判断。所以这里补
// 一条最粗的规则（L2 的折中方案）：**返回的字面量里，任何一层借用都不许指向本函数
// 的栈帧**。只有 `NODE_RETURN` 会调用它 —— 「返回值」是借用唯一能离开本帧的出口。
//
// 判据直接复用 `reference_rank_of` —— 它已经能算出 `&x`、引用变量、返回借用的调用
// 这三种来源的 pointee_rank，非负即「被借的存储跟着本次调用一起消失」。
//
// 递归只走「值确实由这些字段拼出来」的位置：结构体字段、数组元素。**不进函数调用
// 实参** —— `return wrap(Holder { src: &local })` 里借用能不能逃出去取决于 wrap 拿
// 它做什么，静态判断只会误报，那一层留给生命周期参数。
//
// 剩下的洞：借用装进结构体后先存进变量、再返回变量（`let h = Holder{...}; return h;`）
// 仍然不报 —— 要堵它得给变量记「持有本帧借用」的状态并跟着赋值传播，那是另一半工作。
static void check_escaping_borrow(BorrowContext *context, ASTNode *expression,
                                  const char *field_name,
                                  const ASTNode *diagnostic_node) {
    if (!expression) return;

    if (expression->type == NODE_STRUCT_LITERAL) {
        StructLiteralNode *literal = (StructLiteralNode *)expression;
        for (ASTNode *node = literal->fields; node; node = node->next) {
            StructInitFieldNode *field = (StructInitFieldNode *)node;
            check_escaping_borrow(context, field->expression, field->name,
                                  diagnostic_node);
        }
        return;
    }

    if (expression->type == NODE_ARRAY_LITERAL) {
        ArrayLiteralNode *array = (ArrayLiteralNode *)expression;
        for (ASTNode *element = array->elements; element;
             element = element->next) {
            check_escaping_borrow(context, element, field_name, diagnostic_node);
        }
        return;
    }

    int pointee_rank = -1;
    if (!reference_rank_of(context, expression, &pointee_rank)) return;
    if (pointee_rank < 0) return; // 借的是函数外的存储，跟着返回没问题

    if (field_name) {
        report_error_soft(diagnostic_node,
                          "field '%s' of the returned struct borrows a value that "
                          "lives in this function", field_name);
        // 中文：返回的结构体里这个字段借了本函数的局部值
    } else {
        report_error_soft(diagnostic_node,
                          "the returned value borrows something that lives in this "
                          "function");
        // 中文：返回值得以存在的借用指向本函数的局部存储
    }
}

// ---------------------------------------------------------------------------
// NLL：活跃性预扫描
// ---------------------------------------------------------------------------
//
// 词法模型里借用的寿命 = 作用域，`let r = &mut x; r[0] = 1; print(x);` 会被拦下，
// 因为 r 到作用域结束都算「还在借」。NLL 把寿命改成「最后一次使用」：
//
//   1. 预扫描把函数体里的语句按**正式遍历的顺序**从 1 开始编号，同时记下每个名字
//      最后一次出现在哪条语句（last_stmt），以及它是否出现在「指针可能活过这条语句」
//      的位置（leaks：装进结构体/数组、作为调用实参、作为返回值）。
//   2. `let r = &mut x;` 建立的持久借用把 dead_at 设成 last_use(r)；leaks 或算不出来
//      就保持 INT_MAX，也就是退化成原来的词法语义。
//   3. 冲突判定（collect_overlapping_borrows）跳过 dead_at 已经过去的记录。
//
// 为什么用独立的预扫描而不是复用正式遍历：正式遍历里边走边查，看不到后面还有没有
// 使用。两趟遍历必须**顺序一致**（语句编号要对上），所以 prescan_* 的分支顺序刻意
// 抄 walk_* 的顺序，改一边记得改另一边。
//
// 安全阀：预扫描遇到不认识的节点就置 unknown，整个函数退回词法语义。宁可少优化，
// 不能在「可能还有一次使用没看见」的情况下缩短借用。

// 语句编号从 1 开始；-1 表示「这个名字一次都没在表达式里出现过」。
static LastUseEntry *last_use_find(BorrowContext *context, const char *name) {
    if (!name) return NULL;
    for (LastUseEntry *entry = context->last_uses; entry; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) return entry;
    }
    return NULL;
}

static LastUseEntry *last_use_ensure(BorrowContext *context, const char *name) {
    if (!name || !*name) return NULL;
    LastUseEntry *entry = last_use_find(context, name);
    if (entry) return entry;

    entry = (LastUseEntry *)malloc(sizeof(LastUseEntry));
    if (!entry) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    entry->name = strdup(name);
    if (!entry->name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    entry->last_stmt = -1;
    entry->leaks = 0;
    entry->next = context->last_uses;
    context->last_uses = entry;
    return entry;
}

// 记一次「名字在语句 stmt 里被用到」。
//
// `p.left` 这种带点标识符算到基名 p 上：借用是挂在基名上的，活跃期也要按基名算。
static void last_use_record(BorrowContext *context, const char *name, int stmt,
                            int leaks) {
    if (!name || !*name) return;

    char buffer[256];
    const char *dot = strchr(name, '.');
    if (dot) {
        size_t length = (size_t)(dot - name);
        if (length == 0 || length >= sizeof(buffer)) return;
        memcpy(buffer, name, length);
        buffer[length] = '\0';
        name = buffer;
    }

    LastUseEntry *entry = last_use_ensure(context, name);
    if (!entry) return;
    if (stmt > entry->last_stmt) entry->last_stmt = stmt;
    if (leaks) entry->leaks = 1;
}

static void free_last_uses(BorrowContext *context) {
    while (context->last_uses) {
        LastUseEntry *next = context->last_uses->next;
        free(context->last_uses->name);
        free(context->last_uses);
        context->last_uses = next;
    }
}

// 别名复制 `let to = from;`：from 的活跃期要覆盖 to 的活跃期。
//
// 不做这一步的话 `let r = &mut x; let s = r; use(s); x = 1;` 会让 r 的借用停在
// 第二句 —— s 还指着 x，那是真的悬垂。
typedef struct NllScan {
    BorrowContext *context;
    int index;          // 当前语句编号
    int unknown;        // 遇到不认识的节点
    AliasEdge *edges;
} NllScan;

static void free_alias_edges(AliasEdge *edge) {
    while (edge) {
        AliasEdge *next = edge->next;
        free(edge);
        edge = next;
    }
}

static void add_alias_edge(NllScan *scan, const char *from, const char *to) {
    if (!from || !to) return;
    AliasEdge *edge = (AliasEdge *)malloc(sizeof(AliasEdge));
    if (!edge) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    edge->from = from;
    edge->to = to;
    edge->next = scan->edges;
    scan->edges = edge;
}

// 反复传播到不动点：链式复制（`let a = r; let b = a;`）一轮可能不够。
static void propagate_aliases(NllScan *scan) {
    int changed = 1;
    int rounds = 0;
    while (changed && rounds < 64) {
        changed = 0;
        rounds++;
        for (AliasEdge *edge = scan->edges; edge; edge = edge->next) {
            LastUseEntry *to = last_use_find(scan->context, edge->to);
            if (!to) continue;
            LastUseEntry *from = last_use_ensure(scan->context, edge->from);
            if (!from) continue;
            if (to->last_stmt > from->last_stmt) {
                from->last_stmt = to->last_stmt;
                changed = 1;
            }
            if (to->leaks && !from->leaks) {
                from->leaks = 1;
                changed = 1;
            }
        }
    }
}

static void prescan_statement_list(NllScan *scan, ASTNode *statement);
static void prescan_expression(NllScan *scan, ASTNode *expression, int leaks);

// 预扫描一条语句。编号顺序必须和 walk_statement 完全一致。
static void prescan_statement(NllScan *scan, ASTNode *statement) {
    if (!statement) return;
    scan->index++;

    switch (statement->type) {
        case NODE_VAR_DECL: {
            VarDeclNode *declaration = (VarDeclNode *)statement;
            prescan_expression(scan, declaration->expression, 0);
            // `let s = r;` 是别名复制，不当逃逸处理，交给 propagate_aliases。
            if (declaration->expression &&
                declaration->expression->type == NODE_IDENTIFIER) {
                add_alias_edge(scan,
                               ((IdentifierNode *)declaration->expression)->name,
                               declaration->name);
            }
            break;
        }

        case NODE_ASSIGNMENT: {
            AssignmentNode *assignment = (AssignmentNode *)statement;
            // 赋值目标也是「用到这个名字」：`r.x = 5` 里 r 确实被用了。正式遍历不读
            // 左值（走 resolve_identifier），这里不补的话 r 会被当成「从未使用」，
            // 借用一直活到作用域结束。写入不是把指针交出去，所以 leaks 留 0。
            last_use_record(scan->context, assignment->name, scan->index, 0);
            // 右值赋给别的变量等于把指针交出去，按逃逸算（保守方向）。
            prescan_expression(scan, assignment->expression, 1);
            break;
        }

        case NODE_INDEX_ASSIGNMENT: {
            IndexAssignmentNode *assignment = (IndexAssignmentNode *)statement;
            // 基对象在正式遍历里走的是 resolve_lvalue（不走 walk_expression），
            // 但 `r[0] = 1` 里读 r 这件事对活跃性照样算一次使用，必须记下来。
            prescan_expression(scan, assignment->target->array, 0);
            prescan_expression(scan, assignment->target->index, 0);
            prescan_expression(scan, assignment->expression, 1);
            break;
        }

        case NODE_IF_STATEMENT: {
            IfStatementNode *if_node = (IfStatementNode *)statement;
            prescan_expression(scan, if_node->condition, 0);
            prescan_statement_list(scan, if_node->consequence);
            prescan_statement_list(scan, if_node->alternative);
            break;
        }

        case NODE_FOR_STATEMENT: {
            ForStatementNode *for_node = (ForStatementNode *)statement;
            prescan_statement_list(scan, for_node->initializer);
            prescan_expression(scan, for_node->condition, 0);
            prescan_statement_list(scan, for_node->update);
            prescan_statement_list(scan, for_node->body);
            break;
        }

        case NODE_RETURN:
            prescan_expression(scan, ((ReturnNode *)statement)->expression, 1);
            break;

        case NODE_MATCH_STATEMENT:
            prescan_expression(scan, statement, 0);
            break;

        case NODE_BREAK_STATEMENT:
        case NODE_CONTINUE_STATEMENT:
            break;

        default:
            prescan_expression(scan, statement, 0);
            break;
    }
}

static void prescan_statement_list(NllScan *scan, ASTNode *statement) {
    for (; statement; statement = statement->next) {
        prescan_statement(scan, statement);
    }
}

// 预扫描一个表达式。`leaks` 表示这个表达式的值可能活过当前语句。
static void prescan_expression(NllScan *scan, ASTNode *expression, int leaks) {
    if (!expression) return;

    switch (expression->type) {
        case NODE_LITERAL:
        case NODE_SIZEOF:
            return;

        case NODE_IDENTIFIER:
            last_use_record(scan->context, ((IdentifierNode *)expression)->name,
                            scan->index, leaks);
            return;

        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            prescan_expression(scan, binary->left, leaks);
            prescan_expression(scan, binary->right, leaks);
            return;
        }

        case NODE_REFERENCE:
            // 取地址的结果是个指针，可能活过这条语句。
            prescan_expression(scan, ((ReferenceNode *)expression)->target, 1);
            return;

        case NODE_DEREFERENCE:
            // 解引用取出来的是**值**（拷贝），不是把指针交出去，所以不按逃逸处理 ——
            // 否则 `*r` 出现在实参位置就会让 r 的借用被迫活到作用域结束，
            // 而 `*r` 正是推荐的解引用写法。
            prescan_expression(scan, ((DereferenceNode *)expression)->operand, 0);
            return;

        case NODE_CAST:
            prescan_expression(scan, ((CastNode *)expression)->expression, leaks);
            return;

        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            // 方法调用的接收者写在调用名的点号前面。
            const char *dot = strrchr(call->name, '.');
            if (dot && dot != call->name) {
                char buffer[256];
                size_t length = (size_t)(dot - call->name);
                if (length < sizeof(buffer)) {
                    memcpy(buffer, call->name, length);
                    buffer[length] = '\0';
                    last_use_record(scan->context, buffer, scan->index, 1);
                }
            }
            // 实参可能被调用方存起来，按逃逸算。
            for (ASTNode *argument = call->arguments; argument;
                 argument = argument->next) {
                prescan_expression(scan, argument, 1);
            }
            return;
        }

        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)expression;
            // 下标取出来的是元素值（拷贝），指针本身没交出去，所以基对象不算逃逸。
            // `r[0] = 1` 这种最常见的用法正是靠这条才不至于把 r 判成逃逸。
            prescan_expression(scan, index->array, 0);
            prescan_expression(scan, index->index, 0);
            return;
        }

        case NODE_FIELD_ACCESS: {
            FieldAccessNode *access = (FieldAccessNode *)expression;
            // 字段可能是引用，值逃出去就等于基对象的借用逃出去。
            prescan_expression(scan, access->object, leaks);
            return;
        }

        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)expression;
            for (ASTNode *node = literal->fields; node; node = node->next) {
                prescan_expression(scan, ((StructInitFieldNode *)node)->expression,
                                   1);
            }
            return;
        }

        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)expression;
            for (ASTNode *element = array->elements; element;
                 element = element->next) {
                prescan_expression(scan, element, 1);
            }
            return;
        }

        case NODE_TRY:
            prescan_expression(scan, ((TryNode *)expression)->inner, leaks);
            return;

        case NODE_MATCH_STATEMENT: {
            MatchStatementNode *match_node = (MatchStatementNode *)expression;
            prescan_expression(scan, match_node->expression, 0);
            for (ASTNode *node = match_node->arms; node; node = node->next) {
                MatchArmNode *arm = (MatchArmNode *)node;
                prescan_statement_list(scan, arm->body);
                prescan_expression(scan, arm->value, 1);
            }
            return;
        }

        default:
            scan->unknown = 1;
            return;
    }
}

// `TAP_BORROW_LEXICAL=1` 关掉 NLL，退回词法语义，方便对比和排查误报。
static int borrow_nll_enabled_from_env(void) {
    const char *flag = getenv("TAP_BORROW_LEXICAL");
    if (!flag || !*flag) return 1;
    return strcmp(flag, "0") == 0;
}

// 每个函数开始前跑一次：清表、预扫描、算别名传播。
static void prepare_nll(BorrowContext *context, FunctionNode *function) {
    free_last_uses(context);
    context->statement_index = 0;
    context->nll_enabled = borrow_nll_enabled_from_env();
    if (!context->nll_enabled) return;

    NllScan scan;
    scan.context = context;
    scan.index = 0;
    scan.unknown = 0;
    scan.edges = NULL;
    prescan_statement_list(&scan, function->body);
    if (scan.unknown) {
        // 有没认全的节点：整个函数退回词法语义，宁可少放行也不误判悬垂。
        context->nll_enabled = 0;
        free_last_uses(context);
    } else {
        propagate_aliases(&scan);
    }
    free_alias_edges(scan.edges);
    context->statement_index = 0; // 正式遍历从 1 开始重新编号
}

// 把刚建立的持久借用的失效点收窄到「引用变量最后一次被使用的语句」。
//
// `stop` 是 walk_expression 之后、登记借用之前的链表头 —— 这一段里新增的记录就是
// 这条 let 建立的借用（前面 walk_expression 产生的临时记录已经排在 stop 之后了）。
static void apply_borrow_lifetime(BorrowContext *context, BorrowRecord *stop,
                                  const char *name) {
    if (!context->nll_enabled) return;

    LastUseEntry *entry = last_use_find(context, name);
    // 没找到使用（一次都没出现过）、或者指针会逃出这条语句 → 活到作用域结束。
    if (!entry || entry->leaks || entry->last_stmt < 0) return;
    // 只在声明之前用过同名变量（影子声明），编号对不上，保守处理。
    if (entry->last_stmt < context->statement_index) return;

    for (BorrowRecord *record = context->borrows; record && record != stop;
         record = record->next) {
        record->dead_at = entry->last_stmt;
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

        case NODE_IDENTIFIER: {
            LValueInfo info;
            resolve_lvalue(context, expression, &info);
            check_plain_use(context, info.base, info.path, expression);
            break;
        }

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
            LValueInfo info;
            resolve_lvalue(context, expression, &info);
            if (info.base) {
                // 下标表达式本身就是一个左值：只按这条路径查，别把整个数组算成
                // 「读过一遍」，否则 `&mut v[0]` 会拦住 `v[1]`。
                check_plain_use(context, info.base, info.path, expression);
            } else {
                walk_expression(context, index->array);
            }
            walk_expression(context, index->index);
            break;
        }

        case NODE_FIELD_ACCESS: {
            FieldAccessNode *access = (FieldAccessNode *)expression;
            LValueInfo info;
            resolve_lvalue(context, expression, &info);
            if (info.base) {
                check_plain_use(context, info.base, info.path, expression);
            } else {
                // 接收者是调用结果、或者路径穿过了指针：退回原来的遍历方式。
                walk_expression(context, access->object);
            }
            break;
        }

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
    // 语句编号：和 prepare_nll 的预扫描按同样的顺序递增，NLL 的失效点才对得上。
    context->statement_index++;
    // 临时借用是**语句级**的：上一句的 `&mut x` 到这一句就不存在了。
    clear_temp_borrows(context);
    switch (statement->type) {
        case NODE_VAR_DECL: {
            VarDeclNode *declaration = (VarDeclNode *)statement;
            walk_expression(context, declaration->expression);
            // 这里之后的记录才是这条 let 建立的持久借用，用来给 NLL 收窄失效点。
            BorrowRecord *borrow_mark = context->borrows;

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
                mark_persistent_borrow(context, info.base, info.path,
                                       reference->is_mutable);
            }
            mark_method_receiver_borrow(context, declaration->expression,
                                        statement);

            // NLL：借用的寿命收到「这个引用变量最后一次被使用」为止。
            apply_borrow_lifetime(context, borrow_mark, declaration->name);

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

            LValueInfo info;
            resolve_identifier(context, assignment->name, &info);
            check_write_target(context, &info, statement);

            // 寿命与别名登记仍然按「整个引用变量」做：`r = &other` 改的是 r 这个槽，
            // 它指向哪里是类型层面的事，和字段路径无关。
            BorrowSymbol *target = info.base;
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
            resolve_lvalue(context, (ASTNode *)assignment->target, &info);
            check_write_target(context, &info, statement);
            // 只走下标表达式：目标本身是写入位置，已经查过了。再当读表达式走一遍
            // 会对同一个错误报两条（「不能赋值」+「不能使用」）。
            walk_expression(context, assignment->target->index);
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
            if (reference_rank_of(context, expression, &pointee_rank)) {
                if (pointee_rank >= 0) {
                    report_error_soft(
                        statement,
                        "cannot return a reference to a value that lives in this function");
                    // 中文：不能返回指向本函数内局部值的引用
                }
                break;
            }

            // 引用藏进结构体 / 数组字面量之后 reference_rank_of 认不出来（字面量
            // 本身的类型不是引用），只能按字段递归查一遍。见 check_escaping_borrow。
            if (expression && (expression->type == NODE_STRUCT_LITERAL ||
                               expression->type == NODE_ARRAY_LITERAL)) {
                check_escaping_borrow(context, expression, NULL, statement);
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
    context->borrows = NULL;
    context->rank = 0;
    // NLL 的活跃性分析：必须在正式遍历之前跑完（正式遍历看不到后面的使用）。
    prepare_nll(context, function);

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
    // 兜底：参数（秩 0）上的借用不会在 pop 里被清掉，函数结束时统一释放。
    clear_all_borrows(context);

    // 签名检查放在函数体之后：函数体里的错误更具体（「返回了局部值的引用」比
    // 「引用参数不是恰好一个」更能说明问题），先报更有用的那条。
    check_borrow_signature(function);
    free_last_uses(context);
}

int check_borrows(ProgramNode *program) {
    if (!program) return 0;

    BorrowContext context;
    context.program = program;
    context.symbols = NULL;
    context.borrows = NULL;
    context.rank = 0;
    context.statement_index = 0;
    context.nll_enabled = 0;
    context.last_uses = NULL;

    // 只统计本次借用检查报了几条：调用方可能已经跑过别的阶段（现在不会，但
    // 计数是全局的，用差值才不会把别人的错误算到自己头上）。
    int before = tap_error_count();

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        check_function(&context, (FunctionNode *)node);
    }

    return tap_error_count() > before;
}
