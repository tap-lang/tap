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

// 深拷贝一个参数类型链表。
static ASTNode *clone_type_list(ASTNode *types) {
    ASTNode *head = NULL;
    ASTNode **tail = &head;
    for (ASTNode *type = types; type; type = type->next) {
        ASTNode *copy = (ASTNode *)tap_type_clone((VarTypeNode *)type);
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

VarTypeNode *tap_type_clone(const VarTypeNode *source) {
    if (!source) return NULL;
    // 函数指针要放在 is_pointer 之前判断：两者互斥，但顺序写反了会漏。
    if (source->is_function_pointer) {
        return create_function_pointer_type(
            clone_type_list(source->parameter_types),
            tap_type_clone(source->return_type));
    }
    if (source->is_reference) {
        return create_reference_type(
            tap_type_clone(source->element_type), source->reference_mutable);
    }
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
    // 函数指针按签名逐项比较：参数个数、每个参数类型、返回类型都要一致。
    if (left->is_function_pointer || right->is_function_pointer) {
        if (!left->is_function_pointer || !right->is_function_pointer) return 0;
        ASTNode *left_param = left->parameter_types;
        ASTNode *right_param = right->parameter_types;
        while (left_param && right_param) {
            if (!tap_type_equal((VarTypeNode *)left_param,
                                (VarTypeNode *)right_param)) return 0;
            left_param = left_param->next;
            right_param = right_param->next;
        }
        if (left_param || right_param) return 0;
        return tap_type_equal(left->return_type, right->return_type);
    }
    if (left->is_pointer != right->is_pointer ||
        left->is_array != right->is_array) {
        return 0;
    }
    // 借用和裸指针不是同一个类型：`&T` 有借用检查，`*T` 没有，互相赋值必须显式
    // 走 `as`。`&mut T` 和 `&T` 同样不能混（`&mut T` 可退化成 `&T`，反过来不行，
    // 那条降级在 expression_assignable_to 里单独放行）。
    if (left->is_reference != right->is_reference) return 0;
    if (left->is_reference &&
        left->reference_mutable != right->reference_mutable) {
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

int tap_reference_coerces(const VarTypeNode *source, const VarTypeNode *target) {
    if (!source || !target) return 0;
    if (!source->is_reference || !target->is_reference) return 0;
    // 只有「独占 → 共享」这一个方向：`&T` 的调用方没承诺独占，不能升级成 `&mut T`。
    if (!source->reference_mutable || target->reference_mutable) return 0;
    return tap_type_equal(source->element_type, target->element_type);
}

int tap_receiver_type_matches(const VarTypeNode *parameter,
                              const VarTypeNode *receiver) {
    if (!parameter || !receiver) return 0;
    // 完全一致：`self: T` 收 `T`，`self: &T` 收 `&T`。
    if (tap_type_equal(parameter, receiver)) return 1;
    // `&mut T` 的接收者可以调 `&self` 方法：独占借用天然满足共享借用的要求。
    if (tap_reference_coerces(receiver, parameter)) return 1;
    // 自动取地址：方法要 `&T`，接收者是 `T` 时调用点补一个取地址。
    // 这里只放行引用，`*T` 不参与 —— 裸指针没有借用承诺，不能直接当借用传。
    return parameter->is_reference && parameter->element_type &&
           tap_type_equal(parameter->element_type, receiver);
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
