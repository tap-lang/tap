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

// Runtime 路径由 main 初始化一次，之后所有输出模式复用。
static char runtime_static_path[RUN_PATH_MAX];
static char runtime_shared_path[RUN_PATH_MAX];

typedef struct {
    // 本次编译/运行产生的临时目录。
    char directory[RUN_PATH_MAX];
    // 临时 LLVM IR 文件路径。
    char ir_file[RUN_PATH_MAX];
    // 临时目标文件路径。
    char object_file[RUN_PATH_MAX];
    // 临时可执行文件路径。
    char executable_file[RUN_PATH_MAX];
} TempWorkspace;

// 判断 Runtime 候选文件是否存在且可读。
static int runtime_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

// 拼接一个 Runtime 候选路径，只有文件存在时才写入输出缓冲区。
static int find_runtime_file(
    char *output, size_t output_size, const char *directory, const char *filename) {
    int length = snprintf(output, output_size, "%s/%s", directory, filename);
    if (length < 0 || (size_t)length >= output_size || !runtime_file_exists(output)) {
        output[0] = '\0';
        return 0;
    }
    return 1;
}

// 解析编译器可执行文件所在目录，兼容构建目录和安装目录布局。
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

// 缓存 Runtime 静态库和动态库路径，避免后续编译重复扫描文件系统。
void configure_runtime(const char *compiler_path) {
    runtime_static_path[0] = '\0';
    runtime_shared_path[0] = '\0';

    char compiler_dir[RUN_PATH_MAX];
    compiler_directory(compiler_path, compiler_dir, sizeof(compiler_dir));
    const char *configured_dir = getenv("TAP_RUNTIME_PATH");
    const char *directories[3] = {
        configured_dir && *configured_dir ? configured_dir : compiler_dir,
        compiler_dir,
        NULL
    };
    char installed_dir[RUN_PATH_MAX];
    snprintf(installed_dir, sizeof(installed_dir), "%s/../lib", compiler_dir);
    directories[2] = installed_dir;

#ifdef _WIN32
    const char *static_names[] = {"libtap_runtime.a", "tap_runtime.lib", NULL};
    const char *shared_names[] = {"tap_runtime.dll", "libtap_runtime.dll", NULL};
#elif defined(__APPLE__)
    const char *static_names[] = {"libtap_runtime.a", NULL};
    const char *shared_names[] = {"libtap_runtime.dylib", NULL};
#else
    const char *static_names[] = {"libtap_runtime.a", NULL};
    const char *shared_names[] = {"libtap_runtime.so", NULL};
#endif

    // 按配置目录、开发目录、安装目录的优先级搜索 Runtime。
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

// 根据临时目录生成 IR、目标文件和可执行文件路径。
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

// 创建一次编译/运行使用的临时工作目录。
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
                               "%s%stap-XXXXXX", temp_root, separator);
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

// 删除单个临时文件；文件不存在视为成功。
static int remove_temp_file(const char *path) {
    if (!path[0] || remove(path) == 0 || errno == ENOENT) return 0;
    fprintf(stderr, "failed to delete temporary file: %s: %s\n", path, strerror(errno)); // 中文：删除临时文件失败
    return 1;
}

// 清理临时工作目录和其中的中间产物。
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

// 启动子进程并返回其退出码。
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

#if !defined(_WIN32)
// 执行外部查询命令并读取首行输出。
static int read_command_line(const char *command, char *output, size_t output_size) {
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        fprintf(stderr, "failed to run command: %s\n", command); // 中文：执行命令失败
        return 1;
    }
    if (!fgets(output, (int)output_size, pipe)) {
        pclose(pipe);
        fprintf(stderr, "failed to read command output: %s\n", command); // 中文：读取命令输出失败
        return 1;
    }
    int status = pclose(pipe);
    if (status != 0) {
        fprintf(stderr, "command failed: %s\n", command); // 中文：命令执行失败
        return 1;
    }
    output[strcspn(output, "\r\n")] = '\0';
    if (!output[0]) {
        fprintf(stderr, "command output is empty: %s\n", command); // 中文：命令输出为空
        return 1;
    }
    return 0;
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
// 使用 gcc 查询系统启动文件的实际安装路径。
static int gcc_file_path(const char *filename, char *output, size_t output_size) {
    char command[128];
    int length = snprintf(command, sizeof(command), "gcc -print-file-name=%s", filename);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        fprintf(stderr, "gcc query command is too long: %s\n", filename); // 中文：gcc 查询命令过长
        return 1;
    }
    if (read_command_line(command, output, output_size) != 0) return 1;
    if (!runtime_file_exists(output)) {
        fprintf(stderr, "gcc could not find required file: %s\n", filename); // 中文：gcc 找不到必需文件
        return 1;
    }
    return 0;
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
// 在若干候选路径中选择第一个存在的文件。
static const char *first_existing_file(const char *const paths[]) {
    for (size_t index = 0; paths[index]; index++) {
        if (runtime_file_exists(paths[index])) return paths[index];
    }
    return NULL;
}
#endif

