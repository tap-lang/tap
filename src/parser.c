#include <ctype.h>
#include "parser.h"
#include "ast.h"
#include "lexer.h"

extern int debug;

// 辅助函数：打印token信息
void print_token(Token *token) {
    printf("Token: type=%s, lexeme='%s'\n", TokenNames[token->type], token->lexeme);
}

// 创建解析器
Parser *create_parser(Lexer *lexer) {
    Parser *parser = (Parser *)malloc(sizeof(Parser));
    if (!parser) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    parser->lexer = lexer;
    parser->current_token = get_next_token(lexer);
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
 
static int decimal_width(int value) {
    int width = 1;
    while (value >= 10) {
        value /= 10;
        width++;
    }
    return width;
}

// 打印代码当前行以及上下各context_lines行
void print_code_line(const char *source_code, int line, int context_lines) {
    if (!source_code || line < 1 || context_lines < 0) return;

    int start_line = line > context_lines ? line - context_lines : 1;
    int end_line = line + context_lines;
    int line_number_width = decimal_width(end_line);
    int current_line = 1;
    const char *line_start = source_code;

    while (*line_start != '\0' && current_line <= end_line) {
        const char *line_end = strchr(line_start, '\n');
        size_t line_length = line_end
            ? (size_t)(line_end - line_start)
            : strlen(line_start);

        if (current_line >= start_line && current_line <= end_line) {
            printf("%s %*d  %.*s\n",
                   current_line == line ? "->" : "  ",
                   line_number_width,
                   current_line,
                   (int)line_length,
                   line_start);
        }

        if (!line_end) break;
        line_start = line_end + 1;
        current_line++;
    }
}

// 解析错误处理
void parser_error(Parser *parser, const char *message) {
    fprintf(stderr, "Parse error: %s (file %s, line %d, column %d)\n", 
            message,
            parser->lexer->filename,
            parser->current_token->line, 
            parser->current_token->column
    );
    printf("Current token: %s\n", parser->current_token->lexeme);
    // 打印当前代码行的上下3行
    print_code_line(parser->lexer->source, parser->current_token->line, 3);
  
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
        snprintf(message, sizeof(message), "期望 标记 `%s`，但得到 `%s`", TokenNames[expected_type], TokenNames[parser->current_token->type]);
        parser_error(parser, message);
    }
}

// 前置声明
static ASTNode *parse_expression(Parser *parser); // 解析表达式（支持加法和减法）
static ASTNode *parse_term(Parser *parser); // 解析项（乘法和除法）
static ASTNode *parse_factor(Parser *parser); // 解析因子（基本表达式）
static ASTNode *parse_function_call(Parser *parser, char *function_name); // 解析函数调用
static ASTNode *parse_expression_statement(Parser *parser);
static ASTNode *parse_simple_statement(Parser *parser, int consume_semicolon);
static ASTNode *parse_if_statement(Parser *parser); // 解析条件语句
static ASTNode *parse_for_statement(Parser *parser);
static ASTNode *parse_block(Parser *parser); // 解析代码块（由花括号包围的语句序列）

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
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    strcat(expanded, ".");
    strcat(expanded, component);
    return expanded;
}

