#include "token.h"

// token 类型 → 诊断里显示的名字。
//
// **必须用指定初始化器**：这张表曾经是按位置填的，往枚举中间插一个关键字（比如
// `mut`）就会让后面所有名字整体错位 —— 诊断会打印成 `expected token ','` 而不是
// `';'`，看起来像解析器坏了，实际上是这张表错了。写成 [TOKEN_X] 之后，枚举怎么
// 增删都不会再牵连别的项。
const char *TokenNames[] = {
    [TOKEN_PUB] = "pub",
    [TOKEN_FN] = "fn",
    [TOKEN_EXTERN] = "extern",
    [TOKEN_RETURN] = "return",
    [TOKEN_LET] = "let",
    [TOKEN_CONST] = "const",
    [TOKEN_SIZEOF] = "sizeof",
    [TOKEN_ENUM] = "enum",
    [TOKEN_STRUCT] = "struct",
    [TOKEN_IF] = "if",
    [TOKEN_FOR] = "for",
    [TOKEN_WHILE] = "while",
    [TOKEN_BREAK] = "break",
    [TOKEN_CONTINUE] = "continue",
    [TOKEN_ELSE] = "else",
    [TOKEN_ELSEIF] = "elseif",
    [TOKEN_IMPORT] = "import",
    [TOKEN_AS] = "as",
    [TOKEN_MATCH] = "match",
    [TOKEN_TRAIT] = "trait",
    [TOKEN_IMPL] = "impl",
    [TOKEN_MUT] = "mut",

    [TOKEN_IDENTIFIER] = "identifier",
    [TOKEN_INT] = "int",
    [TOKEN_UINT] = "uint",
    [TOKEN_I8] = "i8",
    [TOKEN_U8] = "u8",
    [TOKEN_I16] = "i16",
    [TOKEN_U16] = "u16",
    [TOKEN_I32] = "i32",
    [TOKEN_U32] = "u32",
    [TOKEN_I64] = "i64",
    [TOKEN_U64] = "u64",
    [TOKEN_I128] = "i128",
    [TOKEN_U128] = "u128",
    [TOKEN_F32] = "f32",
    [TOKEN_F64] = "f64",
    [TOKEN_BOOL] = "bool",
    [TOKEN_STRING] = "String",
    [TOKEN_C_STRING] = "c_string",
    [TOKEN_CHAR] = "char",
    [TOKEN_CHAR_LITERAL] = "char_literal",

    [TOKEN_PLUS] = "+",
    [TOKEN_MINUS] = "-",
    [TOKEN_INCREMENT] = "++",
    [TOKEN_DECREMENT] = "--",
    [TOKEN_MULTIPLY] = "*",
    [TOKEN_DIVIDE] = "/",
    [TOKEN_MODULO] = "%",
    [TOKEN_ASSIGN] = "=",
    [TOKEN_FAT_ARROW] = "=>",
    [TOKEN_EQUAL] = "==",
    [TOKEN_NOT_EQUAL] = "!=",
    [TOKEN_NOT] = "!",
    [TOKEN_LESS_THAN] = "<",
    [TOKEN_GREATER_THAN] = ">",
    [TOKEN_LESS_THAN_OR_EQUAL] = "<=",
    [TOKEN_GREATER_THAN_OR_EQUAL] = ">=",
    [TOKEN_AND] = "&&",
    [TOKEN_OR] = "||",
    [TOKEN_REFERENCE] = "&",
    [TOKEN_BITWISE_OR] = "|",
    [TOKEN_BITWISE_XOR] = "^",
    [TOKEN_BITWISE_NOT] = "~",

    [TOKEN_LPAREN] = "(",
    [TOKEN_RPAREN] = ")",
    [TOKEN_LBRACE] = "{",
    [TOKEN_RBRACE] = "}",
    [TOKEN_SEMICOLON] = ";",
    [TOKEN_COMMA] = ",",
    [TOKEN_COLON] = ":",
    [TOKEN_DOT] = ".",
    [TOKEN_ELLIPSIS] = "...",
    [TOKEN_LBRACKET] = "[",
    [TOKEN_RBRACKET] = "]",
    [TOKEN_QUESTION] = "?",
    [TOKEN_NULL] = "null",

    [TOKEN_EOF] = "EOF"
};

void free_token(Token *token) {
    if (token) {
        if (token->lexeme) {
            free(token->lexeme);
        }
        if (token->type == TOKEN_STRING && token->value.string_value) {
            free(token->value.string_value);
        }
        free(token);
    }
}