// 使用 ld 把目标文件和 Runtime 静态库链接成可执行文件。
static int link_object_file(const char *object_file, const char *exe_file, int static_link) {
    if (!runtime_static_path[0]) {
        fprintf(stderr,
            "error: tap Runtime static library not found; set TAP_RUNTIME_PATH\n"); // 中文：找不到 tap Runtime 静态库；请设置 TAP_RUNTIME_PATH
        return 1;
    }

    int result = 0;
#ifdef _WIN32
    // Windows 上暂时保留 clang 作为链接驱动，避免手动处理 MSVC/MinGW 运行库差异。
    const char *linker = "clang";
    char *const dynamic_argv[] = {(char *)linker, (char *)object_file, runtime_static_path,
                                  "-o", (char *)exe_file, NULL};
    char *const static_argv[] = {(char *)linker, "-static", (char *)object_file,
                                 runtime_static_path, "-o", (char *)exe_file, NULL};
    result = run_process(static_link ? static_argv : dynamic_argv);
#elif defined(__APPLE__)
    if (static_link) {
        fprintf(stderr,
            "error: -static is not supported on macOS because libSystem is only available as a dynamic library\n"); // 中文：macOS 不支持完整静态链接
        return 1;
    }
    // macOS 直接调用 ld 时必须显式传入 SDK、架构和平台版本。
    char sdk_path[RUN_PATH_MAX];
    char sdk_version[64];
    if (read_command_line("xcrun --sdk macosx --show-sdk-path",
                          sdk_path, sizeof(sdk_path)) != 0 ||
        read_command_line("xcrun --sdk macosx --show-sdk-version",
                          sdk_version, sizeof(sdk_version)) != 0) {
        return 1;
    }
#if defined(__aarch64__)
    const char *arch = "arm64";
#elif defined(__x86_64__)
    const char *arch = "x86_64";
#else
    const char *arch = "arm64";
#endif
    char *const argv[] = {
        "ld",
        "-o", (char *)exe_file,
        (char *)object_file,
        runtime_static_path,
        "-lSystem",
        "-syslibroot", sdk_path,
        "-arch", (char *)arch,
        "-platform_version", "macos", sdk_version, sdk_version,
        NULL
    };
    result = run_process(argv);
#else
    if (static_link) {
        // Linux 静态链接需要启动文件，路径由 gcc 查询后交给 ld 使用。
        char crt1[RUN_PATH_MAX];
        char crti[RUN_PATH_MAX];
        char crtn[RUN_PATH_MAX];
        char crtbegin[RUN_PATH_MAX];
        char crtend[RUN_PATH_MAX];
        if (gcc_file_path("crt1.o", crt1, sizeof(crt1)) != 0 ||
            gcc_file_path("crti.o", crti, sizeof(crti)) != 0 ||
            gcc_file_path("crtn.o", crtn, sizeof(crtn)) != 0 ||
            gcc_file_path("crtbeginT.o", crtbegin, sizeof(crtbegin)) != 0 ||
            gcc_file_path("crtend.o", crtend, sizeof(crtend)) != 0) {
            return 1;
        }

        char *const argv[] = {
            "ld",
            "-static",
            "-o", (char *)exe_file,
            crt1,
            crti,
            crtbegin,
            (char *)object_file,
            runtime_static_path,
            "-L/usr/lib/x86_64-linux-gnu",
            "-L/lib/x86_64-linux-gnu",
            "-L/usr/lib/aarch64-linux-gnu",
            "-L/lib/aarch64-linux-gnu",
            "-L/usr/lib64",
            "-L/lib64",
            "-lc",
            crtend,
            crtn,
            NULL
        };
        result = run_process(argv);
    } else {
        // Linux 动态链接需要手动补齐 C 运行时入口文件和动态链接器。
        const char *const crt1_candidates[] = {
            "/usr/lib/x86_64-linux-gnu/Scrt1.o",
            "/usr/lib64/Scrt1.o",
            "/usr/lib/Scrt1.o",
            NULL
        };
        const char *const crti_candidates[] = {
            "/usr/lib/x86_64-linux-gnu/crti.o",
            "/usr/lib64/crti.o",
            "/usr/lib/crti.o",
            NULL
        };
        const char *const crtn_candidates[] = {
            "/usr/lib/x86_64-linux-gnu/crtn.o",
            "/usr/lib64/crtn.o",
            "/usr/lib/crtn.o",
            NULL
        };
        const char *const dynamic_linker_candidates[] = {
            "/lib64/ld-linux-x86-64.so.2",
            "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
            "/lib/ld-linux-aarch64.so.1",
            "/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
            NULL
        };
        const char *crt1 = first_existing_file(crt1_candidates);
        const char *crti = first_existing_file(crti_candidates);
        const char *crtn = first_existing_file(crtn_candidates);
        const char *dynamic_linker = first_existing_file(dynamic_linker_candidates);
        if (!crt1 || !crti || !crtn || !dynamic_linker) {
            fprintf(stderr,
                "error: system CRT files for direct ld linking were not found\n"); // 中文：找不到直接 ld 链接所需的系统 CRT 文件
            return 1;
        }
        char *const argv[] = {
            "ld",
            "-dynamic-linker", (char *)dynamic_linker,
            "-o", (char *)exe_file,
            (char *)crt1,
            (char *)crti,
            (char *)object_file,
            runtime_static_path,
            "-L/usr/lib/x86_64-linux-gnu",
            "-L/lib/x86_64-linux-gnu",
            "-L/usr/lib/aarch64-linux-gnu",
            "-L/lib/aarch64-linux-gnu",
            "-L/usr/lib64",
            "-L/lib64",
            "-lc",
            (char *)crtn,
            NULL
        };
        result = run_process(argv);
    }
#endif
    if (result != 0) {
        fprintf(stderr, "link failed with linker exit code: %d\n", result); // 中文：链接失败，链接器退出码
        return 1;
    }
    return 0;
}

