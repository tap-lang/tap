#include "module.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MODULE_PATH_MAX _MAX_PATH
#else
#define MODULE_PATH_MAX PATH_MAX
#endif

#include "lexer.h"
#include "parser.h"

typedef struct ModuleExport {
    char *name;
    char *symbol;
    struct ModuleExport *next;
} ModuleExport;

typedef struct LoadedModule {
    char *path;
    char *prefix;
    ModuleExport *exports;
    struct LoadedModule *next;
} LoadedModule;

typedef struct ImportBinding {
    ImportNode *import_node;
    LoadedModule *module;
    struct ImportBinding *next;
} ImportBinding;

typedef struct {
    ProgramNode *functions;
    LoadedModule *modules;
    const char *compiler_path;
    const char *root_path;
    unsigned next_module_id;
} ModuleContext;

static char *copy_string(const char *value) {
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (!copy) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    memcpy(copy, value, size);
    return copy;
}

static int file_exists(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static char *join_path(const char *directory, const char *suffix) {
    size_t size = strlen(directory) + strlen(suffix) + 2;
    char *path = malloc(size);
    if (!path) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    snprintf(path, size, "%s/%s", directory, suffix);
    return path;
}

static const char *last_separator(const char *path) {
    const char *separator = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!separator || (backslash && backslash > separator)) separator = backslash;
#endif
    return separator;
}

static char *path_directory(const char *path) {
    const char *separator = last_separator(path);
    if (!separator) return copy_string(".");

    size_t length = (size_t)(separator - path);
    if (length == 0) length = 1;
    char *directory = malloc(length + 1);
    if (!directory) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    memcpy(directory, path, length);
    directory[length] = '\0';
    return directory;
}

static char *canonical_path(const char *path) {
    char resolved[MODULE_PATH_MAX];
#ifdef _WIN32
    if (!_fullpath(resolved, path, MODULE_PATH_MAX)) return NULL;
#else
    if (!realpath(path, resolved)) return NULL;
#endif
    return copy_string(resolved);
}

static char *compiler_directory(const char *compiler_path) {
    char *resolved = canonical_path(compiler_path);
    const char *path = resolved ? resolved : compiler_path;
    char *directory = path_directory(path);
    free(resolved);
    return directory;
}

static char *module_relative_path(const char *module_name) {
    size_t length = strlen(module_name);
    char *path = malloc(length + 4);
    if (!path) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }

    for (size_t index = 0; index < length; index++) {
        path[index] = module_name[index] == '.' ? '/' : module_name[index];
    }
    memcpy(path + length, ".tp", 4);
    return path;
}

static char *existing_canonical_path(char *candidate) {
    if (!candidate) return NULL;
    if (!file_exists(candidate)) {
        free(candidate);
        return NULL;
    }

    char *canonical = canonical_path(candidate);
    free(candidate);
    return canonical;
}

static int is_std_module(const char *module_name) {
    return strncmp(module_name, "std.", 4) == 0;
}

static char *resolve_module_path(
    const ImportNode *import_node, const char *compiler_path) {
    char *relative = module_relative_path(import_node->module_name);
    if (!relative) return NULL;

    if (is_std_module(import_node->module_name)) {
        const char *std_path = getenv("4YUE_STD_PATH");
        if (std_path && std_path[0] != '\0') {
            char *std_relative = module_relative_path(import_node->module_name + 4);
            char *candidate = std_relative ? join_path(std_path, std_relative) : NULL;
            free(std_relative);
            char *resolved = existing_canonical_path(candidate);
            if (resolved) {
                free(relative);
                return resolved;
            }
        }
    }

    char *importer_directory = path_directory(import_node->filename);
    char *candidate = importer_directory ? join_path(importer_directory, relative) : NULL;
    free(importer_directory);
    char *resolved = existing_canonical_path(candidate);
    if (resolved) {
        free(relative);
        return resolved;
    }

    const char *module_path = getenv("4YUE_MODULE_PATH");
    if (module_path && module_path[0] != '\0') {
        resolved = existing_canonical_path(join_path(module_path, relative));
        if (resolved) {
            free(relative);
            return resolved;
        }
    }

    resolved = existing_canonical_path(copy_string(relative));
    if (resolved) {
        free(relative);
        return resolved;
    }

    char *directory = compiler_directory(compiler_path);
    if (directory) {
        candidate = join_path(directory, "..");
        char *source_root = candidate;
        candidate = source_root ? join_path(source_root, relative) : NULL;
        free(source_root);
        resolved = existing_canonical_path(candidate);
        if (resolved) {
            free(directory);
            free(relative);
            return resolved;
        }

        candidate = join_path(directory, "../share/4yue");
        char *install_root = candidate;
        candidate = install_root ? join_path(install_root, relative) : NULL;
        free(install_root);
        resolved = existing_canonical_path(candidate);
        free(directory);
        if (resolved) {
            free(relative);
            return resolved;
        }
    }

    free(relative);
    return NULL;
}

