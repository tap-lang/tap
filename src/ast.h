#ifndef AST_H
#define AST_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// AST节点类型
enum NodeType {
    NODE_PROGRAM,                   // 程序节点
    NODE_IMPORT,                    // 模块导入节点
    NODE_FUNCTION,                  // 函数节点
    NODE_STATEMENT,                 // 语句节点
    NODE_EXPRESSION,                // 表达式节点
    NODE_IDENTIFIER,                // 标识符节点
    NODE_LITERAL,                   // 字面量节点
    NODE_RETURN,                    // 返回语句节点
    NODE_BINARY_OP,                 // 二元操作符节点
    NODE_REFERENCE,                 // 取地址表达式节点
    NODE_SIZEOF,                    // 编译期类型大小表达式节点
    NODE_CAST,                      // 显式类型转换表达式节点（expr as Type）
    NODE_VAR_DECL,                  // 变量声明节点
    NODE_ASSIGNMENT,                // 赋值语句节点
    NODE_ARRAY_LITERAL,             // 数组字面量节点
    NODE_INDEX_EXPRESSION,          // 数组索引表达式节点
    NODE_INDEX_ASSIGNMENT,          // 数组索引赋值节点
    NODE_FIELD_ASSIGNMENT,          // 字段赋值节点（`target 字段 = 表达式`）
    NODE_ENUM,                      // 枚举声明节点
    NODE_ENUM_VARIANT,              // 枚举成员声明节点
    NODE_STRUCT,                    // 结构体声明节点
    NODE_STRUCT_FIELD,              // 结构体字段声明节点
    NODE_STRUCT_LITERAL,            // 结构体字面量节点
    NODE_STRUCT_INIT_FIELD,         // 结构体初始化字段节点
    NODE_FUNCTION_CALL,             // 函数调用节点
    NODE_IF_STATEMENT,              // 条件语句节点
    NODE_FOR_STATEMENT,             // for 循环节点
    NODE_BREAK_STATEMENT,           // break 语句节点
    NODE_CONTINUE_STATEMENT,        // continue 语句节点
    NODE_MATCH_STATEMENT,           // match 解构语句节点
    NODE_MATCH_ARM,                 // match 分支节点
    NODE_TRY,                       // ? 传播表达式节点
    NODE_VAR_TYPE,                  // 数据类型节点
    NODE_TRAIT,                     // trait 接口声明节点
    NODE_TRAIT_METHOD,              // trait 方法签名节点
    NODE_IMPL,                      // impl 接口实现节点
    NODE_FIELD_ACCESS,              // 字段访问表达式（接收者是任意表达式）
    NODE_DEREFERENCE                // 解引用表达式（*p）
};

// 字面量类型
enum LiteralType {
    LITERAL_INT,                    // 整数 int
    LITERAL_UINT,                   // 无符号整数 uint
    LITERAL_I8,                     // 8位有符号整数 i8
    LITERAL_U8,                     // 8位无符号整数 u8
    LITERAL_I16,                    // 16位有符号整数 i16
    LITERAL_U16,                    // 16位无符号整数 u16
    LITERAL_I32,                    // 32位整数 i32
    LITERAL_U32,                    // 32位无符号整数 u32
    LITERAL_I64,                    // 64位整数 i64
    LITERAL_U64,                    // 64位无符号整数 u64
    LITERAL_I128,                   // 128位有符号整数 i128
    LITERAL_U128,                   // 128位无符号整数 u128
    LITERAL_FLOAT,                  // 浮点数 float
    LITERAL_F32,                    // 32位浮点数 f32
    LITERAL_F64,                    // 64位浮点数 f64
    LITERAL_STRING,                 // 长度化字符串 String
    LITERAL_C_STRING,               // C 兼容零结尾字符串指针 c_string
    LITERAL_BOOL,                   // 布尔值 bool
    LITERAL_CHAR,                   // Unicode 码点 char
    LITERAL_NULL,                   // 空指针字面量：没有自己的类型，只能赋给 *T
    // 无返回值。**不是值类型**：只出现在函数的返回位置（省略 `: T` 的声明、
    // 省略返回类型的函数指针签名）。它进不了变量、参数、字段，也进不了泛型实参 ——
    // 那些位置一旦出现就是编译错误，所以 tap_type_equal / clone / generics 都不必处理它。
    LITERAL_VOID
};

