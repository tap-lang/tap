#include <ctype.h>
#include "parser.h"
#include "ast.h"
#include "lexer.h"
#include "helpers.h"

// 辅助函数：打印token信息
void print_token(Token *token) {
    printf("Token: type=%s, lexeme='%s'\n", TokenNames[token->type], token->lexeme);
}

// 创建解析器
Parser *create_parser(Lexer *lexer) {
    Parser *parser = (Parser *)malloc(sizeof(Parser));
    if (!parser) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    parser->lexer = lexer;
    parser->current_token = get_next_token(lexer);
    parser->loop_depth = 0;
    parser->variadic_depth = 0;
    return parser;
}

// 释放解析器
void free_parser(Parser *parser) {
    if (parser) {
        if (parser->current_token) {
            free_token(parser->current_token);
        }
        free(parser);
    }
}
 
// 解析错误处理
void parser_error(Parser *parser, const char *message) {
    print_diagnostic(stderr, "error", parser->lexer->filename,
                     parser->current_token->line, parser->current_token->column,
                     "%s", message);
    //printf("Current token: %s\n", parser->current_token->lexeme);
    exit(1);
}


// 消费当前标记并获取下一个
static void consume(Parser *parser, enum TokenType expected_type) {
    if (parser->current_token->type == expected_type) {
        Token *old_token = parser->current_token;
        parser->current_token = get_next_token(parser->lexer);
        free_token(old_token);
    } else {
        char message[256];
        // snprintf(message, sizeof(message), "期望 %s 类型的标记，但得到 %s 类型", TokenNames[expected_type], TokenNames[parser->current_token->type]);
        snprintf(message, sizeof(message), "expected token `%s`, but got `%s`", TokenNames[expected_type], TokenNames[parser->current_token->type]); // 中文：期望指定标记，但得到当前标记
        parser_error(parser, message);
    }
}

// 前置声明
static ASTNode *parse_expression(Parser *parser); // 解析表达式（逻辑或、逻辑与、比较、加减、乘除取模）
static ASTNode *parse_logical_or(Parser *parser); // 解析逻辑或 ||
static ASTNode *parse_logical_and(Parser *parser); // 解析逻辑与 &&
static ASTNode *parse_bitwise_or(Parser *parser); // 解析按位或 |
static ASTNode *parse_bitwise_xor(Parser *parser); // 解析按位异或 ^
static ASTNode *parse_bitwise_and(Parser *parser); // 解析按位与 &
static ASTNode *parse_shift(Parser *parser); // 解析移位 << >>
static ASTNode *parse_addition(Parser *parser);
static ASTNode *parse_term(Parser *parser); // 解析项（乘法、除法和取模）
static ASTNode *parse_factor(Parser *parser); // 解析因子（一元运算和基本表达式）
static ASTNode *parse_function_call(Parser *parser, char *function_name); // 解析函数调用
static ASTNode *parse_expression_statement(Parser *parser);
static ASTNode *parse_simple_statement(Parser *parser, int consume_semicolon);
static ASTNode *parse_if_statement(Parser *parser); // 解析条件语句
static ASTNode *parse_for_statement(Parser *parser);
static ASTNode *parse_while_statement(Parser *parser);
static ASTNode *parse_loop_control_statement(Parser *parser);
static ASTNode *parse_struct_literal(Parser *parser, const char *struct_name);
static ASTNode *parse_block(Parser *parser); // 解析代码块（由花括号包围的语句序列）

// 预读下一个 token 的类型但不消费。词法层刻意不把 `<<` / `>>` 合并成双字符 token
//（合并会破坏泛型的 `<` / `>` 配对扫描），移位的识别只能靠这里做前瞻。
static enum TokenType peek_next_token_type(Parser *parser) {
    char *saved_current = parser->lexer->current;
    int saved_line = parser->lexer->line;
    int saved_column = parser->lexer->column;

    Token *token = get_next_token(parser->lexer);
    enum TokenType type = token ? token->type : TOKEN_EOF;
    if (token) free_token(token);

    parser->lexer->current = saved_current;
    parser->lexer->line = saved_line;
    parser->lexer->column = saved_column;
    return type;
}

// 前瞻跳过 `<...>`，判断紧随其后的 token 是不是 expected。
// 泛型实参和小于号在语法上有歧义，靠这个把几种用法区分开。
static int generic_arguments_followed_by(Parser *parser, enum TokenType expected) {
    if (parser->current_token->type != TOKEN_LESS_THAN) return 0;

    char *saved_current = parser->lexer->current;
    int saved_line = parser->lexer->line;
    int saved_column = parser->lexer->column;
    int depth = 1;
    int result = 0;

    while (depth > 0) {
        Token *token = get_next_token(parser->lexer);
        if (token->type == TOKEN_EOF) {
            free_token(token);
            break;
        }
        if (token->type == TOKEN_LESS_THAN) depth++;
        if (token->type == TOKEN_GREATER_THAN) depth--;
        free_token(token);
    }
    if (depth == 0) {
        Token *token = get_next_token(parser->lexer);
        result = token->type == expected;
        free_token(token);
    }

    parser->lexer->current = saved_current;
    parser->lexer->line = saved_line;
    parser->lexer->column = saved_column;
    return result;
}

// 泛型函数调用：`name<T>(...)`。
static int looks_like_generic_call(Parser *parser) {
    return generic_arguments_followed_by(parser, TOKEN_LPAREN);
}

// 泛型结构体字面量：`Name<T> { ... }`。
static int looks_like_generic_struct_literal(Parser *parser) {
    return generic_arguments_followed_by(parser, TOKEN_LBRACE);
}

// 泛型枚举的显式类型实参：`Enum<T>.Member(...)`。
static int looks_like_generic_variant(Parser *parser) {
    return generic_arguments_followed_by(parser, TOKEN_DOT);
}

static int is_module_component(const Token *token) {
    if ((token->type == TOKEN_STRING && token->value.string_value) ||
        !token->lexeme ||
        (!isalpha((unsigned char)token->lexeme[0]) && token->lexeme[0] != '_')) {
        return 0;
    }
    for (const char *character = token->lexeme + 1; *character; character++) {
        if (!isalnum((unsigned char)*character) && *character != '_') return 0;
    }
    return 1;
}

