#ifndef TAGGED_H
#define TAGGED_H

#include "ast.h"

// 把带载荷的枚举展开成同名结构体，供泛型单态化和代码生成直接消费。
// 无载荷的枚举保持 i32 表示，不做任何改写。
int lower_payload_enums(ProgramNode *program);

#endif
