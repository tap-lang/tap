#ifndef TAP_NAMES_H
#define TAP_NAMES_H

#include "ast.h"

// 名字归一：把「根确定是当前函数局部变量」的点号标识符（`p.x`、`self.inner.value`）
// 改写成 NODE_FIELD_ACCESS 链，让字段访问在 AST 里只有一种表示。
//
// 必须在 load_modules 之后调用 —— import 可以写在文件末尾，模块别名和类型名这时才齐全。
void normalize_qualified_names(ProgramNode *program);

#endif
