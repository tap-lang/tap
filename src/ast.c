#include "ast.h"
#include <inttypes.h>

static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) {
        putchar(' ');
    }
}

static const char *literal_type_str(enum LiteralType t) {
    switch (t) {
    case LITERAL_INT: return "int";
    case LITERAL_UINT: return "uint";
    case LITERAL_I8: return "i8";
    case LITERAL_U8: return "u8";
    case LITERAL_I16: return "i16";
    case LITERAL_U16: return "u16";
    case LITERAL_I32: return "i32";
    case LITERAL_U32: return "u32";
    case LITERAL_I64: return "i64";
    case LITERAL_U64: return "u64";
    case LITERAL_I128: return "i128";
    case LITERAL_U128: return "u128";
    case LITERAL_FLOAT: return "float";
    case LITERAL_F32: return "f32";
    case LITERAL_F64: return "f64";
    case LITERAL_STRING: return "string";
    case LITERAL_BOOL: return "bool";
    default: return "?";
    }
}

static void print_var_type(const VarTypeNode *type) {
    if (type->is_array) {
        printf("[");
        print_var_type(type->element_type);
        printf("; %" PRIu64 "]", type->array_length);
    } else if (type->is_pointer) {
        printf("ptr<");
        print_var_type(type->element_type);
        printf(">");
    } else if (type->enum_name) {
        printf("%s", type->enum_name);
    } else if (type->struct_name) {
        printf("%s", type->struct_name);
    } else {
        printf("%s", literal_type_str(type->type));
    }
}

static const char *binary_op_str(enum BinaryOpType op) {
    switch (op) {
    case OP_ADD: return "+";
    case OP_SUBTRACT: return "-";
    case OP_MULTIPLY: return "*";
    case OP_DIVIDE: return "/";
    case OP_EQUAL: return "==";
    case OP_NOT_EQUAL: return "!=";
    case OP_LESS_THAN: return "<";
    case OP_GREATER_THAN: return ">";
    case OP_LESS_THAN_OR_EQUAL: return "<=";
    case OP_GREATER_THAN_OR_EQUAL: return ">=";
    case OP_AND: return "&&";
    case OP_OR: return "||";
    default: return "?";
    }
}

static void print_literal_value(const LiteralNode *lit) {
    switch (lit->literal_type) {
    case LITERAL_STRING:
        printf("%s", lit->value.string_value ? lit->value.string_value : "");
        break;
    case LITERAL_BOOL:
        printf("%s", lit->value.bool_value ? "true" : "false");
        break;
    case LITERAL_FLOAT:
    case LITERAL_F32:
    case LITERAL_F64:
        printf("%g", lit->value.float_value);
        break;
    default:
        if (lit->integer_text) {
            printf("%s", lit->integer_text);
        } else {
            printf("%" PRIu64, lit->value.int_value);
        }
        break;
    }
}

static void dump_expr(ASTNode *n, int depth);

