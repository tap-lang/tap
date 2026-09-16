#include "prelude.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define PRELUDE_PATH_MAX _MAX_PATH
#else
#define PRELUDE_PATH_MAX PATH_MAX
#endif

#include "lexer.h"
#include "parser.h"
#include "helpers.h"

static int file_exists(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    fclose(file);
    return 1;
}

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

static char *compiler_directory(const char *compiler_path) {
    char resolved[PRELUDE_PATH_MAX];
    const char *path = compiler_path;

#ifdef _WIN32
    if (_fullpath(resolved, compiler_path, PRELUDE_PATH_MAX)) path = resolved;
#else
    if (realpath(compiler_path, resolved)) path = resolved;
#endif

    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (!slash) return NULL;

    size_t length = (size_t)(slash - path);
    char *directory = malloc(length + 1);
    if (!directory) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    memcpy(directory, path, length);
    directory[length] = '\0';
    return directory;
}

static char *resolve_prelude_path(const char *compiler_path) {
    const char *std_path = getenv("TAP_STD_PATH");
    if (std_path && std_path[0] != '\0') {
        char *candidate = join_path(std_path, "prelude.tp");
        if (candidate && file_exists(candidate)) return candidate;
        free(candidate);
    }

    if (file_exists("std/prelude.tp")) return copy_string("std/prelude.tp");

    char *directory = compiler_directory(compiler_path);
    if (!directory) return NULL;

    char *candidate = join_path(directory, "../std/prelude.tp");
    if (candidate && file_exists(candidate)) {
        free(directory);
        return candidate;
    }
    free(candidate);

    candidate = join_path(directory, "../share/tap/std/prelude.tp");
    free(directory);
    if (candidate && file_exists(candidate)) return candidate;
    free(candidate);
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

static FunctionNode *find_internal_duplicate(ASTNode *functions) {
    for (ASTNode *left = functions; left; left = left->next) {
        if (left->type != NODE_FUNCTION) continue;
        const char *name = ((FunctionNode *)left)->name;
        FunctionNode *duplicate = find_function(left->next, name);
        if (duplicate) return duplicate;
    }
    return NULL;
}

static FunctionNode *find_cross_duplicate(ASTNode *prelude, ASTNode *user) {
    for (ASTNode *node = prelude; node; node = node->next) {
        if (node->type != NODE_FUNCTION) continue;
        const char *name = ((FunctionNode *)node)->name;
        FunctionNode *duplicate = find_function(user, name);
        if (duplicate) return duplicate;
    }
    return NULL;
}

static void prepend_functions(ProgramNode *program, ProgramNode *prelude) {
    ASTNode *functions = prelude->functions;
    if (!functions) return;

    ASTNode *tail = functions;
    while (tail->next) tail = tail->next;
    tail->next = program->functions;
    program->functions = functions;
    prelude->functions = NULL;
}

int load_prelude(ProgramNode *program, const char *compiler_path) {
    char *path = resolve_prelude_path(compiler_path);
    if (!path) {
        fprintf(stderr,
            "error: standard library prelude.tp not found; run from the project directory or set TAP_STD_PATH\n"); // 中文：找不到标准库 prelude.tp；请从项目目录运行，或设置 TAP_STD_PATH
        return 1;
    }

    Lexer *lexer = create_lexer(path);
    if (!lexer) {
        free(path);
        return 1;
    }
    Parser *parser = create_parser(lexer);
    if (!parser) {
        free_lexer(lexer);
        free(path);
        return 1;
    }

    ProgramNode *prelude = parse_program(parser);
    FunctionNode *duplicate = find_internal_duplicate(program->functions);
    if (!duplicate) duplicate = find_internal_duplicate(prelude->functions);
    if (!duplicate) duplicate = find_cross_duplicate(prelude->functions, program->functions);

    if (duplicate) {
        print_diagnostic(stderr, "error", duplicate->filename,
                         duplicate->line, duplicate->column,
                         "duplicate function definition '%s'", duplicate->name); // 中文：重复函数定义
        free_ast((ASTNode *)prelude);
        free_parser(parser);
        free_lexer(lexer);
        free(path);
        return 1;
    }

    prepend_functions(program, prelude);
    free_ast((ASTNode *)prelude);
    free_parser(parser);
    free_lexer(lexer);
    free(path);
    return 0;
}