static char *append_name_component(char *name, const char *component) {
    size_t size = strlen(name) + strlen(component) + 2;
    char *expanded = realloc(name, size);
    if (!expanded) {
        free(name);
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    strcat(expanded, ".");
    strcat(expanded, component);
    return expanded;
}

static ImportNode *parse_import(Parser *parser) {
    consume(parser, TOKEN_IMPORT);
    if (!is_module_component(parser->current_token)) {
        parser_error(parser, "expected module name"); // 中文：期望模块名
    }

    int line = parser->current_token->line;
    int column = parser->current_token->column;
    char *module_name = strdup(parser->current_token->lexeme);
    consume(parser, parser->current_token->type);

    while (parser->current_token->type == TOKEN_DOT) {
        consume(parser, TOKEN_DOT);
        if (!is_module_component(parser->current_token)) {
            free(module_name);
            parser_error(parser, "expected module path identifier"); // 中文：期望模块路径标识符
        }

        module_name = append_name_component(module_name, parser->current_token->lexeme);
        consume(parser, parser->current_token->type);
    }

    const char *last_dot = strrchr(module_name, '.');
    char *alias = strdup(last_dot ? last_dot + 1 : module_name);
    if (parser->current_token->type == TOKEN_AS) {
        consume(parser, TOKEN_AS);
        if (!is_module_component(parser->current_token)) {
            free(alias);
            free(module_name);
            parser_error(parser, "expected module alias"); // 中文：期望模块别名
        }
        free(alias);
        alias = strdup(parser->current_token->lexeme);
        consume(parser, parser->current_token->type);
    }

    consume(parser, TOKEN_SEMICOLON);
    ImportNode *import_node = create_import(
        module_name, alias, parser->lexer->filename, line, column);
    free(alias);
    free(module_name);
    return import_node;
}

static VarTypeNode *parse_type(Parser *parser) {
    if (parser->current_token->type == TOKEN_MULTIPLY) {
        consume(parser, TOKEN_MULTIPLY);
        VarTypeNode *element_type = parse_type(parser);
        return create_pointer_type(element_type);
    }

    if (parser->current_token->type == TOKEN_LBRACKET) {
        consume(parser, TOKEN_LBRACKET);
        VarTypeNode *element_type = parse_type(parser);
        consume(parser, TOKEN_SEMICOLON);
        if (parser->current_token->type != TOKEN_I32 ||
            !isdigit((unsigned char)parser->current_token->lexeme[0])) {
            free_ast((ASTNode *)element_type);
            parser_error(parser, "array length must be a positive integer literal"); // 中文：数组长度必须是正整数字面量
        }
        uint64_t length = strtoull(parser->current_token->lexeme, NULL, 10);
        if (length == 0 || length > INT64_MAX) {
            free_ast((ASTNode *)element_type);
            parser_error(parser, "array length must be between 1 and INT64_MAX"); // 中文：数组长度必须在 1 到 INT64_MAX 之间
        }
        consume(parser, TOKEN_I32);
        consume(parser, TOKEN_RBRACKET);
        // 数组节点保留完整元素类型，从而支持多维数组。
        return create_array_type(element_type, length);
    }

    enum LiteralType type;
    enum TokenType token_type = parser->current_token->type;

    switch (token_type) {
        case TOKEN_INT: type = LITERAL_INT; break;
        case TOKEN_UINT: type = LITERAL_UINT; break;
        case TOKEN_I8: type = LITERAL_I8; break;
        case TOKEN_U8: type = LITERAL_U8; break;
        case TOKEN_I16: type = LITERAL_I16; break;
        case TOKEN_U16: type = LITERAL_U16; break;
        case TOKEN_I32: type = LITERAL_I32; break;
        case TOKEN_U32: type = LITERAL_U32; break;
        case TOKEN_I64: type = LITERAL_I64; break;
        case TOKEN_U64: type = LITERAL_U64; break;
        case TOKEN_I128: type = LITERAL_I128; break;
        case TOKEN_U128: type = LITERAL_U128; break;
        case TOKEN_FLOAT: type = LITERAL_FLOAT; break;
        case TOKEN_F32: type = LITERAL_F32; break;
        case TOKEN_F64: type = LITERAL_F64; break;
        case TOKEN_BOOL: type = LITERAL_BOOL; break;
        case TOKEN_STRING: type = LITERAL_STRING; break;
        case TOKEN_IDENTIFIER: {
            char *type_name = strdup(parser->current_token->lexeme);
            consume(parser, TOKEN_IDENTIFIER);
            VarTypeNode *named_type = create_enum_type(type_name);
            free(type_name);
            if (parser->current_token->type == TOKEN_LESS_THAN) {
                consume(parser, TOKEN_LESS_THAN);
                add_var_type_argument(named_type, parse_type(parser));
                while (parser->current_token->type == TOKEN_COMMA) {
                    consume(parser, TOKEN_COMMA);
                    add_var_type_argument(named_type, parse_type(parser));
                }
                consume(parser, TOKEN_GREATER_THAN);
            }
            return named_type;
        }
        default:
            parser_error(parser, "expected type"); // 中文：期望类型
            return NULL;
    }

    consume(parser, token_type);
    return create_var_type(type);
}

// 解析顶层枚举声明，枚举成员可用可选尾逗号结束。
static EnumNode *parse_enum(Parser *parser) {
    consume(parser, TOKEN_ENUM);
    if (parser->current_token->type != TOKEN_IDENTIFIER) {
        parser_error(parser, "expected enum name"); // 中文：期望枚举名称
    }

    char *enum_name = strdup(parser->current_token->lexeme);
    consume(parser, TOKEN_IDENTIFIER);
    EnumNode *enum_node = create_enum(enum_name);
    free(enum_name);

    // 可选的泛型类型参数，形如 enum Option<T>。
    if (parser->current_token->type == TOKEN_LESS_THAN) {
        consume(parser, TOKEN_LESS_THAN);
        for (;;) {
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                parser_error(parser, "expected generic type parameter"); // 中文：期望泛型类型参数
            }
            for (ASTNode *node = enum_node->type_params; node; node = node->next) {
                if (strcmp(((IdentifierNode *)node)->name,
                           parser->current_token->lexeme) == 0) {
                    parser_error(parser, "duplicate generic type parameter"); // 中文：重复的泛型类型参数
                }
            }
            add_enum_type_param(
                enum_node, create_identifier(parser->current_token->lexeme));
            consume(parser, TOKEN_IDENTIFIER);
            if (parser->current_token->type != TOKEN_COMMA) break;
            consume(parser, TOKEN_COMMA);
        }
        consume(parser, TOKEN_GREATER_THAN);
    }

    consume(parser, TOKEN_LBRACE);
    if (parser->current_token->type == TOKEN_RBRACE) {
        parser_error(parser, "enum must declare at least one variant"); // 中文：枚举必须至少声明一个成员
    }

    while (parser->current_token->type != TOKEN_RBRACE &&
           parser->current_token->type != TOKEN_EOF) {
        if (parser->current_token->type != TOKEN_IDENTIFIER) {
            parser_error(parser, "expected enum variant name"); // 中文：期望枚举成员名称
        }
        char *variant_name = strdup(parser->current_token->lexeme);
        consume(parser, TOKEN_IDENTIFIER);
        EnumVariantNode *variant = create_enum_variant(variant_name);
        free(variant_name);

        // 可选的载荷类型列表，形如 Variant(T1, T2)。
        if (parser->current_token->type == TOKEN_LPAREN) {
            consume(parser, TOKEN_LPAREN);
            if (parser->current_token->type == TOKEN_RPAREN) {
                parser_error(parser, "variant payload must declare at least one type"); // 中文：成员载荷至少要声明一个类型
            }
            for (;;) {
                add_enum_variant_payload(variant, parse_type(parser));
                if (parser->current_token->type != TOKEN_COMMA) break;
                consume(parser, TOKEN_COMMA);
            }
            consume(parser, TOKEN_RPAREN);
        }

        add_enum_variant(enum_node, variant);

        if (parser->current_token->type == TOKEN_COMMA) {
            consume(parser, TOKEN_COMMA);
            if (parser->current_token->type == TOKEN_RBRACE) break;
            continue;
        }
        break;
    }

    consume(parser, TOKEN_RBRACE);
    return enum_node;
}