// 先写出临时目标文件，再链接为指定可执行文件。
static int compile_with_temp_object(CodeGenContext *context, const char *exe_file,
                                    const char *object_file, int static_link) {
    if (write_object_to_file(context, object_file) != 0 ||
        link_object_file(object_file, exe_file, static_link) != 0) {
        fprintf(stderr, "failed to generate executable\n"); // 中文：生成可执行文件失败
        return 1;
    }

#ifndef _WIN32
    if (chmod(exe_file, 0755) != 0) {
        fprintf(stderr, "failed to set executable permissions: %s\n", exe_file); // 中文：设置可执行权限失败
        return 1;
    }
#endif

    return 0;
}

// 将当前模块编译为可执行文件。
int compile_to_executable(CodeGenContext *context, const char *exe_file, int static_link) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    int result = compile_with_temp_object(
        context, exe_file, workspace.object_file, static_link);
    if (cleanup_temp_workspace(&workspace) != 0 && result == 0) result = 1;
    return result;
}

// 执行已生成的可执行文件，并转发用户程序参数。
static int execute_file(const char *exe_file, int program_argc, char **program_argv) {
    char *relative_path = NULL;
    const char *exec_path = exe_file;

#ifndef _WIN32
    // POSIX 执行当前目录下的文件时需要显式加 "./"，否则会按 PATH 查找。
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

    // argv[0] 是程序路径，后续元素原样转发给 tap 程序。
    char **argv = malloc(sizeof(char *) * ((size_t)program_argc + 2));
    if (!argv) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        free(relative_path);
        return 1;
    }
    argv[0] = (char *)exec_path;
    for (int i = 0; i < program_argc; i++) {
        argv[i + 1] = program_argv[i];
    }
    argv[program_argc + 1] = NULL;
    int result = run_process(argv);
    free(argv);
    free(relative_path);
    return result;
}

// 编译当前模块并立即执行生成的程序。
int compile_and_run(CodeGenContext *context, const char *exe_file,
                    int program_argc, char **program_argv, int static_link) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    const char *run_file = exe_file ? exe_file : workspace.executable_file;
    int compile_result = compile_with_temp_object(
        context, run_file, workspace.object_file, static_link);
    int result = compile_result;
    if (compile_result == 0) {
        result = execute_file(run_file, program_argc, program_argv);
    }

    if (exe_file && compile_result == 0 && remove(exe_file) != 0 && errno != ENOENT) {
        fprintf(stderr, "failed to delete temporary executable: %s: %s\n", // 中文：删除临时可执行文件失败
                exe_file, strerror(errno));
        if (result == 0) result = 1;
    }
    if (cleanup_temp_workspace(&workspace) != 0 && result == 0) result = 1;
    return result;
}

// 使用 lli 解释执行临时 IR，并加载 Runtime 动态库解析 extern 符号。
int run_with_lli(CodeGenContext *context) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    int result = write_ir_to_file(context, workspace.ir_file);
    if (result != 0) {
        fprintf(stderr, "failed to write temporary IR file\n"); // 中文：写入临时 IR 文件失败
        result = 1;
    } else if (!runtime_shared_path[0]) {
        fprintf(stderr,
            "error: tap Runtime shared library not found; set TAP_RUNTIME_PATH\n"); // 中文：找不到 tap Runtime 共享库；请设置 TAP_RUNTIME_PATH
        result = 1;
    } else {
        // lli 通过加载 Runtime 动态库向 extern 声明暴露符号。
        char load_option[RUN_PATH_MAX + 8];
        snprintf(load_option, sizeof(load_option), "--load=%s", runtime_shared_path);
        char *const argv[] = {"lli", load_option, workspace.ir_file, NULL};
        result = run_process(argv);
    }

    if (cleanup_temp_workspace(&workspace) != 0 && result == 0) result = 1;
    return result;
}