static FunctionNode *find_function(ASTNode *functions, const char *name) {
    for (ASTNode *node = functions; node; node = node->next) {
        if (node->type == NODE_FUNCTION &&
            strcmp(((FunctionNode *)node)->name, name) == 0) {
            return (FunctionNode *)node;
        }
    }
    return NULL;
}

static FunctionNode *find_internal_duplicate(
    ASTNode *functions, FunctionNode **previous) {
    for (ASTNode *node = functions; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        FunctionNode *duplicate = find_function(node->next, ((FunctionNode *)node)->name);
        if (duplicate) {
            *previous = (FunctionNode *)node;
            return duplicate;
        }
    }
    return NULL;
}

static int report_duplicate(FunctionNode *duplicate, FunctionNode *previous) {
    fprintf(stderr, "%s:%d:%d: error: duplicate function definition '%s'\n", // 中文：重复函数定义
        duplicate->filename, duplicate->line, duplicate->column, duplicate->name);
    if (previous) {
        fprintf(stderr, "%s:%d:%d: note: previous definition is here\n", // 中文：此处已有定义
            previous->filename, previous->line, previous->column);
    }
    return 1;
}

static LoadedModule *find_loaded_module(LoadedModule *modules, const char *path) {
    for (; modules; modules = modules->next) {
        if (strcmp(modules->path, path) == 0) return modules;
    }
    return NULL;
}

static ModuleExport *find_export(LoadedModule *module, const char *name) {
    for (ModuleExport *export = module ? module->exports : NULL;
         export; export = export->next) {
        if (strcmp(export->name, name) == 0) return export;
    }
    return NULL;
}

static LoadedModule *create_loaded_module(ModuleContext *context, const char *path) {
    LoadedModule *module = calloc(1, sizeof(LoadedModule));
    if (!module) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }

    char prefix[64];
    snprintf(prefix, sizeof(prefix), "__4yue_module_%u", context->next_module_id++);
    module->path = copy_string(path);
    module->prefix = copy_string(prefix);
    if (!module->path || !module->prefix) {
        free(module->path);
        free(module->prefix);
        free(module);
        return NULL;
    }
    module->next = context->modules;
    context->modules = module;
    return module;
}

static void free_modules(LoadedModule *modules) {
    while (modules) {
        LoadedModule *next = modules->next;
        ModuleExport *export = modules->exports;
        while (export) {
            ModuleExport *export_next = export->next;
            free(export->name);
            free(export->symbol);
            free(export);
            export = export_next;
        }
        free(modules->path);
        free(modules->prefix);
        free(modules);
        modules = next;
    }
}

static char *create_symbol(const char *prefix, const char *name) {
    size_t size = strlen(prefix) + strlen(name) + 2;
    char *symbol = malloc(size);
    if (!symbol) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    snprintf(symbol, size, "%s.%s", prefix, name);
    return symbol;
}

static int register_exports(LoadedModule *module, ProgramNode *program) {
    FunctionNode *previous = NULL;
    FunctionNode *duplicate = find_internal_duplicate(program->functions, &previous);
    if (duplicate) return report_duplicate(duplicate, previous);

    ModuleExport **tail = &module->exports;
    for (ASTNode *node = program->functions; node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        ModuleExport *export = calloc(1, sizeof(ModuleExport));
        if (!export) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return 1;
        }
        export->name = copy_string(function->name);
        export->symbol = create_symbol(module->prefix, function->name);
        if (!export->name || !export->symbol) {
            free(export->name);
            free(export->symbol);
            free(export);
            return 1;
        }

        free(function->name);
        function->name = copy_string(export->symbol);
        if (!function->name) {
            free(export->name);
            free(export->symbol);
            free(export);
            return 1;
        }
        *tail = export;
        tail = &export->next;
    }
    return 0;
}

static ImportBinding *find_binding(ImportBinding *bindings, const char *alias) {
    for (; bindings; bindings = bindings->next) {
        if (strcmp(bindings->import_node->alias, alias) == 0) return bindings;
    }
    return NULL;
}