static void dump_expr(ASTNode *n, int depth) {
    if (!n) {
        print_indent(depth);
        printf("(null)\n");
        return;
    }
    switch (n->type) {
    case NODE_IDENTIFIER:
        print_indent(depth);
        printf("Identifier: %s\n", ((IdentifierNode *)n)->name);
        break;
    case NODE_LITERAL:
        print_indent(depth);
        printf("Literal(%s): ", literal_type_str(((LiteralNode *)n)->literal_type));
        print_literal_value((LiteralNode *)n);
        printf("\n");
        break;
    case NODE_BINARY_OP: {
        BinaryOpNode *b = (BinaryOpNode *)n;
        print_indent(depth);
        printf("BinaryOp: %s\n", binary_op_str(b->op_type));
        dump_expr(b->left, depth + 2);
        dump_expr(b->right, depth + 2);
        break;
    }
    case NODE_REFERENCE:
        print_indent(depth);
        printf("Reference: &\n");
        dump_expr(((ReferenceNode *)n)->target, depth + 2);
        break;
    case NODE_ARRAY_LITERAL: {
        ArrayLiteralNode *array = (ArrayLiteralNode *)n;
        print_indent(depth);
        printf("ArrayLiteral[\n");
        for (ASTNode *element = array->elements; element; element = element->next) {
            dump_expr(element, depth + 2);
        }
        if (array->is_repeat) {
            print_indent(depth + 2);
            printf("repeat: %" PRIu64 "\n", array->repeat_count);
        }
        print_indent(depth);
        printf("]\n");
        break;
    }
    case NODE_INDEX_EXPRESSION: {
        IndexExpressionNode *index = (IndexExpressionNode *)n;
        print_indent(depth);
        printf("Index\n");
        dump_expr(index->array, depth + 2);
        dump_expr(index->index, depth + 2);
        break;
    }
    case NODE_FUNCTION_CALL: {
        FunctionCallNode *fc = (FunctionCallNode *)n;
        print_indent(depth);
        printf("Call: %s(\n", fc->name);
        for (ASTNode *a = fc->arguments; a; a = a->next) {
            dump_expr(a, depth + 2);
        }
        print_indent(depth);
        printf(")\n");
        break;
    }
    case NODE_STRUCT_LITERAL: {
        StructLiteralNode *literal = (StructLiteralNode *)n;
        print_indent(depth);
        printf("StructLiteral: %s {\n", literal->struct_name);
        for (ASTNode *field = literal->fields; field; field = field->next) {
            StructInitFieldNode *init = (StructInitFieldNode *)field;
            print_indent(depth + 2);
            printf("%s:\n", init->name);
            dump_expr(init->expression, depth + 4);
        }
        print_indent(depth);
        printf("}\n");
        break;
    }
    default:
        print_indent(depth);
        printf("(unknown expr node type %d)\n", n->type);
        break;
    }
}

static void dump_stmt(ASTNode *n, int depth);

static void dump_stmt_list(ASTNode *head, int depth) {
    for (ASTNode *s = head; s; s = s->next) {
        dump_stmt(s, depth);
    }
}

static void dump_stmt(ASTNode *n, int depth) {
    if (!n) {
        return;
    }
    switch (n->type) {
    case NODE_FUNCTION_CALL:
        dump_expr(n, depth);
        break;
    case NODE_VAR_DECL: {
        VarDeclNode *v = (VarDeclNode *)n;
        print_indent(depth);
        printf("%s: %s", v->is_const ? "ConstDecl" : "VarDecl", v->name);
        if (v->type) {
            printf(" : ");
            print_var_type(v->type);
        }
        printf("\n");
        if (v->expression) {
            print_indent(depth + 2);
            printf("init:\n");
            dump_expr(v->expression, depth + 4);
        }
        break;
    }
    case NODE_INDEX_ASSIGNMENT: {
        IndexAssignmentNode *assignment = (IndexAssignmentNode *)n;
        print_indent(depth);
        printf("IndexAssign\n");
        dump_expr((ASTNode *)assignment->target, depth + 2);
        dump_expr(assignment->expression, depth + 2);
        break;
    }
    case NODE_ASSIGNMENT: {
        AssignmentNode *assignment = (AssignmentNode *)n;
        print_indent(depth);
        printf("Assign: %s\n", assignment->name);
        dump_expr(assignment->expression, depth + 2);
        break;
    }
    case NODE_RETURN:
        print_indent(depth);
        printf("Return\n");
        dump_expr(((ReturnNode *)n)->expression, depth + 2);
        break;
    case NODE_PRINT:
        print_indent(depth);
        printf("Print(\n");
        for (ASTNode *a = ((PrintNode *)n)->arguments; a; a = a->next) {
            dump_expr(a, depth + 2);
        }
        print_indent(depth);
        printf(")\n");
        break;
    case NODE_IF_STATEMENT: {
        IfStatementNode *in = (IfStatementNode *)n;
        print_indent(depth);
        printf("If\n");
        print_indent(depth + 2);
        printf("condition:\n");
        dump_expr(in->condition, depth + 4);
        print_indent(depth + 2);
        printf("then:\n");
        dump_stmt_list(in->consequence, depth + 4);
        if (in->alternative) {
            print_indent(depth + 2);
            printf("else:\n");
            if (in->alternative->type == NODE_IF_STATEMENT) {
                dump_stmt(in->alternative, depth + 4);
            } else {
                dump_stmt_list(in->alternative, depth + 4);
            }
        }
        break;
    }
    case NODE_FOR_STATEMENT: {
        ForStatementNode *for_node = (ForStatementNode *)n;
        print_indent(depth);
        printf("For\n");
        if (for_node->initializer) {
            print_indent(depth + 2);
            printf("initializer:\n");
            dump_stmt(for_node->initializer, depth + 4);
        }
        if (for_node->condition) {
            print_indent(depth + 2);
            printf("condition:\n");
            dump_expr(for_node->condition, depth + 4);
        }
        if (for_node->update) {
            print_indent(depth + 2);
            printf("update:\n");
            dump_stmt(for_node->update, depth + 4);
        }
        print_indent(depth + 2);
        printf("body:\n");
        dump_stmt_list(for_node->body, depth + 4);
        break;
    }
    case NODE_BREAK_STATEMENT:
        print_indent(depth);
        printf("Break\n");
        break;
    case NODE_CONTINUE_STATEMENT:
        print_indent(depth);
        printf("Continue\n");
        break;
    default:
        print_indent(depth);
        printf("(unknown stmt node type %d)\n", n->type);
        break;
    }
}

