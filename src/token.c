#include "token.h"

const char *TokenNames[] = {
    "pub", "fn", "extern", "return", "let", "const", "sizeof", "enum", "struct", "if", "for", "while", "break", "continue",
    "else", "elseif", "import", "as", "match",
    "identifier", "int", "uint", "i8", "u8", "i16", "u16", "i32", "u32",
    "i64", "u64", "i128", "u128", "float", "f32", "f64",
    "bool", "string",
    "+", "-", "++", "--", "*", "/", "%", "=", "=>", "==", "!=", "!", "<", ">", "<=", ">=", "&&", "||", "&", "|", "^", "~",
    "(", ")", "{", "}", ";", ",", ":", ".", "...", "[", "]", "?",
    "EOF"
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
