#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "lexer.h"
#include "parser.h"
#include "module.h"
#include "prelude.h"
#include "generics.h"
#include "codegen.h"
#include "run.h"
#include "helpers.h"
#include "version.h"

// 打印用法
static void print_usage() {
    printf("用法: tap [选项] <源文件>\n");
    printf("      tap run [选项] <源文件> [-- 程序参数...]\n");
    printf("命令:\n");
    printf("  run               编译为本地可执行文件并运行\n");
    printf("选项:\n");
    printf("  -h, --help        显示此帮助信息\n");
    printf("  -o <文件>         指定输出文件\n");
    printf("  -static           静态链接生成的本地可执行文件\n");
    printf("  -ir               生成LLVM IR代码\n");
    printf("  -emit-obj         生成目标文件\n");
    printf("  -emit-wasm        生成WebAssembly目标文件\n");
    printf("  -lex              只输出词法分析结果\n");
    printf("  -parse            只输出语法分析结果\n");
    printf("  -run-lli          生成LLVM IR并使用lli运行程序\n");
    printf("  -V, --version     显示版本号\n");
}

int main(int argc, char *argv[]) {

#ifdef _WIN32
    // Windows 控制台默认用系统代码页（简体中文为 936），而编译器和源码都按 UTF-8
    // 输出中文，字节被按 GBK 解释就会显示成乱码。这里把控制台切到 UTF-8 代码页。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    char *input_file = NULL;       
    char *output_file = NULL;   
    int emit_ir = 0;        
    int emit_obj = 0;
    int emit_wasm = 0;
    int run_lli = 0;
    int run_native = 0;
    int static_link = 0;
    int lex_only = 0;
    int parse_only = 0;
    int program_argc = 0;
    char **program_argv = NULL;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            program_argc = argc - i - 1;
            program_argv = &argv[i + 1];
            break;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "error: -o requires an output file\n"); // 中文：-o 选项需要指定输出文件
                print_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "-static") == 0) {
            static_link = 1;
        } else if (strcmp(argv[i], "-ir") == 0) {
            emit_ir = 1;
        } else if (strcmp(argv[i], "-emit-obj") == 0) {
            emit_obj = 1;
        } else if (strcmp(argv[i], "-emit-wasm") == 0) {
            emit_wasm = 1;
        } else if (strcmp(argv[i], "-run-lli") == 0) {
            run_lli = 1;
        } else if (strcmp(argv[i], "-lex") == 0) {
            lex_only = 1;
        } else if (strcmp(argv[i], "-parse") == 0) {
            parse_only = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option %s\n", argv[i]); // 中文：未知选项
            print_usage();
            return 1;
        } else if (strcmp(argv[i], "run") == 0 && !input_file && !run_native) {
            run_native = 1;
        } else if (!input_file) {
            input_file = argv[i];
        } else {
            fprintf(stderr, "error: multiple input files\n"); // 中文：多个输入文件
            print_usage();
            return 1;
        }
    }

    // 检查输入文件
    if (!input_file) {
        fprintf(stderr, "error: no input file specified\n"); // 中文：未指定输入文件
        print_usage();
        return 1;
    }

    if (run_native && (emit_ir || emit_obj || emit_wasm || run_lli || lex_only || parse_only)) {
        fprintf(stderr, "error: run cannot be combined with other output or execution modes\n"); // 中文：run 命令不能与其他输出或运行模式组合使用
        return 1;
    }
    if (!run_native && program_argc > 0) {
        fprintf(stderr, "error: program arguments can only be used with run\n"); // 中文：程序参数只能与 run 命令一起使用
        return 1;
    }
    if (static_link && (emit_ir || emit_obj || emit_wasm || run_lli || lex_only || parse_only)) {
        fprintf(stderr, "error: -static can only be used when generating or running a native executable\n"); // 中文：-static 只能用于生成或运行本地可执行文件
        return 1;
    }

    // Runtime 库只解析一次，确保本地链接和 lli 使用一致的 ABI。
    configure_runtime(argv[0]);

    // 1. 词法分析（在 create_lexer 内读入源文件）
    Lexer *lexer = create_lexer(input_file);
    if (!lexer) {
        return 1;
    }

    if (lex_only) {
        print_lexer(lexer);
        free_lexer(lexer);
        return 0;
    }

    // 2. 语法分析
    Parser *parser = create_parser(lexer);
    if (!parser) {
        free_lexer(lexer);
        return 1;
    }

    ProgramNode *program = parse_program(parser);

    if (parse_only) {
        print_ast(program);
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return 0;
    }

    if (load_modules(program, input_file, argv[0]) != 0 
        || load_prelude(program, argv[0]) != 0
        || specialize_generics(program) != 0
    ) {
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return 1;
    }

    // 3. 代码生成
    CodeGenContext *codegen_context = create_codegen_context(input_file);
    if (!codegen_context) {
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return 1;
    }

    generate_code(codegen_context, program);

    int result = 0;

    // 输出生成的LLVM IR
    if (emit_ir) {
        char *ir_file = output_file ? output_file : "output.ll";
        if (write_ir_to_file(codegen_context, ir_file) != 0) {
            fprintf(stderr, "failed to write IR file\n"); // 中文：写入 IR 文件失败
            result = 1;
        } else {
            printf("IR代码已写入到 %s\n", ir_file);
        }
    } else if (emit_obj) {
        char *obj_file = output_file ? output_file : "output.o";
        if (write_object_to_file(codegen_context, obj_file) != 0) {
            fprintf(stderr, "failed to write object file\n"); // 中文：写入目标文件失败
            result = 1;
        } else {
            printf("目标文件已写入到 %s\n", obj_file);
        }
    } else if (emit_wasm) {
        char *wasm_file = output_file ? output_file : "output.wasm";
        if (write_wasm_to_file(codegen_context, wasm_file) != 0) {
            fprintf(stderr, "failed to write WebAssembly file\n"); // 中文：写入 WebAssembly 文件失败
            result = 1;
        } else {
            printf("WebAssembly目标文件已写入到 %s\n", wasm_file);
        }
    } else if (run_lli) {
        result = run_with_lli(codegen_context);
    } else {
        // 默认行为生成可执行文件；run命令会在编译成功后执行它。
        char *default_exe_file = NULL;
        char *exe_file = output_file;
        if (!exe_file && !run_native) {
            default_exe_file = get_basename_no_ext(input_file);
            exe_file = default_exe_file;
        }

        if (!run_native && !exe_file) {
            result = 1;
        } else {
            result = run_native
                ? compile_and_run(codegen_context, exe_file, program_argc, program_argv, static_link)
                : compile_to_executable(codegen_context, exe_file, static_link);
        }
        free(default_exe_file);
    }

    // 清理资源
    free_codegen_context(codegen_context);
    free_ast((ASTNode *)program);
    free_parser(parser);
    free_lexer(lexer);

    return result;
}
