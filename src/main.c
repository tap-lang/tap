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
#include "tagged.h"
#include "generics.h"
#include "sema.h"
#include "borrow.h"
#include "codegen.h"
#include "run.h"
#include "helpers.h"
#include "version.h"

// 打印用法
static void print_usage() {
    printf("用法: tap [选项] <源文件>\n");
    printf("      tap run [选项] [源文件] [-- 程序参数...]\n");
    printf("      tap check [选项] [源文件]\n");
    printf("      tap build\n");
    printf("      tap new <名字>\n");
    printf("命令:\n");
    printf("  run               编译为本地可执行文件并运行；不给文件时编译并运行当前包\n");
    printf("  check             只做编译期检查（词法/语法/类型/借用），不产出、不链接\n");
    printf("  build             编译当前包，产物放到 <包根>/build/<name>\n");
    printf("  new <名字>        新建项目（转发给 tapm new）\n");
    printf("选项:\n");
    printf("  -h, --help        显示此帮助信息\n");
    printf("  -o <文件>         指定输出文件\n");
    printf("  -static           静态链接生成的本地可执行文件\n");
    printf("  -ir               生成LLVM IR代码\n");
    printf("  -emit-obj         生成目标文件\n");
    printf("  -emit-wasm        生成WebAssembly目标文件\n");
    printf("  -l<名字>          链接库；可重复，顺序即命令行顺序\n");
    printf("  -L<目录>          追加库搜索路径；可重复\n");
    printf("  -Wl,<参数>        把参数透传给链接器，逗号分隔多个\n");
    printf("  -lex              只输出词法分析结果\n");
    printf("  -parse            只输出语法分析结果\n");
    printf("  -g                生成调试信息（DWARF 行号表），可用 gdb / lldb 调试\n");
    printf("  -run-lli          生成LLVM IR并使用lli运行程序\n");
    printf("  -v, --verbose     把实际执行的链接/运行命令回显到 stderr\n");
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
    int debug_info = 0;
    int verbose = 0;
    int build_package = 0;
    int check_only = 0;
    int program_argc = 0;
    char **program_argv = NULL;

    // 链接参数按出现顺序收集，最终原样交给 link_object_file()。
    LinkOptions link_options;
    link_options_init(&link_options);

    // `new` 是包管理器的活，tap 只转发：`tap new demo` → `tapm new demo`。
    // 放在参数解析之前，`new` 之后的参数原样透传，不被 tap 自己的选项规则干扰
    // （项目名不该因为长得像选项就被拒掉）。
    if (argc >= 2 && strcmp(argv[1], "new") == 0) {
        return run_tapm(argv[0], argc - 1, &argv[1]);
    }

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
        } else if (strcmp(argv[i], "-g") == 0) {
            debug_info = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-l") == 0) {
            // `-l foo` 和 `-lfoo` 两种写法都认，和 cc 的惯例一致。
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -l requires a library name\n"); // 中文：-l 选项需要指定库名
                print_usage();
                return 1;
            }
            if (link_options_add(&link_options, LINK_ARG_LIBRARY, argv[++i]) != 0) return 1;
        } else if (strncmp(argv[i], "-l", 2) == 0) {
            if (link_options_add(&link_options, LINK_ARG_LIBRARY, argv[i] + 2) != 0) return 1;
        } else if (strcmp(argv[i], "-L") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -L requires a directory\n"); // 中文：-L 选项需要指定目录
                print_usage();
                return 1;
            }
            if (link_options_add(&link_options, LINK_ARG_SEARCH_DIR, argv[++i]) != 0) return 1;
        } else if (strncmp(argv[i], "-L", 2) == 0) {
            if (link_options_add(&link_options, LINK_ARG_SEARCH_DIR, argv[i] + 2) != 0) return 1;
        } else if (strncmp(argv[i], "-Wl,", 4) == 0) {
            if (!argv[i][4]) {
                fprintf(stderr, "error: -Wl, requires a linker argument\n"); // 中文：-Wl, 需要指定链接器参数
                print_usage();
                return 1;
            }
            if (link_options_add(&link_options, LINK_ARG_RAW, argv[i] + 4) != 0) return 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option %s\n", argv[i]); // 中文：未知选项
            print_usage();
            return 1;
        } else if (strcmp(argv[i], "run") == 0 && !input_file && !run_native &&
                   !build_package && !check_only) {
            run_native = 1;
        } else if (strcmp(argv[i], "check") == 0 && !input_file && !run_native &&
                   !build_package && !check_only) {
            check_only = 1;
        } else if (strcmp(argv[i], "build") == 0 && !input_file && !build_package &&
                   !check_only) {
            build_package = 1;
        } else if (!input_file &&
                   (strcmp(argv[i], "run") == 0 || strcmp(argv[i], "check") == 0 ||
                    strcmp(argv[i], "build") == 0)) {
            // 走到这里说明上面三个分支都没接住 —— 也就是已经给过命令了。
            // 不拦的话 `tap check run` 会把 "run" 当成文件名去打开。
            fprintf(stderr,
                "error: only one command can be given ('run', 'check', 'build')\n"); // 中文：只能给一个命令
            print_usage();
            return 1;
        } else if (!input_file) {
            input_file = argv[i];
        } else {
            fprintf(stderr, "error: multiple input files\n"); // 中文：多个输入文件
            print_usage();
            return 1;
        }
    }

    // `build` 是包级命令，不接受文件参数。
    if (build_package && input_file) {
        fprintf(stderr,
            "error: `build` takes no file argument; use `tap <file> -o <out>` for single-file builds\n"); // 中文：build 不接受文件参数
        return 1;
    }

    // 包模式：`tap build`，或 `tap run` / `tap check` 不带文件时，从当前目录向上找
    // package.toml，按里面的 entry 编译，产物放到 <包根>/build/<name>。
    // 只在没给文件参数时才走这条路，`tap run main.tp` 的行为完全不变。
    if (build_package || (run_native && !input_file) || (check_only && !input_file)) {
        if (output_file) {
            fprintf(stderr,
                "error: -o cannot be combined with package mode\n"); // 中文：包模式不能和 -o 组合
            return 1;
        }

        if (check_only) {
            // 检查不该有副作用：用只读版解析，不创建 build/ 目录。
            char *package_source = NULL;
            if (resolve_package_entry(".", &package_source, NULL) != 0) {
                return 1;
            }
            input_file = package_source;
        } else {
            char *package_source = NULL;
            char *package_output = NULL;
            if (resolve_package_target(".", "build", &package_source, &package_output) != 0) {
                return 1;
            }
            input_file = package_source;
            output_file = package_output;
            // `build` 只编译不运行；`run` 保持 run_native，编译完接着跑。
            if (build_package) run_native = 0;
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
    // DWARF 是本地目标的格式，wasm 的调试信息是另一套（source map / DWARF-in-wasm），
    // 现在没实现，直接拒掉而不是生成一份没人认的元数据。
    if (debug_info && emit_wasm) {
        fprintf(stderr, "error: -g cannot be combined with -emit-wasm\n"); // 中文：-g 不能与 -emit-wasm 组合
        return 1;
    }
    // check 只检查不产出，所以任何「要写出东西」或「要执行」的选项都不能和它同用。
    // `-v` 不算：它只影响回显，检查过程照样能打外部命令。
    if (check_only &&
        (emit_ir || emit_obj || emit_wasm || run_lli || lex_only || parse_only ||
         static_link || output_file)) {
        fprintf(stderr,
            "error: check cannot be combined with output or execution modes\n"); // 中文：check 不能与其他输出或执行模式组合
        return 1;
    }

    // Runtime 库只解析一次，确保本地链接和 lli 使用一致的 ABI。
    configure_runtime(argv[0]);

    // `-v` 只影响外部命令的回显，和任何输出模式都不冲突。放在这里统一打开，
    // 后面的链接 / lli / 运行三处都读这个开关。
    set_verbose_commands(verbose);

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

    // 语法错误是「恢复后继续解析」的，所以 parse_program 会正常返回一棵**残缺**的 AST。
    // 这时候不能往下走：后续阶段看到的是少了语句的程序，只会报出一串假错误。
    // `-parse` 例外 —— 它本来就是给人看 AST 的，把残缺的树打出来反而有助于排查，
    // 只是退出码仍然要给 1。
    int syntax_errors = parser_had_error(parser);

    if (parse_only) {
        print_ast(program);
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return syntax_errors ? 1 : 0;
    }

    if (syntax_errors) {
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return 1;
    }

    // TAP_NO_SEMA=1 跳过语义分析，用于对照实验：确认 Codegen 的缓存命中确实来自 Sema，
    // 而不是 Codegen 自己的缓存。排查「某个类型问题是不是 Sema 引入的」时也用它。
    const char *skip_sema = getenv("TAP_NO_SEMA");

    if (load_modules(program, input_file, argv[0]) != 0 
        || load_prelude(program, argv[0]) != 0
        || lower_payload_enums(program) != 0
        || specialize_generics(program) != 0
        || (!(skip_sema && skip_sema[0]) && analyze_program(program) != 0)
    ) {
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return 1;
    }

    // 借用检查紧跟在 Sema 之后：它只看 AST 和类型标注，不依赖 Codegen 的推断结果，
    // 所以放在生成任何 IR 之前就能给出诊断。TAP_NO_BORROW=1 可以跳过，
    // 用来确认「某段代码编不过」到底是借用检查还是别的阶段拦下的。
    //
    // 借用检查会**报完继续查**（一个文件里的多处借用错误一次全报），所以它返回
    // 状态而不是直接退出；报过错之后 AST 的状态不再可信，不能继续进 Codegen。
    const char *skip_borrow = getenv("TAP_NO_BORROW");
    if (!(skip_borrow && skip_borrow[0]) && check_borrows(program)) {
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

    set_codegen_debug_info(codegen_context, debug_info);
    generate_code(codegen_context, program);

    // 类型检查长在 Codegen 里，现在也是「报完继续查」：同一个文件里的多处类型错误
    // 会一次全报。报过错之后 IR 只是勉强能过验证，绝不能拿去写文件、链接或运行。
    if (codegen_had_error(codegen_context)) {
        link_options_free(&link_options);
        free_codegen_context(codegen_context);
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return 1;
    }

    // `check` 到此为止：诊断已经全部产出（类型检查在 Codegen 里，IR 校验也在
    // generate_code 内部跑过），剩下的写文件、链接、运行一律跳过。
    // 这就是 `-ir -o /dev/null` 想干的事，只是不写文件、不打「已写入」的提示。
    if (check_only) {
        link_options_free(&link_options);
        free_codegen_context(codegen_context);
        free_ast((ASTNode *)program);
        free_parser(parser);
        free_lexer(lexer);
        return 0;
    }

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
                ? compile_and_run(codegen_context, exe_file, program_argc, program_argv,
                                  static_link, &link_options)
                : compile_to_executable(codegen_context, exe_file, static_link, &link_options);
        }
        free(default_exe_file);
    }

    // 清理资源
    link_options_free(&link_options);
    free_codegen_context(codegen_context);
    free_ast((ASTNode *)program);
    free_parser(parser);
    free_lexer(lexer);

    return result;
}
