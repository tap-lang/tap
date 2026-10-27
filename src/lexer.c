#include "lexer.h"
#include "helpers.h"

// 创建词法分析器（读入 filename 指向的源文件）
Lexer *create_lexer(const char *filename) {
    char *source = read_source_file(filename);
    if (!source) {
        return NULL;
    }

    Lexer *lexer = (Lexer *)malloc(sizeof(Lexer));
    if (!lexer) {
        free(source);
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    lexer->filename = filename;
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
    return lexer;
}

// 释放词法分析器
void free_lexer(Lexer *lexer) {
    if (lexer) {
        free(lexer->source);
        free(lexer);
    }
}

void print_lexer(Lexer *lexer) {
    for (;;) {
        Token *token = get_next_token(lexer);
        const char *lexeme = token->lexeme ? token->lexeme : "";
        printf("%s:%d:%d\t%s\t'%s'\n", lexer->filename, token->line,
               token->column, TokenNames[token->type], lexeme);
        enum TokenType type = token->type;
        free_token(token);
        if (type == TOKEN_EOF) break;
    }
}

// 检查是否到达文件末尾
static int is_at_end(Lexer *lexer) {
    return *lexer->current == '\0';
}

// 获取当前字符并前进
static char advance(Lexer *lexer) {
    lexer->current++;
    lexer->column++;
    return lexer->current[-1];
}

// 获取当前字符但不前进
static char peek(Lexer *lexer) {
    return *lexer->current;
}

// 获取下一个字符但不前进
static char peek_next(Lexer *lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static void skip_block_comment(Lexer *lexer) {
    int start_line = lexer->line;
    int start_column = lexer->column;
    advance(lexer); // /
    advance(lexer); // *

    while (!is_at_end(lexer)) {
        if (peek(lexer) == '*' && peek_next(lexer) == '/') {
            advance(lexer);
            advance(lexer);
            return;
        }
        if (peek(lexer) == '\n') {
            advance(lexer);
            lexer->line++;
            lexer->column = 1;
        } else {
            advance(lexer);
        }
    }

    print_diagnostic(stderr, "error", lexer->filename, start_line, start_column,
                     "unterminated block comment"); // 中文：未闭合的多行注释
    exit(1);
}

// 跳过空白字符
static void skip_whitespace(Lexer *lexer) {
    while (1) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance(lexer);
                break;
            case '\n':
                advance(lexer);
                lexer->line++;
                lexer->column = 1;
                break;
            case '/':
                if (peek_next(lexer) == '/') {
                    // 单行注释
                    while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                        advance(lexer);
                    }
                } else if (peek_next(lexer) == '*') {
                    skip_block_comment(lexer);
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

// 创建新标记
static Token *create_token(Lexer *lexer, enum TokenType type, const char *start, const char *end) {
    Token *token = (Token *)malloc(sizeof(Token));
    if (!token) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    
    int length = end - start;
    token->lexeme = (char *)malloc(length + 1);
    if (!token->lexeme) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    
    strncpy(token->lexeme, start, length);
    token->lexeme[length] = '\0';
    token->type = type;
    token->line = lexer->line;
    token->column = lexer->column - length;
    memset(&token->value, 0, sizeof(token->value));
    
    return token;
}

// 检查是否是关键字（先按长度分支，memcmp 定长比较）
static enum TokenType check_keyword(const char *text, int length) {
    switch (length) {
    case 2:
        if (memcmp(text, "as", 2) == 0) return TOKEN_AS;
        if (memcmp(text, "fn", 2) == 0) return TOKEN_FN;
        if (memcmp(text, "if", 2) == 0) return TOKEN_IF;
        if (memcmp(text, "i8", 2) == 0) return TOKEN_I8;
        if (memcmp(text, "u8", 2) == 0) return TOKEN_U8;
        break;
    case 3:
        if (memcmp(text, "pub", 3) == 0) return TOKEN_PUB;
        if (memcmp(text, "for", 3) == 0) return TOKEN_FOR;
        if (memcmp(text, "let", 3) == 0) return TOKEN_LET;
        if (memcmp(text, "int", 3) == 0) return TOKEN_INT;
        if (memcmp(text, "i16", 3) == 0) return TOKEN_I16;
        if (memcmp(text, "u16", 3) == 0) return TOKEN_U16;
        if (memcmp(text, "i32", 3) == 0) return TOKEN_I32;
        if (memcmp(text, "u32", 3) == 0) return TOKEN_U32;
        if (memcmp(text, "i64", 3) == 0) return TOKEN_I64;
        if (memcmp(text, "u64", 3) == 0) return TOKEN_U64;
        if (memcmp(text, "f32", 3) == 0) return TOKEN_F32;
        if (memcmp(text, "f64", 3) == 0) return TOKEN_F64;
        break;
    case 4:
        if (memcmp(text, "else", 4) == 0) return TOKEN_ELSE;
        if (memcmp(text, "enum", 4) == 0) return TOKEN_ENUM;
        if (memcmp(text, "bool", 4) == 0) return TOKEN_BOOL;
        if (memcmp(text, "char", 4) == 0) return TOKEN_CHAR;
        if (memcmp(text, "null", 4) == 0) return TOKEN_NULL;
        if (memcmp(text, "true", 4) == 0) return TOKEN_BOOL;
        if (memcmp(text, "uint", 4) == 0) return TOKEN_UINT;
        if (memcmp(text, "i128", 4) == 0) return TOKEN_I128;
        if (memcmp(text, "u128", 4) == 0) return TOKEN_U128;
        if (memcmp(text, "impl", 4) == 0) return TOKEN_IMPL;
        break;
    case 5:
        if (memcmp(text, "break", 5) == 0) return TOKEN_BREAK;
        if (memcmp(text, "const", 5) == 0) return TOKEN_CONST;
        if (memcmp(text, "false", 5) == 0) return TOKEN_BOOL;
        if (memcmp(text, "match", 5) == 0) return TOKEN_MATCH;
        if (memcmp(text, "trait", 5) == 0) return TOKEN_TRAIT;
        if (memcmp(text, "while", 5) == 0) return TOKEN_WHILE;
        break;
    case 6:
        if (memcmp(text, "elseif", 6) == 0) return TOKEN_ELSEIF;
        if (memcmp(text, "extern", 6) == 0) return TOKEN_EXTERN;
        if (memcmp(text, "import", 6) == 0) return TOKEN_IMPORT;
        if (memcmp(text, "return", 6) == 0) return TOKEN_RETURN;
        if (memcmp(text, "sizeof", 6) == 0) return TOKEN_SIZEOF;
        if (memcmp(text, "struct", 6) == 0) return TOKEN_STRUCT;
        // String 是携带长度的 Tap 文本类型；小写 string 不再作为关键字保留。
        if (memcmp(text, "String", 6) == 0) return TOKEN_STRING;
        break;
    case 8:
        if (memcmp(text, "continue", 8) == 0) return TOKEN_CONTINUE;
        // c_string 单独作为关键字，表示 C ABI 的零结尾字符指针。
        if (memcmp(text, "c_string", 8) == 0) return TOKEN_C_STRING;
        break;
    default:
        break;
    }
    return TOKEN_IDENTIFIER;
}

// 从 UTF-8 字节序列解码一个 Unicode 码点；返回占用的字节数，非法时返回 0。
static size_t decode_utf8_char(const unsigned char *text, size_t length,
                               uint32_t *value) {
    if (!length) return 0;
    unsigned char first = text[0];
    size_t width = 1;
    uint32_t codepoint = first;
    if (first <= 0x7F) {
        width = 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
        width = 2;
        if (length < width || text[1] < 0x80 || text[1] > 0xBF) return 0;
        codepoint = ((uint32_t)(first & 0x1F) << 6) | (text[1] & 0x3F);
    } else if (first >= 0xE0 && first <= 0xEF) {
        width = 3;
        if (length < width || text[1] < 0x80 || text[1] > 0xBF ||
            text[2] < 0x80 || text[2] > 0xBF) return 0;
        if ((first == 0xE0 && text[1] < 0xA0) ||
            (first == 0xED && text[1] > 0x9F)) return 0;
        codepoint = ((uint32_t)(first & 0x0F) << 12) |
                    ((uint32_t)(text[1] & 0x3F) << 6) | (text[2] & 0x3F);
    } else if (first >= 0xF0 && first <= 0xF4) {
        width = 4;
        if (length < width || text[1] < 0x80 || text[1] > 0xBF ||
            text[2] < 0x80 || text[2] > 0xBF ||
            text[3] < 0x80 || text[3] > 0xBF) return 0;
        if ((first == 0xF0 && text[1] < 0x90) ||
            (first == 0xF4 && text[1] > 0x8F)) return 0;
        codepoint = ((uint32_t)(first & 0x07) << 18) |
                    ((uint32_t)(text[1] & 0x3F) << 12) |
                    ((uint32_t)(text[2] & 0x3F) << 6) | (text[3] & 0x3F);
    } else {
        return 0;
    }
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return 0;
    }
    if (value) *value = codepoint;
    return width;
}

// 解析单引号 Unicode 字符字面量，例如 'a'、'中'、'😀' 和 '\\n'。
static Token *character(Lexer *lexer) {
    const char *start = lexer->current;
    advance(lexer); // 跳过开头的单引号
    if (is_at_end(lexer) || peek(lexer) == '\n' || peek(lexer) == '\r') {
        print_diagnostic(stderr, "error", lexer->filename, lexer->line, lexer->column,
                         "unterminated character literal");
        exit(1);
    }

    uint32_t value = 0;
    if (peek(lexer) == '\\') {
        advance(lexer);
        if (is_at_end(lexer)) {
            print_diagnostic(stderr, "error", lexer->filename, lexer->line, lexer->column,
                             "unterminated character literal");
            exit(1);
        }
        switch (advance(lexer)) {
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case '0': value = 0; break;
            case '\\': value = '\\'; break;
            case '\'': value = '\''; break;
            case '"': value = '"'; break;
            default:
                print_diagnostic(stderr, "error", lexer->filename,
                                 lexer->line, lexer->column - 1,
                                 "unknown character escape");
                exit(1);
        }
    } else {
        size_t width = decode_utf8_char(
            (const unsigned char *)lexer->current,
            strlen(lexer->current), &value);
        if (!width) {
            print_diagnostic(stderr, "error", lexer->filename, lexer->line, lexer->column,
                             "invalid UTF-8 character literal");
            exit(1);
        }
        while (width-- > 0) advance(lexer);
    }

    if (peek(lexer) != '\'') {
        print_diagnostic(stderr, "error", lexer->filename, lexer->line, lexer->column,
                         "character literal must contain exactly one character");
        exit(1);
    }
    advance(lexer);
    Token *token = create_token(lexer, TOKEN_CHAR_LITERAL, start, lexer->current);
    token->value.char_value = value;
    return token;
}

// 解析标识符或关键字
static Token *identifier(Lexer *lexer) {
    const char *start = lexer->current;
    while (isalpha(peek(lexer)) || isdigit(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }
    
    Token *token = create_token(lexer, TOKEN_IDENTIFIER, start, lexer->current);

    // 检查是否是关键字
    enum TokenType keyword_type = check_keyword(token->lexeme, strlen(token->lexeme));
    if (keyword_type != TOKEN_IDENTIFIER) {
        token->type = keyword_type;
        
        // 为布尔值设置值
        if (keyword_type == TOKEN_BOOL) {
            token->value.bool_value = (strcmp(token->lexeme, "true") == 0);
        }
    }
    
    return token;
}

// 解析数字
static Token *number(Lexer *lexer) {
    const char *start = lexer->current;
    int has_dot = 0;
    int has_exponent = 0;
    
    while (isdigit(peek(lexer))) {
        advance(lexer);
    }
    
    // 检查是否是浮点数
    if (peek(lexer) == '.' && isdigit(peek_next(lexer))) {
        has_dot = 1;
        advance(lexer);
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
    }
    
    // 指数记法：e/E 后必须是可选符号加至少一位数字，否则回退，把 e 留给标识符。
    if (peek(lexer) == 'e' || peek(lexer) == 'E') {
        char *mark = lexer->current;
        int mark_column = lexer->column;
        advance(lexer);
        if (peek(lexer) == '+' || peek(lexer) == '-') {
            advance(lexer);
        }
        if (isdigit(peek(lexer))) {
            has_exponent = 1;
            while (isdigit(peek(lexer))) {
                advance(lexer);
            }
        } else {
            lexer->current = mark;
            lexer->column = mark_column;
        }
    }
    
    Token *token;
    if (has_dot || has_exponent) {
        token = create_token(lexer, TOKEN_F32, start, lexer->current);
        token->value.float_value = atof(token->lexeme);
    } else {
        token = create_token(lexer, TOKEN_I32, start, lexer->current);
        token->value.int_value = strtoull(token->lexeme, NULL, 10);
    }
    
    return token;
}

// 解析字符串
static Token *string(Lexer *lexer) {
    advance(lexer); // 跳过开头的引号
    const char *start = lexer->current;
    
    // 计算字符串长度，处理转义字符
    int length = 0;
    char *buffer = NULL;
    int buffer_size = 0;
    
    while (peek(lexer) != '"' && !is_at_end(lexer)) {
        if (peek(lexer) == '\\') {
            // 处理转义字符
            advance(lexer); // 跳过反斜杠
            
            if (buffer == NULL) {
                // 首次分配缓冲区，大小设为估计值
                buffer_size = 128;
                buffer = (char *)malloc(buffer_size);
                if (!buffer) {
                    fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                    exit(1);
                }
                // 复制之前的字符，注意这里的偏移量是lexer->current - start - 1
                // 因为我们已经advance了一步（跳过了反斜杠）
                strncpy(buffer, start, lexer->current - start - 1);
                length = lexer->current - start - 1;
            }
            
            // 确保缓冲区足够大
            if (length + 1 >= buffer_size) {
                buffer_size *= 2;
                buffer = (char *)realloc(buffer, buffer_size);
                if (!buffer) {
                    fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                    exit(1);
                }
            }
            
            // 处理常见的转义字符
            switch (peek(lexer)) {
                case 'n':
                    buffer[length++] = '\n';
                    break;
                case 't':
                    buffer[length++] = '\t';
                    break;
                case '\\':
                    buffer[length++] = '\\';
                    break;
                case '"':
                    buffer[length++] = '"';
                    break;
                default:
                    // 对于不支持的转义序列，直接添加字符
                    buffer[length++] = peek(lexer);
                    break;
            }
            advance(lexer);
        } else {
            if (peek(lexer) == '\n') {
                advance(lexer);
                lexer->line++;
                lexer->column = 1;
            } else {
                advance(lexer);
            }
        }
    }
    free(buffer);
    
    if (is_at_end(lexer)) {
        print_diagnostic(stderr, "error", lexer->filename, lexer->line, lexer->column,
                         "unterminated string"); // 中文：未闭合的字符串
        exit(1);
    }
    
    advance(lexer); // 跳过结尾的引号
    
    // 创建token时，确保lexeme包含完整的字符串内容
    Token *token = create_token(lexer, TOKEN_STRING, start, lexer->current - 1);
    
    // 确保value.string_value包含完整的字符串，包括第一个字符
    token->value.string_value = strdup(token->lexeme);
    
    // 如果有转义字符，我们需要重新处理
    if (strchr(token->lexeme, '\\') != NULL) {
        // 释放之前分配的空间
        free(token->value.string_value);
        
        // 重新处理转义字符
        buffer_size = 128;
        buffer = (char *)malloc(buffer_size);
        if (!buffer) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        
        length = 0;
        const char *str = token->lexeme;
        while (*str) {
            if (length + 1 >= buffer_size) {
                buffer_size *= 2;
                char *expanded = (char *)realloc(buffer, buffer_size);
                if (!expanded) {
                    free(buffer);
                    fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                    exit(1);
                }
                buffer = expanded;
            }
            if (*str == '\\' && *(str + 1)) {
                str++;
                switch (*str) {
                    case 'n':
                        buffer[length++] = '\n';
                        break;
                    case 't':
                        buffer[length++] = '\t';
                        break;
                    case '\\':
                        buffer[length++] = '\\';
                        break;
                    case '"':
                        buffer[length++] = '"';
                        break;
                    default:
                        buffer[length++] = *str;
                        break;
                }
            } else {
                buffer[length++] = *str;
            }
            str++;
        }
        buffer[length] = '\0';
        token->value.string_value = buffer;
    }
    
    return token;
}

// 获取下一个标记
Token *get_next_token(Lexer *lexer) {
    skip_whitespace(lexer);
    
    if (is_at_end(lexer)) {
        return create_token(lexer, TOKEN_EOF, lexer->current, lexer->current);
    }
    
    char c = advance(lexer);
    
    // 处理字母和下划线（标识符或关键字）
    if (isalpha(c) || c == '_') {
        lexer->current--;
        lexer->column--;
        return identifier(lexer);
    }
    
    // 处理数字
    if (isdigit(c)) {
        lexer->current--;
        lexer->column--;
        return number(lexer);
    }
    
    // 处理字符串
    if (c == '"') {
        lexer->current--;
        lexer->column--;
        return string(lexer);
    }

    // 处理单引号 Unicode 字符字面量
    if (c == '\'') {
        lexer->current--;
        lexer->column--;
        return character(lexer);
    }
    
    // 处理运算符和分隔符
    switch (c) {
        case '(':
            return create_token(lexer, TOKEN_LPAREN, lexer->current - 1, lexer->current);
        case ')':
            return create_token(lexer, TOKEN_RPAREN, lexer->current - 1, lexer->current);
        case '{':
            return create_token(lexer, TOKEN_LBRACE, lexer->current - 1, lexer->current);
        case '}':
            return create_token(lexer, TOKEN_RBRACE, lexer->current - 1, lexer->current);
        case '[':
            return create_token(lexer, TOKEN_LBRACKET, lexer->current - 1, lexer->current);
        case ']':
            return create_token(lexer, TOKEN_RBRACKET, lexer->current - 1, lexer->current);
        case ';':
            return create_token(lexer, TOKEN_SEMICOLON, lexer->current - 1, lexer->current);
        case ',':
            return create_token(lexer, TOKEN_COMMA, lexer->current - 1, lexer->current);
        case '+':
            if (peek(lexer) == '+') {
                advance(lexer);
                return create_token(lexer, TOKEN_INCREMENT, lexer->current - 2, lexer->current);
            }
            return create_token(lexer, TOKEN_PLUS, lexer->current - 1, lexer->current);
        case '-':
            if (peek(lexer) == '-') {
                advance(lexer);
                return create_token(lexer, TOKEN_DECREMENT, lexer->current - 2, lexer->current);
            }
            return create_token(lexer, TOKEN_MINUS, lexer->current - 1, lexer->current);
        case '*':
            return create_token(lexer, TOKEN_MULTIPLY, lexer->current - 1, lexer->current);
        case '/':
            return create_token(lexer, TOKEN_DIVIDE, lexer->current - 1, lexer->current);
        case '%':
            return create_token(lexer, TOKEN_MODULO, lexer->current - 1, lexer->current);
        case '=':
            if (peek(lexer) == '>') {
                advance(lexer);
                return create_token(lexer, TOKEN_FAT_ARROW, lexer->current - 2, lexer->current);
            } else if (peek(lexer) == '=') {
                advance(lexer);
                return create_token(lexer, TOKEN_EQUAL, lexer->current - 2, lexer->current);
            } else {
                return create_token(lexer, TOKEN_ASSIGN, lexer->current - 1, lexer->current);
            }
        case '!':
            if (peek(lexer) == '=') {
                advance(lexer);
                return create_token(lexer, TOKEN_NOT_EQUAL, lexer->current - 2, lexer->current);
            }
            return create_token(lexer, TOKEN_NOT, lexer->current - 1, lexer->current);
        // 注意：`<` 和 `>` 一律按单字符产出，**不合并**成 `<<` / `>>`。
        // 泛型实参的配对扫描（Parser 的 generic_arguments_followed_by）靠逐个数字符
        // `<` / `>` 来配平嵌套，合并成双字符 token 会让 `Vec<Vec<i32>>` 直接解析失败。
        // 移位的识别交给 Parser 做前瞻。
        case '<':
            if (peek(lexer) == '=') {
                advance(lexer);
                return create_token(lexer, TOKEN_LESS_THAN_OR_EQUAL, lexer->current - 2, lexer->current);
            } else {
                return create_token(lexer, TOKEN_LESS_THAN, lexer->current - 1, lexer->current);
            }
        case '>':
            if (peek(lexer) == '=') {
                advance(lexer);
                return create_token(lexer, TOKEN_GREATER_THAN_OR_EQUAL, lexer->current - 2, lexer->current);
            } else {
                return create_token(lexer, TOKEN_GREATER_THAN, lexer->current - 1, lexer->current);
            }
        case '&':
            if (peek(lexer) == '&') {
                advance(lexer);
                return create_token(lexer, TOKEN_AND, lexer->current - 2, lexer->current);
            }
            return create_token(lexer, TOKEN_REFERENCE, lexer->current - 1, lexer->current);
        case '|':
            if (peek(lexer) == '|') {
                advance(lexer);
                return create_token(lexer, TOKEN_OR, lexer->current - 2, lexer->current);
            }
            return create_token(lexer, TOKEN_BITWISE_OR, lexer->current - 1, lexer->current);
        case '^':
            return create_token(lexer, TOKEN_BITWISE_XOR, lexer->current - 1, lexer->current);
        case '~':
            return create_token(lexer, TOKEN_BITWISE_NOT, lexer->current - 1, lexer->current);
        case ':':
            return create_token(lexer, TOKEN_COLON, lexer->current - 1, lexer->current);
        case '.':
            // `...` 变参声明。注意 `..`（两个点）本身在 tap 里没有含义，
            // 所以只有连读三个点才产出 TOKEN_ELLIPSIS，否则退化成 TOKEN_DOT
            // （成员访问 `socket.fd` 和浮点字面量 `1.5` 都走单点路径）。
            if (peek(lexer) == '.' && peek_next(lexer) == '.') {
                advance(lexer);
                advance(lexer);
                return create_token(lexer, TOKEN_ELLIPSIS, lexer->current - 3, lexer->current);
            }
            return create_token(lexer, TOKEN_DOT, lexer->current - 1, lexer->current);
        case '?':
            return create_token(lexer, TOKEN_QUESTION, lexer->current - 1, lexer->current);
    }
    
    // 未识别的字符
    print_diagnostic(stderr, "error", lexer->filename, lexer->line, lexer->column - 1,
                     "unrecognized character '%c'", c); // 中文：未识别的字符
    exit(1);
    return NULL; // 不会执行到这里
}