void print_ast(const ProgramNode *program) {
    printf("Program\n");
    for (ASTNode *node = program->imports; node; node = node->next) {
        if (node->type == NODE_IMPORT) {
            ImportNode *import_node = (ImportNode *)node;
            printf("  Import: %s as %s\n", import_node->module_name, import_node->alias);
        }
    }
    for (ASTNode *node = program->enums; node; node = node->next) {
        if (node->type == NODE_ENUM) {
            EnumNode *enum_node = (EnumNode *)node;
            printf("  Enum: %s\n", enum_node->name);
            for (ASTNode *variant = enum_node->variants; variant; variant = variant->next) {
                if (variant->type == NODE_IDENTIFIER) {
                    print_indent(4);
                    printf("Variant: %s\n", ((IdentifierNode *)variant)->name);
                }
            }
        }
    }
    for (ASTNode *node = program->structs; node; node = node->next) {
        if (node->type == NODE_STRUCT) {
            StructNode *struct_node = (StructNode *)node;
            printf("  Struct: %s\n", struct_node->name);
            for (ASTNode *field = struct_node->fields; field; field = field->next) {
                if (field->type == NODE_STRUCT_FIELD) {
                    StructFieldNode *struct_field = (StructFieldNode *)field;
                    print_indent(4);
                    printf("Field: %s : ", struct_field->name);
                    print_var_type(struct_field->field_type);
                    printf("\n");
                }
            }
        }
    }
    for (ASTNode *fn = program->functions; fn; fn = fn->next) {
        if (fn->type != NODE_FUNCTION) {
            continue;
        }
        FunctionNode *f = (FunctionNode *)fn;
        printf("  %sFunction: %s", f->is_extern ? "Extern " : "", f->name);
        if (f->return_type) {
            printf(" -> ");
            print_var_type(f->return_type);
        }
        printf("\n");

        ASTNode *p = f->params;
        ASTNode *pt = f->param_types;
        while (p || pt) {
            print_indent(4);
            printf("Param: ");
            if (p && p->type == NODE_IDENTIFIER) {
                printf("%s", ((IdentifierNode *)p)->name);
            } else {
                printf("?");
            }
            if (pt && pt->type == NODE_VAR_TYPE) {
                printf(" : ");
                print_var_type((VarTypeNode *)pt);
            }
            printf("\n");
            p = p ? p->next : NULL;
            pt = pt ? pt->next : NULL;
        }

        print_indent(4);
        printf("Body:\n");
        dump_stmt_list(f->body, 6);
    }
}

// 创建程序节点
ProgramNode *create_program() {
    ProgramNode *program = (ProgramNode *)malloc(sizeof(ProgramNode));
    if (!program) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    program->base.type = NODE_PROGRAM;
    program->base.next = NULL;
    program->imports = NULL;
    program->enums = NULL;
    program->structs = NULL;
    program->constants = NULL;
    program->functions = NULL;
    return program;
}

ImportNode *create_import(
    const char *module_name, const char *alias, const char *filename, int line, int column) {
    ImportNode *import_node = (ImportNode *)malloc(sizeof(ImportNode));
    if (!import_node) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    import_node->base.type = NODE_IMPORT;
    import_node->base.next = NULL;
    import_node->module_name = strdup(module_name);
    import_node->alias = strdup(alias);
    import_node->filename = strdup(filename);
    import_node->line = line;
    import_node->column = column;
    return import_node;
}