static VarDeclNode *parse_var_decl(Parser *parser, int consume_semicolon) {
    enum TokenType declaration_type = parser->current_token->type;
    int is_const = declaration_type == TOKEN_CONST;
    if (declaration_type != TOKEN_LET && declaration_type != TOKEN_CONST) {
        parser_error(parser, "expected let or const declaration"); // 中文：期望 let 或 const 声明
    }
    consume(parser, declaration_type);

    if (parser->current_token->type != TOKEN_IDENTIFIER) {
        parser_error(parser, is_const ? "expected constant name" : "expected variable name"); // 中文：期望常量名或变量名
    }
    char *name = strdup(parser->current_token->lexeme);
    consume(parser, TOKEN_IDENTIFIER);

    VarTypeNode *type = NULL;
    if (parser->current_token->type == TOKEN_COLON) {
        consume(parser, TOKEN_COLON);
        type = parse_type(parser);
    } else if (is_const) {
        free(name);
        parser_error(parser, "constant declarations require an explicit type"); // 中文：常量声明必须显式指定类型
    }

    consume(parser, TOKEN_ASSIGN);
    ASTNode *expression = parse_expression(parser);
    if (consume_semicolon) consume(parser, TOKEN_SEMICOLON);

    VarDeclNode *declaration = create_var_decl(name, type, expression, is_const);
    free(name);
    return declaration;
}

// 解析顶层结构体声明，字段以逗号分隔并支持可选尾逗号。
static StructNode *parse_struct(Parser *parser) {
    consume(parser, TOKEN_STRUCT);
    if (parser->current_token->type != TOKEN_IDENTIFIER) {
        parser_error(parser, "expected struct name"); // 中文：期望结构体名称
    }

    char *struct_name = strdup(parser->current_token->lexeme);
    consume(parser, TOKEN_IDENTIFIER);
    StructNode *struct_node = create_struct(struct_name);
    free(struct_name);

    if (parser->current_token->type == TOKEN_LESS_THAN) {
        consume(parser, TOKEN_LESS_THAN);
        for (;;) {
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                parser_error(parser, "expected generic type parameter"); // 中文：期望泛型类型参数
            }
            for (ASTNode *node = struct_node->type_params; node; node = node->next) {
                if (strcmp(((IdentifierNode *)node)->name,
                           parser->current_token->lexeme) == 0) {
                    parser_error(parser, "duplicate generic type parameter"); // 中文：重复的泛型类型参数
                }
            }
            add_struct_type_param(
                struct_node, create_identifier(parser->current_token->lexeme));
            consume(parser, TOKEN_IDENTIFIER);
            if (parser->current_token->type != TOKEN_COMMA) break;
            consume(parser, TOKEN_COMMA);
        }
        consume(parser, TOKEN_GREATER_THAN);
    }

    consume(parser, TOKEN_LBRACE);
    if (parser->current_token->type == TOKEN_RBRACE) {
        parser_error(parser, "struct must declare at least one field"); // 中文：结构体必须至少声明一个字段
    }

    while (parser->current_token->type != TOKEN_RBRACE &&
           parser->current_token->type != TOKEN_EOF) {
        if (parser->current_token->type != TOKEN_IDENTIFIER) {
            parser_error(parser, "expected struct field name"); // 中文：期望结构体字段名
        }
        char *field_name = strdup(parser->current_token->lexeme);
        consume(parser, TOKEN_IDENTIFIER);
        consume(parser, TOKEN_COLON);
        VarTypeNode *field_type = parse_type(parser);
        StructFieldNode *field = create_struct_field(field_name, field_type);
        free(field_name);
        add_struct_field(struct_node, field);

        if (parser->current_token->type == TOKEN_COMMA) {
            consume(parser, TOKEN_COMMA);
            if (parser->current_token->type == TOKEN_RBRACE) break;
            continue;
        }
        break;
    }

    consume(parser, TOKEN_RBRACE);
    return struct_node;
}

// 解析代码块（由花括号包围的语句序列）
static ASTNode *parse_statement(Parser *parser);
static ASTNode *parse_match(Parser *parser, int as_value);

// 解析一条语句；不消费包围代码块的右大括号。
static ASTNode *parse_statement(Parser *parser) {
    if (parser->current_token->type == TOKEN_LET ||
        parser->current_token->type == TOKEN_CONST) {
        return (ASTNode *)parse_var_decl(parser, 1);
    }
    if (parser->current_token->type == TOKEN_RETURN) {
        // 解析返回语句
        consume(parser, TOKEN_RETURN);

        // 解析返回表达式（支持整数、变量和表达式）
        ASTNode *expression = parse_expression(parser);

        consume(parser, TOKEN_SEMICOLON);
        return (ASTNode *)create_return(expression);
    }
    if (parser->current_token->type == TOKEN_IF) {
        return parse_if_statement(parser);
    }
    if (parser->current_token->type == TOKEN_FOR) {
        return parse_for_statement(parser);
    }
    if (parser->current_token->type == TOKEN_WHILE) {
        return parse_while_statement(parser);
    }
    if (parser->current_token->type == TOKEN_BREAK ||
        parser->current_token->type == TOKEN_CONTINUE) {
        return parse_loop_control_statement(parser);
    }
    if (parser->current_token->type == TOKEN_MATCH) {
        return parse_match(parser, 0);
    }
    if (is_module_component(parser->current_token)) {
        return parse_expression_statement(parser);
    }

    parser_error(parser, "expected statement"); // 中文：期望语句
    return NULL;
}

static ASTNode *parse_block(Parser *parser) {
    // 创建一个临时的函数节点来存储代码块中的语句
    FunctionNode *block = create_function("block");

    // 解析代码块中的语句
    while (parser->current_token->type != TOKEN_RBRACE && parser->current_token->type != TOKEN_EOF) {
        add_statement(block, parse_statement(parser));
    }

    // 保存语句列表并释放临时函数节点
    ASTNode *statements = block->body;
    free(block->name);
    free(block);

    return statements;
}

// 解析条件语句（if、elseif、else）
static ASTNode *parse_if_statement(Parser *parser) {
    // 检查当前token是否是if或elseif
    if (parser->current_token->type == TOKEN_IF) {
        consume(parser, TOKEN_IF);
    } else if (parser->current_token->type == TOKEN_ELSEIF) {
        consume(parser, TOKEN_ELSEIF);
    } else {
        parser_error(parser, "expected if or elseif keyword"); // 中文：期望 if 或 elseif 关键字
        return NULL;
    }
    
    // 解析条件表达式
    consume(parser, TOKEN_LPAREN);
    ASTNode *condition = parse_expression(parser);
    consume(parser, TOKEN_RPAREN);
    
    // 解析条件为真时执行的代码块
    consume(parser, TOKEN_LBRACE);
    ASTNode *consequence = parse_block(parser);
    consume(parser, TOKEN_RBRACE);
    
    // 解析可选的 else 或 elseif 部分
    ASTNode *alternative = NULL;
    if (parser->current_token->type == TOKEN_ELSE) {
        consume(parser, TOKEN_ELSE);

        // `else if` 与 `elseif` 等价，都继续解析下一个条件分支。
        if (parser->current_token->type == TOKEN_IF) {
            alternative = parse_if_statement(parser);
        } else {
            // 解析 else 代码块
            consume(parser, TOKEN_LBRACE);
            alternative = parse_block(parser);
            consume(parser, TOKEN_RBRACE);
        }
    } else if (parser->current_token->type == TOKEN_ELSEIF) {
        // 递归解析下一个条件分支（elseif）
        alternative = parse_if_statement(parser);
    }
    
    // 创建条件语句节点
    IfStatementNode *if_node = create_if_statement(condition, consequence, alternative);
    return (ASTNode *)if_node;
}