static ImportNode *parse_import(Parser *parser) {
    consume(parser, TOKEN_IMPORT);
    if (!is_module_component(parser->current_token)) {
        parser_error(parser, "期望模块名");
    }

    int line = parser->current_token->line;
    int column = parser->current_token->column;
    char *module_name = strdup(parser->current_token->lexeme);
    consume(parser, parser->current_token->type);

    while (parser->current_token->type == TOKEN_DOT) {
        consume(parser, TOKEN_DOT);
        if (!is_module_component(parser->current_token)) {
            free(module_name);
            parser_error(parser, "期望模块路径标识符");
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
            parser_error(parser, "期望模块别名");
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
        case TOKEN_ARRAY:
            // Array has no dedicated AST type yet; preserve the existing placeholder.
            type = LITERAL_STRING;
            break;
        default:
            parser_error(parser, "期望类型");
            return NULL;
    }

    consume(parser, token_type);
    return create_var_type(type);
}

// 解析代码块（由花括号包围的语句序列）
static ASTNode *parse_block(Parser *parser) {
    // 创建一个临时的函数节点来存储代码块中的语句
    FunctionNode *block = create_function("block");
    
    // 解析代码块中的语句
    while (parser->current_token->type != TOKEN_RBRACE && parser->current_token->type != TOKEN_EOF) {
        // 解析语句
        if (parser->current_token->type == TOKEN_PRINT) {
            if (debug) printf("解析打印语句 in {代码块} \n");
            // 解析打印语句
            consume(parser, TOKEN_PRINT);
            consume(parser, TOKEN_LPAREN);
            
            // 创建打印节点
            PrintNode *print_node = create_print();
            
            // 解析打印参数列表（支持多个表达式，逗号分隔）
            if (parser->current_token->type != TOKEN_RPAREN) {
                // 解析第一个参数
                ASTNode *expression = parse_expression(parser);
                add_print_argument(print_node, expression);
                
                // 解析更多参数
                while (parser->current_token->type == TOKEN_COMMA) {
                    consume(parser, TOKEN_COMMA);
                    expression = parse_expression(parser);
                    add_print_argument(print_node, expression);
                }
            }
            
            consume(parser, TOKEN_RPAREN);
            consume(parser, TOKEN_SEMICOLON);
            
            // 添加到代码块
            add_statement(block, (ASTNode *)print_node);
        } else if (parser->current_token->type == TOKEN_LET) {
            // 解析let语句
            consume(parser, TOKEN_LET);
            
            // 解析变量名
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                parser_error(parser, "期望变量名");
            }
            char *var_name = strdup(parser->current_token->lexeme);
            consume(parser, TOKEN_IDENTIFIER);
            
            // 解析可选的类型注解
            VarTypeNode *type = NULL;
            if (parser->current_token->type == TOKEN_COLON) {
                consume(parser, TOKEN_COLON);
                type = parse_type(parser);
            }
            
            // 解析等号
            consume(parser, TOKEN_ASSIGN);
            
            // 解析表达式作为变量的初始值
            ASTNode *expression = parse_expression(parser);
            
            consume(parser, TOKEN_SEMICOLON);
            
            // 创建变量声明节点并添加到代码块
            VarDeclNode *var_decl = create_var_decl(var_name, type, expression);
            free(var_name);
            add_statement(block, (ASTNode *)var_decl);
        } else if (parser->current_token->type == TOKEN_RETURN) {
            // 解析返回语句
            consume(parser, TOKEN_RETURN);
            
            // 解析返回表达式（支持整数、变量和表达式）
            ASTNode *expression = parse_expression(parser);
            
            consume(parser, TOKEN_SEMICOLON);
            
            // 创建返回节点并添加到代码块
            ReturnNode *return_node = create_return(expression);
            add_statement(block, (ASTNode *)return_node);
        } else if (parser->current_token->type == TOKEN_IF) {
            // 解析条件语句
            ASTNode *if_statement = parse_if_statement(parser);
            add_statement(block, if_statement);
        } else if (parser->current_token->type == TOKEN_FOR) {
            add_statement(block, parse_for_statement(parser));
        } else if (is_module_component(parser->current_token)) {
            ASTNode *statement = parse_expression_statement(parser);
            add_statement(block, statement);
        } else {
            parser_error(parser, "期望语句");
        }
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
        parser_error(parser, "期望 if 或 elseif 关键字");
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
        
        // 解析 else 代码块
        consume(parser, TOKEN_LBRACE);
        alternative = parse_block(parser);
        consume(parser, TOKEN_RBRACE);
    } else if (parser->current_token->type == TOKEN_ELSEIF) {
        // 递归解析下一个条件分支（elseif）
        alternative = parse_if_statement(parser);
    }
    
    // 创建条件语句节点
    IfStatementNode *if_node = create_if_statement(condition, consequence, alternative);
    return (ASTNode *)if_node;
}

