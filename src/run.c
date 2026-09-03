#include "run.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define RUN_PATH_MAX MAX_PATH
#else
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define RUN_PATH_MAX PATH_MAX
extern char **environ;
#endif

extern int debug;

// Runtime paths are configured once by main and reused by all output modes.
static char runtime_static_path[RUN_PATH_MAX];
static char runtime_shared_path[RUN_PATH_MAX];

typedef struct {
    char directory[RUN_PATH_MAX];
    char ir_file[RUN_PATH_MAX];
    char object_file[RUN_PATH_MAX];
    char executable_file[RUN_PATH_MAX];
} TempWorkspace;

// Runtime lookup only needs regular readable library files.
static int runtime_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

// Build one candidate path and keep it only when the library exists.
static int find_runtime_file(
    char *output, size_t output_size, const char *directory, const char *filename) {
    int length = snprintf(output, output_size, "%s/%s", directory, filename);
    if (length < 0 || (size_t)length >= output_size || !runtime_file_exists(output)) {
        output[0] = '\0';
        return 0;
    }
    return 1;
}

// Resolve the directory of argv[0] for build-tree and installed layouts.
static void compiler_directory(
    const char *compiler_path, char *directory, size_t directory_size) {
    char resolved[RUN_PATH_MAX];
    const char *path = compiler_path;
#ifdef _WIN32
    if (_fullpath(resolved, compiler_path, sizeof(resolved))) path = resolved;
#else
    if (realpath(compiler_path, resolved)) path = resolved;
#endif

    const char *separator = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!separator || (backslash && backslash > separator)) separator = backslash;
#endif
    if (!separator) {
        snprintf(directory, directory_size, ".");
        return;
    }

    size_t length = (size_t)(separator - path);
    if (length >= directory_size) length = directory_size - 1;
    memcpy(directory, path, length);
    directory[length] = '\0';
}

// Cache both Runtime forms so later compile operations avoid repeated filesystem scans.
void configure_runtime(const char *compiler_path) {
    runtime_static_path[0] = '\0';
    runtime_shared_path[0] = '\0';

    char compiler_dir[RUN_PATH_MAX];
    compiler_directory(compiler_path, compiler_dir, sizeof(compiler_dir));
    const char *configured_dir = getenv("4YUE_RUNTIME_PATH");
    const char *directories[3] = {
        configured_dir && *configured_dir ? configured_dir : compiler_dir,
        compiler_dir,
        NULL
    };
    char installed_dir[RUN_PATH_MAX];
    snprintf(installed_dir, sizeof(installed_dir), "%s/../lib", compiler_dir);
    directories[2] = installed_dir;

#ifdef _WIN32
    const char *static_names[] = {"lib4yue_runtime.a", "4yue_runtime.lib", NULL};
    const char *shared_names[] = {"4yue_runtime.dll", "lib4yue_runtime.dll", NULL};
#elif defined(__APPLE__)
    const char *static_names[] = {"lib4yue_runtime.a", NULL};
    const char *shared_names[] = {"lib4yue_runtime.dylib", NULL};
#else
    const char *static_names[] = {"lib4yue_runtime.a", NULL};
    const char *shared_names[] = {"lib4yue_runtime.so", NULL};
#endif

    // Search configured, development, and installed layouts in priority order.
    for (size_t directory_index = 0; directory_index < 3; directory_index++) {
        const char *directory = directories[directory_index];
        if (!runtime_static_path[0]) {
            for (size_t name_index = 0; static_names[name_index]; name_index++) {
                if (find_runtime_file(runtime_static_path, sizeof(runtime_static_path),
                                      directory, static_names[name_index])) {
                    break;
                }
            }
        }
        if (!runtime_shared_path[0]) {
            for (size_t name_index = 0; shared_names[name_index]; name_index++) {
                if (find_runtime_file(runtime_shared_path, sizeof(runtime_shared_path),
                                      directory, shared_names[name_index])) {
                    break;
                }
            }
        }
    }
}