// 创建函数节点
FunctionNode *create_function(char *name) {
    FunctionNode *function = (FunctionNode *)malloc(sizeof(FunctionNode));
    if (!function) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    function->base.type = NODE_FUNCTION;
    function->base.next = NULL;
    function->name = strdup(name);
    function->filename = NULL;
    function->line = 0;
    function->column = 0;
    function->is_extern = 0;
    function->params = NULL;
    function->param_types = NULL;
    function->body = NULL;
    function->return_type = NULL;
    return function;
}

// 创建枚举声明节点，成员稍后通过 add_enum_variant 追加。
EnumNode *create_enum(char *name) {
    EnumNode *enum_node = (EnumNode *)malloc(sizeof(EnumNode));
    if (!enum_node) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    enum_node->base.type = NODE_ENUM;
    enum_node->base.next = NULL;
    enum_node->name = strdup(name);
    enum_node->variants = NULL;
    return enum_node;
}

// 创建结构体声明节点，字段稍后通过 add_struct_field 追加。
StructNode *create_struct(char *name) {
    StructNode *struct_node = (StructNode *)malloc(sizeof(StructNode));
    if (!struct_node) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    struct_node->base.type = NODE_STRUCT;
    struct_node->base.next = NULL;
    struct_node->name = strdup(name);
    struct_node->fields = NULL;
    return struct_node;
}

// 创建结构体字段声明节点，接管 field_type 的所有权。
StructFieldNode *create_struct_field(char *name, VarTypeNode *field_type) {
    StructFieldNode *field = (StructFieldNode *)malloc(sizeof(StructFieldNode));
    if (!field) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    field->base.type = NODE_STRUCT_FIELD;
    field->base.next = NULL;
    field->name = strdup(name);
    field->field_type = field_type;
    return field;
}

// 创建结构体字面量节点，字段稍后通过 add_struct_init_field 追加。
StructLiteralNode *create_struct_literal(char *struct_name) {
    StructLiteralNode *literal = (StructLiteralNode *)malloc(sizeof(StructLiteralNode));
    if (!literal) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    literal->base.type = NODE_STRUCT_LITERAL;
    literal->base.next = NULL;
    literal->struct_name = strdup(struct_name);
    literal->fields = NULL;
    return literal;
}

// 创建结构体初始化字段节点，接管 expression 的所有权。
StructInitFieldNode *create_struct_init_field(char *name, ASTNode *expression) {
    StructInitFieldNode *field =
        (StructInitFieldNode *)malloc(sizeof(StructInitFieldNode));
    if (!field) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    field->base.type = NODE_STRUCT_INIT_FIELD;
    field->base.next = NULL;
    field->name = strdup(name);
    field->expression = expression;
    return field;
}

// 创建标识符节点
IdentifierNode *create_identifier(char *name) {
    IdentifierNode *identifier = (IdentifierNode *)malloc(sizeof(IdentifierNode));
    if (!identifier) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    identifier->base.type = NODE_IDENTIFIER;
    identifier->base.next = NULL;
    identifier->name = strdup(name);
    return identifier;
}

// 创建整数字面量节点
LiteralNode *create_int_literal(uint64_t value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_I32;
    literal->integer_text = NULL;
    literal->value.int_value = value;
    return literal;
}

LiteralNode *create_int_literal_text(const char *value) {
    LiteralNode *literal = create_int_literal(strtoull(value, NULL, 10));
    literal->integer_text = strdup(value);
    return literal;
}

// 创建字符串字面量节点
LiteralNode *create_string_literal(char *value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_STRING;
    literal->integer_text = NULL;
    literal->value.string_value = strdup(value);
    return literal;
}

// 创建浮点数字面量节点
LiteralNode *create_float_literal(double value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_FLOAT;
    literal->integer_text = NULL;
    literal->value.float_value = value;
    return literal;
}

// 创建布尔字面量节点
LiteralNode *create_bool_literal(int value) {
    LiteralNode *literal = (LiteralNode *)malloc(sizeof(LiteralNode));
    if (!literal) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    literal->base.type = NODE_LITERAL;
    literal->base.next = NULL;
    literal->literal_type = LITERAL_BOOL;
    literal->integer_text = NULL;
    literal->value.bool_value = value;
    return literal;
}

