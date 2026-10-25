#ifndef TYPES_H
#define TYPES_H

#include "ast.h"

// 完整类型（VarTypeNode）的深拷贝。
//
// 具名类型携带的泛型实参必须一起复制：丢掉实参会让 `Vec<i32>` 退化成 `Vec`，
// 之后的方法查找、字段类型比较和 LLVM 类型构造都会拿错。
VarTypeNode *tap_type_clone(const VarTypeNode *source);

// 完整类型的等价判断。
//
// 具名类型比较名字**和泛型实参**；数组比较长度和元素类型；指针比较元素类型。
//
// 单态化（generics.c）和代码生成（codegen.c）共用这一份：两边对「同一类型」
// 必须给出相同答案，否则会出现「单态化认为是同一个实例、codegen 认为是两个」
// 这类静默错误。
int tap_type_equal(const VarTypeNode *left, const VarTypeNode *right);

// 在程序里按名字查找声明。
//
// 单态化和 Codegen 都要查结构体 / 枚举 / 字段，两边原本各写了一份 —— 而且
// Codegen 那份多了 `node->type` 检查，行为并不完全一致。统一到这里。
StructNode *tap_find_struct(ProgramNode *program, const char *name);
EnumNode *tap_find_enum(ProgramNode *program, const char *name);
// index_out 可传 NULL；非空时回填字段在结构体里的下标（用于 GEP）。
StructFieldNode *tap_find_struct_field(StructNode *structure, const char *name,
                                       unsigned *index_out);

// 把 `base.field` 形式的限定名按**最后一个**点拆开，成功返回 1。
//
// 按最后一个点切是这里的核心约定：模块私有枚举被改名成 `__tap_module_0.E.B`
// （两个点），只有从右边切才能拿到正确的 base。
// *base_out 由调用方 free；*field_out 指向 name 内部，不单独释放。
int tap_split_qualified_name(const char *name, char **base_out,
                             const char **field_out);

#endif
