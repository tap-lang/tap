#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "module.h"
#include "prelude.h"
#include "codegen.h"
#include "run.h"
#include "version.h"

// 打印用法
static void print_usage() {
    printf("用法: 4yue [选项] <源文件>\n");
    printf("      4yue run [选项] <源文件>\n");
    printf("命令:\n");
    printf("  run               编译为本地可执行文件并运行\n");
    printf("选项:\n");
    printf("  -h, --help        显示此帮助信息\n");
    printf("  -o <文件>         指定输出文件\n");
    printf("  -ir               生成LLVM IR代码\n");
    printf("  -emit-obj         生成目标文件\n");
    printf("  -lex              只输出词法分析结果\n");
    printf("  -parse            只输出语法分析结果\n");
    printf("  -run-lli          生成LLVM IR并使用lli运行程序\n");
    printf("  -V, --version     显示版本号\n");
}

static void print_version() {
    printf("4yue version %s (%s)\n", VERSION, GIT_COMMIT_ID);
}

// 全局debug变量，供其他模块使用
int debug = 0;

// 只输出词法分析结果
static void run_lex_only(Lexer *lexer) {
    for (;;) {
        Token *tok = get_next_token(lexer);
        const char *lex = tok->lexeme ? tok->lexeme : "";
        printf("%s:%d:%d\t%s\t'%s'\n", lexer->filename, tok->line, tok->column, TokenNames[tok->type], lex);
        enum TokenType ty = tok->type;
        free_token(tok);
        if (ty == TOKEN_EOF) {
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    
    char *input_file = NULL;       
    char *output_file = NULL;   
    int emit_ir = 0;        
    int emit_obj = 0;
    int run_lli = 0;
    int run_native = 0;
    int lex_only = 0;
    int parse_only = 0;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "错误: -o 选项需要指定输出文件\n");
                print_usage();
                return 1;
            }
        } else if (strcmp(argv[i], "-ir") == 0) {
            emit_ir = 1;
        } else if (strcmp(argv[i], "-emit-obj") == 0) {
            emit_obj = 1;
        } else if (strcmp(argv[i], "-run-lli") == 0) {
            run_lli = 1;
        } else if (strcmp(argv[i], "-lex") == 0) {
            lex_only = 1;
        } else if (strcmp(argv[i], "-parse") == 0) {
            parse_only = 1;
        } else if (strcmp(argv[i], "-debug") == 0) {
            debug = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "错误: 未知选项 %s\n", argv[i]);
            print_usage();
            return 1;
        } else if (strcmp(argv[i], "run") == 0 && !input_file && !run_native) {
            run_native = 1;
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

    if (run_native && (emit_ir || emit_obj || run_lli || lex_only || parse_only)) {
        fprintf(stderr, "错误: run 命令不能与其他输出或运行模式组合使用\n");
        return 1;
    }

    if (debug) {
        printf("调试信息:\n");
    }

    // 1. 词法分析（在 create_lexer 内读入源文件）
    Lexer *lexer = create_lexer(input_file);
    if (!lexer) {
        return 1;
    }

    if (lex_only) {
        run_lex_only(lexer);
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

    if (load_modules(program, input_file, argv[0]) != 0 ||
        load_prelude(program, argv[0]) != 0) {
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
    } else if (run_lli) {
        run_with_lli(codegen_context);
    } else {
        // 默认行为生成可执行文件；run命令会在编译成功后执行它。
        char *exe_file = output_file ? output_file : "output";

        if (debug) printf("生成可执行文件...\n");

        int result = run_native
            ? compile_and_run(codegen_context, exe_file)
            : compile_to_executable(codegen_context, exe_file);

        free_codegen_context(codegen_context);
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return result;
    }

    // 清理资源
    free_codegen_context(codegen_context);
    free_ast((ASTNode *)program);
    free_parser(parser);
    free_lexer(lexer);

    return 0;
}
