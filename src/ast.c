#include "ast.h"

extern int debug;

// 创建程序节点
ProgramNode *create_program() {
    if (debug) printf("- 创建程序节点\n");
    ProgramNode *program = (ProgramNode *)malloc(sizeof(ProgramNode));
    if (!program) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    program->base.type = NODE_PROGRAM;
    program->base.next = NULL;
    program->functions = NULL;
    return program;
}

// 创建函数节点
FunctionNode *create_function(char *name) {
    if (debug) printf("  - 创建函数节点: %s\n", name);
    FunctionNode *function = (FunctionNode *)malloc(sizeof(FunctionNode));
    if (!function) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    function->base.type = NODE_FUNCTION;
    function->base.next = NULL;
    function->name = strdup(name);
    function->params = NULL;
    function->body = NULL;
    return function;
}

// 创建标识符节点
IdentifierNode *create_identifier(char *name) {
    IdentifierNode *identifier = (IdentifierNode *)malloc(sizeof(IdentifierNode));
    if (!identifier) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    identifier->base.type = NODE_IDENTIFIER;
    identifier->base.next = NULL;
    identifier->name = strdup(name);
    return identifier;
}

// 创建整数字面量节点
LiteralNode *create_int_literal(int value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_INT;
    literal->value.int_value = value;
    return literal;
}

// 创建字符串字面量节点
LiteralNode *create_string_literal(char *value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_STRING;
    literal->value.string_value = strdup(value);
    return literal;
}

// 创建浮点数字面量节点
LiteralNode *create_float_literal(double value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_FLOAT;
    literal->value.float_value = value;
    return literal;
}

// 创建布尔字面量节点
LiteralNode *create_bool_literal(int value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_BOOL;
    literal->value.bool_value = value;
    return literal;
}

// 创建返回语句节点
ReturnNode *create_return(ASTNode *expression) {
    ReturnNode *return_node = (ReturnNode *)malloc(sizeof(ReturnNode));
    if (!return_node) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    return_node->base.type = NODE_RETURN;
    return_node->base.next = NULL;
    return_node->expression = expression;
    return return_node;
}

// 创建打印语句节点
PrintNode *create_print() {
    PrintNode *print_node = (PrintNode *)malloc(sizeof(PrintNode));
    if (!print_node) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    print_node->base.type = NODE_PRINT;
    print_node->base.next = NULL;
    print_node->arguments = NULL;
    return print_node;
}

// 添加打印参数
void add_print_argument(PrintNode *print_node, ASTNode *argument) {
    if (!argument) return;
    
    // 将参数添加到参数列表的末尾
    if (!print_node->arguments) {
        print_node->arguments = argument;
    } else {
        ASTNode *current = print_node->arguments;
        while (current->next) {
            current = current->next;
        }
        current->next = argument;
    }
    argument->next = NULL;
}

// 创建二元操作节点
BinaryOpNode *create_binary_op(enum BinaryOpType op_type, ASTNode *left, ASTNode *right) {
    BinaryOpNode *binary_op = (BinaryOpNode *)malloc(sizeof(BinaryOpNode));
    if (!binary_op) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    binary_op->base.type = NODE_BINARY_OP;
    binary_op->base.next = NULL;
    binary_op->op_type = op_type;
    binary_op->left = left;
    binary_op->right = right;
    return binary_op;
}

// 创建变量声明节点
VarDeclNode *create_var_decl(char *name, ASTNode *expression) {
    VarDeclNode *var_decl = (VarDeclNode *)malloc(sizeof(VarDeclNode));
    if (!var_decl) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    var_decl->base.type = NODE_VAR_DECL;
    var_decl->base.next = NULL;
    var_decl->name = strdup(name);
    var_decl->expression = expression;
    return var_decl;
}

// 创建函数调用节点
FunctionCallNode *create_function_call(char *name) {
    FunctionCallNode *function_call = (FunctionCallNode *)malloc(sizeof(FunctionCallNode));
    if (!function_call) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    function_call->base.type = NODE_FUNCTION_CALL;
    function_call->base.next = NULL;
    function_call->name = strdup(name);
    function_call->arguments = NULL;
    return function_call;
}