// 解析 match 解构语句：
//   match (value) {
//       Enum.Variant(a, b) => { ... }
//       _ => { ... }
//   }
// 分支体既可以是代码块，也可以是单条语句。
static ASTNode *parse_match(Parser *parser, int as_value) {
    consume(parser, TOKEN_MATCH);
    consume(parser, TOKEN_LPAREN);
    ASTNode *expression = parse_expression(parser);
    consume(parser, TOKEN_RPAREN);

    MatchStatementNode *statement = create_match_statement(expression);

    consume(parser, TOKEN_LBRACE);
    if (parser->current_token->type == TOKEN_RBRACE) {
        parser_error(parser, "match must declare at least one arm"); // 中文：match 至少要有一个分支
    }

    while (parser->current_token->type != TOKEN_RBRACE &&
           parser->current_token->type != TOKEN_EOF) {
        int arm_line = parser->current_token->line;
        int arm_column = parser->current_token->column;
        MatchArmNode *arm = NULL;

        if (parser->current_token->type == TOKEN_IDENTIFIER &&
            strcmp(parser->current_token->lexeme, "_") == 0) {
            // 通配分支。
            consume(parser, TOKEN_IDENTIFIER);
            if (parser->current_token->type == TOKEN_LPAREN) {
                parser_error(parser, "wildcard arm cannot bind payload values"); // 中文：通配分支不能绑定载荷
            }
            arm = create_match_arm(NULL, NULL);
        } else {
            if (!is_module_component(parser->current_token)) {
                parser_error(parser, "expected match pattern"); // 中文：期望 match 模式
            }
            char *enum_name = strdup(parser->current_token->lexeme);
            consume(parser, parser->current_token->type);

            if (parser->current_token->type != TOKEN_DOT) {
                free(enum_name);
                parser_error(parser, "match pattern must be Enum.Variant or _"); // 中文：match 模式必须是 Enum.Variant 或 _
            }
            consume(parser, TOKEN_DOT);
            if (!is_module_component(parser->current_token)) {
                free(enum_name);
                parser_error(parser, "expected variant name"); // 中文：期望枚举成员名称
            }
            char *variant_name = strdup(parser->current_token->lexeme);
            consume(parser, parser->current_token->type);

            arm = create_match_arm(enum_name, variant_name);
            free(enum_name);
            free(variant_name);

            // 可选的载荷绑定列表。
            if (parser->current_token->type == TOKEN_LPAREN) {
                consume(parser, TOKEN_LPAREN);
                if (parser->current_token->type == TOKEN_RPAREN) {
                    parser_error(parser, "binding list must declare at least one name"); // 中文：绑定列表至少要有一个名字
                }
                for (;;) {
                    if (parser->current_token->type != TOKEN_IDENTIFIER) {
                        parser_error(parser, "expected binding name"); // 中文：期望绑定变量名
                    }
                    char *binding_name = strdup(parser->current_token->lexeme);
                    consume(parser, TOKEN_IDENTIFIER);
                    IdentifierNode *binding = create_identifier(binding_name);
                    free(binding_name);
                    add_match_binding(arm, binding);

                    if (parser->current_token->type != TOKEN_COMMA) break;
                    consume(parser, TOKEN_COMMA);
                }
                consume(parser, TOKEN_RPAREN);
            }
        }

        arm->filename = strdup(parser->lexer->filename);
        arm->line = arm_line;
        arm->column = arm_column;

        consume(parser, TOKEN_FAT_ARROW);

        if (as_value) {
            // 表达式形式：分支体就是这个分支的值。
            // 末尾可以写逗号或分号分隔，也可以什么都不写。
            arm->value = parse_expression(parser);
            if (parser->current_token->type == TOKEN_COMMA) {
                consume(parser, TOKEN_COMMA);
            } else if (parser->current_token->type == TOKEN_SEMICOLON) {
                consume(parser, TOKEN_SEMICOLON);
            }
        } else if (parser->current_token->type == TOKEN_LBRACE) {
            consume(parser, TOKEN_LBRACE);
            arm->body = parse_block(parser);
            consume(parser, TOKEN_RBRACE);
        } else {
            arm->body = parse_statement(parser);
        }

        add_match_arm(statement, arm);
    }

    consume(parser, TOKEN_RBRACE);
    return (ASTNode *)statement;
}

// 解析普通函数定义或无函数体的外部函数声明。
static FunctionNode *parse_function(Parser *parser, int is_extern) {
    // 解析 fn 关键字
    consume(parser, TOKEN_FN);
    
    // 解析函数名
    if (parser->current_token->type != TOKEN_IDENTIFIER) {
        parser_error(parser, "expected function name"); // 中文：期望函数名
    }
    char *function_name = strdup(parser->current_token->lexeme);
    int function_line = parser->current_token->line;
    int function_column = parser->current_token->column;
    consume(parser, TOKEN_IDENTIFIER);
    
    // 创建函数节点
    FunctionNode *function = create_function(function_name);
    function->filename = strdup(parser->lexer->filename);
    function->line = function_line;
    function->column = function_column;
    function->is_extern = is_extern;
    free(function_name);

    // 解析函数名后的泛型类型参数，例如 fn identity<T, U>(...)。
    if (parser->current_token->type == TOKEN_LESS_THAN) {
        consume(parser, TOKEN_LESS_THAN);
        for (;;) {
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                parser_error(parser, "expected generic type parameter"); // 中文：期望泛型类型参数
            }
            for (ASTNode *node = function->type_params; node; node = node->next) {
                if (strcmp(((IdentifierNode *)node)->name,
                           parser->current_token->lexeme) == 0) {
                    parser_error(parser, "duplicate generic type parameter"); // 中文：重复的泛型类型参数
                }
            }
            add_type_param(
                function, create_identifier(parser->current_token->lexeme));
            consume(parser, TOKEN_IDENTIFIER);
            if (parser->current_token->type != TOKEN_COMMA) break;
            consume(parser, TOKEN_COMMA);
        }
        consume(parser, TOKEN_GREATER_THAN);
    }
    
    // 解析参数列表
    consume(parser, TOKEN_LPAREN);

    // `...` 不能单独出现：C 的变参 ABI 要求省略号之前至少有一个具名参数，
    // 否则调用方无从推断变参起始位置。
    if (parser->current_token->type == TOKEN_ELLIPSIS) {
        parser_error(parser, "variadic marker `...` requires at least one named parameter"); // 中文：变参标记前至少要有一个具名参数
    }

    // 解析参数
    if (parser->current_token->type == TOKEN_IDENTIFIER) {
        // 解析第一个参数
        char *param_name = strdup(parser->current_token->lexeme);
        consume(parser, TOKEN_IDENTIFIER);
        IdentifierNode *param = create_identifier(param_name);
        free(param_name);
        add_param(function, param);

        // 解析参数类型；未标注时使用默认 i32，保持参数与类型链表对齐。
        VarTypeNode *param_type = create_var_type(LITERAL_I32);
        if (parser->current_token->type == TOKEN_COLON) {
            consume(parser, TOKEN_COLON);
            free(param_type);
            param_type = parse_type(parser);
        }
        add_param_type(function, param_type);
        
        // 解析更多参数
        while (parser->current_token->type == TOKEN_COMMA) {
            consume(parser, TOKEN_COMMA);

            // `...` 变参标记：必须位于参数列表末尾。extern 声明直接绑 C 的变参 ABI；
            // 普通函数的变参只能原样转发给另一个变参函数，函数体形态由 Codegen 校验。
            if (parser->current_token->type == TOKEN_ELLIPSIS) {
                function->is_variadic = 1;
                consume(parser, TOKEN_ELLIPSIS);
                if (parser->current_token->type != TOKEN_RPAREN) {
                    parser_error(parser, "variadic marker `...` must be the last parameter"); // 中文：变参标记必须是最后一个参数
                }
                break;
            }

            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                parser_error(parser, "expected parameter name"); // 中文：期望参数名
            }
            
            param_name = strdup(parser->current_token->lexeme);
            consume(parser, TOKEN_IDENTIFIER);
            param = create_identifier(param_name);
            free(param_name);
            add_param(function, param);
            
            // 解析参数类型
            param_type = create_var_type(LITERAL_I32);
            if (parser->current_token->type == TOKEN_COLON) {
                consume(parser, TOKEN_COLON);
                free(param_type);
                param_type = parse_type(parser);
            }
            add_param_type(function, param_type);
        }
    }
    
    consume(parser, TOKEN_RPAREN);
    
    // 解析函数返回类型（如果有）
    if (parser->current_token->type == TOKEN_COLON) {
        consume(parser, TOKEN_COLON);

        function->return_type = parse_type(parser);
    }

    // External functions end after their signature and are implemented by the C Runtime.
    if (is_extern) {
        consume(parser, TOKEN_SEMICOLON);
        return function;
    }
    
    // 解析函数体：语句分派统一交给 parse_statement，避免和 parse_block 重复。
    consume(parser, TOKEN_LBRACE);
    // 函数体内允许出现 `...` 转发，靠 variadic_depth 标记作用域。
    parser->variadic_depth += function->is_variadic;
    function->body = parse_block(parser);
    parser->variadic_depth -= function->is_variadic;
    consume(parser, TOKEN_RBRACE);

    return function;
}