// 二元操作符类型  + - * / == != < > <= >= && ||
enum BinaryOpType {
    OP_ADD,                         // 加号 +
    OP_SUBTRACT,                    // 减号 -
    OP_MULTIPLY,                    // 乘号 *
    OP_DIVIDE,                      // 除号 /
    OP_MODULO,                      // 取模 %
    OP_EQUAL,                       // 等于号 ==
    OP_NOT_EQUAL,                   // 不等于号 !=
    OP_LESS_THAN,                   // 小于号 <
    OP_GREATER_THAN,                // 大于号 >
    OP_LESS_THAN_OR_EQUAL,          // 小于等于号 <=
    OP_GREATER_THAN_OR_EQUAL,       // 大于等于号 >=
    OP_AND,                         // 与运算符 &&
    OP_OR,                          // 或运算符 ||
    // 位运算必须追加在末尾：Codegen 用 `>= OP_EQUAL && <= OP_GREATER_THAN_OR_EQUAL`
    // 判断比较、用 `>= OP_ADD && <= OP_MODULO` 判断算术，插在中间会破坏这两个范围判断。
    OP_BITWISE_AND,                 // 按位与 &
    OP_BITWISE_OR,                  // 按位或 |
    OP_BITWISE_XOR,                 // 按位异或 ^
    OP_SHIFT_LEFT,                  // 左移 <<
    OP_SHIFT_RIGHT                  // 右移 >>
};

// 源码位置。
//
// file 指向 Lexer 持有的文件名，节点不拥有它，free_ast 不释放；同一个文件里的
// 所有节点共享同一个指针。line / column 从 1 开始，全 0 表示位置未知。
//
// 位置统一放在 ASTNode 里，而不是让每个节点各存一份三字段：诊断代码只需要
// `node->loc`，不必先知道具体节点类型。
typedef struct {
    const char *file;
    uint32_t line;
    uint32_t column;
} SourceLoc;

// 前置声明：ASTNode 要挂一个指向类型节点的指针，而 VarTypeNode 定义在它后面。
struct VarTypeNode;

// 基础AST节点结构
typedef struct ASTNode {
    enum NodeType type;             // 节点类型
    struct ASTNode *next;           // 用于链表结构
    SourceLoc loc;                  // 源码位置；未知时全 0
    // 表达式的完整类型（标量也有）；NULL 表示还没推断过。
    //
    // 由 Codegen 的类型推断按需填充并缓存，节点拥有它，free_ast 负责释放。
    // 同一个表达式节点在 AST 里唯一、符号表位置固定，所以推断结果不会随调用时机变化。
    // 独立 Sema 阶段落地后，这里就是 Sema 与 Codegen 之间的接口。
    struct VarTypeNode *resolved_type;
} ASTNode;

// 程序节点
typedef struct {
    ASTNode base;
    ASTNode *imports;      // 模块导入列表
    ASTNode *enums;        // 枚举声明列表
    ASTNode *structs;      // 结构体声明列表
    ASTNode *constants;    // 顶层常量列表
    ASTNode *functions;    // 函数列表
    ASTNode *traits;       // trait 声明列表
    ASTNode *impls;        // impl 实现列表
} ProgramNode;

// 模块导入节点
//
// 支持四种写法：
//   import std.math;                 整模块导入，按 `math.sin(...)` 访问（名称空间）
//   import std.math as m;            整模块导入并改名，按 `m.sin(...)` 访问
//   import std.math.*;               通配导入，pub 成员直接按裸名 `sin(...)` 访问
//   import std.math.sin;             单成员导入，按裸名 `sin(...)` 访问
//   import std.math.sin as sine;     单成员导入并改名，按裸名 `sine(...)` 访问
typedef struct {
    ASTNode base;
    char *module_name;     // 点分隔模块名，例如 std.math（解析后已去掉成员段）
    char *member;          // 单成员导入的成员名；NULL 表示整模块/通配导入
    char *alias;           // 绑定的裸名（名称空间导入时是名称空间）；通配导入为 NULL
    int is_wildcard;       // 1 表示 `import mod.*`：导入全部 pub 成员
    char *filename;        // import 所在文件
    int line;              // 模块名所在行
    int column;            // 模块名所在列
} ImportNode;

