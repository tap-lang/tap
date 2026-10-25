#include "types.h"

#include <stdlib.h>
#include <string.h>

// 复制具名类型携带的泛型实参。
static void clone_type_arguments(VarTypeNode *destination, ASTNode *arguments) {
    for (ASTNode *argument = arguments; argument; argument = argument->next) {
        add_var_type_argument(
            destination, tap_type_clone((VarTypeNode *)argument));
    }
}

VarTypeNode *tap_type_clone(const VarTypeNode *source) {
    if (!source) return NULL;
    if (source->is_pointer) {
        return create_pointer_type(tap_type_clone(source->element_type));
    }
    if (source->is_array) {
        return create_array_type(
            tap_type_clone(source->element_type), source->array_length);
    }
    if (source->struct_name) {
        VarTypeNode *copy = create_struct_type(source->struct_name);
        clone_type_arguments(copy, source->type_arguments);
        return copy;
    }
    if (source->enum_name) {
        VarTypeNode *copy = create_enum_type(source->enum_name);
        clone_type_arguments(copy, source->type_arguments);
        return copy;
    }
    return create_var_type(source->type);
}

int tap_type_equal(const VarTypeNode *left, const VarTypeNode *right) {
    if (!left || !right) return left == right;
    if (left->is_pointer != right->is_pointer ||
        left->is_array != right->is_array) {
        return 0;
    }
    if (left->is_pointer) {
        return tap_type_equal(left->element_type, right->element_type);
    }
    if (left->is_array) {
        return left->array_length == right->array_length &&
               tap_type_equal(left->element_type, right->element_type);
    }

    // 具名类型：名字相同还不够，泛型实参也必须逐项相同。
    const char *left_name = left->struct_name ? left->struct_name : left->enum_name;
    const char *right_name = right->struct_name ? right->struct_name : right->enum_name;
    if (left_name || right_name) {
        if (!left_name || !right_name || strcmp(left_name, right_name) != 0) return 0;
        ASTNode *left_argument = left->type_arguments;
        ASTNode *right_argument = right->type_arguments;
        while (left_argument && right_argument) {
            if (!tap_type_equal((VarTypeNode *)left_argument,
                                (VarTypeNode *)right_argument)) return 0;
            left_argument = left_argument->next;
            right_argument = right_argument->next;
        }
        return !left_argument && !right_argument;
    }
    return left->type == right->type;
}

StructNode *tap_find_struct(ProgramNode *program, const char *name) {
    if (!program || !name) return NULL;
    for (ASTNode *node = program->structs; node; node = node->next) {
        if (node->type != NODE_STRUCT) continue;
        StructNode *structure = (StructNode *)node;
        if (strcmp(structure->name, name) == 0) return structure;
    }
    return NULL;
}

EnumNode *tap_find_enum(ProgramNode *program, const char *name) {
    if (!program || !name) return NULL;
    for (ASTNode *node = program->enums; node; node = node->next) {
        if (node->type != NODE_ENUM) continue;
        EnumNode *enum_node = (EnumNode *)node;
        if (strcmp(enum_node->name, name) == 0) return enum_node;
    }
    return NULL;
}

StructFieldNode *tap_find_struct_field(StructNode *structure, const char *name,
                                       unsigned *index_out) {
    if (!structure || !name) return NULL;
    unsigned index = 0;
    for (ASTNode *node = structure->fields; node; node = node->next, index++) {
        StructFieldNode *field = (StructFieldNode *)node;
        if (strcmp(field->name, name) == 0) {
            if (index_out) *index_out = index;
            return field;
        }
    }
    return NULL;
}

int tap_split_qualified_name(const char *name, char **base_out,
                             const char **field_out) {
    if (!name || !base_out || !field_out) return 0;

    // 必须是最后一个点：`__tap_module_0.E.B` 有两个点，从左边切会把
    // `__tap_module_0` 当成 base，枚举成员就查不到了。
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return 0;

    size_t length = (size_t)(dot - name);
    char *base = (char *)malloc(length + 1);
    if (!base) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    memcpy(base, name, length);
    base[length] = '\0';

    *base_out = base;
    *field_out = dot + 1;
    return 1;
}