// 解析函数调用
static ASTNode *parse_function_call(Parser *parser, char *function_name) {
    // 创建函数调用节点
    FunctionCallNode *function_call = create_function_call(function_name);

    // 解析显式泛型实参，例如 malloc<i32>(...)。
    if (parser->current_token->type == TOKEN_LESS_THAN) {
        consume(parser, TOKEN_LESS_THAN);
        add_type_argument(function_call, parse_type(parser));
        while (parser->current_token->type == TOKEN_COMMA) {
            consume(parser, TOKEN_COMMA);
            add_type_argument(function_call, parse_type(parser));
        }
        consume(parser, TOKEN_GREATER_THAN);
    }
    
    // 解析参数列表
    consume(parser, TOKEN_LPAREN);
    
    // 解析参数
    if (parser->current_token->type == TOKEN_ELLIPSIS) {
        parser_error(parser, "`...` must follow at least one named argument"); // 中文：`...` 前面至少要有一个具名实参
    }
    if (parser->current_token->type != TOKEN_RPAREN) {
        // 解析第一个参数
        ASTNode *arg_expression = parse_expression(parser);
        add_argument(function_call, arg_expression);
        
        // 解析更多参数
        while (parser->current_token->type == TOKEN_COMMA) {
            consume(parser, TOKEN_COMMA);

            // `...` 展开：把本函数的变参原样转发给被调用者。它只在变参函数体内
            // 有意义（否则无变参可转发），而且必须是实参列表的最后一项。
            if (parser->current_token->type == TOKEN_ELLIPSIS) {
                if (parser->variadic_depth == 0) {
                    parser_error(parser, "`...` can only forward the variadic arguments of the enclosing variadic function"); // 中文：`...` 只能转发所在变参函数的变参
                }
                consume(parser, TOKEN_ELLIPSIS);
                if (parser->current_token->type != TOKEN_RPAREN) {
                    parser_error(parser, "`...` must be the last argument"); // 中文：`...` 必须是最后一个实参
                }
                function_call->forwards_variadic = 1;
                break;
            }

            arg_expression = parse_expression(parser);
            add_argument(function_call, arg_expression);
        }
    }
    
    consume(parser, TOKEN_RPAREN);
    
    return (ASTNode *)function_call;
}

static ASTNode *parse_expression_statement(Parser *parser) {
    return parse_simple_statement(parser, 1);
}

static ASTNode *parse_simple_statement(Parser *parser, int consume_semicolon) {
    ASTNode *expression = parse_expression(parser);
    ASTNode *statement = expression;

    if (expression && expression->type == NODE_IDENTIFIER &&
        parser->current_token->type == TOKEN_ASSIGN) {
        const char *name = ((IdentifierNode *)expression)->name;
        consume(parser, TOKEN_ASSIGN);
        statement = (ASTNode *)create_assignment(name, parse_expression(parser));
        free_ast(expression);
    } else if (expression && expression->type == NODE_INDEX_EXPRESSION &&
               parser->current_token->type == TOKEN_ASSIGN) {
        consume(parser, TOKEN_ASSIGN);
        statement = (ASTNode *)create_index_assignment(
            (IndexExpressionNode *)expression, parse_expression(parser));
    } else if (expression && expression->type == NODE_IDENTIFIER &&
               (parser->current_token->type == TOKEN_INCREMENT ||
                parser->current_token->type == TOKEN_DECREMENT)) {
        const char *name = ((IdentifierNode *)expression)->name;
        enum BinaryOpType operation = parser->current_token->type == TOKEN_INCREMENT
            ? OP_ADD
            : OP_SUBTRACT;
        consume(parser, parser->current_token->type);
        ASTNode *one = (ASTNode *)create_int_literal(1);
        ASTNode *updated = (ASTNode *)create_binary_op(operation, expression, one);
        statement = (ASTNode *)create_assignment(name, updated);
    } else if (!expression || expression->type != NODE_FUNCTION_CALL) {
        parser_error(parser, "expected assignment, increment, decrement, or function call"); // 中文：期望赋值、自增、自减或函数调用
    }

    if (consume_semicolon) consume(parser, TOKEN_SEMICOLON);
    return statement;
}

static VarDeclNode *parse_for_initializer(Parser *parser) {
    return parse_var_decl(parser, 0);
}