// 变量类型节点
typedef struct VarTypeNode {
    ASTNode base;
    enum LiteralType type;  // 标量类型，数组节点中保留最终元素类型
    char *enum_name;        // 非空时表示枚举类型，后端按 i32 降低
    char *struct_name;      // 非空时表示结构体类型
    ASTNode *type_arguments; // 泛型具名类型的实参列表（VarTypeNode）
    int is_array;
    int is_pointer;
    // 借用引用 `&T` / `&mut T`。
    //
    // 和 is_pointer **同时为 1**：借用在内存里就是一个指针，Codegen 的所有指针路径
    // （GEP、load/store、按引用传参）原样复用，不必逐处改判断。额外的两个标志只服务
    // 类型系统 —— 让 `&T` 和 `*T` 在 tap_type_equal 里不相等，并且能被借用检查区分。
    //
    // 语义差别：`*T` 是裸指针，编译器不管别名也不管寿命；`&T` 承诺「借用期内源对象
    // 有效且不被独占修改」，由 src/borrow.c 在编译期检查。
    int is_reference;
    int reference_mutable; // 1 表示 `&mut T`（独占借用），0 表示 `&T`（共享只读借用）
    // 函数指针：`*fn(i32, i32): i32`。
    //
    // 和 is_pointer 互斥 —— 它不是数据指针，解引用 / 下标 / 取元素类型全都不适用。
    // 单独一个标志位而不是复用 is_pointer，是因为几十处 `is_pointer` 的判断都默认
    // element_type 非空，混进去会在 validate_var_type、free、dump 这些地方踩空指针。
    int is_function_pointer;
    ASTNode *parameter_types;        // 参数类型列表（VarTypeNode）；无参为 NULL
    struct VarTypeNode *return_type; // 返回类型；NULL 表示不返回值
    uint64_t array_length;
    struct VarTypeNode *element_type; // 数组/指针拥有的递归元素类型
} VarTypeNode;

// 枚举成员声明节点
typedef struct {
    ASTNode base;
    char *name;             // 成员名
    ASTNode *payload_types; // 载荷类型列表（VarTypeNode），无载荷时为 NULL
    unsigned tag;           // 降级时回填：从 0 开始的成员序号
    unsigned field_index;   // 降级时回填：首个载荷字段在内部结构体中的下标
} EnumVariantNode;

// 枚举声明节点
typedef struct {
    ASTNode base;
    char *name;            // 枚举类型名
    ASTNode *type_params;  // 泛型类型参数列表（IdentifierNode）
    ASTNode *variants;     // 枚举成员列表（EnumVariantNode）
    int has_payload;       // 任一成员带载荷时置位；由降级 pass 回填
    int is_pub;            // pub 声明：可以被其他模块导入
} EnumNode;

// 结构体声明节点
typedef struct {
    ASTNode base;
    char *name;            // 结构体类型名
    ASTNode *type_params;  // 泛型类型参数列表（IdentifierNode）
    ASTNode *fields;       // 字段声明列表（StructFieldNode）
    int is_tagged_enum;    // 由载荷枚举降级生成，而非用户声明
    char *tagged_enum_name; // 降级生成时记录来源枚举名；实例化后名字带 $ 后缀，靠它映射回枚举
    int is_pub;            // pub 声明：可以被其他模块导入
} StructNode;

// 结构体字段声明节点
typedef struct {
    ASTNode base;
    char *name;            // 字段名
    VarTypeNode *field_type; // 字段类型
} StructFieldNode;

// 结构体字面量节点
typedef struct {
    ASTNode base;
    char *struct_name;     // 结构体类型名
    ASTNode *type_arguments; // 泛型结构体实参列表（VarTypeNode）
    ASTNode *fields;       // 初始化字段列表（StructInitFieldNode）
} StructLiteralNode;

// 结构体初始化字段节点
typedef struct {
    ASTNode base;
    char *name;            // 字段名
    ASTNode *expression;   // 字段初始化表达式
} StructInitFieldNode;

// 函数节点
typedef struct {
    ASTNode base;
    char *name;            // 函数名
    char *original_name;   // 改名前的原名；模块私有符号被改名成 __tap_module_N.name 后，
                           // 诊断优先用原名，避免泄漏内部符号名
    char *filename;        // 函数定义所在文件
    int line;              // 函数名所在行
    int column;            // 函数名所在列
    int is_extern;         // 外部函数只生成 LLVM 声明
    int is_variadic;       // 参数列表末尾带 `...`（变参 ABI，仅 extern 允许）
    int is_pub;            // pub 声明：可以被其他模块导入
    ASTNode *type_params;  // 泛型类型参数列表（IdentifierNode）
    ASTNode *params;       // 参数列表
    ASTNode *param_types;  // 参数类型列表
    ASTNode *body;         // 函数体语句列表
    VarTypeNode *return_type; // 返回值类型
} FunctionNode;

