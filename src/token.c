#include "token.h"

const char *TokenNames[] = {
    "fn", "return", "print", "let", "if", "for", "break", "continue",
    "else", "elseif", "import", "as",
    "identifier", "int", "uint", "i8", "u8", "i16", "u16", "i32", "u32",
    "i64", "u64", "i128", "u128", "float", "f32", "f64",
    "bool", "string", "array",
    "+", "-", "++", "--", "*", "/", "=", "==", "!=", "<", ">", "<=", ">=", "&&", "||",
    "(", ")", "{", "}", ";", ",", ":", ".", "[", "]",
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