static ASTNode *parse_for_statement(Parser *parser) {
    consume(parser, TOKEN_FOR);
    consume(parser, TOKEN_LPAREN);

    ASTNode *initializer = NULL;
    if (parser->current_token->type == TOKEN_LET ||
        parser->current_token->type == TOKEN_CONST) {
        initializer = (ASTNode *)parse_for_initializer(parser);
    } else if (parser->current_token->type != TOKEN_SEMICOLON) {
        initializer = parse_simple_statement(parser, 0);
    }
    consume(parser, TOKEN_SEMICOLON);

    ASTNode *condition = NULL;
    if (parser->current_token->type != TOKEN_SEMICOLON) {
        condition = parse_expression(parser);
    }
    consume(parser, TOKEN_SEMICOLON);

    ASTNode *update = NULL;
    if (parser->current_token->type != TOKEN_RPAREN) {
        update = parse_simple_statement(parser, 0);
    }
    consume(parser, TOKEN_RPAREN);

    consume(parser, TOKEN_LBRACE);
    parser->loop_depth++;
    ASTNode *body = parse_block(parser);
    parser->loop_depth--;
    consume(parser, TOKEN_RBRACE);

    return (ASTNode *)create_for_statement(initializer, condition, update, body);
}

static ASTNode *parse_while_statement(Parser *parser) {
    consume(parser, TOKEN_WHILE);
    consume(parser, TOKEN_LPAREN);
    ASTNode *condition = parse_expression(parser);
    consume(parser, TOKEN_RPAREN);

    consume(parser, TOKEN_LBRACE);
    parser->loop_depth++;
    ASTNode *body = parse_block(parser);
    parser->loop_depth--;
    consume(parser, TOKEN_RBRACE);

    return (ASTNode *)create_for_statement(NULL, condition, NULL, body);
}

static ASTNode *parse_loop_control_statement(Parser *parser) {
    enum TokenType type = parser->current_token->type;
    if (parser->loop_depth == 0) {
        parser_error(parser, type == TOKEN_BREAK
            ? "break can only be used inside a loop" // 中文：break 只能在循环中使用
            : "continue can only be used inside a loop"); // 中文：continue 只能在循环中使用
    }

    consume(parser, type);
    consume(parser, TOKEN_SEMICOLON);
    return type == TOKEN_BREAK
        ? create_break_statement()
        : create_continue_statement();
}

// 解析因子（标识符或整数）
static ASTNode *parse_array_literal(Parser *parser) {
    consume(parser, TOKEN_LBRACKET);
    ArrayLiteralNode *array = create_array_literal();
    if (parser->current_token->type != TOKEN_RBRACKET) {
        add_array_element(array, parse_expression(parser));
        if (parser->current_token->type == TOKEN_SEMICOLON) {
            consume(parser, TOKEN_SEMICOLON);
            if (parser->current_token->type != TOKEN_I32 ||
                !isdigit((unsigned char)parser->current_token->lexeme[0])) {
                free_ast((ASTNode *)array);
                parser_error(parser, "array repeat initializer length must be a positive integer literal"); // 中文：数组重复初始化长度必须是正整数数字面量
            }
            uint64_t repeat_count = strtoull(parser->current_token->lexeme, NULL, 10);
            if (repeat_count == 0 || repeat_count > INT64_MAX) {
                free_ast((ASTNode *)array);
                parser_error(parser, "array repeat initializer length must be between 1 and INT64_MAX"); // 中文：数组重复初始化长度必须在 1 到 INT64_MAX 之间
            }
            consume(parser, TOKEN_I32);
            set_array_repeat(array, repeat_count);
        } else {
            while (parser->current_token->type == TOKEN_COMMA) {
                consume(parser, TOKEN_COMMA);
                add_array_element(array, parse_expression(parser));
            }
        }
    }
    consume(parser, TOKEN_RBRACKET);
    return (ASTNode *)array;
}

// 解析结构体字面量：Type { field: value, ... }。
static ASTNode *parse_struct_literal(Parser *parser, const char *struct_name) {
    StructLiteralNode *literal = create_struct_literal((char *)struct_name);
    if (parser->current_token->type == TOKEN_LESS_THAN) {
        consume(parser, TOKEN_LESS_THAN);
        add_struct_literal_type_argument(literal, parse_type(parser));
        while (parser->current_token->type == TOKEN_COMMA) {
            consume(parser, TOKEN_COMMA);
            add_struct_literal_type_argument(literal, parse_type(parser));
        }
        consume(parser, TOKEN_GREATER_THAN);
    }
    consume(parser, TOKEN_LBRACE);

    if (parser->current_token->type != TOKEN_RBRACE) {
        for (;;) {
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                free_ast((ASTNode *)literal);
                parser_error(parser, "expected struct initializer field name"); // 中文：期望结构体初始化字段名
            }
            char *field_name = strdup(parser->current_token->lexeme);
            consume(parser, TOKEN_IDENTIFIER);
            consume(parser, TOKEN_COLON);
            ASTNode *expression = parse_expression(parser);
            StructInitFieldNode *field =
                create_struct_init_field(field_name, expression);
            free(field_name);
            add_struct_init_field(literal, field);

            if (parser->current_token->type != TOKEN_COMMA) break;
            consume(parser, TOKEN_COMMA);
            if (parser->current_token->type == TOKEN_RBRACE) break;
        }
    }

    consume(parser, TOKEN_RBRACE);
    return (ASTNode *)literal;
}

// 解析 sizeof(T)；T 使用与变量声明相同的完整类型语法。
static ASTNode *parse_sizeof_expression(Parser *parser) {
    consume(parser, TOKEN_SIZEOF);
    consume(parser, TOKEN_LPAREN);
    VarTypeNode *operand_type = parse_type(parser);
    consume(parser, TOKEN_RPAREN);
    return (ASTNode *)create_sizeof(operand_type);
}