static int build_temp_paths(TempWorkspace *workspace, const char *separator) {
    int ir_length = snprintf(workspace->ir_file, sizeof(workspace->ir_file),
                             "%s%sprogram.ll", workspace->directory, separator);
    int object_length = snprintf(workspace->object_file, sizeof(workspace->object_file),
                                 "%s%sprogram.o", workspace->directory, separator);
#ifdef _WIN32
    const char *executable_name = "program.exe";
#else
    const char *executable_name = "program";
#endif
    int executable_length = snprintf(workspace->executable_file,
        sizeof(workspace->executable_file), "%s%s%s", workspace->directory,
        separator, executable_name);

    if (ir_length < 0 || (size_t)ir_length >= sizeof(workspace->ir_file) ||
        object_length < 0 || (size_t)object_length >= sizeof(workspace->object_file) ||
        executable_length < 0 ||
            (size_t)executable_length >= sizeof(workspace->executable_file)) {
        fprintf(stderr, "temporary file path is too long\n"); // 中文：临时文件路径过长
        return 1;
    }
    return 0;
}

static int create_temp_workspace(TempWorkspace *workspace) {
    memset(workspace, 0, sizeof(*workspace));

#ifdef _WIN32
    char temp_path[RUN_PATH_MAX];
    char temp_name[RUN_PATH_MAX];
    DWORD path_length = GetTempPathA(sizeof(temp_path), temp_path);
    if (path_length == 0 || path_length >= sizeof(temp_path) ||
        GetTempFileNameA(temp_path, "4yu", 0, temp_name) == 0 ||
        !DeleteFileA(temp_name) || !CreateDirectoryA(temp_name, NULL)) {
        fprintf(stderr, "failed to create temporary directory\n"); // 中文：创建临时目录失败
        return 1;
    }
    snprintf(workspace->directory, sizeof(workspace->directory), "%s", temp_name);
    if (build_temp_paths(workspace, "\\") != 0) {
        RemoveDirectoryA(workspace->directory);
        return 1;
    }
#else
    const char *temp_root = getenv("TMPDIR");
    if (!temp_root || !*temp_root) temp_root = "/tmp";

    const char *separator = temp_root[strlen(temp_root) - 1] == '/' ? "" : "/";
    int path_length = snprintf(workspace->directory, sizeof(workspace->directory),
                               "%s%s4yue-XXXXXX", temp_root, separator);
    if (path_length < 0 || (size_t)path_length >= sizeof(workspace->directory) ||
        !mkdtemp(workspace->directory)) {
        fprintf(stderr, "failed to create temporary directory: %s\n", strerror(errno)); // 中文：创建临时目录失败
        workspace->directory[0] = '\0';
        return 1;
    }
    if (build_temp_paths(workspace, "/") != 0) {
        rmdir(workspace->directory);
        workspace->directory[0] = '\0';
        return 1;
    }
#endif

    return 0;
}

static int remove_temp_file(const char *path) {
    if (!path[0] || remove(path) == 0 || errno == ENOENT) return 0;
    fprintf(stderr, "failed to delete temporary file: %s: %s\n", path, strerror(errno)); // 中文：删除临时文件失败
    return 1;
}

static int cleanup_temp_workspace(TempWorkspace *workspace) {
    int result = 0;
    result |= remove_temp_file(workspace->ir_file);
    result |= remove_temp_file(workspace->object_file);
    result |= remove_temp_file(workspace->executable_file);

    if (workspace->directory[0]) {
#ifdef _WIN32
        if (!RemoveDirectoryA(workspace->directory)) {
            fprintf(stderr, "failed to delete temporary directory: %s\n", workspace->directory); // 中文：删除临时目录失败
            result = 1;
        }
#else
        if (rmdir(workspace->directory) != 0 && errno != ENOENT) {
            fprintf(stderr, "failed to delete temporary directory: %s: %s\n", // 中文：删除临时目录失败
                    workspace->directory, strerror(errno));
            result = 1;
        }
#endif
    }
    return result;
}

static int run_process(char *const argv[]) {
    fflush(NULL);

#ifdef _WIN32
    intptr_t result = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
    if (result == -1) {
        fprintf(stderr, "failed to start program: %s: %s\n", argv[0], strerror(errno)); // 中文：启动程序失败
        return 1;
    }
    return (int)result;
#else
    pid_t pid;
    int spawn_result = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (spawn_result != 0) {
        fprintf(stderr, "failed to start program: %s: %s\n", argv[0], // 中文：启动程序失败
                strerror(spawn_result));
        return 1;
    }

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            fprintf(stderr, "failed to wait for program: %s\n", strerror(errno)); // 中文：等待程序结束失败
            return 1;
        }
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