static int add_binding(
    ImportBinding **bindings, ImportNode *import_node, LoadedModule *module) {
    ImportBinding *existing = find_binding(*bindings, import_node->alias);
    if (existing) {
        if (existing->module == module) return 0;
        fprintf(stderr, "%s:%d:%d: error: namespace '%s' is already used for module '%s'\n", // 中文：名称空间已用于模块
            import_node->filename, import_node->line, import_node->column,
            import_node->alias, existing->import_node->module_name);
        fprintf(stderr, "%s:%d:%d: note: namespace was first imported here\n", // 中文：名称空间首次在此导入
            existing->import_node->filename, existing->import_node->line,
            existing->import_node->column);
        return 1;
    }

    ImportBinding *binding = malloc(sizeof(ImportBinding));
    if (!binding) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    binding->import_node = import_node;
    binding->module = module;
    binding->next = *bindings;
    *bindings = binding;
    return 0;
}

static void free_bindings(ImportBinding *bindings) {
    while (bindings) {
        ImportBinding *next = bindings->next;
        free(bindings);
        bindings = next;
    }
}

static void append_functions(ProgramNode *destination, ProgramNode *source) {
    if (!source->functions) return;
    if (!destination->functions) {
        destination->functions = source->functions;
    } else {
        ASTNode *tail = destination->functions;
        while (tail->next) tail = tail->next;
        tail->next = source->functions;
    }
    source->functions = NULL;
}

static int load_module(
    ModuleContext *context, const char *path, LoadedModule **result_module);

static int build_bindings(
    ModuleContext *context, ProgramNode *program, ImportBinding **bindings) {
    for (ASTNode *node = program->imports; node; node = node->next) {
        ImportNode *import_node = (ImportNode *)node;
        char *path = resolve_module_path(import_node, context->compiler_path);
        if (!path) {
            fprintf(stderr, "%s:%d:%d: error: module '%s' not found\n", // 中文：找不到模块
                import_node->filename, import_node->line, import_node->column,
                import_node->module_name);
            return 1;
        }
        if (context->root_path && strcmp(path, context->root_path) == 0) {
            fprintf(stderr, "%s:%d:%d: error: entry file cannot import itself\n", // 中文：入口文件不能导入自身
                import_node->filename, import_node->line, import_node->column);
            free(path);
            return 1;
        }

        LoadedModule *module = NULL;
        int result = load_module(context, path, &module);
        free(path);
        if (result != 0) return result;
        if (add_binding(bindings, import_node, module) != 0) return 1;
    }
    return 0;
}

static int rewrite_expression(
    ASTNode *expression, LoadedModule *current_module, ImportBinding *bindings);

static int rewrite_statement_list(
    ASTNode *statements, LoadedModule *current_module, ImportBinding *bindings) {
    for (ASTNode *statement = statements; statement; statement = statement->next) {
        int result = 0;
        switch (statement->type) {
            case NODE_VAR_DECL:
                result = rewrite_expression(
                    ((VarDeclNode *)statement)->expression, current_module, bindings);
                break;
            case NODE_ASSIGNMENT:
                result = rewrite_expression(
                    ((AssignmentNode *)statement)->expression, current_module, bindings);
                break;
            case NODE_RETURN:
                result = rewrite_expression(
                    ((ReturnNode *)statement)->expression, current_module, bindings);
                break;
            case NODE_PRINT:
                for (ASTNode *argument = ((PrintNode *)statement)->arguments;
                     argument && result == 0; argument = argument->next) {
                    result = rewrite_expression(argument, current_module, bindings);
                }
                break;
            case NODE_FUNCTION_CALL:
                result = rewrite_expression(statement, current_module, bindings);
                break;
            case NODE_IF_STATEMENT: {
                IfStatementNode *if_node = (IfStatementNode *)statement;
                result = rewrite_expression(if_node->condition, current_module, bindings);
                if (result == 0) {
                    result = rewrite_statement_list(
                        if_node->consequence, current_module, bindings);
                }
                if (result == 0) {
                    result = rewrite_statement_list(
                        if_node->alternative, current_module, bindings);
                }
                break;
            }
            case NODE_FOR_STATEMENT: {
                ForStatementNode *for_node = (ForStatementNode *)statement;
                if (for_node->initializer) {
                    result = rewrite_statement_list(
                        for_node->initializer, current_module, bindings);
                }
                if (result == 0) {
                    result = rewrite_expression(
                        for_node->condition, current_module, bindings);
                }
                if (result == 0 && for_node->update) {
                    result = rewrite_statement_list(
                        for_node->update, current_module, bindings);
                }
                if (result == 0) {
                    result = rewrite_statement_list(
                        for_node->body, current_module, bindings);
                }
                break;
            }
            default:
                break;
        }
        if (result != 0) return result;
    }
    return 0;
}