// 创建返回语句节点
ReturnNode *create_return(ASTNode *expression) {
    ReturnNode *return_node = (ReturnNode *)malloc(sizeof(ReturnNode));
    if (!return_node) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
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
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
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
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    binary_op->base.type = NODE_BINARY_OP;
    binary_op->base.next = NULL;
    binary_op->op_type = op_type;
    binary_op->left = left;
    binary_op->right = right;
    return binary_op;
}

// 创建取地址表达式节点，并接管 target 的所有权。
ReferenceNode *create_reference(ASTNode *target) {
    ReferenceNode *reference = (ReferenceNode *)malloc(sizeof(ReferenceNode));
    if (!reference) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    reference->base.type = NODE_REFERENCE;
    reference->base.next = NULL;
    reference->target = target;
    return reference;
}

// 创建变量类型节点
VarTypeNode *create_var_type(enum LiteralType type) {
    VarTypeNode *var_type = (VarTypeNode *)malloc(sizeof(VarTypeNode));
    if (!var_type) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    var_type->base.type = NODE_VAR_TYPE;
    var_type->base.next = NULL;
    var_type->type = type;
    var_type->enum_name = NULL;
    var_type->struct_name = NULL;
    var_type->is_array = 0;
    var_type->is_pointer = 0;
    var_type->array_length = 0;
    var_type->element_type = NULL;
    return var_type;
}