static ASTNode *parse_primary(Parser *parser) {
    Token *token = parser->current_token;

    if (token->type == TOKEN_SIZEOF) {
        return parse_sizeof_expression(parser);
    }
    if (token->type == TOKEN_I32 && isdigit((unsigned char)token->lexeme[0])) {
        LiteralNode *int_literal = create_int_literal_text(token->lexeme);
        consume(parser, TOKEN_I32);
        return (ASTNode *)int_literal;
    }
    if (token->type == TOKEN_F32 && isdigit((unsigned char)token->lexeme[0])) {
        LiteralNode *float_literal = create_float_literal(token->value.float_value);
        float_literal->literal_type = LITERAL_F32;
        consume(parser, TOKEN_F32);
        return (ASTNode *)float_literal;
    }
    if (token->type == TOKEN_BOOL &&
        (strcmp(token->lexeme, "true") == 0 ||
         strcmp(token->lexeme, "false") == 0)) {
        LiteralNode *bool_literal = create_bool_literal(token->value.bool_value);
        consume(parser, TOKEN_BOOL);
        return (ASTNode *)bool_literal;
    }
    if (token->type == TOKEN_STRING && token->value.string_value) {
        LiteralNode *string_literal = create_string_literal(token->value.string_value);
        consume(parser, TOKEN_STRING);
        return (ASTNode *)string_literal;
    }
    if (token->type == TOKEN_LBRACKET) {
        return parse_array_literal(parser);
    }
    if (is_module_component(token)) {
        int line = token->line;
        int column = token->column;
        enum TokenType token_type = token->type;
        char *name = strdup(token->lexeme);
        consume(parser, token_type);

        if (parser->current_token->type == TOKEN_LBRACE ||
            looks_like_generic_struct_literal(parser)) {
            ASTNode *literal = parse_struct_literal(parser, name);
            free(name);
            return literal;
        }

        // 泛型枚举的显式类型实参：`Enum<T>.Member(...)` 或 `Enum<T>.Member`。
        // 实参先收集成链表，等构造调用或标识符建好后再挂上去。
        ASTNode *explicit_arguments = NULL;
        if (looks_like_generic_variant(parser)) {
            consume(parser, TOKEN_LESS_THAN);
            for (;;) {
                append_type_argument(&explicit_arguments, parse_type(parser));
                if (parser->current_token->type != TOKEN_COMMA) break;
                consume(parser, TOKEN_COMMA);
            }
            consume(parser, TOKEN_GREATER_THAN);

            consume(parser, TOKEN_DOT);
            if (!is_module_component(parser->current_token)) {
                free(name);
                parser_error(parser, "expected variant name after type arguments"); // 中文：期望类型实参之后的成员名
            }
            name = append_name_component(name, parser->current_token->lexeme);
            consume(parser, parser->current_token->type);
        }

        while (parser->current_token->type == TOKEN_DOT) {
            consume(parser, TOKEN_DOT);
            if (!is_module_component(parser->current_token)) {
                free(name);
                parser_error(parser, "expected namespace member"); // 中文：期望名称空间成员
            }
            name = append_name_component(name, parser->current_token->lexeme);
            consume(parser, parser->current_token->type);
        }

        if (parser->current_token->type == TOKEN_LPAREN ||
            looks_like_generic_call(parser)) {
            ASTNode *function_call = parse_function_call(parser, name);
            FunctionCallNode *call = (FunctionCallNode *)function_call;
            call->filename = strdup(parser->lexer->filename);
            call->line = line;
            call->column = column;
            call->enum_type_arguments = explicit_arguments;
            free(name);
            return function_call;
        }

        IdentifierNode *identifier = create_identifier(name);
        identifier->enum_type_arguments = explicit_arguments;
        free(name);
        return (ASTNode *)identifier;
    }
    if (token->type == TOKEN_LPAREN) {
        consume(parser, TOKEN_LPAREN);
        ASTNode *expression = parse_expression(parser);
        consume(parser, TOKEN_RPAREN);
        return expression;
    }

    parser_error(parser, "expected factor (integer, string, array, identifier, or parenthesized expression)"); // 中文：期望因子（整数、字符串、数组、标识符或括号表达式）
    return NULL;
}

static ASTNode *parse_factor(Parser *parser) {
    // match 出现在表达式位置时按「表达式形式」解析：每个分支是一个值。
    if (parser->current_token->type == TOKEN_MATCH) {
        return parse_match(parser, 1);
    }

    if (parser->current_token->type == TOKEN_REFERENCE) {
        consume(parser, TOKEN_REFERENCE);
        return (ASTNode *)create_reference(parse_factor(parser));
    }

    if (parser->current_token->type == TOKEN_MINUS) {
        consume(parser, TOKEN_MINUS);
        ASTNode *factor = parse_factor(parser);
        return (ASTNode *)create_binary_op(
            OP_SUBTRACT, (ASTNode *)create_int_literal(0), factor);
    }

    // 逻辑非直接降级为与 false 比较，复用已有的比较运算语义。
    if (parser->current_token->type == TOKEN_NOT) {
        consume(parser, TOKEN_NOT);
        ASTNode *factor = parse_factor(parser);
        return (ASTNode *)create_binary_op(
            OP_EQUAL, factor, (ASTNode *)create_bool_literal(0));
    }

    // 按位取反降级为 `x ^ -1`：全 1 与 x 异或就是逐位取反。
    // LLVMConstInt 对全 1 值只取低位，所以各宽度下都能得到正确的 -1。
    if (parser->current_token->type == TOKEN_BITWISE_NOT) {
        consume(parser, TOKEN_BITWISE_NOT);
        ASTNode *factor = parse_factor(parser);
        return (ASTNode *)create_binary_op(
            OP_BITWISE_XOR, factor, (ASTNode *)create_int_literal((uint64_t)-1));
    }

    ASTNode *expression = parse_primary(parser);
    for (;;) {
        // 后缀 `?`：Result 传播，失败时从当前函数提前返回 Err。
        // 绑定最紧，`a?[0]` 读作 `(a?)[0]`。
        if (parser->current_token->type == TOKEN_QUESTION) {
            int line = parser->current_token->line;
            int column = parser->current_token->column;
            consume(parser, TOKEN_QUESTION);
            TryNode *try_node = create_try(expression);
            try_node->filename = strdup(parser->lexer->filename);
            try_node->line = line;
            try_node->column = column;
            expression = (ASTNode *)try_node;
            continue;
        }

        if (parser->current_token->type == TOKEN_LBRACKET) {
            consume(parser, TOKEN_LBRACKET);
            ASTNode *index = parse_expression(parser);
            consume(parser, TOKEN_RBRACKET);
            expression = (ASTNode *)create_index_expression(expression, index);
            continue;
        }

        // 标识符接收者会在 parse_primary 中解析为限定调用（例如 s.len()）。
        // 此处处理字符串字面量、数组元素和函数返回值的内建方法。
        if (parser->current_token->type == TOKEN_DOT) {
            int line = parser->current_token->line;
            int column = parser->current_token->column;
            consume(parser, TOKEN_DOT);
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                free_ast(expression);
                parser_error(parser, "expected string method name");
            }
            const char *method_name = parser->current_token->lexeme;
            const char *internal_name = NULL;
            if (strcmp(method_name, "len") == 0) {
                internal_name = "__tap_builtin_string_len";
            } else if (strcmp(method_name, "byte_at") == 0) {
                internal_name = "__tap_builtin_string_byte_at";
            } else if (strcmp(method_name, "slice") == 0) {
                internal_name = "__tap_builtin_string_slice";
            } else {
                free_ast(expression);
                parser_error(parser,
                    "only string methods 'len', 'byte_at', and 'slice' are supported");
            }
            consume(parser, TOKEN_IDENTIFIER);
            consume(parser, TOKEN_LPAREN);

            // 将接收者作为第一个内部参数保存。
            FunctionCallNode *call = create_function_call(internal_name);
            call->filename = strdup(parser->lexer->filename);
            call->line = line;
            call->column = column;
            add_argument(call, expression);
            if (parser->current_token->type != TOKEN_RPAREN) {
                add_argument(call, parse_expression(parser));
            }
            while (parser->current_token->type == TOKEN_COMMA) {
                consume(parser, TOKEN_COMMA);
                add_argument(call, parse_expression(parser));
            }
            consume(parser, TOKEN_RPAREN);
            expression = (ASTNode *)call;
            continue;
        }

        break;
    }
    return expression;
}

// 解析项（乘除取模）
static ASTNode *parse_term(Parser *parser) {
    ASTNode *left = parse_factor(parser);
    
    while (parser->current_token->type == TOKEN_MULTIPLY ||
           parser->current_token->type == TOKEN_DIVIDE ||
           parser->current_token->type == TOKEN_MODULO) {
        Token *token = parser->current_token;
        if (token->type == TOKEN_MULTIPLY) {
            consume(parser, TOKEN_MULTIPLY);
            left = (ASTNode *)create_binary_op(OP_MULTIPLY, left, parse_factor(parser));
        } else if (token->type == TOKEN_DIVIDE) {
            consume(parser, TOKEN_DIVIDE);
            left = (ASTNode *)create_binary_op(OP_DIVIDE, left, parse_factor(parser));
        } else {
            consume(parser, TOKEN_MODULO);
            left = (ASTNode *)create_binary_op(OP_MODULO, left, parse_factor(parser));
        }
    }
    
    return left;
}

