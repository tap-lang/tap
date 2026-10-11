#ifndef GENERICS_H
#define GENERICS_H

#include "ast.h"

// 在代码生成前实例化程序中使用到的泛型函数。
int specialize_generics(ProgramNode *program);

// 校验每个 impl 块里的方法和 trait 声明的签名一致；不一致时打印诊断并返回 1。
// 要在所有模块加载完、类型改名之后调用。
int validate_impl_signatures(ProgramNode *program);

#endif // GENERICS_H
