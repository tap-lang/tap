#include <stdio.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>

int main() {
    // 初始化LLVM
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    printf("初始化LLVM完成\n");
    
    // 创建上下文和模块
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("hello_module", context);
    
    // 创建函数类型: i32 () - 使用带上下文的类型创建函数
    LLVMTypeRef return_type = LLVMInt32TypeInContext(context);
    LLVMTypeRef function_type = LLVMFunctionType(return_type, NULL, 0, 0);
    
    // 创建main函数
    LLVMValueRef main_func = LLVMAddFunction(module, "main", function_type);
    printf("创建main函数 完成\n");

    // 创建基本块
    LLVMBasicBlockRef entry_block = LLVMAppendBasicBlock(main_func, "entry");
    
    // 创建IR构建器
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMPositionBuilderAtEnd(builder, entry_block);
    
    // 创建字符串常量
    LLVMValueRef hello_str = LLVMBuildGlobalStringPtr(builder, "Hello, World!\n", "hello_str");
    
    // 声明puts函数 - 使用带上下文的类型创建函数
    // 使用LLVMPointerType创建指针类型（接受元素类型和地址空间）
    LLVMTypeRef int8_type = LLVMInt8TypeInContext(context);
    LLVMTypeRef int8_ptr_type = LLVMPointerType(int8_type, 0);
    LLVMTypeRef puts_type = LLVMFunctionType(LLVMInt32TypeInContext(context), 
                                            (LLVMTypeRef[]){int8_ptr_type}, 
                                            1, 0);
    LLVMValueRef puts_func = LLVMAddFunction(module, "puts", puts_type);
    
    // 调用puts函数
    LLVMBuildCall2(builder, puts_type, puts_func, (LLVMValueRef[]){hello_str}, 1, "");
    
    printf("调用puts函数 完成\n");

    // 创建返回值 - 使用带上下文的类型创建函数
    LLVMValueRef ret_val = LLVMConstInt(LLVMInt32TypeInContext(context), 0, 0);
    LLVMBuildRet(builder, ret_val);
    
    printf("创建返回值 完成\n");

    // 验证模块
    char *error = NULL;
    if (LLVMVerifyModule(module, LLVMAbortProcessAction, &error)) {
        fprintf(stderr, "模块验证失败: %s\n", error);
        LLVMDisposeMessage(error);
    }

    printf("验证模块 完成\n");
    
    // 打印IR代码
    printf("生成的LLVM IR:\n");
    LLVMDumpModule(module);
    
    // 保存到文件
    if (LLVMPrintModuleToFile(module, "output.ll", &error)) {
        fprintf(stderr, "保存文件失败: %s\n", error);
        LLVMDisposeMessage(error);
    }
    
    // 清理资源
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);
    
    return 0;
}

/**
# 编译命令
# 使用系统LLVM
clang -o build/ir_hello tests/llvm_ir_hello.c `llvm-config --cflags --ldflags --libs core analysis executionengine target`

# 或者使用brew安装的LLVM（macOS）
clang -o build/ir_hello tests/llvm_ir_hello.c -I/opt/homebrew/opt/llvm/include -L/opt/homebrew/opt/llvm/lib -lLLVM
*/