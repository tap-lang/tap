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
#include "helpers.h"

typedef struct ModuleExport {
    char *name;      // 源语言里写的名字
    char *symbol;    // 改写后的内部符号名；extern 和 pub 类型与 name 相同
    int is_constant;
    int is_type;     // struct / enum 声明
    int is_pub;      // 只有 pub 的能被其他模块按 `alias.name` 访问
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

// 同上，但允许 value 为 NULL —— 类型重写里「不需要改名」就是 NULL，不是错误。
static char *copy_string_or_null(const char *value) {
    return value ? copy_string(value) : NULL;
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

// 把点分模块名转成相对路径，并接上给定的源文件后缀。
static char *module_relative_path(const char *module_name, const char *extension) {
    size_t length = strlen(module_name);
    size_t extension_length = strlen(extension);
    char *path = malloc(length + extension_length + 1);
    if (!path) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }

    for (size_t index = 0; index < length; index++) {
        path[index] = module_name[index] == '.' ? '/' : module_name[index];
    }
    memcpy(path + length, extension, extension_length + 1);
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

static char *resolve_module_path_with_extension(
    const ImportNode *import_node, const char *compiler_path,
    const char *extension) {
    char *relative = module_relative_path(import_node->module_name, extension);
    if (!relative) return NULL;

    if (is_std_module(import_node->module_name)) {
        const char *std_path = getenv("TAP_STD_PATH");
        if (std_path && std_path[0] != '\0') {
            char *std_relative =
                module_relative_path(import_node->module_name + 4, extension);
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

    const char *module_path = getenv("TAP_MODULE_PATH");
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

        candidate = join_path(directory, "../share/tap");
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

// 源文件后缀同时支持 .tp 和 .tap。先找 .tp：同一目录下两者同名时以 .tp 为准，
// 这样给已有模块补一个 .tap 副本不会悄悄改变解析结果。
static char *resolve_module_path(
    const ImportNode *import_node, const char *compiler_path) {
    char *resolved = resolve_module_path_with_extension(
        import_node, compiler_path, ".tp");
    if (resolved) return resolved;
    return resolve_module_path_with_extension(
        import_node, compiler_path, ".tap");
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
    print_diagnostic(stderr, "error", duplicate->filename, duplicate->line, duplicate->column,
                     "duplicate function definition '%s'", duplicate->name); // 中文：重复函数定义
    if (previous) {
        print_diagnostic(stderr, "note", previous->filename, previous->line, previous->column,
                         "previous definition is here"); // 中文：此处已有定义
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

// 外部访问（`alias.name`）只认 pub 的顶层声明。模块内部的裸名引用走 find_export，
// 那里不看 pub —— 本模块自己的东西当然都能用。
static ModuleExport *find_public_export(LoadedModule *module, const char *name) {
    ModuleExport *export = find_export(module, name);
    return export && export->is_pub ? export : NULL;
}

// 只在本模块导出的**常量**里找。表达式里的裸名只有常量才可能合法出现：函数名
// 只能出现在调用位置（由 rewrite_call 处理），类型名走 NODE_VAR_TYPE。把函数名也
// 算进来会误伤同名局部变量，例如 `with_capacity(capacity: uint)` 里的参数 capacity。
static ModuleExport *find_constant_export(LoadedModule *module, const char *name) {
    for (ModuleExport *export = module ? module->exports : NULL;
         export; export = export->next) {
        if (export->is_constant && strcmp(export->name, name) == 0) return export;
    }
    return NULL;
}

// 在模块导出的**类型**里按原名找。类型重写用它把非 pub 类型换成模块私有符号。
static ModuleExport *find_type_export(LoadedModule *module, const char *name) {
    for (ModuleExport *export = module ? module->exports : NULL;
         export; export = export->next) {
        if (export->is_type && strcmp(export->name, name) == 0) return export;
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
    snprintf(prefix, sizeof(prefix), "__tap_module_%u", context->next_module_id++);
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

// 登记一个顶层类型声明。pub 类型保持裸名（其他模块直接写类型名就能引用），
// 非 pub 类型改名成模块私有符号。改名结果通过 renamed_out 交回调用方写回定义处；
// pub 类型不需要改名，renamed_out 置 NULL。
static int register_type_export(
    LoadedModule *module, const char *name, int is_pub,
    ModuleExport ***tail, char **renamed_out) {
    ModuleExport *export = calloc(1, sizeof(ModuleExport));
    if (!export) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    export->name = copy_string(name);
    export->symbol = is_pub ? copy_string(name) : create_symbol(module->prefix, name);
    export->is_type = 1;
    export->is_pub = is_pub;
    if (!export->name || !export->symbol) {
        free(export->name);
        free(export->symbol);
        free(export);
        return 1;
    }

    **tail = export;
    *tail = &export->next;
    *renamed_out = is_pub ? NULL : export->symbol;
    return 0;
}

// 把模块的顶层符号登记成「原名 → 内部符号名」，并就地改写定义处的名字。
//
//   - 常量、普通函数：一律改名成 `__tap_module_N.name`。**非 pub 的也要改名** ——
//     否则它仍然占着全局名字，会和其他模块的同名符号撞。
//   - `extern` 函数：符号名就是 C 符号名，加前缀会链接不到，所以保持原名。
//   - 类型：pub 的保持裸名，非 pub 的改名成模块私有。两个模块因此可以各自拥有
//     同名私有类型，互不干扰。
// 只有 pub 的会被 find_public_export 放行给其他模块。
static int register_exports(LoadedModule *module, ProgramNode *program) {
    FunctionNode *previous = NULL;
    FunctionNode *duplicate = find_internal_duplicate(program->functions, &previous);
    if (duplicate) return report_duplicate(duplicate, previous);

    ModuleExport **tail = &module->exports;
    for (ASTNode *node = program->constants; node; node = node->next) {
        VarDeclNode *constant = (VarDeclNode *)node;
        ModuleExport *export = calloc(1, sizeof(ModuleExport));
        if (!export) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return 1;
        }
        export->name = copy_string(constant->name);
        export->symbol = create_symbol(module->prefix, constant->name);
        export->is_constant = 1;
        export->is_pub = constant->is_pub;
        if (!export->name || !export->symbol) {
            free(export->name);
            free(export->symbol);
            free(export);
            return 1;
        }

        free(constant->name);
        constant->name = copy_string(export->symbol);
        if (!constant->name) {
            free(export->name);
            free(export->symbol);
            free(export);
            return 1;
        }
        *tail = export;
        tail = &export->next;
    }

    for (ASTNode *node = program->functions; node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        ModuleExport *export = calloc(1, sizeof(ModuleExport));
        if (!export) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return 1;
        }
        export->name = copy_string(function->name);
        // extern 的符号名必须和 C 符号一致，加了前缀就链接不到了。
        export->symbol = function->is_extern
            ? copy_string(function->name)
            : create_symbol(module->prefix, function->name);
        export->is_pub = function->is_pub;
        if (!export->name || !export->symbol) {
            free(export->name);
            free(export->symbol);
            free(export);
            return 1;
        }

        if (!function->is_extern) {
            free(function->name);
            function->name = copy_string(export->symbol);
            if (!function->name) {
                free(export->name);
                free(export->symbol);
                free(export);
                return 1;
            }
        }
        *tail = export;
        tail = &export->next;
    }

    for (ASTNode *node = program->structs; node; node = node->next) {
        StructNode *structure = (StructNode *)node;
        char *renamed = NULL;
        if (register_type_export(
                module, structure->name, structure->is_pub, &tail, &renamed) != 0) {
            return 1;
        }
        if (renamed) {
            free(structure->name);
            structure->name = copy_string(renamed);
            if (!structure->name) return 1;
        }
    }

    for (ASTNode *node = program->enums; node; node = node->next) {
        EnumNode *enum_node = (EnumNode *)node;
        char *renamed = NULL;
        if (register_type_export(
                module, enum_node->name, enum_node->is_pub, &tail, &renamed) != 0) {
            return 1;
        }
        if (renamed) {
            free(enum_node->name);
            enum_node->name = copy_string(renamed);
            if (!enum_node->name) return 1;
        }
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
        print_diagnostic(stderr, "error", import_node->filename,
                         import_node->line, import_node->column,
                         "namespace '%s' is already used for module '%s'",
                         import_node->alias, existing->import_node->module_name); // 中文：名称空间已用于模块
        print_diagnostic(stderr, "note", existing->import_node->filename,
                         existing->import_node->line, existing->import_node->column,
                         "namespace was first imported here"); // 中文：名称空间首次在此导入
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

// 合并模块中导出的顶层类型、常量和函数；源 ProgramNode 不再拥有这些链表。
static void append_functions(ProgramNode *destination, ProgramNode *source) {
    if (source->enums) {
        if (!destination->enums) {
            destination->enums = source->enums;
        } else {
            ASTNode *tail = destination->enums;
            while (tail->next) tail = tail->next;
            tail->next = source->enums;
        }
        source->enums = NULL;
    }

    if (source->structs) {
        if (!destination->structs) {
            destination->structs = source->structs;
        } else {
            ASTNode *tail = destination->structs;
            while (tail->next) tail = tail->next;
            tail->next = source->structs;
        }
        source->structs = NULL;
    }

    if (source->constants) {
        if (!destination->constants) {
            destination->constants = source->constants;
        } else {
            ASTNode *tail = destination->constants;
            while (tail->next) tail = tail->next;
            tail->next = source->constants;
        }
        source->constants = NULL;
    }

    if (source->functions) {
        if (!destination->functions) {
            destination->functions = source->functions;
        } else {
            ASTNode *tail = destination->functions;
            while (tail->next) tail = tail->next;
            tail->next = source->functions;
        }
        source->functions = NULL;
    }
}

// 将模块成员名解析为内部唯一符号名，例如 math.PI -> __tap_module_0.PI。
static char *resolve_exported_name(
    const char *name, LoadedModule *current_module, ImportBinding *bindings) {
    char *dot = strchr(name, '.');
    ModuleExport *export = NULL;

    if (dot) {
        size_t alias_length = (size_t)(dot - name);
        char *alias = malloc(alias_length + 1);
        if (!alias) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return NULL;
        }
        memcpy(alias, name, alias_length);
        alias[alias_length] = '\0';

        ImportBinding *binding = find_binding(bindings, alias);
        free(alias);
        if (!binding) return NULL;
        // 外部访问只认 pub：非 pub 的顶层符号是模块私有的。
        export = find_public_export(binding->module, dot + 1);
    } else {
        export = find_export(current_module, name);
    }

    return export ? copy_string(export->symbol) : NULL;
}

static int load_module(
    ModuleContext *context, const char *path, LoadedModule **result_module);

static int build_bindings(
    ModuleContext *context, ProgramNode *program, ImportBinding **bindings) {
    for (ASTNode *node = program->imports; node; node = node->next) {
        ImportNode *import_node = (ImportNode *)node;
        char *path = resolve_module_path(import_node, context->compiler_path);
        if (!path) {
            print_diagnostic(stderr, "error", import_node->filename,
                             import_node->line, import_node->column,
                             "module '%s' not found", import_node->module_name); // 中文：找不到模块
            return 1;
        }
        if (context->root_path && strcmp(path, context->root_path) == 0) {
            print_diagnostic(stderr, "error", import_node->filename,
                             import_node->line, import_node->column,
                             "entry file cannot import itself"); // 中文：入口文件不能导入自身
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
    ProgramNode *program, ASTNode *expression,
    LoadedModule *current_module, ImportBinding *bindings);

// 判断限定标识符是否是当前文件里的 Enum.Member 枚举成员。
// 本模块私有类型的裸名引用要跟着改名。pub 类型保持裸名（其他模块直接写类型名引用），
// 外来类型和标量不动。返回要替换成的新名字，NULL 表示不用改。
static char *resolve_type_name(LoadedModule *module, const char *name) {
    ModuleExport *export = find_type_export(module, name);
    if (!export || export->is_pub) return NULL;
    return export->symbol;
}

// 递归重写一个类型节点里的类型名。泛型实参在 type_arguments，数组/指针在 element_type。
static void rewrite_var_type(LoadedModule *module, VarTypeNode *type) {
    if (!type) return;

    if (type->struct_name) {
        char *copy = copy_string_or_null(resolve_type_name(module, type->struct_name));
        if (copy) {
            free(type->struct_name);
            type->struct_name = copy;
        }
    }
    if (type->enum_name) {
        char *copy = copy_string_or_null(resolve_type_name(module, type->enum_name));
        if (copy) {
            free(type->enum_name);
            type->enum_name = copy;
        }
    }

    for (ASTNode *node = type->type_arguments; node; node = node->next) {
        rewrite_var_type(module, (VarTypeNode *)node);
    }
    rewrite_var_type(module, type->element_type);
}

// 把 `Foo.Bar` 里的枚举前缀换成本模块私有符号名。返回新名字（调用方负责释放），
// NULL 表示前缀不是本模块的私有类型、不需要改。
//
// 按**最后一个**点切分：模块前缀本身带点（`__tap_module_0.E`），用第一个点会切错。
static char *resolve_enum_member_name(LoadedModule *module, const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return NULL;

    size_t prefix_length = (size_t)(dot - name);
    char *prefix = malloc(prefix_length + 1);
    if (!prefix) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    memcpy(prefix, name, prefix_length);
    prefix[prefix_length] = '\0';
    char *renamed = resolve_type_name(module, prefix);
    free(prefix);
    if (!renamed) return NULL;

    size_t total = strlen(renamed) + strlen(dot) + 1;
    char *joined = malloc(total);
    if (!joined) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    snprintf(joined, total, "%s%s", renamed, dot);
    return joined;
}

// 模块自己声明的类型（结构体字段、枚举载荷）和顶层常量的类型标注，里面的类型名
// 同样要跟着改名。函数签名由 rewrite_functions 处理，函数体走表达式重写。
static void rewrite_declared_types(LoadedModule *module, ProgramNode *program) {
    for (ASTNode *node = program->structs; node; node = node->next) {
        StructNode *structure = (StructNode *)node;
        for (ASTNode *field = structure->fields; field; field = field->next) {
            rewrite_var_type(module, ((StructFieldNode *)field)->field_type);
        }
    }
    for (ASTNode *node = program->enums; node; node = node->next) {
        EnumNode *enum_node = (EnumNode *)node;
        for (ASTNode *variant = enum_node->variants; variant; variant = variant->next) {
            for (ASTNode *payload = ((EnumVariantNode *)variant)->payload_types;
                 payload; payload = payload->next) {
                rewrite_var_type(module, (VarTypeNode *)payload);
            }
        }
    }
    for (ASTNode *node = program->constants; node; node = node->next) {
        rewrite_var_type(module, ((VarDeclNode *)node)->type);
    }
}

// 解析 `alias.` 前缀对应的导入绑定；没有同名导入时返回 NULL。
static ImportBinding *binding_for_qualified_name(
    const char *name, ImportBinding *bindings) {
    char *dot = strchr(name, '.');
    if (!dot) return NULL;
    size_t alias_length = (size_t)(dot - name);
    char *alias = malloc(alias_length + 1);
    if (!alias) return NULL;
    memcpy(alias, name, alias_length);
    alias[alias_length] = '\0';
    ImportBinding *binding = find_binding(bindings, alias);
    free(alias);
    return binding;
}

static int is_local_enum_member(ProgramNode *program, const char *name) {
    char *dot = strchr(name, '.');
    if (!dot || dot == name || strchr(dot + 1, '.')) return 0;

    size_t enum_name_length = (size_t)(dot - name);
    for (ASTNode *node = program->enums; node; node = node->next) {
        EnumNode *enum_node = (EnumNode *)node;
        if (strlen(enum_node->name) == enum_name_length &&
            strncmp(enum_node->name, name, enum_name_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int rewrite_statement_list(
    ProgramNode *program, ASTNode *statements,
    LoadedModule *current_module, ImportBinding *bindings) {
    for (ASTNode *statement = statements; statement; statement = statement->next) {
        int result = 0;
        switch (statement->type) {
            case NODE_VAR_DECL:
                // 变量声明上的类型标注里可能引用了本模块的私有类型。
                rewrite_var_type(
                    current_module, ((VarDeclNode *)statement)->type);
                result = rewrite_expression(
                    program, ((VarDeclNode *)statement)->expression,
                    current_module, bindings);
                break;
            case NODE_ASSIGNMENT:
                result = rewrite_expression(
                    program, ((AssignmentNode *)statement)->expression,
                    current_module, bindings);
                break;
            case NODE_INDEX_ASSIGNMENT:
                result = rewrite_expression(
                    program,
                    (ASTNode *)((IndexAssignmentNode *)statement)->target,
                    current_module, bindings);
                if (result == 0) {
                    result = rewrite_expression(
                        program,
                        ((IndexAssignmentNode *)statement)->expression,
                        current_module, bindings);
                }
                break;
            case NODE_RETURN:
                result = rewrite_expression(
                    program, ((ReturnNode *)statement)->expression,
                    current_module, bindings);
                break;
            case NODE_FUNCTION_CALL:
                result = rewrite_expression(program, statement, current_module, bindings);
                break;
            case NODE_IF_STATEMENT: {
                IfStatementNode *if_node = (IfStatementNode *)statement;
                result = rewrite_expression(
                    program, if_node->condition, current_module, bindings);
                if (result == 0) {
                    result = rewrite_statement_list(
                        program, if_node->consequence, current_module, bindings);
                }
                if (result == 0) {
                    result = rewrite_statement_list(
                        program, if_node->alternative, current_module, bindings);
                }
                break;
            }
            case NODE_FOR_STATEMENT: {
                ForStatementNode *for_node = (ForStatementNode *)statement;
                if (for_node->initializer) {
                    result = rewrite_statement_list(
                        program, for_node->initializer, current_module, bindings);
                }
                if (result == 0) {
                    result = rewrite_expression(
                        program,
                        for_node->condition, current_module, bindings);
                }
                if (result == 0 && for_node->update) {
                    result = rewrite_statement_list(
                        program, for_node->update, current_module, bindings);
                }
                if (result == 0) {
                    result = rewrite_statement_list(
                        program, for_node->body, current_module, bindings);
                }
                break;
            }
            case NODE_MATCH_STATEMENT: {
                // match 分支体也是语句链表，漏掉这一支的话分支里调用本模块函数
                // 会报 `undefined function`。绑定变量是声明，不参与改写。
                MatchStatementNode *match_node = (MatchStatementNode *)statement;
                result = rewrite_expression(
                    program, match_node->expression, current_module, bindings);
                for (ASTNode *arm_node = match_node->arms;
                     arm_node && result == 0; arm_node = arm_node->next) {
                    MatchArmNode *arm = (MatchArmNode *)arm_node;
                    // 模式里的枚举名要跟着改名（本模块私有枚举会被加前缀）。
                    char *renamed_enum = copy_string_or_null(
                        resolve_type_name(current_module, arm->enum_name));
                    if (renamed_enum) {
                        free(arm->enum_name);
                        arm->enum_name = renamed_enum;
                    }
                    if (arm->body) {
                        result = rewrite_statement_list(
                            program, arm->body, current_module, bindings);
                    }
                    if (result == 0 && arm->value) {
                        result = rewrite_expression(
                            program, arm->value, current_module, bindings);
                    }
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
    if (strcmp(call->name, "__tap_builtin_string_len") == 0 ||
        strcmp(call->name, "__tap_builtin_string_byte_at") == 0 ||
        strcmp(call->name, "__tap_builtin_string_slice") == 0) {
        return 0;
    }

    // 枚举成员构造 `E.B(...)`：E 是本模块私有枚举时前缀要跟着改名。改完就是
    // 模块内部符号，不需要再走别名解析。
    char *renamed_member = resolve_enum_member_name(current_module, call->name);
    if (renamed_member) {
        free(call->name);
        call->name = renamed_member;
        return 0;
    }

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
            // 没有同名导入时，保留给后端按 receiver.method() 方法调用解析。
            free(alias);
            return 0;
        }
        export = find_public_export(binding->module, dot + 1);
        if (!export) {
            // 区分「压根没有这个符号」和「有但不是 pub」：后者是可见性问题，
            // 直接说「是私有的」比说「不存在」有用得多。
            if (find_export(binding->module, dot + 1)) {
                print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                                 "'%s' is private to module '%s'",
                                 dot + 1, binding->import_node->module_name); // 中文：该符号是模块私有的
            } else {
                print_diagnostic(stderr, "error", call->filename, call->line, call->column,
                                 "module '%s' has no function '%s'",
                                 binding->import_node->module_name, dot + 1); // 中文：模块中没有函数
            }
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
    ProgramNode *program, ASTNode *expression,
    LoadedModule *current_module, ImportBinding *bindings) {
    if (!expression) return 0;

    switch (expression->type) {
        case NODE_BINARY_OP: {
            BinaryOpNode *binary = (BinaryOpNode *)expression;
            int result = rewrite_expression(program, binary->left, current_module, bindings);
            return result == 0
                ? rewrite_expression(program, binary->right, current_module, bindings)
                : result;
        }
        case NODE_REFERENCE:
            return rewrite_expression(
                program, ((ReferenceNode *)expression)->target,
                current_module, bindings);
        case NODE_ARRAY_LITERAL: {
            ArrayLiteralNode *array = (ArrayLiteralNode *)expression;
            for (ASTNode *element = array->elements; element; element = element->next) {
                int result = rewrite_expression(
                    program, element, current_module, bindings);
                if (result != 0) return result;
            }
            return 0;
        }
        case NODE_STRUCT_LITERAL: {
            StructLiteralNode *literal = (StructLiteralNode *)expression;
            // 结构体名和泛型实参里的类型名都要跟着改名。
            char *renamed = copy_string_or_null(
                resolve_type_name(current_module, literal->struct_name));
            if (renamed) {
                free(literal->struct_name);
                literal->struct_name = renamed;
            }
            for (ASTNode *node = literal->type_arguments; node; node = node->next) {
                rewrite_var_type(current_module, (VarTypeNode *)node);
            }
            for (ASTNode *node = literal->fields; node; node = node->next) {
                StructInitFieldNode *field = (StructInitFieldNode *)node;
                int result = rewrite_expression(
                    program, field->expression, current_module, bindings);
                if (result != 0) return result;
            }
            return 0;
        }
        case NODE_SIZEOF:
            // sizeof(SomeType) 里的类型名同样要跟着改名。
            rewrite_var_type(
                current_module, ((SizeofNode *)expression)->operand_type);
            return 0;
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            // 显式泛型实参里的类型名也要跟着改名。
            for (ASTNode *type = call->type_arguments; type; type = type->next) {
                rewrite_var_type(current_module, (VarTypeNode *)type);
            }
            for (ASTNode *argument = call->arguments; argument; argument = argument->next) {
                int result = rewrite_expression(program, argument, current_module, bindings);
                if (result != 0) return result;
            }
            return rewrite_call(call, current_module, bindings);
        }
        case NODE_MATCH_STATEMENT: {
            // `return match (...) { ... };` 这种表达式形式的 match。语句形式走
            // rewrite_statement_list 的同名分支。
            MatchStatementNode *match_node = (MatchStatementNode *)expression;
            int result = rewrite_expression(
                program, match_node->expression, current_module, bindings);
            for (ASTNode *arm_node = match_node->arms;
                 arm_node && result == 0; arm_node = arm_node->next) {
                MatchArmNode *arm = (MatchArmNode *)arm_node;
                // 模式里的枚举名要跟着改名（本模块私有枚举会被加前缀）。
                char *renamed_enum = copy_string_or_null(
                    resolve_type_name(current_module, arm->enum_name));
                if (renamed_enum) {
                    free(arm->enum_name);
                    arm->enum_name = renamed_enum;
                }
                if (arm->body) {
                    result = rewrite_statement_list(
                        program, arm->body, current_module, bindings);
                }
                if (result == 0 && arm->value) {
                    result = rewrite_expression(
                        program, arm->value, current_module, bindings);
                }
            }
            return result;
        }
        case NODE_TRY:
            // `module.fn()?`：被传播的内层表达式同样要解析成模块内部符号名。
            return rewrite_expression(
                program, ((TryNode *)expression)->inner, current_module, bindings);
        case NODE_INDEX_EXPRESSION: {
            IndexExpressionNode *index = (IndexExpressionNode *)expression;
            int result = rewrite_expression(program, index->array, current_module, bindings);
            return result == 0
                ? rewrite_expression(program, index->index, current_module, bindings)
                : result;
        }
        case NODE_IDENTIFIER: {
            IdentifierNode *identifier = (IdentifierNode *)expression;
            char *dot = strchr(identifier->name, '.');

            // 不带点的裸名：可能是本模块导出的常量。导出时声明已经被改写成
            // `__tap_module_N.NAME`，这里不跟着改写引用就会找不到符号。只看常量，
            // 函数名和类型名不会以裸名出现在表达式里（见 find_constant_export）；
            // current_module 为 NULL（主程序）时直接返回，主程序不受影响。
            if (!dot) {
                ModuleExport *export = find_constant_export(current_module, identifier->name);
                if (!export) return 0;
                char *resolved = copy_string(export->symbol);
                if (!resolved) return 1;
                free(identifier->name);
                identifier->name = resolved;
                return 0;
            }

            // 枚举成员 `Foo.Bar`：Foo 是本模块的私有枚举时前缀要跟着改名，否则
            // 后面按别名解析会找不到符号。改完就是最终形态，不用再往下走。
            char *renamed_member =
                resolve_enum_member_name(current_module, identifier->name);
            if (renamed_member) {
                free(identifier->name);
                identifier->name = renamed_member;
                return 0;
            }

            if (is_local_enum_member(program, identifier->name)) return 0;

            size_t alias_length = (size_t)(dot - identifier->name);
            char *alias = malloc(alias_length + 1);
            if (!alias) {
                fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                return 1;
            }
            memcpy(alias, identifier->name, alias_length);
            alias[alias_length] = '\0';
            ImportBinding *binding = find_binding(bindings, alias);
            free(alias);
            if (!binding) {
                // 未导入同名前缀时，保留给后端按结构体字段或枚举成员处理。
                return 0;
            }

            char *resolved = resolve_exported_name(identifier->name, current_module, bindings);
            if (!resolved) {
                // 区分「模块里压根没这个符号」和「有但不是 pub」。
                ImportBinding *owner = binding_for_qualified_name(identifier->name, bindings);
                if (owner && find_export(owner->module, dot + 1)) {
                    fprintf(stderr, "error: '%s' is private to module '%s'\n",
                            dot + 1, owner->import_node->module_name); // 中文：该符号是模块私有的
                } else {
                    fprintf(stderr, "error: unknown module constant '%s'\n", identifier->name); // 中文：未知模块常量
                }
                return 1;
            }
            free(identifier->name);
            identifier->name = resolved;
            return 0;
        }
        default:
            return 0;
    }
}

static int rewrite_functions(
    ProgramNode *program, LoadedModule *current_module, ImportBinding *bindings) {
    for (ASTNode *node = program->functions; node; node = node->next) {
        FunctionNode *function = (FunctionNode *)node;
        // 签名里的类型名也要跟着改名，否则函数签名会引用到不存在的类型。
        for (ASTNode *type = function->param_types; type; type = type->next) {
            rewrite_var_type(current_module, (VarTypeNode *)type);
        }
        rewrite_var_type(current_module, function->return_type);

        int result = rewrite_statement_list(program, function->body, current_module, bindings);
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
    // 类型改名之后，模块自己的类型声明和常量类型标注也要跟着改。
    if (result == 0) rewrite_declared_types(module, program);
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