// 解析比较表达式（==, !=, <, >, <=, >=）
static ASTNode *parse_comparison(Parser *parser) {
    ASTNode *left = parse_shift(parser);
    
    while (parser->current_token->type == TOKEN_EQUAL || 
           parser->current_token->type == TOKEN_NOT_EQUAL || 
           parser->current_token->type == TOKEN_LESS_THAN || 
           parser->current_token->type == TOKEN_GREATER_THAN || 
           parser->current_token->type == TOKEN_LESS_THAN_OR_EQUAL || 
           parser->current_token->type == TOKEN_GREATER_THAN_OR_EQUAL) {
        Token *token = parser->current_token;
        enum BinaryOpType op_type;
        
        switch (token->type) {
            case TOKEN_EQUAL:
                op_type = OP_EQUAL;
                break;
            case TOKEN_NOT_EQUAL:
                op_type = OP_NOT_EQUAL;
                break;
            case TOKEN_LESS_THAN:
                op_type = OP_LESS_THAN;
                break;
            case TOKEN_GREATER_THAN:
                op_type = OP_GREATER_THAN;
                break;
            case TOKEN_LESS_THAN_OR_EQUAL:
                op_type = OP_LESS_THAN_OR_EQUAL;
                break;
            case TOKEN_GREATER_THAN_OR_EQUAL:
                op_type = OP_GREATER_THAN_OR_EQUAL;
                break;
            default:
                parser_error(parser, "expected comparison operator"); // 中文：期望比较操作符
                return NULL;
        }
        
        consume(parser, token->type);
        left = (ASTNode *)create_binary_op(op_type, left, parse_shift(parser));
    }
    
    return left;
}

// 解析表达式（加减）
static ASTNode *parse_addition(Parser *parser) {
    ASTNode *left = parse_term(parser);
    
    while (parser->current_token->type == TOKEN_PLUS || parser->current_token->type == TOKEN_MINUS) {
        Token *token = parser->current_token;
        if (token->type == TOKEN_PLUS) {
            consume(parser, TOKEN_PLUS);
            left = (ASTNode *)create_binary_op(OP_ADD, left, parse_term(parser));
        } else if (token->type == TOKEN_MINUS) {
            consume(parser, TOKEN_MINUS);
            left = (ASTNode *)create_binary_op(OP_SUBTRACT, left, parse_term(parser));
        }
    }
    
    return left;
}

// 解析移位（<< >>）。词法层不合并双字符，靠前瞻判断是不是成对的 `<` / `>`；
// 单个 `<` / `>` 属于比较运算，留给 parse_comparison 处理。
static ASTNode *parse_shift(Parser *parser) {
    ASTNode *left = parse_addition(parser);

    while (parser->current_token->type == TOKEN_LESS_THAN ||
           parser->current_token->type == TOKEN_GREATER_THAN) {
        enum TokenType operator_type = parser->current_token->type;
        if (peek_next_token_type(parser) != operator_type) break;

        consume(parser, operator_type);
        consume(parser, operator_type);
        left = (ASTNode *)create_binary_op(
            operator_type == TOKEN_LESS_THAN ? OP_SHIFT_LEFT : OP_SHIFT_RIGHT,
            left, parse_addition(parser));
    }

    return left;
}

// 解析按位与（&）。一元位置的 `&` 是取地址，在 parse_factor 里就被消耗了；
// 能走到这里的 `&` 一定是二元运算符。
static ASTNode *parse_bitwise_and(Parser *parser) {
    ASTNode *left = parse_comparison(parser);

    while (parser->current_token->type == TOKEN_REFERENCE) {
        consume(parser, TOKEN_REFERENCE);
        left = (ASTNode *)create_binary_op(
            OP_BITWISE_AND, left, parse_comparison(parser));
    }

    return left;
}

// 解析按位异或（^）。优先级低于按位与，高于按位或。
static ASTNode *parse_bitwise_xor(Parser *parser) {
    ASTNode *left = parse_bitwise_and(parser);

    while (parser->current_token->type == TOKEN_BITWISE_XOR) {
        consume(parser, TOKEN_BITWISE_XOR);
        left = (ASTNode *)create_binary_op(
            OP_BITWISE_XOR, left, parse_bitwise_and(parser));
    }

    return left;
}

// 解析按位或（|）。优先级低于按位异或，高于逻辑与。
static ASTNode *parse_bitwise_or(Parser *parser) {
    ASTNode *left = parse_bitwise_xor(parser);

    while (parser->current_token->type == TOKEN_BITWISE_OR) {
        consume(parser, TOKEN_BITWISE_OR);
        left = (ASTNode *)create_binary_op(
            OP_BITWISE_OR, left, parse_bitwise_xor(parser));
    }

    return left;
}

// 解析逻辑与（&&）。优先级低于比较，高于逻辑或。
static ASTNode *parse_logical_and(Parser *parser) {
    ASTNode *left = parse_bitwise_or(parser);

    while (parser->current_token->type == TOKEN_AND) {
        consume(parser, TOKEN_AND);
        left = (ASTNode *)create_binary_op(OP_AND, left, parse_bitwise_or(parser));
    }

    return left;
}

// 解析逻辑或（||）。优先级低于逻辑与。
static ASTNode *parse_logical_or(Parser *parser) {
    ASTNode *left = parse_logical_and(parser);

    while (parser->current_token->type == TOKEN_OR) {
        consume(parser, TOKEN_OR);
        left = (ASTNode *)create_binary_op(OP_OR, left, parse_logical_and(parser));
    }

    return left;
}

static ASTNode *parse_expression(Parser *parser) {
    return parse_logical_or(parser);
}

// 解析程序
ProgramNode *parse_program(Parser *parser) {
    ProgramNode *program = create_program();
    
    // 解析所有模块导入、顶层常量和函数定义
    while (parser->current_token->type != TOKEN_EOF) {
        if (parser->current_token->type == TOKEN_IMPORT) {
            add_import(program, parse_import(parser));
        } else if (parser->current_token->type == TOKEN_ENUM) {
            add_enum(program, parse_enum(parser));
        } else if (parser->current_token->type == TOKEN_STRUCT) {
            add_struct(program, parse_struct(parser));
        } else if (parser->current_token->type == TOKEN_CONST) {
            add_constant(program, parse_var_decl(parser, 1));
        } else if (parser->current_token->type == TOKEN_EXTERN) {
            // Only functions are supported by the first external ABI version.
            consume(parser, TOKEN_EXTERN);
            if (parser->current_token->type != TOKEN_FN) {
                parser_error(parser, "extern must be followed by a function declaration"); // 中文：extern 后必须是函数声明
            }
            add_function(program, parse_function(parser, 1));
        } else if (parser->current_token->type == TOKEN_FN) {
            FunctionNode *function = parse_function(parser, 0);
            add_function(program, function);
        } else {
            parser_error(parser, "expected module import, enum declaration, struct declaration, constant declaration, or function definition"); // 中文：期望模块导入、枚举声明、结构体声明、常量声明或函数定义
        }
    }
    
    return program;
}
