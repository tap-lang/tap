#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lexer.h"
#include "parser.h"
#include "codegen.h"

// 读取文件内容
static char *read_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "无法打开文件: %s\n", filename);
        return NULL;
    }

    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // 分配内存
    char *buffer = (char *)malloc(file_size + 1);
    if (!buffer) {
        fprintf(stderr, "内存分配失败\n");
        fclose(file);
        return NULL;
    }

    // 读取文件内容
    size_t bytes_read = fread(buffer, 1, file_size, file);
    buffer[bytes_read] = '\0';

    fclose(file);
    return buffer;
}

// 打印用法
static void print_usage() {
    printf("用法: 4yue <源文件>\n");
    printf("选项:\n");
    printf("  -h, --help    显示此帮助信息\n");
    printf("  -o <文件>     指定输出文件\n");
    printf("  -emit-ir      生成LLVM IR代码\n");
    printf("  -emit-obj     生成目标文件\n");
    printf("  -run          编译并运行程序\n");
}

int main(int argc, char *argv[]) {
    // 默认选项
    char *output_file = NULL;
    int emit_ir = 0;
    int emit_obj = 0;
    int run = 0;
    char *input_file = NULL;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "错误: -o 选项需要指定输出文件\n");
                print_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "-emit-ir") == 0) {
            emit_ir = 1;
        } else if (strcmp(argv[i], "-emit-obj") == 0) {
            emit_obj = 1;
        } else if (strcmp(argv[i], "-run") == 0) {
            run = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "错误: 未知选项 %s\n", argv[i]);
            print_usage();
            return 1;
        } else if (!input_file) {
            input_file = argv[i];
        } else {
            fprintf(stderr, "错误: 多个输入文件\n");
            print_usage();
            return 1;
        }
    }

    // 检查输入文件
    if (!input_file) {
        fprintf(stderr, "错误: 未指定输入文件\n");
        print_usage();
        return 1;
    }


    // 读取源文件
    char *source_code = read_file(input_file);
    if (!source_code) {
        return 1;
    }

    // 1. 词法分析
    Lexer *lexer = create_lexer(input_file, source_code);
    if (!lexer) {
        free(source_code);
        return 1;
    }

    // 2. 语法分析
    Parser *parser = create_parser(lexer);
    if (!parser) {
        free_lexer(lexer);
        free(source_code);
        return 1;
    }

    ProgramNode *program = parse_program(parser);

    // 3. 代码生成
    CodeGenContext *codegen_context = create_codegen_context(input_file);
    if (!codegen_context) {
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        free(source_code);
        return 1;
    }

    generate_code(codegen_context, program);

    // 输出生成的LLVM IR
    if (emit_ir) {
        char *ir_file = output_file ? output_file : "output.ll";
        if (write_ir_to_file(codegen_context, ir_file) != 0) {
            fprintf(stderr, "写入IR文件失败\n");
        } else {
            printf("IR代码已写入到 %s\n", ir_file);
        }
    } else if (emit_obj) {  // todo 
        char *obj_file = output_file ? output_file : "output.o";
        if (write_object_to_file(codegen_context, obj_file) != 0) {
            fprintf(stderr, "写入目标文件失败\n");
        } else {
            printf("目标文件已写入到 %s\n", obj_file);
        }
    } else if (run) {
        // 使用LLVM解释器(lli)执行生成的代码，而不是自己实现执行逻辑
        printf("执行程序...\n");
        
        // 生成临时IR文件
        char *temp_ir_file = "temp_output.ll";
        if (write_ir_to_file(codegen_context, temp_ir_file) != 0) {
            fprintf(stderr, "写入临时IR文件失败\n");
        } else {
            // 直接运行lli执行生成的IR代码
            // 我们的修改确保了函数名不会被修改，所以main函数应该是可用的
            int result = system("lli temp_output.ll");
            
            printf("程序执行完毕，返回值: %d\n", WEXITSTATUS(result));
            
            // 删除临时文件
            remove(temp_ir_file);
        }
    } else {
        // 默认行为：生成可执行文件
        char *exe_file = output_file ? output_file : "output";
        
        printf("生成可执行文件...\n");
        
        // 生成临时IR文件
        char *temp_ir_file = "temp_output.ll";
        
        if (write_ir_to_file(codegen_context, temp_ir_file) != 0) {
            fprintf(stderr, "写入临时IR文件失败\n");
        } else {
            // 直接使用clang编译IR文件生成可执行文件，让clang处理整个编译过程
            char clang_command[256];
            snprintf(clang_command, sizeof(clang_command), "clang %s -o %s", temp_ir_file, exe_file);
            
            if (system(clang_command) != 0) {
                fprintf(stderr, "生成可执行文件失败\n");
            } else {
                printf("可执行文件已生成: %s\n", exe_file);
                
                // 设置可执行权限
                chmod(exe_file, 0755);
            }
            
            // 删除临时IR文件
            remove(temp_ir_file);
        }
    }

    // 清理资源
    free_codegen_context(codegen_context);
    free_ast((ASTNode *)program);
    free_parser(parser);
    free_lexer(lexer);
    free(source_code);

    return 0;
}