// 标识符节点
typedef struct {
    ASTNode base;
    char *name;            // 标识符名称
    ASTNode *enum_type_arguments; // 显式枚举类型实参，仅 Enum<T>.Member 形式使用
    ASTNode *type_bounds;  // 作为泛型类型参数时的约束（TraitNode 名字的 VarTypeNode 列表）
} IdentifierNode;

// 字面量节点
typedef struct {
    ASTNode base;
    enum LiteralType literal_type;
    char *integer_text;       // 十进制整数文本（0x/0o/0b 原文已归一化），用于构造 i128/u128 常量
    // 只有 LITERAL_STRING 用它：`value.string_value` 的字节数。
    // 字符串转义可以产出 NUL（`\0`、`\x00`、`\u{0}`），所以不能用 strlen 量长度。
    size_t string_length;
    union {
        uint64_t int_value;
        uint32_t char_value;
        char *string_value;
        double float_value;
        int bool_value;
    } value;
} LiteralNode;

// 返回语句节点
typedef struct {
    ASTNode base;
    ASTNode *expression;   // 返回表达式
} ReturnNode;

// 二元操作节点
typedef struct {
    ASTNode base;
    enum BinaryOpType op_type;
    ASTNode *left;         // 左操作数
    ASTNode *right;        // 右操作数
} BinaryOpNode;

// 取地址表达式节点
//
// `&x` 产生共享借用 `&T`，`&mut x` 产生独占借用 `&mut T`。两者生成同一个指针值，
// 区别只在类型和借用检查。
typedef struct {
    ASTNode base;
    ASTNode *target;       // 被取地址的可寻址表达式
    int is_mutable;        // 1 表示 `&mut x`
} ReferenceNode;

// 解引用表达式节点（`*p`）。取值直接用；`*p = x` 这种左值写法由 parser 脱糖成
// `p[0] = x`（见 parse_simple_statement），所以这里不需要区分「读」和「写」。
typedef struct {
    ASTNode base;
    ASTNode *operand;      // 被解引用的指针表达式，类型必须是 *T
} DereferenceNode;

// 编译期类型大小表达式节点
typedef struct {
    ASTNode base;
    VarTypeNode *operand_type; // 要计算 ABI 大小的完整类型
} SizeofNode;

// 显式类型转换表达式节点。
typedef struct {
    ASTNode base;
    ASTNode *expression;      // 待转换的表达式
    VarTypeNode *target_type; // 转换目标类型
    char *filename;           // 转换操作所在文件
    int line;                 // `as` 所在行
    int column;               // `as` 所在列
} CastNode;


// 变量声明节点
typedef struct {
    ASTNode base;
    char *name;            // 变量名
    VarTypeNode *type;     // 变量类型
    ASTNode *expression;   // 初始化表达式
    int is_const;          // 是否为 const 常量声明
    int is_pub;            // pub 声明：可以被其他模块导入（仅顶层常量有意义）
} VarDeclNode;

// 赋值语句节点
typedef struct {
    ASTNode base;
    char *name;
    ASTNode *expression;
} AssignmentNode;

typedef struct {
    ASTNode base;
    ASTNode *elements;
    uint64_t count;
    int is_repeat;
    uint64_t repeat_count;
} ArrayLiteralNode;

typedef struct {
    ASTNode base;
    ASTNode *array;
    ASTNode *index;
} IndexExpressionNode;

// 字段访问表达式：`接收者.字段名`，接收者是任意表达式。
//
// 两种来源：parser 对「接收者不是标识符」的写法直接产出（`arr[0].x`、`make().x`），
// names.c 把「根是局部变量」的带点标识符改写成链（`a.b.c` → 两层 FIELD_ACCESS）。
// 改写之后，字段访问在 AST 里只有这一种表示，读和写（FieldAssignmentNode）共用同一套
// 地址计算。`表达式.方法()` 形式的调用还不支持。
typedef struct {
    ASTNode base;
    ASTNode *object;       // 接收者表达式
    char *field;           // 字段名
    char *filename;
    int line;
    int column;
} FieldAccessNode;

typedef struct {
    ASTNode base;
    IndexExpressionNode *target;
    ASTNode *expression;
} IndexAssignmentNode;

