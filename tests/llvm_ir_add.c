#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <stdio.h>

LLVMValueRef createAddFunction(LLVMModuleRef module, LLVMBuilderRef builder) {
    // 创建函数类型: i32 (i32, i32)
    LLVMTypeRef param_types[] = {LLVMInt32Type(), LLVMInt32Type()};
    LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 2, 0);
    
    // 创建函数
    LLVMValueRef func = LLVMAddFunction(module, "add", func_type);
    
    // 创建基本块
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    
    // 获取参数
    LLVMValueRef param_a = LLVMGetParam(func, 0);
    LLVMValueRef param_b = LLVMGetParam(func, 1);
    
    // 创建加法指令
    LLVMValueRef result = LLVMBuildAdd(builder, param_a, param_b, "add_result");
    
    // 返回结果
    LLVMBuildRet(builder, result);
    
    return func;
}

LLVMValueRef createFactorialFunction(LLVMModuleRef module, LLVMBuilderRef builder) {
    // 函数类型: i32 (i32)
    LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), 
                                            (LLVMTypeRef[]){LLVMInt32Type()}, 1, 0);
    
    LLVMValueRef func = LLVMAddFunction(module, "factorial", func_type);
    LLVMValueRef n_param = LLVMGetParam(func, 0);
    
    // 创建基本块
    LLVMBasicBlockRef entry = LLVMAppendBasicBlock(func, "entry");
    LLVMBasicBlockRef then_block = LLVMAppendBasicBlock(func, "then");
    LLVMBasicBlockRef else_block = LLVMAppendBasicBlock(func, "else");
    LLVMBasicBlockRef merge_block = LLVMAppendBasicBlock(func, "merge");
    
    LLVMPositionBuilderAtEnd(builder, entry);
    
    // 条件判断: if n <= 1
    LLVMValueRef one = LLVMConstInt(LLVMInt32Type(), 1, 0);
    LLVMValueRef condition = LLVMBuildICmp(builder, LLVMIntSLE, n_param, one, "condition");
    LLVMBuildCondBr(builder, condition, then_block, else_block);
    
    // then块: return 1
    LLVMPositionBuilderAtEnd(builder, then_block);
    LLVMBuildBr(builder, merge_block);
    
    // else块: return n * factorial(n-1)
    LLVMPositionBuilderAtEnd(builder, else_block);
    LLVMValueRef n_minus_one = LLVMBuildSub(builder, n_param, one, "n_minus_one");
    LLVMValueRef recursive_call = LLVMBuildCall2(builder, func_type, func, 
                                                (LLVMValueRef[]){n_minus_one}, 1, "recursive");
    LLVMValueRef result = LLVMBuildMul(builder, n_param, recursive_call, "result");
    LLVMBuildBr(builder, merge_block);
    
    // merge块: phi节点选择返回值
    LLVMPositionBuilderAtEnd(builder, merge_block);
    LLVMValueRef phi = LLVMBuildPhi(builder, LLVMInt32Type(), "result_phi");
    LLVMAddIncoming(phi, (LLVMValueRef[]){one, result}, 
                   (LLVMBasicBlockRef[]){then_block, else_block}, 2);
    LLVMBuildRet(builder, phi);
    
    return func;
}

int main() {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("math_module", context);
    LLVMBuilderRef builder = LLVMCreateBuilder();
    
    // 创建函数
    createAddFunction(module, builder);
    createFactorialFunction(module, builder);
    
    // 创建main函数测试
    LLVMTypeRef main_type = LLVMFunctionType(LLVMInt32Type(), NULL, 0, 0);
    LLVMValueRef main_func = LLVMAddFunction(module, "main", main_type);
    LLVMBasicBlockRef main_entry = LLVMAppendBasicBlock(main_func, "entry");
    LLVMPositionBuilderAtEnd(builder, main_entry);
    
    // 调用factorial(5)
    LLVMValueRef five = LLVMConstInt(LLVMInt32Type(), 5, 0);
    LLVMTypeRef fact_type = LLVMFunctionType(LLVMInt32Type(), 
                                            (LLVMTypeRef[]){LLVMInt32Type()}, 1, 0);
    LLVMValueRef fact_func = LLVMGetNamedFunction(module, "factorial");
    LLVMValueRef fact_result = LLVMBuildCall2(builder, fact_type, fact_func, 
                                             (LLVMValueRef[]){five}, 1, "fact_result");
    
    LLVMBuildRet(builder, fact_result);
    
    // 输出IR
    printf("生成的数学函数IR:\n");
    LLVMDumpModule(module);
    
    // 清理
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);
    
    return 0;
}

/**
# 编译命令
# 使用系统LLVM
clang -o build/ir_add tests/llvm_ir_add.c `llvm-config --cflags --ldflags --libs core analysis executionengine target`

# 或者使用brew安装的LLVM（macOS）
clang -o build/ir_add tests/llvm_ir_add.c -I/opt/homebrew/opt/llvm/include -L/opt/homebrew/opt/llvm/lib -lLLVM
*/