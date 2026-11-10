#include "module.h"
#include "helpers.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MODULE_PATH_MAX _MAX_PATH
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MODULE_PATH_MAX PATH_MAX
#endif

#include "lexer.h"
#include "parser.h"
#include "helpers.h"
#include "generics.h"
#include "prelude.h"

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

// 一条 import 的绑定方式：
//   NAMESPACE  整模块导入，成员按 `alias.member` 访问
//   MEMBER     单成员导入（可带 as 别名），按裸名 `alias` 访问
//   WILDCARD   通配导入，模块全部 pub 成员按裸名访问
typedef enum {
    IMPORT_NAMESPACE,
    IMPORT_MEMBER,
    IMPORT_WILDCARD
} ImportKind;

typedef struct ImportBinding {
    ImportNode *import_node;
    LoadedModule *module;
    ImportKind kind;
    struct ImportBinding *next;
} ImportBinding;

typedef struct {
    ProgramNode *functions;
    LoadedModule *modules;
    const char *compiler_path;
    const char *root_path;
    unsigned next_module_id;
    // 路径解析阶段的致命错误消息。解析层拿不到 import 语句的位置，只把消息带回来，
    // 由 build_bindings 按语句位置打印 —— 保证和别的诊断一个格式。
    char *error;
    // Prelude 已经提供的顶层名字。通配/单成员导入不许盖住它们：Prelude 在所有模块
    // 装载完之后才注入，不在这里拦就会静默换掉 `print`（编译通过、行为变了）。
    const PreludeNames *prelude_names;
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
    // 优先问操作系统要可执行文件位置：靠 PATH 调用时 argv[0] 只是裸名字，
    // canonical_path 解析不出来，装好的编译器就找不到 ../share/tap/std。
    char executable[MODULE_PATH_MAX];
    if (tap_executable_path(executable, sizeof(executable))) {
        return path_directory(executable);
    }

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

// TAP_MODULE_PATH 的目录分隔符：Windows 用分号（冒号被盘符占用），其余用冒号。
#ifdef _WIN32
#define MODULE_PATH_SEPARATOR ';'
#else
#define MODULE_PATH_SEPARATOR ':'
#endif

// 如果 relative 的第一段是一个包（该目录下有 package.toml），返回加上 src/ 的相对路径。
// 包边界只判第一段：`foo.lib` 里 foo 是包名，`internal/util` 不再探测。
// 不是包（或只有一段）时返回 NULL。
static char *package_scoped_relative(const char *root, const char *relative) {
    const char *separator = strchr(relative, '/');
    if (!separator) return NULL; // 单段模块就是文件，没有包边界

    size_t package_length = (size_t)(separator - relative);
    char *package_directory = malloc(package_length + 1);
    if (!package_directory) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    memcpy(package_directory, relative, package_length);
    package_directory[package_length] = '\0';

    char *marker = join_path(root, package_directory);
    free(package_directory);
    char *manifest = marker ? join_path(marker, "package.toml") : NULL;
    free(marker);
    int is_package = manifest && file_exists(manifest);
    free(manifest);
    if (!is_package) return NULL;

    const char *rest = separator + 1;
    size_t size = package_length + sizeof("/src/") + strlen(rest);
    char *scoped = malloc(size);
    if (!scoped) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        exit(1);
    }
    snprintf(scoped, size, "%.*s/src/%s", (int)package_length, relative, rest);
    return scoped;
}

// 在一个搜索根下找模块：先按原路径，再按包边界。
// 两条都命中时不静默偏向——和「成员名与模块文件重名」的处理一致。但这里是纯路径
// 解析，拿不到 import 语句的位置，所以只把消息写进 *error，交给 build_bindings 打印。
static char *resolve_under_root(const char *root, const char *relative,
                                const char *module_name, char **error) {
    char *plain = join_path(root, relative);
    int plain_exists = plain && file_exists(plain);

    char *scoped = package_scoped_relative(root, relative);
    char *scoped_path = scoped ? join_path(root, scoped) : NULL;
    int scoped_exists = scoped_path && file_exists(scoped_path);

    if (plain_exists && scoped_exists) {
        size_t size = strlen(module_name) + strlen(plain) +
                      strlen(scoped_path) + 64;
        char *message = malloc(size);
        if (message) {
            snprintf(message, size,
                "ambiguous module '%s': both '%s' and '%s' exist",
                module_name, plain, scoped_path); // 中文：模块歧义
            *error = message;
        }
        free(scoped);
        free(plain);
        free(scoped_path);
        return NULL;
    }
    free(scoped);

    if (scoped_exists) {
        free(plain);
        char *resolved = canonical_path(scoped_path);
        free(scoped_path);
        return resolved;
    }
    free(scoped_path);
    return existing_canonical_path(plain);
}

// 从某个目录向上找到含 package.toml 的那一层，即包根；找不到返回 NULL。
// 包根下的 packages/ 是依赖所在处，这是「tap build 独立于 tapm」的支点。
static char *package_root_from(const char *start_directory) {
    char *directory = copy_string(start_directory);
    if (!directory) return NULL;

    char *canonical = canonical_path(directory);
    if (canonical) {
        free(directory);
        directory = canonical;
    }

    while (directory) {
        char *manifest = join_path(directory, "package.toml");
        int found = manifest && file_exists(manifest);
        free(manifest);
        if (found) return directory;

        char *parent = join_path(directory, "..");
        char *resolved = parent ? canonical_path(parent) : NULL;
        free(parent);
        if (!resolved || strcmp(resolved, directory) == 0) {
            free(resolved); // 到顶了
            break;
        }
        free(directory);
        directory = resolved;
    }

    free(directory);
    return NULL;
}

static char *package_root_of(const char *filename) {
    char *directory = path_directory(filename);
    if (!directory) return NULL;
    char *root = package_root_from(directory);
    free(directory);
    return root;
}

// 在 package.toml 的 [package] 段里读一个字符串键。
//
// 极简扫描，不引入通用 TOML 解析器：只认 `键 = "值"` 这一种写法，
// 注释、空行、别的键、别的段一律跳过。找不到返回 NULL。
static char *read_package_field(const char *package_root, const char *key) {
    char *manifest = join_path(package_root, "package.toml");
    if (!manifest) return NULL;
    char *content = read_source_file(manifest);
    free(manifest);
    if (!content) return NULL;

    size_t key_length = strlen(key);
    int in_package_section = 0;
    char *cursor = content;

    while (*cursor) {
        char *line_end = strchr(cursor, '\n');
        size_t length = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        while (length > 0 &&
               (cursor[length - 1] == '\r' || cursor[length - 1] == ' ' ||
                cursor[length - 1] == '\t')) {
            length--;
        }

        const char *field = cursor;
        size_t remaining = length;
        while (remaining > 0 && (*field == ' ' || *field == '\t')) {
            field++;
            remaining--;
        }

        if (remaining > 0 && *field == '[') {
            in_package_section =
                remaining >= 9 && strncmp(field, "[package]", 9) == 0;
        } else if (in_package_section && remaining > key_length &&
                   strncmp(field, key, key_length) == 0) {
            const char *rest = field + key_length;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest == '=') {
                rest++;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest == '"') {
                    const char *value_end = strchr(rest + 1, '"');
                    if (value_end) {
                        size_t value_length = (size_t)(value_end - rest - 1);
                        char *value = malloc(value_length + 1);
                        if (!value) {
                            fprintf(stderr, "Out of memory\n");
                            exit(1);
                        }
                        memcpy(value, rest + 1, value_length);
                        value[value_length] = '\0';
                        free(content);
                        return value;
                    }
                }
            }
        }

        if (!line_end) break;
        cursor = line_end + 1;
    }

    free(content);
    return NULL;
}

