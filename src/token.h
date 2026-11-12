#ifndef TOKEN_H
#define TOKEN_H

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

// 标记类型
enum TokenType {
    // 关键字
    TOKEN_PUB,       // pub 顶层可见性修饰符
    TOKEN_FN,        // fn 函数定义
    TOKEN_EXTERN,    // extern 外部函数声明
    TOKEN_RETURN,    // return 返回语句
    TOKEN_LET,       // let 语句
    TOKEN_CONST,     // const 常量声明
    TOKEN_SIZEOF,    // sizeof 编译期类型大小表达式
    TOKEN_ENUM,      // enum 枚举声明
    TOKEN_STRUCT,    // struct 结构体声明
    TOKEN_IF,        // if 条件语句
    TOKEN_FOR,       // for 循环
    TOKEN_WHILE,     // while 循环
    TOKEN_BREAK,     // break 跳出循环
    TOKEN_CONTINUE,  // continue 进入下一轮循环
    TOKEN_ELSE,      // else 语句
    TOKEN_ELSEIF,    // elseif 语句
    TOKEN_IMPORT,    // import 模块导入
    TOKEN_AS,        // as 导入别名
    TOKEN_MATCH,     // match 载荷枚举解构
    TOKEN_TRAIT,     // trait 接口声明
    TOKEN_IMPL,      // impl 接口实现
    // `mut` 只出现在借用语法里：`&mut T` 是独占借用，`&T` 是共享借用。
    // 它不是变量修饰符 —— tap 的局部变量本来就可写。
    TOKEN_MUT,       // mut 独占借用标记

    // 标识符和字面量
    TOKEN_IDENTIFIER,  // 标识符
    TOKEN_INT,         // 32/64位整数
    TOKEN_UINT,        // 32/64位无符号整数
    TOKEN_I8,          // 8位有符号整数
    TOKEN_U8,          // 8位无符号整数
    TOKEN_I16,         // 16位有符号整数
    TOKEN_U16,         // 16位无符号整数
    TOKEN_I32,         // 32位整数
    TOKEN_U32,         // 32位无符号整数
    TOKEN_I64,         // 64位整数
    TOKEN_U64,         // 64位无符号整数
    TOKEN_I128,        // 128位有符号整数
    TOKEN_U128,        // 128位无符号整数
    TOKEN_F32,         // 32位浮点数
    TOKEN_F64,         // 64位浮点数
    TOKEN_BOOL,        // 布尔值
    TOKEN_STRING,      // String 类型或字符串字面量
    TOKEN_C_STRING,    // C 兼容的零结尾字符串指针 c_string
    TOKEN_CHAR,        // char 类型
    TOKEN_CHAR_LITERAL,// 字符字面量

    // 运算符
    TOKEN_PLUS,                     // +
    TOKEN_MINUS,                    // -
    TOKEN_INCREMENT,                // ++
    TOKEN_DECREMENT,                // --
    TOKEN_MULTIPLY,                 // *
    TOKEN_DIVIDE,                   // /
    TOKEN_MODULO,                   // %
    TOKEN_ASSIGN,                   // =
    TOKEN_FAT_ARROW,                // => match 分支
    TOKEN_EQUAL,                    // ==
    TOKEN_NOT_EQUAL,                // !=
    TOKEN_NOT,                      // !
    TOKEN_LESS_THAN,                // <
    TOKEN_GREATER_THAN,             // >
    TOKEN_LESS_THAN_OR_EQUAL,       // <=
    TOKEN_GREATER_THAN_OR_EQUAL,    // >=
    TOKEN_AND,                      // &&
    TOKEN_OR,                       // ||
    // `&` 在语法上有歧义：一元位置是取地址，二元位置是按位与。词法层无法区分，
    // 统一产出 TOKEN_REFERENCE，由 Parser 按所处位置决定语义。
    TOKEN_REFERENCE,                // &
    TOKEN_BITWISE_OR,               // |
    TOKEN_BITWISE_XOR,              // ^
    TOKEN_BITWISE_NOT,              // ~

    // 分隔符
    TOKEN_LPAREN,      // (
    TOKEN_RPAREN,      // )
    TOKEN_LBRACE,      // {
    TOKEN_RBRACE,      // }
    TOKEN_SEMICOLON,   // ;
    TOKEN_COMMA,       // ,
    TOKEN_COLON,       // :
    TOKEN_DOT,         // .
    TOKEN_ELLIPSIS,    // ... 变参声明
    TOKEN_LBRACKET,    // [
    TOKEN_RBRACKET,    // ]
    TOKEN_QUESTION,    // ?
    TOKEN_NULL,        // null 空指针字面量

    // 特殊标记
    TOKEN_EOF          // 文件结束
};

// 各 TokenType 对应的可读名称。用指定初始化器按下标填，**不依赖枚举顺序** ——
// 往枚举中间插新关键字不会让诊断里的名字错位。
extern const char *TokenNames[];

// 标记结构体
typedef struct {
    enum TokenType type;
    char *lexeme;      // 标记的文本内容
    int line;          // 行号
    int column;        // 列号

    // 只有 TOKEN_STRING 用它：解码转义之后的字节数。
    // 转义可以产出 NUL（`\0`、`\x00`、`\u{0}`），所以 string_value 不能用 strlen 量长度。
    size_t string_length;

    // 根据标记类型存储不同的值
    union {
        unsigned long long int_value;
        uint32_t char_value;
        char *string_value;
        double float_value;
        int bool_value;
    } value;
} Token;

void free_token(Token *token);

#endif // TOKEN_H
