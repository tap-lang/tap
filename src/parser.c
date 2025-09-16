#include "parser.h"

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

// 解析错误处理
void parser_error(Parser *parser, const char *message) {
    fprintf(stderr, "解析错误 (行 %d, 列 %d): %s\n", 
            parser->current_token->line, 
            parser->current_token->column, 
            message);
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
        snprintf(message, sizeof(message), "期望 %d 类型的标记，但得到 %d 类型", expected_type, parser->current_token->type);
        parser_error(parser, message);
    }
}

// 解析函数定义
static FunctionNode *parse_function(Parser *parser) {
    // 解析 fn 关键字
    consume(parser, TOKEN_FN);
    
    // 解析函数名
    if (parser->current_token->type != TOKEN_IDENTIFIER) {
        parser_error(parser, "期望函数名");
    }
    char *function_name = strdup(parser->current_token->lexeme);
    consume(parser, TOKEN_IDENTIFIER);
    
    // 创建函数节点
    FunctionNode *function = create_function(function_name);
    free(function_name);
    
    // 解析参数列表
    consume(parser, TOKEN_LPAREN);
    
    // 暂时不支持参数
    
    consume(parser, TOKEN_RPAREN);
    
    // 解析函数体
    consume(parser, TOKEN_LBRACE);
    
    // 解析函数体中的语句
    while (parser->current_token->type != TOKEN_RBRACE && parser->current_token->type != TOKEN_EOF) {
        // 解析语句
        if (parser->current_token->type == TOKEN_PRINT) {
            // 解析打印语句
            consume(parser, TOKEN_PRINT);
            consume(parser, TOKEN_LPAREN);
            
            // 解析打印参数（暂时只支持字符串字面量）
            if (parser->current_token->type != TOKEN_STRING) {
                parser_error(parser, "期望字符串字面量作为print函数参数");
            }
            LiteralNode *string_literal = create_string_literal(parser->current_token->value.string_value);
            consume(parser, TOKEN_STRING);
            
            consume(parser, TOKEN_RPAREN);
            consume(parser, TOKEN_SEMICOLON);
            
            // 创建打印节点并添加到函数体
            PrintNode *print_node = create_print((ASTNode *)string_literal);
            add_statement(function, (ASTNode *)print_node);
        } else if (parser->current_token->type == TOKEN_RETURN) {
            // 解析返回语句
            consume(parser, TOKEN_RETURN);
            
            // 解析返回表达式（暂时只支持整数）
            if (parser->current_token->type != TOKEN_INTEGER) {
                parser_error(parser, "期望整数字面量作为return语句的值");
            }
            LiteralNode *int_literal = create_int_literal(parser->current_token->value.int_value);
            consume(parser, TOKEN_INTEGER);
            
            consume(parser, TOKEN_SEMICOLON);
            
            // 创建返回节点并添加到函数体
            ReturnNode *return_node = create_return((ASTNode *)int_literal);
            add_statement(function, (ASTNode *)return_node);
        } else {
            parser_error(parser, "期望语句");
        }
    }
    
    consume(parser, TOKEN_RBRACE);
    
    return function;
}

// 解析程序
ProgramNode *parse_program(Parser *parser) {
    ProgramNode *program = create_program();
    
    // 解析所有函数定义
    while (parser->current_token->type != TOKEN_EOF) {
        if (parser->current_token->type == TOKEN_FN) {
            FunctionNode *function = parse_function(parser);
            add_function(program, function);
        } else {
            parser_error(parser, "期望函数定义");
        }
    }
    
    return program;
}