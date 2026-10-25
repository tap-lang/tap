#ifndef SEMA_H
#define SEMA_H

#include "ast.h"

// 在生成任何 IR 之前解析整个程序的类型。
//
// 当前是**影子模式**：只负责填 `ASTNode.resolved_type`，不接管诊断、不改变 Codegen
// 的任何行为。Codegen 的 expression_full_type 优先读这个字段，读不到才自己推断。
//
// 这样做的目的：Sema 可以先覆盖能确定的类型，剩下的逐步接管，中间任何时刻行为都和
// 没有 Sema 时完全一致。设置环境变量 TAP_SEMA_STATS=1 会在结束时打印覆盖率。
//
// 返回 0 表示跑完（不代表没有语义错误 —— 诊断仍然由 Codegen 负责）。
int analyze_program(ProgramNode *program);

#endif