// 添加函数到程序
void add_function(ProgramNode *program, FunctionNode *function) {
    if (!program->functions) {                  // 如果程序中没有函数，直接添加
        program->functions = (ASTNode *)function;       
    } else {
        ASTNode *current = program->functions;
        while (current->next) {                 // 遍历到最后一个函数节点
            current = current->next;            
        }
        current->next = (ASTNode *)function;    // 将新函数添加到最后
    }
}

// 添加参数到函数
void add_param(FunctionNode *function, IdentifierNode *param) {
    
    if (debug) printf("  - 添加函数参数: %s\n", param->name);

    if (!function->params) {
        function->params = (ASTNode *)param;
    } else {
        ASTNode *current = function->params;
        while (current->next) {
            current = current->next;
        }
        current->next = (ASTNode *)param;
    }
}

// 添加语句到函数体
void add_statement(FunctionNode *function, ASTNode *statement) {
    if (!function->body) {
        function->body = statement;
    } else {
        ASTNode *current = function->body;
        while (current->next) {
            current = current->next;
        }
        current->next = statement;
    }
}

// 添加参数到函数调用
void add_argument(FunctionCallNode *function_call, ASTNode *argument) {
    if (!function_call->arguments) {
        function_call->arguments = argument;
    } else {
        ASTNode *current = function_call->arguments;
        while (current->next) {
            current = current->next;
        }
        current->next = argument;
    }
}

// 创建条件语句节点
IfStatementNode *create_if_statement(ASTNode *condition, ASTNode *consequence, ASTNode *alternative) {
    IfStatementNode *if_node = (IfStatementNode *)malloc(sizeof(IfStatementNode));
    if (!if_node) {
        fprintf(stderr, "内存分配失败\n");
        exit(1);
    }
    if_node->base.type = NODE_IF_STATEMENT;
    if_node->base.next = NULL;
    if_node->condition = condition;
    if_node->consequence = consequence;
    if_node->alternative = alternative;
    return if_node;
}

// 释放AST节点
void free_ast(ASTNode *node) {
    if (!node) return;

    // 递归释放下一个节点
    free_ast(node->next);

    // 根据节点类型释放特定数据
    switch (node->type) {
        case NODE_PROGRAM: {
            ProgramNode *program = (ProgramNode *)node;
            free_ast(program->functions);
            break;
        }
        case NODE_FUNCTION: {
            FunctionNode *function = (FunctionNode *)node;
            free(function->name);
            free_ast(function->params);
            free_ast(function->body);
            break;
        }
        case NODE_IDENTIFIER: {
            IdentifierNode *identifier = (IdentifierNode *)node;
            free(identifier->name);
            break;
        }
        case NODE_LITERAL: {
            LiteralNode *literal = (LiteralNode *)node;
            if (literal->literal_type == LITERAL_STRING) {
                free(literal->value.string_value);
            }
            break;
        }
        case NODE_RETURN: {
            ReturnNode *return_node = (ReturnNode *)node;
            free_ast(return_node->expression);
            break;
        }
        case NODE_PRINT: {
            PrintNode *print_node = (PrintNode *)node;
            // 释放参数链表 - 直接递归释放整个参数链表
            free_ast(print_node->arguments);
            break;
        }
        case NODE_BINARY_OP: {
            BinaryOpNode *binary_op = (BinaryOpNode *)node;
            free_ast(binary_op->left);
            free_ast(binary_op->right);
            break;
        }
        case NODE_VAR_DECL: {
            VarDeclNode *var_decl = (VarDeclNode *)node;
            free(var_decl->name);
            free_ast(var_decl->expression);
            break;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *function_call = (FunctionCallNode *)node;
            free(function_call->name);
            free_ast(function_call->arguments);
            break;
        }
        case NODE_IF_STATEMENT: {
            IfStatementNode *if_node = (IfStatementNode *)node;
            free_ast(if_node->condition);
            free_ast(if_node->consequence);
            free_ast(if_node->alternative);
            break;
        }
        default:
            break;
    }

    // 释放节点本身
    free(node);
}