// 字段赋值：`p.x = v`、`p.inner.value = v`、`make().x = v`。
//
// 和 IndexAssignmentNode 同形：target 是一个**可寻址的字段访问链**，expression 是新值。
// 在 `names.c` 把 `p.x = v` 从 AssignmentNode 的字符串形式改写成这个节点之前，写路径
// 必须靠 `strchr(name, '.')` 再拆一遍点号，和读路径的 FIELD_ACCESS 是两套实现
// （`&mut a.b.c` 报 `pointer expression type mismatch` 就是这么来的）。
typedef struct {
    ASTNode base;
    FieldAccessNode *target;
    ASTNode *expression;
} FieldAssignmentNode;


// 函数调用节点
typedef struct {
    ASTNode base;
    char *filename;
    int line;
    int column;
    char *name;            // 函数名；方法调用时是**方法名本身**（不含接收者）
    // 方法接收者：`p.shift(1)` 的 `p`、`p.inner.value()` 的 `p.inner`、`make().x()` 的调用结果。
    // NULL 表示普通函数调用（含 `math.abs` 这类模块函数）。早先接收者是名字里的一段
    // 字符串，靠 `strrchr(name, '.')` 拆，于是 `a.b.method()` 和多级接收者都做不了。
    ASTNode *receiver;
    ASTNode *type_arguments; // 显式泛型实参列表（VarTypeNode），用于泛型函数
    ASTNode *enum_type_arguments; // 显式枚举类型实参，仅 Enum<T>.Member(...) 构造使用
    ASTNode *arguments;    // 参数列表
    int forwards_variadic; // 实参列表末尾带 `...`，表示把当前函数的变参原样转发给被调用者
} FunctionCallNode;

// match 分支节点
typedef struct {
    ASTNode base;
    char *enum_name;       // 模式中的枚举名；通配分支为 NULL
    char *variant_name;    // 模式中的成员名；通配分支为 NULL
    ASTNode *bindings;     // 载荷绑定变量列表（IdentifierNode），可空
    ASTNode *body;         // 分支语句链表（语句形式）
    ASTNode *value;        // 分支值表达式（表达式形式），二选一
    char *filename;        // 分支所在文件，用于诊断
    int line;
    int column;
} MatchArmNode;

// match 解构语句节点
typedef struct {
    ASTNode base;
    ASTNode *expression;   // 被匹配的枚举值
    ASTNode *arms;         // MatchArmNode 链表
} MatchStatementNode;

// `?` 传播表达式节点。
// 内层求值为 Result：成功时整个表达式取载荷值，失败时从当前函数提前返回 Err。
typedef struct {
    ASTNode base;
    ASTNode *inner;        // 被传播的 Result 表达式
    char *filename;        // 所在文件，用于诊断
    int line;
    int column;
} TryNode;

// trait 方法签名节点。trait 里只声明签名，不写函数体。
//
// 接收者有三种写法，语义完全不同，所以除了「有没有」还要记住「是哪种」：
//   `self`       按值接收，impl 里首参类型写成 `T`（老写法，保留兼容）
//   `&self`      共享借用，impl 里首参类型写成 `&T`
//   `&mut self`  独占借用，impl 里首参类型写成 `&mut T`
// param_types 里是除接收者外的参数类型。
typedef struct {
    ASTNode base;
    char *name;             // 方法名
    int has_self;           // 是否带接收者（v1 要求必须带）
    int self_is_reference;  // 接收者写成 `&self` 或 `&mut self`
    int self_is_mutable;    // 接收者写成 `&mut self`（隐含 self_is_reference）
    ASTNode *param_types;   // 除接收者外的参数类型列表（VarTypeNode）
    VarTypeNode *return_type;
    char *filename;
    int line;
    int column;
} TraitMethodNode;

// trait 声明节点：一组方法签名，用来约束泛型类型参数。
typedef struct {
    ASTNode base;
    char *name;             // trait 名
    ASTNode *methods;       // TraitMethodNode 列表
    int is_pub;             // pub trait：可以被其他模块导入
    char *filename;
    int line;
    int column;
} TraitNode;

// impl 块里一个方法到脱糖后函数的引用。函数本身挂在 ProgramNode.functions 上，
// 这里只是额外的入口（**不拥有** FunctionNode），供签名校验按源语言顺序取回方法。
typedef struct ImplMethodLink {
    FunctionNode *function;
    struct ImplMethodLink *next;
} ImplMethodLink;