// 建目录（已存在算成功）。包模式要自己准备 <包根>/build。
static int ensure_directory(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0) return 0;
#else
    if (mkdir(path, 0755) == 0) return 0;
#endif
    return errno == EEXIST ? 0 : -1;
}

// 包清单解析出来的四项信息。全部是 malloc 的，由调用方释放。
typedef struct {
    char *root;    // 包根目录
    char *entry;   // [package] entry 的原始值（相对包根）
    char *name;    // [package] name
    char *source;  // 入口文件的绝对路径
} PackageInfo;

static void free_package_info(PackageInfo *info) {
    free(info->root);
    free(info->entry);
    free(info->name);
    free(info->source);
    info->root = info->entry = info->name = info->source = NULL;
}

// 从 start_directory 向上找 package.toml 并读出 entry / name。
// 只读，不创建任何目录 —— `tap check` 用这个，检查不该有副作用。
static int read_package_info(const char *start_directory, PackageInfo *info) {
    memset(info, 0, sizeof(*info));

    info->root = package_root_from(start_directory);
    if (!info->root) {
        fprintf(stderr,
            "error: no package.toml found in this directory or any parent\n"); // 中文：找不到 package.toml
        return 1;
    }

    info->entry = read_package_field(info->root, "entry");
    if (!info->entry) {
        fprintf(stderr,
            "error: package.toml has no [package] entry\n"); // 中文：清单缺少 entry
        free_package_info(info);
        return 1;
    }

    info->name = read_package_field(info->root, "name");
    if (!info->name) {
        fprintf(stderr,
            "error: package.toml has no [package] name\n"); // 中文：清单缺少 name
        free_package_info(info);
        return 1;
    }

    info->source = join_path(info->root, info->entry);
    if (!info->source) {
        free_package_info(info);
        return 1;
    }
    return 0;
}

// 包模式（只读版）：只解析入口文件，不碰 build/ 目录。
// name_out 可以为 NULL。
int resolve_package_entry(const char *start_directory, char **source_out, char **name_out) {
    PackageInfo info;
    if (read_package_info(start_directory, &info) != 0) return 1;

    *source_out = info.source;
    info.source = NULL;
    if (name_out) {
        *name_out = info.name;
        info.name = NULL;
    }
    free_package_info(&info);
    return 0;
}

int resolve_package_target(
    const char *start_directory, const char *build_directory,
    char **source_out, char **output_out) {
    PackageInfo info;
    if (read_package_info(start_directory, &info) != 0) return 1;

    char *build_root = join_path(info.root, build_directory);
    if (!build_root || ensure_directory(build_root) != 0) {
        fprintf(stderr,
            "error: cannot create build directory '%s'\n",
            build_root ? build_root : build_directory); // 中文：无法创建构建目录
        free(build_root);
        free_package_info(&info);
        return 1;
    }
    char *output = join_path(build_root, info.name);
    free(build_root);

    if (!output) {
        free_package_info(&info);
        return 1;
    }

    *source_out = info.source;
    *output_out = output;
    info.source = NULL;
    free_package_info(&info);
    return 0;
}

// 一个搜索根：root 是目录（"." 表示当前工作目录），module_name 是这个根上使用的
// 模块名。std 根要把 `std.` 前缀去掉，其余与源码里写的一致。
typedef struct {
    char *root;
    const char *module_name;
} ModuleSearchRoot;

typedef struct {
    ModuleSearchRoot *items;
    int count;
    int capacity;
} ModuleSearchRoots;

static void add_search_root(ModuleSearchRoots *roots, char *root,
                            const char *module_name) {
    if (!root) return; // copy_string / join_path 失败时已经报过 OOM
    if (roots->count == roots->capacity) {
        int capacity = roots->capacity ? roots->capacity * 2 : 8;
        ModuleSearchRoot *items =
            realloc(roots->items, sizeof(ModuleSearchRoot) * (size_t)capacity);
        if (!items) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            exit(1);
        }
        roots->items = items;
        roots->capacity = capacity;
    }
    roots->items[roots->count].root = root;
    roots->items[roots->count].module_name = module_name;
    roots->count++;
}

static void free_search_roots(ModuleSearchRoots *roots) {
    for (int index = 0; index < roots->count; index++) {
        free(roots->items[index].root);
    }
    free(roots->items);
    roots->items = NULL;
    roots->count = 0;
    roots->capacity = 0;
}

// TAP_MODULE_PATH 的每一段都是一个搜索根。
static void add_module_path_roots(ModuleSearchRoots *roots,
                                  const char *module_path,
                                  const char *module_name) {
    const char *start = module_path;
    for (;;) {
        const char *end = strchr(start, MODULE_PATH_SEPARATOR);
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length > 0) {
            char *root = malloc(length + 1);
            if (!root) {
                fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                exit(1);
            }
            memcpy(root, start, length);
            root[length] = '\0';
            add_search_root(roots, root, module_name);
        }
        if (!end) return;
        start = end + 1;
    }
}

// 在一个根上按给定扩展名找。
static char *resolve_in_root(const char *root, const char *module_name,
                             const char *extension, char **error) {
    char *relative = module_relative_path(module_name, extension);
    if (!relative) return NULL;
    if (strcmp(root, ".") == 0) {
        // 当前工作目录：相对路径直接交给 existing_canonical_path，它负责释放 relative。
        return existing_canonical_path(relative);
    }
    char *resolved = resolve_under_root(root, relative, module_name, error);
    free(relative);
    return resolved;
}

