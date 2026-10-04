// 载荷枚举（tagged union）的降级实现。
//
// 没有 union 类型可用，所以带载荷的枚举被展开成一个同名结构体：
// 字段 0 是 i32 判别标签，之后按声明顺序展平各成员的载荷。访问一律通过构造和
// match，因此这个布局是内部实现细节，将来可以换成更紧凑的编码而不影响源码。
//
// 无载荷的枚举不做改写，继续按 i32 常量表处理，保持既有语义。
#include "tagged.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 深拷贝类型节点：枚举成员与内部结构体字段各自持有自己的类型，避免共享所有权
// 导致 free_ast 重复释放。
static VarTypeNode *clone_type(const VarTypeNode *source) {
    VarTypeNode *copy = create_var_type(source->type);
    if (source->enum_name) copy->enum_name = strdup(source->enum_name);
    if (source->struct_name) copy->struct_name = strdup(source->struct_name);
    if ((source->enum_name && !copy->enum_name) ||
        (source->struct_name && !copy->struct_name)) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    copy->is_array = source->is_array;
    copy->is_pointer = source->is_pointer;
    copy->array_length = source->array_length;
    if (source->element_type) {
        copy->element_type = clone_type(source->element_type);
    }
    for (ASTNode *argument = source->type_arguments;
         argument; argument = argument->next) {
        add_var_type_argument(copy, clone_type((VarTypeNode *)argument));
    }
    return copy;
}

// 载荷字段名使用源语言写不出的 `$`，不会与用户字段冲突。
static char *payload_field_name(const char *variant_name, unsigned index) {
    size_t length = strlen(variant_name) + 16;
    char *name = malloc(length);
    if (!name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    snprintf(name, length, "%s$%u", variant_name, index);
    return name;
}

// 统计一个成员的载荷数量。
static unsigned payload_count(const EnumVariantNode *variant) {
    unsigned count = 0;
    for (ASTNode *type = variant->payload_types; type; type = type->next) count++;
    return count;
}

// 把单个带载荷的枚举展开成同名结构体。
static void lower_enum(ProgramNode *program, EnumNode *enum_node) {
    // 先按声明顺序回填成员序号：无载荷枚举也靠它做常量查表。
    unsigned tag = 0;
    for (ASTNode *node = enum_node->variants; node; node = node->next, tag++) {
        ((EnumVariantNode *)node)->tag = tag;
    }

    int has_payload = 0;
    for (ASTNode *node = enum_node->variants; node; node = node->next) {
        if (payload_count((EnumVariantNode *)node) > 0) {
            has_payload = 1;
            break;
        }
    }
    enum_node->has_payload = has_payload;
    if (!has_payload) return; // 无载荷枚举保持 i32 表示

    StructNode *structure = create_struct(enum_node->name);
    structure->is_tagged_enum = 1;
    structure->is_pub = enum_node->is_pub;
    structure->tagged_enum_name = strdup(enum_node->name);
    if (!structure->tagged_enum_name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    // 泛型枚举的类型参数跟着搬到结构体上，泛型单态化才能按实参实例化出 Option$i32。
    for (ASTNode *node = enum_node->type_params; node; node = node->next) {
        add_struct_type_param(
            structure, create_identifier(((IdentifierNode *)node)->name));
    }
    add_struct(program, structure);

    // 判别标签固定占据字段 0。
    add_struct_field(
        structure, create_struct_field("tag", create_var_type(LITERAL_I32)));

    unsigned next_field = 1;
    for (ASTNode *node = enum_node->variants; node; node = node->next) {
        EnumVariantNode *variant = (EnumVariantNode *)node;
        variant->field_index = next_field;

        unsigned index = 0;
        for (ASTNode *type = variant->payload_types;
             type; type = type->next, index++) {
            char *field_name = payload_field_name(variant->name, index);
            add_struct_field(structure,
                create_struct_field(field_name, clone_type((VarTypeNode *)type)));
            free(field_name);
            next_field++;
        }
    }
}

int lower_payload_enums(ProgramNode *program) {
    for (ASTNode *node = program ? program->enums : NULL; node; node = node->next) {
        if (node->type != NODE_ENUM) continue;
        lower_enum(program, (EnumNode *)node);
    }
    return 0;
}