// impl 节点：为某个具体类型实现某个 trait。
//
// 块内的方法在解析时就被脱糖成**普通的顶层函数**（名字加上类型后缀，如 `show$Point`），
// 直接挂进 ProgramNode.functions —— 单态化、方法查找和 Codegen 都不用为它们开特例。
// 这里只保留归属信息（trait / 类型）和方法引用，供签名校验用。
typedef struct {
    ASTNode base;
    char *trait_name;        // 实现的 trait 名；固有 impl（`impl Type { }`）为 NULL
    char *type_name;         // 目标类型名（v1 只支持具名非泛型类型）
    ImplMethodLink *methods; // 块内方法（脱糖后的函数），按源语言顺序
    char *filename;
    int line;
    int column;
} ImplNode;

// 把源文件名登记成进程级常量，同一个路径只保留一份，返回的指针永久有效。
//
// SourceLoc.file 需要在整个编译过程里保持有效，而 Lexer 的 filename 是借用的
// （module.c / prelude.c 解析完就释放路径）。节点又不能各自 strdup 一份，
// 所以统一走这里。
const char *intern_source_file(const char *path);

// 创建节点的函数声明
ProgramNode *create_program();
ImportNode *create_import(
    const char *module_name, const char *member, const char *alias, int is_wildcard,
    const char *filename, int line, int column);
FunctionNode *create_function(char *name);
EnumNode *create_enum(char *name);
EnumVariantNode *create_enum_variant(char *name);
StructNode *create_struct(char *name);
StructFieldNode *create_struct_field(char *name, VarTypeNode *field_type);
StructLiteralNode *create_struct_literal(char *struct_name);
StructInitFieldNode *create_struct_init_field(char *name, ASTNode *expression);
IdentifierNode *create_identifier(char *name);
VarTypeNode *create_var_type(enum LiteralType type);
VarTypeNode *create_enum_type(const char *name);
VarTypeNode *create_struct_type(const char *name);
// 数组类型接管 element_type 的所有权。
VarTypeNode *create_array_type(VarTypeNode *element_type, uint64_t length);
// 指针类型接管 element_type 的所有权。
VarTypeNode *create_pointer_type(VarTypeNode *element_type);
// 借用类型 `&T` / `&mut T`。element_type 的所有权被接管。
VarTypeNode *create_reference_type(VarTypeNode *element_type, int is_mutable);
// 函数指针类型接管 parameter_types 链表和 return_type 的所有权。
VarTypeNode *create_function_pointer_type(ASTNode *parameter_types,
                                          VarTypeNode *return_type);
LiteralNode *create_int_literal(uint64_t value);
LiteralNode *create_int_literal_text(const char *value);
LiteralNode *create_string_literal(char *value);
// 和 create_string_literal 一样复制 value，但长度由调用方给出 —— 字符串里可能有 NUL。
LiteralNode *create_string_literal_bytes(const char *value, size_t length);
LiteralNode *create_float_literal(double value);
LiteralNode *create_bool_literal(int value);
// 创建 null 字面量。它没有自己的类型，只能出现在有 *T 目标类型的上下文里
// （`let p: *T = null`、`p == null`、作为 *T 实参）。
LiteralNode *create_null_literal(void);
LiteralNode *create_char_literal(uint32_t value);
ReturnNode *create_return(ASTNode *expression);
BinaryOpNode *create_binary_op(enum BinaryOpType op_type, ASTNode *left, ASTNode *right);
ReferenceNode *create_reference(ASTNode *target, int is_mutable);
// 创建解引用表达式节点，并接管 operand 的所有权。
DereferenceNode *create_dereference(ASTNode *operand);
SizeofNode *create_sizeof(VarTypeNode *operand_type);
CastNode *create_cast(ASTNode *expression, VarTypeNode *target_type);
VarDeclNode *create_var_decl(
    char *name, VarTypeNode *type, ASTNode *expression, int is_const);
AssignmentNode *create_assignment(const char *name, ASTNode *expression);
ArrayLiteralNode *create_array_literal(void);
void add_array_element(ArrayLiteralNode *array, ASTNode *element);
void set_array_repeat(ArrayLiteralNode *array, uint64_t repeat_count);
IndexExpressionNode *create_index_expression(ASTNode *array, ASTNode *index);
// 字段访问接管 object 的所有权。
FieldAccessNode *create_field_access(ASTNode *object, const char *field);
IndexAssignmentNode *create_index_assignment(
    IndexExpressionNode *target, ASTNode *expression);