// 解析函数定义
static FunctionNode *parse_function(Parser *parser) {
    
    if (debug) printf("  - 解析函数定义\n");

    // 解析 fn 关键字
    consume(parser, TOKEN_FN);
    
    // 解析函数名
    if (parser->current_token->type != TOKEN_IDENTIFIER) {
        parser_error(parser, "期望函数名");
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
    free(function_name);
    
    // 解析参数列表
    consume(parser, TOKEN_LPAREN);
    
    if(debug) printf("  - 解析参数列表\n");

    // 解析参数
    if (parser->current_token->type == TOKEN_IDENTIFIER) {
         if(debug) printf("  - 解析第一个参数\n");
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
            
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                parser_error(parser, "期望参数名");
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
        
        if(debug){
            // 打印当前token信息用于调试
            printf("After colon, ");
            print_token(parser->current_token);
        }
        
        function->return_type = parse_type(parser);
    }
    
    // 解析函数体
    consume(parser, TOKEN_LBRACE);
    
    if (debug) printf("  - 解析函数体\n");

    // 解析函数体中的语句
    while (parser->current_token->type != TOKEN_RBRACE && parser->current_token->type != TOKEN_EOF) {
        // 解析语句
        if (parser->current_token->type == TOKEN_PRINT) {
            
            if (debug) printf("  - 解析函数体中 print 语句\n");

            // 解析打印语句
            consume(parser, TOKEN_PRINT);
            consume(parser, TOKEN_LPAREN);
            
            // 创建打印节点
            PrintNode *print_node = create_print();
            
            // 解析打印参数列表（支持多个表达式，逗号分隔）
            if (parser->current_token->type != TOKEN_RPAREN) {
                // 解析第一个参数
                ASTNode *expression = parse_expression(parser);
                add_print_argument(print_node, expression);
                
                // 解析更多参数
                while (parser->current_token->type == TOKEN_COMMA) {
                    consume(parser, TOKEN_COMMA);
                    expression = parse_expression(parser);
                    add_print_argument(print_node, expression);
                }
            }
            
            consume(parser, TOKEN_RPAREN);
            consume(parser, TOKEN_SEMICOLON);
            
            // 添加到函数体
            add_statement(function, (ASTNode *)print_node);
        } else if (parser->current_token->type == TOKEN_LET) {
            // 解析let语句
            consume(parser, TOKEN_LET);
            
            // 解析变量名
            if (parser->current_token->type != TOKEN_IDENTIFIER) {
                parser_error(parser, "期望变量名");
            }
            char *var_name = strdup(parser->current_token->lexeme);
            consume(parser, TOKEN_IDENTIFIER);
            
            // 解析可选的类型注解
            VarTypeNode *type = NULL;
            if (parser->current_token->type == TOKEN_COLON) {
                consume(parser, TOKEN_COLON);
                type = parse_type(parser);
            }
            
            // 解析等号
            consume(parser, TOKEN_ASSIGN);
            
            // 解析表达式作为变量的初始值
            ASTNode *expression = parse_expression(parser);
            
            consume(parser, TOKEN_SEMICOLON);
            
            // 创建变量声明节点并添加到函数体
            VarDeclNode *var_decl = create_var_decl(var_name, type, expression);
            free(var_name);
            add_statement(function, (ASTNode *)var_decl);
        } else if (parser->current_token->type == TOKEN_RETURN) {
            // 解析返回语句
            consume(parser, TOKEN_RETURN);
            
            // 解析返回表达式（支持整数、变量和表达式）
            ASTNode *expression = parse_expression(parser);
            
            consume(parser, TOKEN_SEMICOLON);
            
            // 创建返回节点并添加到函数体
            ReturnNode *return_node = create_return(expression);
            add_statement(function, (ASTNode *)return_node);
        } else if (parser->current_token->type == TOKEN_IF) {
            // 解析条件语句
            ASTNode *if_statement = parse_if_statement(parser);
            add_statement(function, if_statement);
        } else if (parser->current_token->type == TOKEN_FOR) {
            add_statement(function, parse_for_statement(parser));
        } else if (is_module_component(parser->current_token)) {
            ASTNode *statement = parse_expression_statement(parser);
            add_statement(function, statement);
        } else {
            parser_error(parser, "期望语句");
        }
    }
    
    consume(parser, TOKEN_RBRACE);
    
    return function;
}

// 解析函数调用
static ASTNode *parse_function_call(Parser *parser, char *function_name) {
    // 创建函数调用节点
    FunctionCallNode *function_call = create_function_call(function_name);
    
    // 解析参数列表
    consume(parser, TOKEN_LPAREN);
    
    // 解析参数
    if (parser->current_token->type != TOKEN_RPAREN) {
        // 解析第一个参数
        ASTNode *arg_expression = parse_expression(parser);
        add_argument(function_call, arg_expression);
        
        // 解析更多参数
        while (parser->current_token->type == TOKEN_COMMA) {
            consume(parser, TOKEN_COMMA);
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
        parser_error(parser, "期望赋值、自增、自减或函数调用");
    }

    if (consume_semicolon) consume(parser, TOKEN_SEMICOLON);
    return statement;
}

static VarDeclNode *parse_for_initializer(Parser *parser) {
    consume(parser, TOKEN_LET);
    if (parser->current_token->type != TOKEN_IDENTIFIER) {
        parser_error(parser, "期望变量名");
    }

    char *name = strdup(parser->current_token->lexeme);
    consume(parser, TOKEN_IDENTIFIER);

    VarTypeNode *type = NULL;
    if (parser->current_token->type == TOKEN_COLON) {
        consume(parser, TOKEN_COLON);
        type = parse_type(parser);
    }

    consume(parser, TOKEN_ASSIGN);
    ASTNode *expression = parse_expression(parser);
    VarDeclNode *declaration = create_var_decl(name, type, expression);
    free(name);
    return declaration;
}

static ASTNode *parse_for_statement(Parser *parser) {
    consume(parser, TOKEN_FOR);
    consume(parser, TOKEN_LPAREN);

    ASTNode *initializer = NULL;
    if (parser->current_token->type == TOKEN_LET) {
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
    ASTNode *body = parse_block(parser);
    consume(parser, TOKEN_RBRACE);

    return (ASTNode *)create_for_statement(initializer, condition, update, body);
}

// 解析因子（标识符或整数）
static ASTNode *parse_factor(Parser *parser) {
    Token *token = parser->current_token;
    
    if (token->type == TOKEN_I32 && isdigit((unsigned char)token->lexeme[0])) {
        // 整数字面量
        LiteralNode *int_literal = create_int_literal_text(token->lexeme);
        consume(parser, TOKEN_I32);
        return (ASTNode *)int_literal;
    } else if (token->type == TOKEN_STRING && token->value.string_value) {
        // 字符串字面量
        LiteralNode *string_literal = create_string_literal(token->value.string_value);
        consume(parser, TOKEN_STRING);
        return (ASTNode *)string_literal;
    } else if (is_module_component(token)) {
        // 标识符、函数调用或名称空间限定函数调用
        int line = token->line;
        int column = token->column;
        enum TokenType token_type = token->type;
        char *name = strdup(token->lexeme);
        consume(parser, token_type);

        while (parser->current_token->type == TOKEN_DOT) {
            consume(parser, TOKEN_DOT);
            if (!is_module_component(parser->current_token)) {
                free(name);
                parser_error(parser, "期望名称空间成员");
            }
            name = append_name_component(name, parser->current_token->lexeme);
            consume(parser, parser->current_token->type);
        }
        
        // 检查是否是函数调用
        if (parser->current_token->type == TOKEN_LPAREN) {
            ASTNode *function_call = parse_function_call(parser, name);
            FunctionCallNode *call = (FunctionCallNode *)function_call;
            call->filename = strdup(parser->lexer->filename);
            call->line = line;
            call->column = column;
            free(name);
            return function_call;
        }

        if (strchr(name, '.')) {
            free(name);
            parser_error(parser, "名称空间成员必须作为函数调用使用");
        }
        
        // 否则是变量
        IdentifierNode *identifier = create_identifier(name);
        free(name);
        return (ASTNode *)identifier;
    } else if (token->type == TOKEN_LPAREN) {
        // 括号表达式
        consume(parser, TOKEN_LPAREN);
        ASTNode *expression = parse_expression(parser);
        consume(parser, TOKEN_RPAREN);
        return expression;
    } else if (token->type == TOKEN_MINUS) {
        // 负号表达式
        consume(parser, TOKEN_MINUS);
        ASTNode *factor = parse_factor(parser);
        // 创建一个表示 -factor 的表达式
        LiteralNode *zero = create_int_literal(0);
        BinaryOpNode *binary_op = create_binary_op(OP_SUBTRACT, (ASTNode *)zero, factor);
        return (ASTNode *)binary_op;
    }
    
    parser_error(parser, "期望因子（整数、字符串、标识符、括号表达式或负号表达式）");
    return NULL; // 不会执行到这里
}

// 解析项（乘除）
static ASTNode *parse_term(Parser *parser) {
    ASTNode *left = parse_factor(parser);
    
    while (parser->current_token->type == TOKEN_MULTIPLY || parser->current_token->type == TOKEN_DIVIDE) {
        Token *token = parser->current_token;
        if (token->type == TOKEN_MULTIPLY) {
            consume(parser, TOKEN_MULTIPLY);
            left = (ASTNode *)create_binary_op(OP_MULTIPLY, left, parse_factor(parser));
        } else if (token->type == TOKEN_DIVIDE) {
            consume(parser, TOKEN_DIVIDE);
            left = (ASTNode *)create_binary_op(OP_DIVIDE, left, parse_factor(parser));
        }
    }
    
    return left;
}

// 解析比较表达式（==, !=, <, >, <=, >=）
static ASTNode *parse_comparison(Parser *parser) {
    ASTNode *left = parse_term(parser);
    
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
                parser_error(parser, "期望比较操作符");
                return NULL;
        }
        
        consume(parser, token->type);
        left = (ASTNode *)create_binary_op(op_type, left, parse_term(parser));
    }
    
    return left;
}

// 解析表达式（加减）
static ASTNode *parse_expression(Parser *parser) {
    ASTNode *left = parse_comparison(parser);
    
    while (parser->current_token->type == TOKEN_PLUS || parser->current_token->type == TOKEN_MINUS) {
        Token *token = parser->current_token;
        if (token->type == TOKEN_PLUS) {
            consume(parser, TOKEN_PLUS);
            left = (ASTNode *)create_binary_op(OP_ADD, left, parse_comparison(parser));
        } else if (token->type == TOKEN_MINUS) {
            consume(parser, TOKEN_MINUS);
            left = (ASTNode *)create_binary_op(OP_SUBTRACT, left, parse_comparison(parser));
        }
    }
    
    return left;
}

// 解析程序
ProgramNode *parse_program(Parser *parser) {
    ProgramNode *program = create_program();
    
    // 解析所有模块导入和函数定义
    while (parser->current_token->type != TOKEN_EOF) {
        if (parser->current_token->type == TOKEN_IMPORT) {
            add_import(program, parse_import(parser));
        } else if (parser->current_token->type == TOKEN_FN) {
            FunctionNode *function = parse_function(parser);
            add_function(program, function);
        } else {
            parser_error(parser, "期望模块导入或函数定义");
        }
    }
    
    return program;
}