// 按「搜索根优先、扩展名次之」解析模块路径。
//
// 顺序很关键：扩展名必须在**每个根内部**判断，不能先把 .tp 在所有根上跑一遍。
// 早期实现是后者，结果低优先级根上的 foo.tp 会抢走导入文件同目录的 foo.tap，
// 与「同一目录下 .tp 优先」的承诺相反，而且没有任何诊断。
static char *resolve_module_path(const char *module_name, const char *filename,
                                 const char *compiler_path, char **error) {
    static const char *extensions[] = {".tp", ".tap"};
    ModuleSearchRoots roots = {NULL, 0, 0};

    // 1. 标准库专用根：`std.` 前缀去掉，且优先于所有普通根。
    if (is_std_module(module_name)) {
        const char *std_path = getenv("TAP_STD_PATH");
        if (std_path && std_path[0] != '\0') {
            add_search_root(&roots, copy_string(std_path), module_name + 4);
        }
    }

    // 2. 导入声明所在文件的目录。
    add_search_root(&roots, path_directory(filename), module_name);

    // 3. TAP_MODULE_PATH，支持多个根（Unix 冒号分隔，Windows 分号）。
    const char *module_path = getenv("TAP_MODULE_PATH");
    if (module_path && module_path[0] != '\0') {
        add_module_path_roots(&roots, module_path, module_name);
    }

    // 4. 包根下的 packages/：依赖装在这里，编译器自己认，不依赖 tapm 设环境变量。
    char *package_root = package_root_of(filename);
    if (package_root) {
        char *dependencies = join_path(package_root, "packages");
        free(package_root);
        add_search_root(&roots, dependencies, module_name);
    }

    // 5. 当前工作目录。
    add_search_root(&roots, copy_string("."), module_name);

    // 6. 编译器相邻的源码目录，以及安装前缀下的 share/tap。
    char *directory = compiler_directory(compiler_path);
    if (directory) {
        add_search_root(&roots, join_path(directory, ".."), module_name);
        add_search_root(&roots, join_path(directory, "../share/tap"), module_name);
        free(directory);
    }

    char *resolved = NULL;
    for (int index = 0; index < roots.count && !resolved && !*error; index++) {
        for (size_t extension = 0;
             extension < sizeof(extensions) / sizeof(extensions[0]);
             extension++) {
            resolved = resolve_in_root(roots.items[index].root,
                                       roots.items[index].module_name,
                                       extensions[extension], error);
            if (resolved || *error) break;
        }
    }

    free_search_roots(&roots);
    return resolved;
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

// 比较两个模块路径是否指向同一个文件。
//
// Windows 的文件系统大小写不敏感，`_fullpath` 却保留传入的大小写：同一个 `mod2.tp`
// 写成 `Mod2.tp` 会得到两个不同的字符串，去重就失效了 —— 结果是一份源码被当成两个
// 模块，各自改名、各自生成函数（`std.Math` 和 `std.math` 就会撞出
// `duplicate function definition '__tap_pow_f64'` 这种看不懂的错）。
// 所以去重按「是不是同一个文件」判断，忽略大小写；显示和存储仍用原始路径。
static int path_equal(const char *left, const char *right) {
#ifdef _WIN32
    return _stricmp(left, right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

static LoadedModule *find_loaded_module(LoadedModule *modules, const char *path) {
    for (; modules; modules = modules->next) {
        if (path_equal(modules->path, path)) return modules;
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

// 登记一个顶层类型声明。**所有类型一律改名**成 `__tap_module_N.Name`，pub 的也不例外。
//
// 早先 pub 类型是保持裸名的（其他模块直接写类型名引用），但那样它就占着全局名字——
// 两个包各写 `pub struct Point` 会撞在同一个符号上，第三方生态根本立不起来。
// 现在引用处靠 `resolve_type_name` 的三级查找把名字解析回来，`pub` 只决定「能不能
// 从模块外引用」，不再决定「名字长什么样」。
//
// 改名结果通过 renamed_out 交回调用方写回定义处。
static int register_type_export(
    LoadedModule *module, const char *name, int is_pub,
    ModuleExport ***tail, char **renamed_out) {
    ModuleExport *export = calloc(1, sizeof(ModuleExport));
    if (!export) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    export->name = copy_string(name);
    export->symbol = create_symbol(module->prefix, name);
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
    *renamed_out = export->symbol;
    return 0;
}

// 把模块的顶层符号登记成「原名 → 内部符号名」，并就地改写定义处的名字。
//
//   - 常量、普通函数：一律改名成 `__tap_module_N.name`。**非 pub 的也要改名** ——
//     否则它仍然占着全局名字，会和其他模块的同名符号撞。
//   - `extern` 函数：符号名就是 C 符号名，加前缀会链接不到，所以保持原名。
//   - 类型：**一律**改名成 `__tap_module_N.Name`，pub 的也一样。pub 只决定「能不能从
//     模块外引用」，不决定名字长什么样 —— 两个模块因此可以各自拥有同名的 pub 类型。
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
            // 保存改名前的原名，诊断里优先用它（避免泄漏 __tap_module_N. 内部符号名）。
            function->original_name = function->name;
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

// 按裸名（名称空间别名 / 单成员导入别名）找绑定。通配导入不绑定裸名，不参与。
static ImportBinding *find_bound_name(ImportBinding *bindings, const char *name) {
    for (; bindings; bindings = bindings->next) {
        if (bindings->kind != IMPORT_WILDCARD && bindings->import_node->alias &&
            strcmp(bindings->import_node->alias, name) == 0) {
            return bindings;
        }
    }
    return NULL;
}

// 限定访问 `alias.member` 只认名称空间导入。单成员导入的 alias 是成员裸名，
// 不是名称空间，不在这里匹配。
static ImportBinding *find_namespace_binding(ImportBinding *bindings, const char *alias) {
    for (; bindings; bindings = bindings->next) {
        if (bindings->kind == IMPORT_NAMESPACE && bindings->import_node->alias &&
            strcmp(bindings->import_node->alias, alias) == 0) {
            return bindings;
        }
    }
    return NULL;
}

// 裸名（不带模块前缀）在单成员 / 通配导入里找符号。单成员导入的成员合法性在
// build_bindings 里已经查过，这里只做查找。
static ModuleExport *find_bare_import(ImportBinding *bindings, const char *name) {
    for (ImportBinding *binding = bindings; binding; binding = binding->next) {
        if (binding->kind == IMPORT_MEMBER) {
            if (binding->import_node->alias &&
                strcmp(binding->import_node->alias, name) == 0) {
                return find_public_export(binding->module, binding->import_node->member);
            }
        } else if (binding->kind == IMPORT_WILDCARD) {
            ModuleExport *export = find_public_export(binding->module, name);
            if (export) return export;
        }
    }
    return NULL;
}

// 裸名常量：只匹配 pub 常量。函数名不会以裸名出现在表达式里（调用走 rewrite_call），
// 类型名走 NODE_VAR_TYPE，都不该在这里命中。
static ModuleExport *find_bare_constant_import(
    ImportBinding *bindings, const char *name) {
    for (ImportBinding *binding = bindings; binding; binding = binding->next) {
        if (binding->kind == IMPORT_MEMBER) {
            if (binding->import_node->alias &&
                strcmp(binding->import_node->alias, name) == 0) {
                ModuleExport *export =
                    find_public_export(binding->module, binding->import_node->member);
                if (export && export->is_constant) return export;
            }
        } else if (binding->kind == IMPORT_WILDCARD) {
            ModuleExport *export = find_constant_export(binding->module, name);
            if (export && export->is_pub) return export;
        }
    }
    return NULL;
}

// 裸名类型：单成员导入（`import m.T;`）和通配导入（`import m.*;`）明确引入的类型名。
static ModuleExport *find_bare_type_import(
    ImportBinding *bindings, const char *name) {
    for (ImportBinding *binding = bindings; binding; binding = binding->next) {
        if (binding->kind == IMPORT_MEMBER) {
            if (binding->import_node->alias &&
                strcmp(binding->import_node->alias, name) == 0) {
                ModuleExport *export =
                    find_public_export(binding->module, binding->import_node->member);
                if (export && export->is_type) return export;
            }
        } else if (binding->kind == IMPORT_WILDCARD) {
            ModuleExport *export = find_type_export(binding->module, name);
            if (export && export->is_pub) return export;
        }
    }
    return NULL;
}

// 命名空间导入（`import std.vec as vec;`）**也会**把模块的 pub 类型名带进裸名作用域——
// `Vec<i32>` 就是这么用的，保住它才不会让 std 全线崩。
//
// 但命名空间导入是**兜底**（四级查找的第④级），所以要求唯一：多个被命名空间导入的模块
// 都导出同一个类型名时，裸名没有确定答案。
//
// **用到才报，不在导入时报。** 「导入」本身没问题，只有真的写出那个裸名才有问题：
// 同时导入两个都导出 `Point` 的包，只要不写裸名 `Point`（用 `let p = a.make();` 让推断
// 定类型，或者写限定名 `a.Point`）就完全正常。导入时报错会把这种完全合法的用法也堵死。
//
// 报错不带位置（`VarTypeNode` 没有行列），所以把涉及的两个模块名都列出来。
static ModuleExport *find_namespace_type_import(
    ImportBinding *bindings, const char *name) {
    ModuleExport *found = NULL;
    ImportBinding *found_binding = NULL;
    for (ImportBinding *binding = bindings; binding; binding = binding->next) {
        if (binding->kind != IMPORT_NAMESPACE) continue;
        ModuleExport *export = find_public_export(binding->module, name);
        if (!export || !export->is_type) continue;
        if (found && found != export) {
            fprintf(stderr,
                "error: type name '%s' is exported by both '%s' and '%s'; the bare name is ambiguous\n", // 中文：裸类型名有歧义
                name, found_binding->import_node->module_name,
                binding->import_node->module_name);
            fprintf(stderr,
                "note: write '%s.%s' or '%s.%s' to pick one\n", // 中文：用限定名挑一个
                found_binding->import_node->alias, name,
                binding->import_node->alias, name);
            exit(1);
        }
        found = export;
        found_binding = binding;
    }
    return found;
}

static int add_binding(
    ImportBinding **bindings, ImportNode *import_node, LoadedModule *module,
    ImportKind kind) {
    // 通配导入不占裸名，只把模块挂上，不会和别的导入冲突。
    if (kind != IMPORT_WILDCARD) {
        ImportBinding *existing = find_bound_name(*bindings, import_node->alias);
        if (existing) {
            if (existing->module == module && existing->kind == kind) return 0;
            const char *label =
                (existing->kind == IMPORT_NAMESPACE && kind == IMPORT_NAMESPACE)
                    ? "namespace" : "name";
            print_diagnostic(stderr, "error", import_node->filename,
                             import_node->line, import_node->column,
                             "%s '%s' is already used for module '%s'",
                             label, import_node->alias,
                             existing->import_node->module_name); // 中文：名称空间已用于模块
            print_diagnostic(stderr, "note", existing->import_node->filename,
                             existing->import_node->line, existing->import_node->column,
                             "%s was first imported here", label); // 中文：名称空间首次在此导入
            return 1;
        }
    }

    ImportBinding *binding = malloc(sizeof(ImportBinding));
    if (!binding) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    binding->import_node = import_node;
    binding->module = module;
    binding->kind = kind;
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

    // trait / impl 也要跟着合并：约束检查和 impl 表都在整个程序的层面做。
    if (source->traits) {
        if (!destination->traits) {
            destination->traits = source->traits;
        } else {
            ASTNode *tail = destination->traits;
            while (tail->next) tail = tail->next;
            tail->next = source->traits;
        }
        source->traits = NULL;
    }

    if (source->impls) {
        if (!destination->impls) {
            destination->impls = source->impls;
        } else {
            ASTNode *tail = destination->impls;
            while (tail->next) tail = tail->next;
            tail->next = source->impls;
        }
        source->impls = NULL;
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

        ImportBinding *binding = find_namespace_binding(bindings, alias);
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

// 导入目标解析结果。
typedef enum {
    IMPORT_RESOLVE_OK,          // 成功，*path_out 是模块文件路径
    IMPORT_RESOLVE_NOT_FOUND,   // 整条路径和前缀都找不到模块
    IMPORT_RESOLVE_AMBIGUOUS,   // 整条路径和前缀都是模块，最后一段无法确定是模块段还是成员名
    IMPORT_RESOLVE_ERROR        // 解析阶段已经报过致命错误（消息在 context->error）
} ImportResolveStatus;

// 把点分路径拆成「模块名 + 成员名」。整条路径优先当模块名解析（`import std.math`
// 就是模块 std.math）；解析不到时退一步把最后一段当成员名、前一段当模块名
// （`import std.math.sin` → 模块 std.math 的成员 sin）。命中成员分支时把
// import_node 就地收窄成真正的模块名，并记下 member。通配导入（mod.*）没有成员，
// 不做回退。
//
// 如果整条路径和「去掉最后一段的前缀」**都能解析成模块**，最后一段既可能是模块路径
// 的一段、也可能是成员名，属于歧义：不静默偏向任何一侧，返回 IMPORT_RESOLVE_AMBIGUOUS
// 让调用方报错（成员名和模块文件重名时用户得自己改名或改用名称空间导入）。
// 成功时 *path_out 拿到模块文件路径，调用方负责 free。
static ImportResolveStatus resolve_import_target(
    ModuleContext *context, ImportNode *import_node, char **path_out) {
    *path_out = NULL;

    char *full = resolve_module_path(
        import_node->module_name, import_node->filename, context->compiler_path,
        &context->error);
    if (context->error) return IMPORT_RESOLVE_ERROR;

    // 通配导入没有成员段，`mod.*` 的模块名就是整条路径。
    if (import_node->is_wildcard) {
        *path_out = full;
        return full ? IMPORT_RESOLVE_OK : IMPORT_RESOLVE_NOT_FOUND;
    }

    const char *last_dot = strrchr(import_node->module_name, '.');
    if (!last_dot || last_dot == import_node->module_name) {
        // 只有一段路径，没有可当成员的最后一段。
        *path_out = full;
        return full ? IMPORT_RESOLVE_OK : IMPORT_RESOLVE_NOT_FOUND;
    }

    size_t prefix_length = (size_t)(last_dot - import_node->module_name);
    char *prefix = malloc(prefix_length + 1);
    if (!prefix) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        free(full);
        return IMPORT_RESOLVE_NOT_FOUND;
    }
    memcpy(prefix, import_node->module_name, prefix_length);
    prefix[prefix_length] = '\0';

    char *prefix_path = resolve_module_path(
        prefix, import_node->filename, context->compiler_path, &context->error);
    if (context->error) {
        free(full);
        free(prefix);
        return IMPORT_RESOLVE_ERROR;
    }

    if (full && prefix_path) {
        // 整条路径和前缀都是模块。光是「有个同名文件」不算歧义 —— 还得前缀模块里
        // 确实有这个成员，否则 `import a.b.c;`（本意是模块 a.b.c）会被一个无关的
        // a/b.tp 永久挡住。只有两边都成立时，最后一段才真的无法确定是模块段还是成员名。
        LoadedModule *prefix_module = NULL;
        if (load_module(context, prefix_path, &prefix_module) != 0) {
            free(full);
            free(prefix_path);
            free(prefix);
            return IMPORT_RESOLVE_ERROR;
        }
        if (find_export(prefix_module, last_dot + 1)) {
            free(full);
            free(prefix_path);
            free(prefix);
            return IMPORT_RESOLVE_AMBIGUOUS;
        }
        // 前缀模块没有这个成员，最后一段只能是模块路径的一段。
        free(prefix_path);
        free(prefix);
        *path_out = full;
        return IMPORT_RESOLVE_OK;
    }

    if (full) {
        free(prefix_path);
        free(prefix);
        *path_out = full;
        return IMPORT_RESOLVE_OK;
    }

    if (prefix_path) {
        // 只有前缀是模块：最后一段当成员名。
        free(import_node->member);
        import_node->member = copy_string(last_dot + 1);
        free(import_node->module_name);
        import_node->module_name = prefix;
        *path_out = prefix_path;
        return IMPORT_RESOLVE_OK;
    }

    free(prefix);
    return IMPORT_RESOLVE_NOT_FOUND;
}

// 报告「成员名与模块文件重名」的歧义。import_node->module_name 此时仍是完整路径
// （歧义分支不改写它），按最后一个点拆出前缀模块名和成员名。
static int report_ambiguous_import(const ImportNode *import_node) {
    const char *last_dot = strrchr(import_node->module_name, '.');
    size_t prefix_length = (size_t)(last_dot - import_node->module_name);
    char *prefix = malloc(prefix_length + 1);
    if (!prefix) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    memcpy(prefix, import_node->module_name, prefix_length);
    prefix[prefix_length] = '\0';

    print_diagnostic(stderr, "error", import_node->filename,
                     import_node->line, import_node->column,
                     "ambiguous import '%s': both module '%s' and member '%s' of module '%s' exist",
                     import_node->module_name, import_node->module_name,
                     last_dot + 1, prefix); // 中文：导入有歧义：模块与成员重名
    print_diagnostic(stderr, "note", import_node->filename,
                     import_node->line, import_node->column,
                     "import the namespace form 'import %s;' and qualify the call, or rename one of them",
                     prefix); // 中文：改用名称空间导入并加限定，或给其中一个改名
    free(prefix);
    return 1;
}

// 本文件自己有没有声明这个名字（顶层函数、常量、结构体、枚举）。模块文件用导出表
// —— 里面的 name 都是源语言原名（函数/常量即便改名成 __tap_module_N.name，导出表仍记原名）；
// 入口文件（module == NULL）直接看 program 的各条顶层链表。
static int has_local_top_level(
    ProgramNode *program, LoadedModule *module, const char *name) {
    if (module) return find_export(module, name) != NULL;

    for (ASTNode *node = program->functions; node; node = node->next) {
        if (strcmp(((FunctionNode *)node)->name, name) == 0) return 1;
    }
    for (ASTNode *node = program->constants; node; node = node->next) {
        if (strcmp(((VarDeclNode *)node)->name, name) == 0) return 1;
    }
    for (ASTNode *node = program->structs; node; node = node->next) {
        if (strcmp(((StructNode *)node)->name, name) == 0) return 1;
    }
    for (ASTNode *node = program->enums; node; node = node->next) {
        if (strcmp(((EnumNode *)node)->name, name) == 0) return 1;
    }
    return 0;
}

// 一条绑定会引入裸名 name 吗？会的话把「源成员名」写到 *member_out —— 模块 + 成员名
// 唯一确定一个符号，用来判断重复导入。
//
// **命名空间导入不算引入裸名**：`import std.vec as vec;` 不会把 `new`/`push` 变成裸名
// （必须写 `vec.new`）。它的 pub 类型名确实能裸着用（`Vec<i32>`），但那是三级查找里的
// 第③级、是**兜底**：本文件自己的定义和单成员/通配导入优先级更高，在场时第③级用不到。
// 所以命名空间导入不能参与这里的重名判断——否则「一个包定义 Point 又依赖另一个也定义
// Point 的包」就会误报。第③级自己的歧义（多个来源）另有一处校验。
static int binding_introduces(
    ImportBinding *binding, const char *name, const char **member_out) {
    if (binding->kind == IMPORT_MEMBER) {
        if (!binding->import_node->alias ||
            strcmp(binding->import_node->alias, name) != 0) {
            return 0;
        }
        *member_out = binding->import_node->member;
        return 1;
    }
    if (binding->kind == IMPORT_WILDCARD) {
        ModuleExport *export = find_public_export(binding->module, name);
        if (!export) return 0;
        *member_out = export->name;
        return 1;
    }
    return 0;
}

// 数一个裸名有几个来源：本文件顶层定义算一个，每条引入它的通配/单成员导入算一个
// （同一模块的同名成员被重复导入只算一次）。> 1 就是重名。
static int count_bare_name_sources(
    ProgramNode *program, LoadedModule *module, ImportBinding *bindings,
    const char *name) {
    int count = has_local_top_level(program, module, name) ? 1 : 0;

    for (ImportBinding *binding = bindings; binding; binding = binding->next) {
        const char *member = NULL;
        if (!binding_introduces(binding, name, &member) || !member) continue;

        int seen = 0;
        for (ImportBinding *previous = bindings; previous != binding;
             previous = previous->next) {
            const char *previous_member = NULL;
            if (previous->module == binding->module &&
                binding_introduces(previous, name, &previous_member) &&
                previous_member && strcmp(previous_member, member) == 0) {
                seen = 1;
                break;
            }
        }
        if (!seen) count++;
    }
    return count;
}

static int report_bare_name_conflict(
    const ImportNode *import_node, const char *name, int is_type) {
    print_diagnostic(stderr, "error", import_node->filename, import_node->line,
                     import_node->column,
                     "import from module '%s' brings in '%s', which is already defined or imported in this file",
                     import_node->module_name, name); // 中文：导入引入的裸名与本地定义/其他导入重名
    // 类型和函数/常量的建议不一样：类型要靠限定名（`a.Point`）消歧，而限定名只对
    // 命名空间导入有效 —— 单成员 / 通配导入没有前缀可用，只能改名。
    print_diagnostic(stderr, "note", import_node->filename, import_node->line,
                     import_node->column,
                     is_type
                         ? "rename one of them, or use a namespace import and write the qualified name" // 中文：改名，或改用名称空间导入并写限定名
                         : "rename one of them, or import the module as a namespace and qualify the call"); // 中文：改名，或改用名称空间导入
    return 1;
}

// 导入的裸名和 Prelude 冲突。比普通的「重名」严重：Prelude 是隐式注入的，用户
// 没写任何 import 就默认拥有 `print` / `panic` / `exit`，静默换掉它是**语义变化**，
// 编译能过、行为却变了。所以这里只报错，不提供「本地优先」的选项。
static int report_prelude_conflict(
    const ImportNode *import_node, const char *name, int is_type) {
    print_diagnostic(stderr, "error", import_node->filename, import_node->line,
                     import_node->column,
                     "import from module '%s' brings in '%s', which is already provided by the prelude",
                     import_node->module_name, name); // 中文：导入的裸名与 Prelude 重名
    if (is_type) {
        print_diagnostic(stderr, "note", import_node->filename, import_node->line,
                         import_node->column,
                         "rename it, or use a namespace import and write the qualified type name"); // 中文：改名，或改用名称空间导入并写限定名
    } else {
        print_diagnostic(stderr, "note", import_node->filename, import_node->line,
                         import_node->column,
                         "rename it, or call it as '%s.%s(...)' with a namespace import",
                         import_node->module_name, name); // 中文：改名，或改用名称空间导入按别名调用
    }
    return 1;
}

// 校验通配/单成员导入引入的裸名：不能和本文件顶层定义重名，也不能和其他导入重名，
// 更不能盖住 Prelude。重名一律报错，不静默偏向任何一侧 —— 这条取代了原来
// 「本地定义优先于通配导入」的规则。
// **类型同样参与**：类型名现在也带模块前缀，撞了就必须说清楚，不能靠全局唯一侥幸过关。
//
// 命名空间导入不在这里查：它的 pub 类型名只是三级查找的兜底，本文件自己的定义和单成员/
// 通配导入优先级更高，在场时兜底用不到，不构成冲突。第③级自己的歧义由
// `find_namespace_type_import` 在真正用到那个裸名时报（见那里的注释）。
static int validate_bare_names(
    ModuleContext *context, ProgramNode *program, LoadedModule *module,
    ImportBinding *bindings) {
    for (ImportBinding *binding = bindings; binding; binding = binding->next) {
        if (binding->kind == IMPORT_NAMESPACE) continue;

        if (binding->kind == IMPORT_MEMBER) {
            const char *name = binding->import_node->alias;
            int prelude_type = 0;
            if (context->prelude_names &&
                prelude_provides_name(context->prelude_names, name, &prelude_type)) {
                return report_prelude_conflict(binding->import_node, name, prelude_type);
            }
            if (count_bare_name_sources(program, module, bindings, name) > 1) {
                ModuleExport *export = find_public_export(
                    binding->module, binding->import_node->member);
                return report_bare_name_conflict(
                    binding->import_node, name, export && export->is_type);
            }
            continue;
        }

        for (ModuleExport *export = binding->module->exports;
             export; export = export->next) {
            if (!export->is_pub) continue;
            if (context->prelude_names &&
                prelude_provides_name(context->prelude_names, export->name, NULL)) {
                return report_prelude_conflict(
                    binding->import_node, export->name, export->is_type);
            }
            if (count_bare_name_sources(program, module, bindings, export->name) > 1) {
                return report_bare_name_conflict(
                    binding->import_node, export->name, export->is_type);
            }
        }
    }
    return 0;
}

static int build_bindings(
    ModuleContext *context, ProgramNode *program, LoadedModule *current_module,
    ImportBinding **bindings) {
    for (ASTNode *node = program->imports; node; node = node->next) {
        ImportNode *import_node = (ImportNode *)node;
        char *path = NULL;
        ImportResolveStatus status = resolve_import_target(context, import_node, &path);
        if (status == IMPORT_RESOLVE_AMBIGUOUS) {
            return report_ambiguous_import(import_node);
        }
        if (status == IMPORT_RESOLVE_ERROR) {
            // 解析阶段拿不到源码位置，消息在这里补上位置再打印。
            if (context->error) {
                print_diagnostic(stderr, "error", import_node->filename,
                                 import_node->line, import_node->column,
                                 "%s", context->error);
                free(context->error);
                context->error = NULL;
            }
            return 1;
        }
        if (status != IMPORT_RESOLVE_OK) {
            print_diagnostic(stderr, "error", import_node->filename,
                             import_node->line, import_node->column,
                             "module '%s' not found", import_node->module_name); // 中文：找不到模块
            return 1;
        }
        if (context->root_path && path_equal(path, context->root_path)) {
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

        ImportKind kind = IMPORT_NAMESPACE;
        if (import_node->is_wildcard) {
            kind = IMPORT_WILDCARD;
        } else if (import_node->member) {
            kind = IMPORT_MEMBER;
            // 单成员导入必须命中一个 pub 成员。分开报「私有」和「不存在」，
            // 和限定访问那边的诊断保持一致。
            if (!find_public_export(module, import_node->member)) {
                if (find_export(module, import_node->member)) {
                    print_diagnostic(stderr, "error", import_node->filename,
                                     import_node->line, import_node->column,
                                     "'%s' is private to module '%s'",
                                     import_node->member,
                                     import_node->module_name); // 中文：该符号是模块私有的
                } else {
                    print_diagnostic(stderr, "error", import_node->filename,
                                     import_node->line, import_node->column,
                                     "module '%s' has no member '%s'",
                                     import_node->module_name,
                                     import_node->member); // 中文：模块中没有该成员
                }
                return 1;
            }
        }
        if (add_binding(bindings, import_node, module, kind) != 0) return 1;
    }

    return validate_bare_names(context, program, current_module, *bindings);
}

static int rewrite_expression(
    ProgramNode *program, ASTNode *expression,
    LoadedModule *current_module, ImportBinding *bindings);

// 限定类型名 `alias.Type`：按**第一个**点切成「模块别名 + 类型名」。
//
// 这是唯一能显式消歧的写法，所以排在查找的最前面：写出来了就按写出来的找，不再
// 走裸名那几级。只认命名空间导入（`import std.vec as vec;`）的别名 —— 单成员导入
// 已经把成员带进裸名作用域，再给它加个前缀没有意义。
//
// 别名不是任何命名空间导入时返回 NULL，让调用方回落到裸名查找：已经改过名的类型
// （`__tap_module_0.Point`）本身就是带点的，必须能原样穿过这里。
//
// report_errors 区分「类型位置」和「探测」：`resolve_enum_member_name` 会拿
// `c.LABEL` 这种**前缀**来试（`c.LABEL.len` 按最后一个点切出来的），那里的成员是常量
// 不是类型，不能报错。真正的类型位置上别名认出来但成员不对就直接报错 —— 用户既然写出
// 了前缀，意图是明确的，再回落到裸名只会得到「undefined struct type 'a.Piont'」。
static ModuleExport *find_qualified_type_import(
    ImportBinding *bindings, const char *name, int report_errors) {
    const char *dot = strchr(name, '.');
    if (!dot || dot == name || !dot[1]) return NULL;

    size_t alias_length = (size_t)(dot - name);
    char *alias = malloc(alias_length + 1);
    if (!alias) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    memcpy(alias, name, alias_length);
    alias[alias_length] = '\0';
    ImportBinding *binding = find_namespace_binding(bindings, alias);
    free(alias);
    if (!binding) return NULL;

    const char *member = dot + 1;
    ModuleExport *export = find_public_export(binding->module, member);
    if (export && export->is_type) return export;
    if (!report_errors) return NULL;

    // 这里报错没有位置可带（VarTypeNode 没有行列信息），所以把模块名写进消息。
    const char *module_name = binding->import_node->module_name;
    if (export) {
        fprintf(stderr, "error: '%s' in module '%s' is not a type\n", // 中文：该成员不是类型
                member, module_name);
    } else if (find_export(binding->module, member)) {
        fprintf(stderr, "error: type '%s' is private to module '%s'\n", // 中文：该类型是模块私有的
                member, module_name);
    } else {
        fprintf(stderr, "error: module '%s' has no type '%s'\n", // 中文：模块中没有该类型
                module_name, member);
    }
    exit(1);
}

// 把一个源语言里的类型名解析成内部唯一符号名。返回 export->symbol（不新分配，调用方
// 负责 copy），NULL 表示这个类型名不认识（交给下游按标量或外部类型处理）。
//
// 四级查找，顺序固定：
//   ① 限定名 `alias.Type` —— 显式写出来的模块前缀，优先级最高
//   ② 本模块自己声明的类型 —— pub 与否都在这，它们现在都带模块前缀
//   ③ 裸名导入明确引入的类型（`import m.T;` / `import m.*;`）
//   ④ 命名空间导入的模块里唯一导出这个类型名的那个
//      （`import std.vec as vec;` 之后写裸名 `Vec<i32>` 就是靠这一级）
// 四级都不命中返回 NULL。report_errors 见 find_qualified_type_import。
static char *resolve_type_name(
    LoadedModule *module, ImportBinding *bindings, const char *name,
    int report_errors) {
    // 通配分支（`_ => ...`）的 `arm->enum_name` 是 NULL，调用方会原样传进来。
    // 不挡的话会一路走到 strcmp(export->name, NULL) 直接段错误。
    if (!name) return NULL;

    ModuleExport *export = find_qualified_type_import(bindings, name, report_errors);
    if (!export) export = find_type_export(module, name);
    if (!export) export = find_bare_type_import(bindings, name);
    if (!export) export = find_namespace_type_import(bindings, name);
    return export ? export->symbol : NULL;
}

// 递归重写一个类型节点里的类型名。泛型实参在 type_arguments，数组/指针在 element_type，
// 函数指针在 parameter_types 和 return_type。
static void rewrite_var_type(
    LoadedModule *module, ImportBinding *bindings, VarTypeNode *type) {
    if (!type) return;

    if (type->struct_name) {
        char *copy = copy_string_or_null(resolve_type_name(module, bindings, type->struct_name, 1));
        if (copy) {
            free(type->struct_name);
            type->struct_name = copy;
        }
    }
    if (type->enum_name) {
        char *copy = copy_string_or_null(resolve_type_name(module, bindings, type->enum_name, 1));
        if (copy) {
            free(type->enum_name);
            type->enum_name = copy;
        }
    }

    for (ASTNode *node = type->type_arguments; node; node = node->next) {
        rewrite_var_type(module, bindings, (VarTypeNode *)node);
    }
    rewrite_var_type(module, bindings, type->element_type);
    for (ASTNode *node = type->parameter_types; node; node = node->next) {
        rewrite_var_type(module, bindings, (VarTypeNode *)node);
    }
    rewrite_var_type(module, bindings, type->return_type);
}

// 把 `Foo.Bar` 里的枚举前缀换成本模块私有符号名。返回新名字（调用方负责释放），
// NULL 表示前缀不是本模块的私有类型、不需要改。
//
// 按**最后一个**点切分：模块前缀本身带点（`__tap_module_0.E`），用第一个点会切错。
static char *resolve_enum_member_name(
    LoadedModule *module, ImportBinding *bindings, const char *name) {
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
    char *renamed = resolve_type_name(module, bindings, prefix, 0);
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
static void rewrite_declared_types(
    LoadedModule *module, ImportBinding *bindings, ProgramNode *program) {
    for (ASTNode *node = program->structs; node; node = node->next) {
        StructNode *structure = (StructNode *)node;
        for (ASTNode *field = structure->fields; field; field = field->next) {
            rewrite_var_type(module, bindings, ((StructFieldNode *)field)->field_type);
        }
    }
    for (ASTNode *node = program->enums; node; node = node->next) {
        EnumNode *enum_node = (EnumNode *)node;
        for (ASTNode *variant = enum_node->variants; variant; variant = variant->next) {
            for (ASTNode *payload = ((EnumVariantNode *)variant)->payload_types;
                 payload; payload = payload->next) {
                rewrite_var_type(module, bindings, (VarTypeNode *)payload);
            }
        }
    }
    for (ASTNode *node = program->constants; node; node = node->next) {
        rewrite_var_type(module, bindings, ((VarDeclNode *)node)->type);
    }
    // impl 的目标类型如果改过名，这里要跟着改，否则约束检查拿到的名字对不上。
    for (ASTNode *node = program->impls; node; node = node->next) {
        ImplNode *impl_node = (ImplNode *)node;
        char *renamed = copy_string_or_null(resolve_type_name(module, bindings, impl_node->type_name, 1));
        if (renamed) {
            free(impl_node->type_name);
            impl_node->type_name = renamed;
        }
    }
    // trait 方法签名里的类型名同样要改，否则和 impl 侧的签名比对会对不上。
    for (ASTNode *node = program->traits; node; node = node->next) {
        TraitNode *trait_node = (TraitNode *)node;
        for (ASTNode *method = trait_node->methods; method; method = method->next) {
            TraitMethodNode *signature = (TraitMethodNode *)method;
            for (ASTNode *type = signature->param_types; type; type = type->next) {
                rewrite_var_type(module, bindings, (VarTypeNode *)type);
            }
            rewrite_var_type(module, bindings, signature->return_type);
        }
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
    ImportBinding *binding = find_namespace_binding(bindings, alias);
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
                    current_module, bindings, ((VarDeclNode *)statement)->type);
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
            case NODE_FIELD_ASSIGNMENT:
                result = rewrite_expression(
                    program,
                    (ASTNode *)((FieldAssignmentNode *)statement)->target,
                    current_module, bindings);
                if (result == 0) {
                    result = rewrite_expression(
                        program,
                        ((FieldAssignmentNode *)statement)->expression,
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
                        resolve_type_name(current_module, bindings, arm->enum_name, 1));
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

// 字符串内建方法名（`s.len()` / `s.byte_at()` / `s.slice()`）。
//
// 模块解析阶段必须认出它们：限定常量上的调用 `conf.NAME.len()` 会被 Parser 拼成一个
// 整串点名字，按常规的「模块.函数」解析会报「module 'mods.conf' has no function
// 'NAME.len'」，而它其实是「常量 NAME 上调 len()」。
static int is_string_builtin_method(const char *name) {
    return strcmp(name, "len") == 0 || strcmp(name, "byte_at") == 0 ||
           strcmp(name, "slice") == 0 || strcmp(name, "char_count") == 0;
}

static int rewrite_call(
    ProgramNode *program, FunctionCallNode *call,
    LoadedModule *current_module, ImportBinding *bindings) {
    // 内建字符串方法不是模块函数，不参与模块导出名称重写。
    if (strcmp(call->name, "__tap_builtin_string_len") == 0 ||
        strcmp(call->name, "__tap_builtin_string_char_count") == 0 ||
        strcmp(call->name, "__tap_builtin_string_byte_at") == 0 ||
        strcmp(call->name, "__tap_builtin_string_slice") == 0) {
        return 0;
    }

    // 枚举成员构造 `E.B(...)`：E 是本模块私有枚举时前缀要跟着改名。改完就是
    // 模块内部符号，不需要再走别名解析。
    char *renamed_member = resolve_enum_member_name(current_module, bindings, call->name);
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

        ImportBinding *binding = find_namespace_binding(bindings, alias);
        if (!binding) {
            // 没有同名导入时，保留给后端按 receiver.method() 方法调用解析。
            free(alias);
            return 0;
        }
        export = find_public_export(binding->module, dot + 1);

        // `conf.NAME.len()`：最后一段是字符串内建、前面还有段时，`NAME.len` 不是模块
        // 函数，而是「限定常量 NAME 上调 len()」。按常量引用的规则改写接收者，
        // 方法名原样留给后端（codegen 的 named_string_method 按最后一个点切分）。
        if (!export) {
            char *last_dot = strrchr(call->name, '.');
            if (last_dot != dot && is_string_builtin_method(last_dot + 1)) {
                size_t receiver_length = (size_t)(last_dot - call->name);
                char *receiver = malloc(receiver_length + 1);
                if (!receiver) {
                    free(alias);
                    fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                    return 1;
                }
                memcpy(receiver, call->name, receiver_length);
                receiver[receiver_length] = '\0';
                char *resolved =
                    resolve_exported_name(receiver, current_module, bindings);
                free(receiver);
                free(alias);
                if (resolved) {
                    size_t size = strlen(resolved) + strlen(last_dot) + 1;
                    char *combined = malloc(size);
                    if (!combined) {
                        free(resolved);
                        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                        return 1;
                    }
                    snprintf(combined, size, "%s%s", resolved, last_dot);
                    free(resolved);
                    free(call->name);
                    call->name = combined;
                }
                return 0;
            }
        }

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
        // 裸调用：先找本模块自己的函数（导出时已改名成 __tap_module_N.f）。「本地定义
        // 与导入同名」这种情况已经在 build_bindings 的 validate_bare_names 里报错了，
        // 走到这里不会有歧义 —— 顺序只是查找顺序，不存在偏向。
        export = find_export(current_module, call->name);
        if (!export) {
            // 入口文件（current_module == NULL）自己的函数保持裸名，后端直接找得到。
            if (current_module == NULL && find_function(program->functions, call->name)) {
                return 0;
            }
            // 再看单成员 / 通配导入（`import mod.fn` / `import mod.*`）：命中就按裸名用。
            export = find_bare_import(bindings, call->name);
        }
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
        case NODE_DEREFERENCE:
            return rewrite_expression(
                program, ((DereferenceNode *)expression)->operand,
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
                resolve_type_name(current_module, bindings, literal->struct_name, 1));
            if (renamed) {
                free(literal->struct_name);
                literal->struct_name = renamed;
            }
            for (ASTNode *node = literal->type_arguments; node; node = node->next) {
                rewrite_var_type(current_module, bindings, (VarTypeNode *)node);
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
                current_module, bindings, ((SizeofNode *)expression)->operand_type);
            return 0;
        case NODE_CAST:
            // `value as Type` 的内层表达式可能包含模块函数调用；目标标量类型本身
            // 通常无需改名，但仍递归处理完整类型以覆盖泛型实例化阶段。
            rewrite_var_type(
                current_module, bindings, ((CastNode *)expression)->target_type);
            return rewrite_expression(
                program, ((CastNode *)expression)->expression,
                current_module, bindings);
        case NODE_FUNCTION_CALL: {
            FunctionCallNode *call = (FunctionCallNode *)expression;
            // 显式泛型实参里的类型名也要跟着改名。
            for (ASTNode *type = call->type_arguments; type; type = type->next) {
                rewrite_var_type(current_module, bindings, (VarTypeNode *)type);
            }
            // `Result<Vec<String>, String>.Ok(...)` 这种**显式枚举类型实参**同样要改。
            // 这块以前漏了没暴露：pub 类型那时保持裸名，不改也对得上；类型带上模块前缀
            // 之后不改就会报 `undefined generic struct type`。
            for (ASTNode *type = call->enum_type_arguments; type; type = type->next) {
                rewrite_var_type(current_module, bindings, (VarTypeNode *)type);
            }
            for (ASTNode *argument = call->arguments; argument; argument = argument->next) {
                int result = rewrite_expression(program, argument, current_module, bindings);
                if (result != 0) return result;
            }
            return rewrite_call(program, call, current_module, bindings);
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
                    resolve_type_name(current_module, bindings, arm->enum_name, 1));
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
        case NODE_FIELD_ACCESS:
            // `接收者.字段`：接收者里可能藏着模块限定名（`mod.make().x`），要递归改写。
            return rewrite_expression(
                program, ((FieldAccessNode *)expression)->object,
                current_module, bindings);
        case NODE_IDENTIFIER: {
            IdentifierNode *identifier = (IdentifierNode *)expression;
            char *dot = strchr(identifier->name, '.');

            // `Enum<T>.Member`（不带调用的形式）里的显式类型实参也要改名。
            // 必须放在下面那些提前 return 之前。
            for (ASTNode *type = identifier->enum_type_arguments;
                 type; type = type->next) {
                rewrite_var_type(current_module, bindings, (VarTypeNode *)type);
            }

            // 不带点的裸名：可能是本模块导出的常量。导出时声明已经被改写成
            // `__tap_module_N.NAME`，这里不跟着改写引用就会找不到符号。只看常量，
            // 函数名和类型名不会以裸名出现在表达式里（见 find_constant_export）。
            // 本模块没有时再看单成员 / 通配导入带进来的 pub 常量。
            if (!dot) {
                ModuleExport *export = find_constant_export(current_module, identifier->name);
                if (!export) {
                    export = find_bare_constant_import(bindings, identifier->name);
                }
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
                resolve_enum_member_name(current_module, bindings, identifier->name);
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
            ImportBinding *binding = find_namespace_binding(bindings, alias);
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
            rewrite_var_type(current_module, bindings, (VarTypeNode *)type);
        }
        rewrite_var_type(current_module, bindings, function->return_type);

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
    if (result == 0) result = build_bindings(context, program, module, &bindings);
    // 类型改名之后，模块自己的类型声明和常量类型标注也要跟着改。
    if (result == 0) rewrite_declared_types(module, bindings, program);
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

int load_modules(ProgramNode *program, const char *input_file,
                 const char *compiler_path, const PreludeNames *prelude_names) {
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
        .next_module_id = 0,
        .error = NULL,
        .prelude_names = prelude_names
    };

    ImportBinding *bindings = NULL;
    int result = build_bindings(&context, program, NULL, &bindings);
    // 入口文件没有自己的模块前缀，但它引用的**导入类型**现在也带前缀了，所以它的
    // struct 字段类型、枚举载荷、常量标注同样要改写。以前 pub 类型保持裸名，这一步
    // 对入口文件是空操作，因此只对模块调；现在漏了就会出现「函数签名改了、声明没改」。
    if (result == 0) rewrite_declared_types(NULL, bindings, program);
    if (result == 0) result = rewrite_functions(program, NULL, bindings);
    if (result == 0) append_functions(program, module_functions);
    // 所有模块的 trait / impl 都合并完、类型名也改完了，这里统一校验签名一致性。
    if (result == 0) result = validate_impl_signatures(program);

    free_bindings(bindings);
    free_modules(context.modules);
    free(root_path);
    free(context.error);
    free_ast((ASTNode *)module_functions);
    return result;
}