// 字段赋值接管 target 和 expression 的所有权。
FieldAssignmentNode *create_field_assignment(
    FieldAccessNode *target, ASTNode *expression);
FunctionCallNode *create_function_call(const char *name);

// 添加子节点的函数
void add_function(ProgramNode *program, FunctionNode *function);
void add_constant(ProgramNode *program, VarDeclNode *constant);
void add_enum(ProgramNode *program, EnumNode *enum_node);
void add_enum_type_param(EnumNode *enum_node, IdentifierNode *type_param);
void add_enum_variant(EnumNode *enum_node, EnumVariantNode *variant);
void add_enum_variant_payload(EnumVariantNode *variant, VarTypeNode *type);
MatchStatementNode *create_match_statement(ASTNode *expression);
TryNode *create_try(ASTNode *inner);
MatchArmNode *create_match_arm(char *enum_name, char *variant_name);
void add_match_arm(MatchStatementNode *statement, MatchArmNode *arm);
void add_match_binding(MatchArmNode *arm, IdentifierNode *binding);
void add_struct(ProgramNode *program, StructNode *struct_node);
void add_struct_type_param(StructNode *struct_node, IdentifierNode *type_param);
void add_struct_field(StructNode *struct_node, StructFieldNode *field);
void add_var_type_argument(VarTypeNode *type, VarTypeNode *type_argument);
void add_struct_literal_type_argument(
    StructLiteralNode *literal, VarTypeNode *type_argument);
void add_struct_init_field(StructLiteralNode *literal, StructInitFieldNode *field);
void add_import(ProgramNode *program, ImportNode *import_node);
void add_param(FunctionNode *function, IdentifierNode *param);
void add_type_param(FunctionNode *function, IdentifierNode *type_param);
void add_param_type(FunctionNode *function, VarTypeNode *type);
void add_statement(FunctionNode *function, ASTNode *statement);
void add_argument(FunctionCallNode *function_call, ASTNode *argument);
void add_type_argument(FunctionCallNode *function_call, VarTypeNode *type_argument);
void append_type_argument(ASTNode **list, VarTypeNode *type_argument);

// trait / impl
TraitMethodNode *create_trait_method(const char *name, const char *filename, int line, int column);
TraitNode *create_trait(const char *name, const char *filename, int line, int column);
ImplNode *create_impl(const char *trait_name, const char *type_name,
                      const char *filename, int line, int column);
void add_trait(ProgramNode *program, TraitNode *trait_node);
void add_impl(ProgramNode *program, ImplNode *impl_node);
void add_trait_method(TraitNode *trait_node, TraitMethodNode *method);
void add_trait_method_param_type(TraitMethodNode *method, VarTypeNode *type);
void add_impl_method(ImplNode *impl_node, FunctionNode *function);
void add_type_param_bound(IdentifierNode *type_param, VarTypeNode *bound);

// 条件语句节点
typedef struct {
    ASTNode base;
    ASTNode *condition;    // 条件表达式
    ASTNode *consequence;  // 条件为真时执行的语句
    // 条件为假时执行的语句链表。`else if` / `elseif` 也统一表示成链表，只是长度为 1
    //（那个 if 节点自己的 next 为 NULL）。因此消费方一律按链表遍历，
    // 不要再用 `alternative->type == NODE_IF_STATEMENT` 去区分「else if」和 else 块：
    // else 块的链表表头恰好是 if 语句时同样命中，会把块内其余语句整段丢掉。
    ASTNode *alternative;
} IfStatementNode;

// 创建条件语句节点
IfStatementNode *create_if_statement(ASTNode *condition, ASTNode *consequence, ASTNode *alternative);

// for 循环节点
typedef struct {
    ASTNode base;
    ASTNode *initializer;
    ASTNode *condition;
    ASTNode *update;
    ASTNode *body;
} ForStatementNode;

ForStatementNode *create_for_statement(
    ASTNode *initializer, ASTNode *condition, ASTNode *update, ASTNode *body);
ASTNode *create_break_statement(void);
ASTNode *create_continue_statement(void);

// 释放AST的函数
void free_ast(ASTNode *node);

// 将语法树打印到 stdout（用于 -parse）
void print_ast(const ProgramNode *program);

#endif // AST_H