static int link_object_file(const char *object_file, const char *exe_file) {
#ifdef _WIN32
    const char *linker = "clang";
#else
    const char *linker = "cc";
#endif
    if (!runtime_static_path[0]) {
        fprintf(stderr,
            "error: 4yue Runtime static library not found; set 4YUE_RUNTIME_PATH\n"); // 中文：找不到 4yue Runtime 静态库；请设置 4YUE_RUNTIME_PATH
        return 1;
    }
    char *const argv[] = {(char *)linker, (char *)object_file, runtime_static_path,
                          "-o", (char *)exe_file, NULL};
    int result = run_process(argv);
    if (result != 0) {
        fprintf(stderr, "link failed with linker exit code: %d\n", result); // 中文：链接失败，链接器退出码
        return 1;
    }
    return 0;
}

static int compile_with_temp_object(CodeGenContext *context, const char *exe_file,
                                    const char *object_file) {
    if (write_object_to_file(context, object_file) != 0 ||
        link_object_file(object_file, exe_file) != 0) {
        fprintf(stderr, "failed to generate executable\n"); // 中文：生成可执行文件失败
        return 1;
    }

#ifndef _WIN32
    if (chmod(exe_file, 0755) != 0) {
        fprintf(stderr, "failed to set executable permissions: %s\n", exe_file); // 中文：设置可执行权限失败
        return 1;
    }
#endif

    if (debug) printf("可执行文件已生成: %s\n", exe_file);
    return 0;
}

int compile_to_executable(CodeGenContext *context, const char *exe_file) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    int result = compile_with_temp_object(context, exe_file, workspace.object_file);
    if (cleanup_temp_workspace(&workspace) != 0 && result == 0) result = 1;
    return result;
}

static int execute_file(const char *exe_file) {
    char *relative_path = NULL;
    const char *exec_path = exe_file;

#ifndef _WIN32
    if (!strchr(exe_file, '/')) {
        size_t path_size = strlen(exe_file) + 3;
        relative_path = malloc(path_size);
        if (!relative_path) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return 1;
        }
        snprintf(relative_path, path_size, "./%s", exe_file);
        exec_path = relative_path;
    }
#endif

    char *const argv[] = {(char *)exec_path, NULL};
    int result = run_process(argv);
    free(relative_path);
    return result;
}

int compile_and_run(CodeGenContext *context, const char *exe_file) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    const char *run_file = exe_file ? exe_file : workspace.executable_file;
    int compile_result = compile_with_temp_object(context, run_file, workspace.object_file);
    int result = compile_result;
    if (compile_result == 0) result = execute_file(run_file);

    if (exe_file && compile_result == 0 && remove(exe_file) != 0 && errno != ENOENT) {
        fprintf(stderr, "failed to delete temporary executable: %s: %s\n", // 中文：删除临时可执行文件失败
                exe_file, strerror(errno));
        if (result == 0) result = 1;
    }
    if (cleanup_temp_workspace(&workspace) != 0 && result == 0) result = 1;
    return result;
}

int run_with_lli(CodeGenContext *context) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    if (debug) printf("执行程序...\n");

    int result = write_ir_to_file(context, workspace.ir_file);
    if (result != 0) {
        fprintf(stderr, "failed to write temporary IR file\n"); // 中文：写入临时 IR 文件失败
        result = 1;
    } else if (!runtime_shared_path[0]) {
        fprintf(stderr,
            "error: 4yue Runtime shared library not found; set 4YUE_RUNTIME_PATH\n"); // 中文：找不到 4yue Runtime 共享库；请设置 4YUE_RUNTIME_PATH
        result = 1;
    } else {
        // lli exposes symbols from the Runtime shared library to extern declarations.
        char load_option[RUN_PATH_MAX + 8];
        snprintf(load_option, sizeof(load_option), "--load=%s", runtime_shared_path);
        char *const argv[] = {"lli", load_option, workspace.ir_file, NULL};
        result = run_process(argv);
        if (debug) printf("程序执行完毕，返回值: %d\n", result);
    }

    if (cleanup_temp_workspace(&workspace) != 0 && result == 0) result = 1;
    return result;
}
