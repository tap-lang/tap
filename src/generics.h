#ifndef GENERICS_H
#define GENERICS_H

#include "ast.h"

// 在代码生成前实例化程序中使用到的泛型函数。
int specialize_generics(ProgramNode *program);

#endif // GENERICS_H