static int rewrite_call(
    FunctionCallNode *call, LoadedModule *current_module, ImportBinding *bindings) {
    // 内建字符串方法不是模块函数，不参与模块导出名称重写。
    if (strcmp(call->name, "__4yue_builtin_string_len") == 0) return 0;

    char *dot = strchr(call->name, '.');
    ModuleExport *export = NULL;

    if (dot) {
        size_t alias_length = (size_t)(dot - call->name);
        char *alias = malloc(alias_length + 1);
        if (!alias) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return 1;
        }
        memcpy(alias, call->name, alias_length);
        alias[alias_length] = '\0';

        ImportBinding *binding = find_binding(bindings, alias);
        if (!binding) {
            // 没有同名导入时，将 name.len() 留给后端按字符串方法解析。
            if (!strchr(dot + 1, '.') && strcmp(dot + 1, "len") == 0) {
                free(alias);
                return 0;
            }
            fprintf(stderr, "%s:%d:%d: error: namespace '%s' was not imported\n", // 中文：未导入名称空间
                call->filename, call->line, call->column, alias);
            free(alias);
            return 1;
        }
        export = find_export(binding->module, dot + 1);
        if (!export) {
            fprintf(stderr, "%s:%d:%d: error: module '%s' has no function '%s'\n", // 中文：模块中没有函数
                call->filename, call->line, call->column,
                binding->import_node->module_name, dot + 1);
            free(alias);
            return 1;
        }
        free(alias);
    } else {
        export = find_export(current_module, call->name);
    }

    if (export) {
        char *resolved = copy_string(export->symbol);
        if (!resolved) return 1;
        free(call->name);
        call->name = resolved;
    }
    return 0;
}

static int rewrite_expression(
    ASTNode *expression, LoadedModule *current_module, ImportBinding *bindings) {
    if (!expression) return 0;

    switch (expression->type) {
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            int result = rewrite_expression(binary->left, current_module, bindings);
            return result == 0
                ? rewrite_expression(binary->right, current_module, bindings)
                : result;
        }
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            for (ASTNode *argument = call->arguments; argument; argument = argument->next) {
                int result = rewrite_expression(argument, current_module, bindings);
                if (result != 0) return result;
            }
            return rewrite_call(call, current_module, bindings);
        }
        default:
            return 0;
    }
}

static int rewrite_functions(
    ProgramNode *program, LoadedModule *current_module, ImportBinding *bindings) {
    for (ASTNode *node = program->functions; node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        int result = rewrite_statement_list(function->body, current_module, bindings);
        if (result != 0) return result;
    }
    return 0;
}

static int load_module(
    ModuleContext *context, const char *path, LoadedModule **result_module) {
    LoadedModule *module = find_loaded_module(context->modules, path);
    if (module) {
        *result_module = module;
        return 0;
    }

    module = create_loaded_module(context, path);
    if (!module) return 1;
    *result_module = module;

    Lexer *lexer = create_lexer(path);
    if (!lexer) return 1;
    Parser *parser = create_parser(lexer);
    ProgramNode *program = parse_program(parser);

    // Register exports before descending so cycles can resolve each side's symbols.
    int result = register_exports(module, program);
    ImportBinding *bindings = NULL;
    if (result == 0) result = build_bindings(context, program, &bindings);
    if (result == 0) result = rewrite_functions(program, module, bindings);
    if (result == 0) {
        append_functions(context->functions, program);
    }

    free_bindings(bindings);
    free_ast((ASTNode *)program);
    free_parser(parser);
    free_lexer(lexer);
    return result;
}

int load_modules(ProgramNode *program, const char *input_file, const char *compiler_path) {
    FunctionNode *previous = NULL;
    FunctionNode *duplicate = find_internal_duplicate(program->functions, &previous);
    if (duplicate) return report_duplicate(duplicate, previous);

    ProgramNode *module_functions = create_program();
    char *root_path = canonical_path(input_file);
    ModuleContext context = {
        .functions = module_functions,
        .modules = NULL,
        .compiler_path = compiler_path,
        .root_path = root_path,
        .next_module_id = 0
    };

    ImportBinding *bindings = NULL;
    int result = build_bindings(&context, program, &bindings);
    if (result == 0) result = rewrite_functions(program, NULL, bindings);
    if (result == 0) append_functions(program, module_functions);

    free_bindings(bindings);
    free_modules(context.modules);
    free(root_path);
    free_ast((ASTNode *)module_functions);
    return result;
}