// 创建枚举类型节点；枚举在 LLVM 中以 i32 形式存储。
VarTypeNode *create_enum_type(const char *name) {
    VarTypeNode *type = create_var_type(LITERAL_I32);
    type->enum_name = strdup(name);
    if (!type->enum_name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    return type;
}

// 创建结构体类型节点；结构体在 LLVM 中以具名 struct 降低。
VarTypeNode *create_struct_type(const char *name) {
    VarTypeNode *type = create_var_type(LITERAL_I32);
    type->struct_name = strdup(name);
    if (!type->struct_name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    return type;
}

// 创建可递归嵌套的数组类型节点。
VarTypeNode *create_array_type(VarTypeNode *element_type, uint64_t length) {
    VarTypeNode *type = create_var_type(element_type->type);
    if (element_type->enum_name) {
        type->enum_name = strdup(element_type->enum_name);
        if (!type->enum_name) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
    }
    if (element_type->struct_name) {
        type->struct_name = strdup(element_type->struct_name);
        if (!type->struct_name) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
    }
    type->is_array = 1;
    type->array_length = length;
    type->element_type = element_type;
    return type;
}

// 创建可递归嵌套的指针类型节点。
VarTypeNode *create_pointer_type(VarTypeNode *element_type) {
    VarTypeNode *type = create_var_type(element_type->type);
    if (element_type->enum_name) {
        type->enum_name = strdup(element_type->enum_name);
        if (!type->enum_name) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
    }
    if (element_type->struct_name) {
        type->struct_name = strdup(element_type->struct_name);
        if (!type->struct_name) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
    }
    type->is_pointer = 1;
    type->element_type = element_type;
    return type;
}

VarDeclNode *create_var_decl(
    char *name, VarTypeNode *type, ASTNode *expression, int is_const) {
    VarDeclNode *var_decl = (VarDeclNode *)malloc(sizeof(VarDeclNode));
    if (!var_decl) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    var_decl->base.type = NODE_VAR_DECL;
    var_decl->base.next = NULL;
    var_decl->name = strdup(name);
    var_decl->type = type;
    var_decl->expression = expression;
    var_decl->is_const = is_const;
    return var_decl;
}

AssignmentNode *create_assignment(const char *name, ASTNode *expression) {
    AssignmentNode *assignment = (AssignmentNode *)malloc(sizeof(AssignmentNode));
    if (!assignment) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    assignment->base.type = NODE_ASSIGNMENT;
    assignment->base.next = NULL;
    assignment->name = strdup(name);
    assignment->expression = expression;
    return assignment;
}

// 创建函数调用节点
ArrayLiteralNode *create_array_literal(void) {
    ArrayLiteralNode *array = (ArrayLiteralNode *)calloc(1, sizeof(ArrayLiteralNode));
    if (!array) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    array->base.type = NODE_ARRAY_LITERAL;
    return array;
}

void add_array_element(ArrayLiteralNode *array, ASTNode *element) {
    if (!array->elements) {
        array->elements = element;
    } else {
        ASTNode *current = array->elements;
        while (current->next) current = current->next;
        current->next = element;
    }
    array->count++;
}

void set_array_repeat(ArrayLiteralNode *array, uint64_t repeat_count) {
    array->is_repeat = 1;
    array->repeat_count = repeat_count;
}

IndexExpressionNode *create_index_expression(ASTNode *array, ASTNode *index) {
    IndexExpressionNode *expression =
        (IndexExpressionNode *)calloc(1, sizeof(IndexExpressionNode));
    if (!expression) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    expression->base.type = NODE_INDEX_EXPRESSION;
    expression->array = array;
    expression->index = index;
    return expression;
}

IndexAssignmentNode *create_index_assignment(
    IndexExpressionNode *target, ASTNode *expression) {
    IndexAssignmentNode *assignment =
        (IndexAssignmentNode *)calloc(1, sizeof(IndexAssignmentNode));
    if (!assignment) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    assignment->base.type = NODE_INDEX_ASSIGNMENT;
    assignment->target = target;
    assignment->expression = expression;
    return assignment;
}

FunctionCallNode *create_function_call(char *name) {
    FunctionCallNode *function_call = (FunctionCallNode *)malloc(sizeof(FunctionCallNode));
    if (!function_call) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    function_call->base.type = NODE_FUNCTION_CALL;
    function_call->base.next = NULL;
    function_call->filename = NULL;
    function_call->line = 0;
    function_call->column = 0;
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

// 添加顶层常量到程序，保持源码中的声明顺序。
void add_constant(ProgramNode *program, VarDeclNode *constant) {
    if (!program->constants) {
        program->constants = (ASTNode *)constant;
        return;
    }

    ASTNode *current = program->constants;
    while (current->next) current = current->next;
    current->next = (ASTNode *)constant;
}

// 添加枚举到程序，保持源码声明顺序。
void add_enum(ProgramNode *program, EnumNode *enum_node) {
    if (!program->enums) {
        program->enums = (ASTNode *)enum_node;
        return;
    }

    ASTNode *current = program->enums;
    while (current->next) current = current->next;
    current->next = (ASTNode *)enum_node;
}

// 添加结构体到程序，保持源码声明顺序。
void add_struct(ProgramNode *program, StructNode *struct_node) {
    if (!program->structs) {
        program->structs = (ASTNode *)struct_node;
        return;
    }

    ASTNode *current = program->structs;
    while (current->next) current = current->next;
    current->next = (ASTNode *)struct_node;
}

// 添加字段到结构体声明，保持源码中的字段顺序。
void add_struct_field(StructNode *struct_node, StructFieldNode *field) {
    if (!struct_node->fields) {
        struct_node->fields = (ASTNode *)field;
        return;
    }

    ASTNode *current = struct_node->fields;
    while (current->next) current = current->next;
    current->next = (ASTNode *)field;
}

// 添加初始化字段到结构体字面量，保持源码顺序。
void add_struct_init_field(StructLiteralNode *literal, StructInitFieldNode *field) {
    if (!literal->fields) {
        literal->fields = (ASTNode *)field;
        return;
    }

    ASTNode *current = literal->fields;
    while (current->next) current = current->next;
    current->next = (ASTNode *)field;
}

// 添加枚举成员到枚举声明，保持源码中的成员顺序。
void add_enum_variant(EnumNode *enum_node, IdentifierNode *variant) {
    if (!enum_node->variants) {
        enum_node->variants = (ASTNode *)variant;
        return;
    }

    ASTNode *current = enum_node->variants;
    while (current->next) current = current->next;
    current->next = (ASTNode *)variant;
}

void add_import(ProgramNode *program, ImportNode *import_node) {
    if (!program->imports) {
        program->imports = (ASTNode *)import_node;
        return;
    }

    ASTNode *current = program->imports;
    while (current->next) current = current->next;
    current->next = (ASTNode *)import_node;
}

// 添加参数到函数
void add_param(FunctionNode *function, IdentifierNode *param) {
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

// 添加参数类型到函数
void add_param_type(FunctionNode *function, VarTypeNode *type) {
    if (!function->param_types) {
        function->param_types = (ASTNode *)type;
    } else {
        ASTNode *current = function->param_types;
        while (current->next) {
            current = current->next;
        }
        current->next = (ASTNode *)type;
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
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    if_node->base.type = NODE_IF_STATEMENT;
    if_node->base.next = NULL;
    if_node->condition = condition;
    if_node->consequence = consequence;
    if_node->alternative = alternative;
    return if_node;
}

ForStatementNode *create_for_statement(
    ASTNode *initializer, ASTNode *condition, ASTNode *update, ASTNode *body) {
    ForStatementNode *for_node = (ForStatementNode *)malloc(sizeof(ForStatementNode));
    if (!for_node) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    for_node->base.type = NODE_FOR_STATEMENT;
    for_node->base.next = NULL;
    for_node->initializer = initializer;
    for_node->condition = condition;
    for_node->update = update;
    for_node->body = body;
    return for_node;
}

static ASTNode *create_control_statement(enum NodeType type) {
    ASTNode *node = (ASTNode *)malloc(sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    node->type = type;
    node->next = NULL;
    return node;
}

ASTNode *create_break_statement(void) {
    return create_control_statement(NODE_BREAK_STATEMENT);
}

ASTNode *create_continue_statement(void) {
    return create_control_statement(NODE_CONTINUE_STATEMENT);
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
            free_ast(program->imports);
            free_ast(program->enums);
            free_ast(program->structs);
            free_ast(program->constants);
            free_ast(program->functions);
            break;
        }
        case NODE_IMPORT: {
            ImportNode *import_node = (ImportNode *)node;
            free(import_node->module_name);
            free(import_node->alias);
            free(import_node->filename);
            break;
        }
        case NODE_FUNCTION: {
            FunctionNode *function = (FunctionNode *)node;
            free(function->name);
            free(function->filename);
            free_ast(function->params);
            free_ast(function->param_types);
            free_ast(function->body);
            free_ast((ASTNode *)function->return_type);
            break;
        }
        case NODE_ENUM: {
            EnumNode *enum_node = (EnumNode *)node;
            free(enum_node->name);
            free_ast(enum_node->variants);
            break;
        }
        case NODE_STRUCT: {
            StructNode *struct_node = (StructNode *)node;
            free(struct_node->name);
            free_ast(struct_node->fields);
            break;
        }
        case NODE_STRUCT_FIELD: {
            StructFieldNode *field = (StructFieldNode *)node;
            free(field->name);
            free_ast((ASTNode *)field->field_type);
            break;
        }
        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)node;
            free(literal->struct_name);
            free_ast(literal->fields);
            break;
        }
        case NODE_STRUCT_INIT_FIELD: {
            StructInitFieldNode *field = (StructInitFieldNode *)node;
            free(field->name);
            free_ast(field->expression);
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
            free(literal->integer_text);
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
        case NODE_REFERENCE:
            free_ast(((ReferenceNode *)node)->target);
            break;
        case NODE_VAR_TYPE: {
            // 数组类型递归拥有它的元素类型。
            VarTypeNode *var_type = (VarTypeNode *)node;
            free(var_type->enum_name);
            free(var_type->struct_name);
            free_ast((ASTNode *)var_type->element_type);
            break;
        }
        case NODE_VAR_DECL: {
            VarDeclNode *var_decl = (VarDeclNode *)node;
            free(var_decl->name);
            free_ast((ASTNode *)var_decl->type);
            free_ast(var_decl->expression);
            break;
        }
        case NODE_ASSIGNMENT: {
            AssignmentNode *assignment = (AssignmentNode *)node;
            free(assignment->name);
            free_ast(assignment->expression);
            break;
        }
        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)node;
            free_ast(array->elements);
            break;
        }
        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)node;
            free_ast(index->array);
            free_ast(index->index);
            break;
        }
        case NODE_INDEX_ASSIGNMENT: {
            IndexAssignmentNode *assignment = (IndexAssignmentNode *)node;
            free_ast((ASTNode *)assignment->target);
            free_ast(assignment->expression);
            break;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *function_call = (FunctionCallNode *)node;
            free(function_call->name);
            free(function_call->filename);
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
        case NODE_FOR_STATEMENT: {
            ForStatementNode *for_node = (ForStatementNode *)node;
            free_ast(for_node->initializer);
            free_ast(for_node->condition);
            free_ast(for_node->update);
            free_ast(for_node->body);
            break;
        }
        default:
            break;
    }

    // 释放节点本身
    free(node);
}
