#include "token.h"

const char *TokenNames[] = {
    "fn", "return", "print", "let", "if", "else", "elseif",
    "identifier", "int", "i32", "i64", "float", "f32", "f64",
    "bool", "string", "array",
    "+", "-", "*", "/", "=", "==", "!=", "<", ">", "<=", ">=", "&&", "||",
    "(", ")", "{", "}", ";", ",", ":", "[", "]",
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
