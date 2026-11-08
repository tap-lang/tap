#ifndef TAP_NAMES_H
#define TAP_NAMES_H

#include "ast.h"

// 名字归一：把「根确定是当前函数局部变量」的点号标识符（`p.x`、`self.inner.value`）
// 改写成 NODE_FIELD_ACCESS 链，让字段访问在 AST 里只有一种表示。
//
// 必须在 load_modules 之后调用。不去判断「这是不是模块名」：局部变量会遮蔽同名的
// 模块别名，所以「根是局部变量」就足够了，模块成员 / 类型成员的根根本不在这里。
//
// 排查用 TAP_TRACE_NAMES=1 打印每一处改写。
void normalize_qualified_names(ProgramNode *program);

#endif
