#include "run.h"
#include "helpers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#define RUN_PATH_MAX MAX_PATH
#define PATH_LIST_SEPARATOR ';'
#define TAPM_PROGRAM_NAME "tapm.exe"
#else
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define RUN_PATH_MAX PATH_MAX
#define PATH_LIST_SEPARATOR ':'
#define TAPM_PROGRAM_NAME "tapm"
extern char **environ;
#endif

// Runtime 路径由 main 初始化一次，之后所有输出模式复用。
static char runtime_static_path[RUN_PATH_MAX];
static char runtime_shared_path[RUN_PATH_MAX];

// tap.exe 自己所在的目录（规范化后），同样只算一次。
// 分发包里连接器、compiler-rt builtins、CRT 导入库都按这个位置摆放，
// 所以链接时要先在这里找，找不到才回头问 PATH 和注册表。
static char compiler_install_dir[RUN_PATH_MAX];

// `-v` 的命令回显开关。见 run.h 里的说明。
static int verbose_commands = 0;

void set_verbose_commands(int enabled) {
    verbose_commands = enabled;
}

// 复制字符串。用自己的一份而不是 strdup：MSVC 下 strdup 是弃用符号，
// 要么带 _CRT_SECURE_NO_WARNINGS，要么换名，不值得为这个引入平台分支。
static char *duplicate_string(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    return copy;
}

void link_options_init(LinkOptions *options) {
    options->items = NULL;
    options->count = 0;
    options->capacity = 0;
}

int link_options_add(LinkOptions *options, LinkArgKind kind, const char *value) {
    if (options->count == options->capacity) {
        int capacity = options->capacity ? options->capacity * 2 : 8;
        LinkArg *items = realloc(options->items, sizeof(LinkArg) * (size_t)capacity);
        if (!items) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return 1;
        }
        options->items = items;
        options->capacity = capacity;
    }

    char *copy = duplicate_string(value);
    if (!copy) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }

    options->items[options->count].kind = kind;
    options->items[options->count].value = copy;
    options->count++;
    return 0;
}

int link_options_is_empty(const LinkOptions *options) {
    return !options || options->count == 0;
}

void link_options_free(LinkOptions *options) {
    if (!options) return;
    for (int i = 0; i < options->count; i++) free(options->items[i].value);
    free(options->items);
    link_options_init(options);
}

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

    // 优先问操作系统要可执行文件位置：靠 PATH 调用时 argv[0] 只是裸名字，
    // realpath 解析不出来，装到 <prefix>/bin 之后就找不到 ../lib。
    if (tap_executable_path(resolved, sizeof(resolved))) {
        path = resolved;
    } else {
#ifdef _WIN32
        if (_fullpath(resolved, compiler_path, sizeof(resolved))) path = resolved;
#else
        if (realpath(compiler_path, resolved)) path = resolved;
#endif
    }

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
    snprintf(compiler_install_dir, sizeof(compiler_install_dir), "%s", compiler_dir);
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
#elif defined(__CYGWIN__)
    // CMake 在 Cygwin 平台按约定给共享库加 cyg 前缀，产出 cygtap_runtime.dll；
    // Makefile 在继承 Windows_NT 环境变量时会命名成 tap_runtime.dll，
    // 也有旧构建命名成 libtap_runtime.so。三种构建方式的名字都接受。
    const char *static_names[] = {"libtap_runtime.a", NULL};
    const char *shared_names[] = {
        "cygtap_runtime.dll", "tap_runtime.dll", "libtap_runtime.so", NULL
    };
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
    DWORD path_length = GetTempPathA(sizeof(temp_path), temp_path);
    if (path_length == 0) {
        fprintf(stderr, "failed to resolve temporary directory: Windows error %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }
    if (path_length >= sizeof(temp_path)) {
        fprintf(stderr, "temporary directory path is too long\n");
        return 1;
    }

    // CreateDirectory is atomic. Avoid the GetTempFileName/DeleteFile race when
    // multiple compiler processes create workspaces at the same time.
    DWORD process_id = GetCurrentProcessId();
    ULONGLONG timestamp = GetTickCount64();
    DWORD create_error = ERROR_ALREADY_EXISTS;
    int directory_created = 0;
    for (unsigned int attempt = 0; attempt < 128; attempt++) {
        int name_length = snprintf(
            workspace->directory, sizeof(workspace->directory),
            "%stap-%lu-%llu-%u", temp_path, (unsigned long)process_id,
            (unsigned long long)timestamp, attempt);
        if (name_length < 0 || (size_t)name_length >= sizeof(workspace->directory)) {
            fprintf(stderr, "temporary directory path is too long\n");
            workspace->directory[0] = '\0';
            return 1;
        }
        if (CreateDirectoryA(workspace->directory, NULL)) {
            directory_created = 1;
            break;
        }
        create_error = GetLastError();
        if (create_error != ERROR_ALREADY_EXISTS && create_error != ERROR_FILE_EXISTS) {
            break;
        }
    }
    if (!directory_created) {
        fprintf(stderr, "failed to create temporary directory: Windows error %lu\n",
                (unsigned long)create_error);
        workspace->directory[0] = '\0';
        return 1;
    }
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
    // `-v` 下保留中间产物：回显出来的链接命令里带着目标文件路径，删掉之后
    // 那条命令就没法照着跑一遍了 —— 而照着跑一遍正是 `-v` 的主要用途。
    if (verbose_commands) {
        fprintf(stderr, "tap: keeping temporary files in %s\n", workspace->directory);
        return 0;
    }

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

#ifdef _WIN32
// Quote one argv entry using the parsing rules used by the Microsoft C Runtime.
static char *quote_windows_argument(const char *argument) {
    size_t argument_length = strlen(argument);
    if (argument_length > (((size_t)-1) - 3) / 2) return NULL;

    char *quoted = malloc(argument_length * 2 + 3);
    if (!quoted) return NULL;

    char *output = quoted;
    const char *cursor = argument;
    *output++ = '"';
    while (*cursor) {
        size_t backslash_count = 0;
        while (*cursor == '\\') {
            backslash_count++;
            cursor++;
        }

        if (*cursor == '"') {
            for (size_t i = 0; i < backslash_count * 2 + 1; i++) *output++ = '\\';
            *output++ = *cursor++;
        } else if (!*cursor) {
            for (size_t i = 0; i < backslash_count * 2; i++) *output++ = '\\';
        } else {
            for (size_t i = 0; i < backslash_count; i++) *output++ = '\\';
            *output++ = *cursor++;
        }
    }
    *output++ = '"';
    *output = '\0';
    return quoted;
}
#endif

// 启动子进程并返回其退出码。
static int run_process(char *const argv[]) {
    fflush(NULL);

#ifdef _WIN32
    size_t argument_count = 0;
    while (argv[argument_count]) argument_count++;
    if (argument_count > (((size_t)-1) / sizeof(char *)) - 1) {
        fprintf(stderr, "too many process arguments\n");
        return 1;
    }

    char **quoted_argv = calloc(argument_count + 1, sizeof(char *));
    if (!quoted_argv) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    for (size_t i = 0; i < argument_count; i++) {
        quoted_argv[i] = quote_windows_argument(argv[i]);
        if (!quoted_argv[i]) {
            for (size_t j = 0; j < i; j++) free(quoted_argv[j]);
            free(quoted_argv);
            fprintf(stderr, "Out of memory\n");
            return 1;
        }
    }

    intptr_t result = _spawnvp(
        _P_WAIT, argv[0], (const char *const *)quoted_argv);
    int spawn_error = errno;
    for (size_t i = 0; i < argument_count; i++) free(quoted_argv[i]);
    free(quoted_argv);
    if (result == -1) {
        fprintf(stderr, "failed to start program: %s: %s\n",
                argv[0], strerror(spawn_error)); // 中文：启动程序失败
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

// 判断一个参数在 shell 里能不能原样写出来。
//
// 用白名单而不是黑名单：链接命令行里出现的都是路径和选项，除了这几十个字符
// 之外一律加引号。`~` 故意不在白名单里 —— 它在词首会被 shell 展开成家目录。
static int argument_needs_quoting(const char *argument) {
    if (!argument[0]) return 1;
    for (const char *cursor = argument; *cursor; cursor++) {
        unsigned char character = (unsigned char)*cursor;
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9')) {
            continue;
        }
        if (strchr("-_./=:+,@%", character)) continue;
        return 1;
    }
    return 0;
}

// 按各自平台的引号规则打印一个参数，让 `-v` 的输出能直接复制粘贴到终端。
static void print_command_argument(const char *argument) {
    if (!argument_needs_quoting(argument)) {
        fputs(argument, stderr);
        return;
    }

#ifdef _WIN32
    char *quoted = quote_windows_argument(argument);
    if (!quoted) {
        fputs(argument, stderr);
        return;
    }
    fputs(quoted, stderr);
    free(quoted);
#else
    fputc('\'', stderr);
    for (const char *cursor = argument; *cursor; cursor++) {
        // POSIX shell 里单引号内部不能出现单引号，只能闭合再拼一个转义过的。
        if (*cursor == '\'') {
            fputs("'\\''", stderr);
        } else {
            fputc(*cursor, stderr);
        }
    }
    fputc('\'', stderr);
#endif
}

// 回显一条实际执行的外部命令，格式 `tap: <标签>: <命令>`。
static void print_command_line(const char *label, char *const argv[]) {
    if (!verbose_commands) return;

    fprintf(stderr, "tap: %s:", label);
    for (int i = 0; argv[i]; i++) {
        fputc(' ', stderr);
        print_command_argument(argv[i]);
    }
    fputc('\n', stderr);
}

// path 是不是可直接执行的文件。
static int is_executable_file(const char *path) {
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

// 在 PATH 里找可执行文件；找到返回 1 并把完整路径写进 output。
static int find_program_on_path(const char *name, char *output, size_t output_size) {
    const char *path = getenv("PATH");
    if (!path || path[0] == '\0') return 0;

    const char *start = path;
    for (;;) {
        const char *end = strchr(start, PATH_LIST_SEPARATOR);
        size_t length = end ? (size_t)(end - start) : strlen(start);
        if (length > 0) {
            char candidate[RUN_PATH_MAX];
            int written = snprintf(
                candidate, sizeof(candidate), "%.*s/%s", (int)length, start, name);
            if (written > 0 && (size_t)written < sizeof(candidate) &&
                is_executable_file(candidate)) {
                snprintf(output, output_size, "%s", candidate);
                return 1;
            }
        }
        if (!end) return 0;
        start = end + 1;
    }
}

// 把 `tap new ...` 转发给 tapm。
//
// tap 自己不实现脚手架——那是包管理器的活，模板也只该有一份（在 tapm 里）。
// 这里只做转发：参数原样透传、退出码原样返回，所以 `tap new demo` 和
// `tapm new demo` 完全等价。
//
// tapm 是独立安装的二进制，没装是最可能的失败，所以提前把查找做完，好给出
// 「装什么、放哪」这种能照着做的提示，而不是一句看不出所以然的 spawn 失败。
int run_tapm(const char *compiler_path, int forwarded_argc, char **forwarded_argv) {
    char program[RUN_PATH_MAX];
    program[0] = '\0';

    // 先看编译器自己旁边：安装布局下两个可执行文件都在 <prefix>/bin。
    char directory[RUN_PATH_MAX];
    directory[0] = '\0';
    compiler_directory(compiler_path, directory, sizeof(directory));
    if (directory[0]) {
        char beside[RUN_PATH_MAX];
        int written = snprintf(
            beside, sizeof(beside), "%s/%s", directory, TAPM_PROGRAM_NAME);
        if (written > 0 && (size_t)written < sizeof(beside) &&
            is_executable_file(beside)) {
            snprintf(program, sizeof(program), "%s", beside);
        }
    }

    // 再看 PATH。
    if (!program[0]) {
        find_program_on_path(TAPM_PROGRAM_NAME, program, sizeof(program));
    }

    if (!program[0]) {
        fprintf(stderr,
            "error: cannot find `%s`; `tap new` is provided by the tapm package manager\n", // 中文：找不到 tapm
            TAPM_PROGRAM_NAME);
        fprintf(stderr,
            "install tapm and put it on PATH, or run `tapm new` directly\n"); // 中文：装上 tapm 并放进 PATH
        return 1;
    }

    char **argv = malloc(sizeof(char *) * ((size_t)forwarded_argc + 2));
    if (!argv) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    argv[0] = program;
    for (int i = 0; i < forwarded_argc; i++) {
        argv[i + 1] = forwarded_argv[i];
    }
    argv[forwarded_argc + 1] = NULL;

    int result = run_process(argv);
    free(argv);
    return result;
}

#if defined(__APPLE__)
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

// 优先读取测试进程预先查询的 SDK 信息，避免批量链接时重复启动 xcrun。
static int macos_sdk_value(
    const char *environment_name, const char *command,
    char *output, size_t output_size) {
    const char *configured = getenv(environment_name);
    if (!configured || !*configured) {
        return read_command_line(command, output, output_size);
    }
    int length = snprintf(output, output_size, "%s", configured);
    if (length < 0 || (size_t)length >= output_size) {
        fprintf(stderr, "%s is too long\n", environment_name);
        return 1;
    }
    return 0;
}
#endif

// 链接器 argv 的动态构建器。
//
// 以前每个平台都是一份定长的 `char *const argv[] = {...}`。加了 -l / -L / -Wl,
// 之后参数个数在编译期未知，只能动态拼。条目一律复制一份：object_file / exe_file /
// runtime 路径都是借用的，混在一起会让释放逻辑变成猜谜。
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
    int failed;
} LinkArgv;

static void link_argv_init(LinkArgv *argv) {
    argv->items = NULL;
    argv->count = 0;
    argv->capacity = 0;
    argv->failed = 0;
}

static int link_argv_push(LinkArgv *argv, const char *argument) {
    if (argv->failed) return 1;

    // +2：末尾要留 NULL 终结符，push 之后立刻写。
    if (argv->count + 2 > argv->capacity) {
        size_t capacity = argv->capacity ? argv->capacity * 2 : 16;
        char **items = realloc(argv->items, sizeof(char *) * capacity);
        if (!items) {
            argv->failed = 1;
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            return 1;
        }
        argv->items = items;
        argv->capacity = capacity;
    }

    char *copy = duplicate_string(argument);
    if (!copy) {
        argv->failed = 1;
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }

    argv->items[argv->count++] = copy;
    argv->items[argv->count] = NULL;
    return 0;
}

// 追加「前缀 + 值」，用于 -l<名字>、-L<目录>、/LIBPATH:<目录> 这类拼在一起的参数。
static int link_argv_push_joined(LinkArgv *argv, const char *prefix, const char *value) {
    size_t length = strlen(prefix) + strlen(value) + 1;
    char *joined = malloc(length);
    if (!joined) {
        argv->failed = 1;
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    snprintf(joined, length, "%s%s", prefix, value);
    int result = link_argv_push(argv, joined);
    free(joined);
    return result;
}

static void link_argv_free(LinkArgv *argv) {
    for (size_t i = 0; i < argv->count; i++) free(argv->items[i]);
    free(argv->items);
    argv->items = NULL;
    argv->count = 0;
    argv->capacity = 0;
}

// 把 `-Wl,a,b` 里逗号分隔的裸参数逐个压进 argv。
//
// 只对「直接调链接器」的路径成立（macOS 的 ld、MinGW 的 ld.lld）：它们是链接器本身，
// 不认驱动前缀。走 cc / gcc / clang 驱动时整串原样透传，驱动自己会拆。
static int link_argv_push_raw_split(LinkArgv *argv, const char *value) {
    for (const char *segment = value; *segment;) {
        const char *comma = strchr(segment, ',');
        size_t length = comma ? (size_t)(comma - segment) : strlen(segment);
        if (length > 0) {
            char *part = malloc(length + 1);
            if (!part) {
                argv->failed = 1;
                fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
                return 1;
            }
            memcpy(part, segment, length);
            part[length] = '\0';
            link_argv_push(argv, part);
            free(part);
        }
        if (!comma) break;
        segment = comma + 1;
    }
    return 0;
}

// 直接调用 ld 系列（macOS 的 ld、MinGW 下的 ld.lld）时的参数展开。
//
// `-Wl,` 是驱动的前缀，ld 自己不认识，所以裸参数要拆成逗号分隔的多个参数；
// `-l` / `-L` 两种写法 ld 和驱动都认，原样拼上就行。
static void push_link_options_direct_ld(LinkArgv *argv, const LinkOptions *options) {
    if (link_options_is_empty(options)) return;

    for (int i = 0; i < options->count; i++) {
        const LinkArg *argument = &options->items[i];
        switch (argument->kind) {
        case LINK_ARG_LIBRARY:
            link_argv_push_joined(argv, "-l", argument->value);
            break;
        case LINK_ARG_SEARCH_DIR:
            link_argv_push_joined(argv, "-L", argument->value);
            break;
        case LINK_ARG_RAW:
            // `-Wl,-rpath,/usr/local/lib` 在 ld 下是 `-rpath /usr/local/lib` 两个参数。
            link_argv_push_raw_split(argv, argument->value);
            break;
        }
    }
}

// 把用户链接参数展开成当前平台链接器认识的 argv 片段。
//
// 三种平台三种写法，所以按平台只保留一份实现：
//   - 驱动（cc / gcc / clang）：-l / -L / -Wl, 三种写法驱动都认，原样透传
//   - macOS 的 ld：直接调 ld，见 push_link_options_direct_ld
//   - MSVC 的 clang-cl：库按 .lib 习惯当输入文件，其余走 /link 交给 link.exe
#if defined(__APPLE__)
static void push_link_options(LinkArgv *argv, const LinkOptions *options) {
    push_link_options_direct_ld(argv, options);
}
#elif defined(_WIN32) && !defined(__MINGW32__)
// 后缀比较忽略大小写：`-lSQLITE3.LIB` 不该被拼成 `SQLITE3.LIB.lib`。
static int has_suffix_ignore_case(const char *text, const char *suffix) {
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    if (text_length < suffix_length) return 0;

    const char *tail = text + (text_length - suffix_length);
    for (size_t i = 0; i < suffix_length; i++) {
        char left = tail[i];
        char right = suffix[i];
        if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        if (left != right) return 0;
    }
    return 1;
}

static int link_argv_push_suffixed(LinkArgv *argv, const char *value, const char *suffix) {
    size_t value_length = strlen(value);
    size_t suffix_length = strlen(suffix);
    char *joined = malloc(value_length + suffix_length + 1);
    if (!joined) {
        argv->failed = 1;
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return 1;
    }
    memcpy(joined, value, value_length);
    memcpy(joined + value_length, suffix, suffix_length + 1);
    int result = link_argv_push(argv, joined);
    free(joined);
    return result;
}

static void push_link_options(LinkArgv *argv, const LinkOptions *options) {
    if (link_options_is_empty(options)) return;

    // 库：MSVC 体系里 `-lsqlite3` 对应的文件名是 sqlite3.lib。
    int needs_linker_section = 0;
    for (int i = 0; i < options->count; i++) {
        const LinkArg *argument = &options->items[i];
        if (argument->kind == LINK_ARG_LIBRARY) {
            if (has_suffix_ignore_case(argument->value, ".lib")) {
                link_argv_push(argv, argument->value);
            } else {
                link_argv_push_suffixed(argv, argument->value, ".lib");
            }
        } else {
            needs_linker_section = 1;
        }
    }

    // 搜索路径和裸参数只能交给 link.exe 自己解释。/link 之后的一切都归它，
    // 所以这一段必须放在 clang-cl 自己的选项之后。
    if (!needs_linker_section) return;

    link_argv_push(argv, "/link");
    for (int i = 0; i < options->count; i++) {
        const LinkArg *argument = &options->items[i];
        if (argument->kind == LINK_ARG_SEARCH_DIR) {
            link_argv_push_joined(argv, "/LIBPATH:", argument->value);
        } else if (argument->kind == LINK_ARG_RAW) {
            // 这里的裸参数要按 link.exe 的写法给，例如 -Wl,/DEBUG
            link_argv_push(argv, argument->value);
        }
    }
}

// 直接调用 lld-link 时的参数展开：没有 clang-cl 那一层，也就没有 `/link` 分隔符，
// 库和 /LIBPATH: 都是 lld-link 自己的参数。
static void push_link_options_lld_link(LinkArgv *argv, const LinkOptions *options) {
    if (link_options_is_empty(options)) return;

    for (int i = 0; i < options->count; i++) {
        const LinkArg *argument = &options->items[i];
        switch (argument->kind) {
        case LINK_ARG_LIBRARY:
            if (has_suffix_ignore_case(argument->value, ".lib")) {
                link_argv_push(argv, argument->value);
            } else {
                link_argv_push_suffixed(argv, argument->value, ".lib");
            }
            break;
        case LINK_ARG_SEARCH_DIR:
            link_argv_push_joined(argv, "/LIBPATH:", argument->value);
            break;
        case LINK_ARG_RAW:
            // lld-link 和 link.exe 的参数写法一致，原样给。
            link_argv_push(argv, argument->value);
            break;
        }
    }
}
#else
static void push_link_options(LinkArgv *argv, const LinkOptions *options) {
    if (link_options_is_empty(options)) return;

    for (int i = 0; i < options->count; i++) {
        const LinkArg *argument = &options->items[i];
        switch (argument->kind) {
        case LINK_ARG_LIBRARY:
            link_argv_push_joined(argv, "-l", argument->value);
            break;
        case LINK_ARG_SEARCH_DIR:
            link_argv_push_joined(argv, "-L", argument->value);
            break;
        case LINK_ARG_RAW:
            // 驱动会把 -Wl, 后面的整串按逗号切开转给链接器，原样带前缀就行。
            link_argv_push_joined(argv, "-Wl,", argument->value);
            break;
        }
    }
}
#endif

#if defined(_WIN32)
// 取路径的父目录。这里不能用 compiler_directory —— 它会去问操作系统
// 「当前可执行文件在哪」，对 ld.lld / lld-link 这种外部程序来说问错了对象。
static int split_parent_directory(
    const char *path, char *directory, size_t directory_size) {
    const char *separator = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    if (!separator) return 0;

    size_t length = (size_t)(separator - path);
    if (length == 0 || length >= directory_size) return 0;
    memcpy(directory, path, length);
    directory[length] = '\0';
    return 1;
}
#endif

#if defined(_WIN32) && defined(__MINGW32__)
// 在 MinGW 下找「能直接调用的 ld.lld」和它配套的 CRT 目录。
//
// 为什么要绕开 gcc：gcc 驱动除了拼链接行什么都不做，却顺带把 libgcc 拖了进来。
// Runtime 已经自带 __main / ___chkstk_ms / i128 的除法辅助符号，链接行里不再需要
// -lgcc，于是可以省掉 gcc 这一层依赖（没装 gcc 的机器也能链接）。
//
// crt2.o、crtbegin.o、libmingw32.a 等都在同一个 lib 目录里，但不同发行版相对 bin
// 的位置不一样：MSYS2 是 <prefix>/lib，w64devkit 是 <prefix>/x86_64-w64-mingw32/lib。
// 所以按 ld.lld 和 gcc 各自的 bin 目录反推，逐个探测到 crt2.o 为止。
static int find_mingw_link_tools(char *linker_path, size_t linker_path_size,
                                 char *crt_directory, size_t crt_directory_size) {
    char lld_path[RUN_PATH_MAX];
    int found_lld = 0;

    // ① tap.exe 同目录的 ld.lld.exe（分发包布局：bin\ld.lld.exe + lib\ 里的 CRT）。
    if (compiler_install_dir[0]) {
        char beside[RUN_PATH_MAX];
        int written = snprintf(beside, sizeof(beside), "%s/ld.lld.exe", compiler_install_dir);
        if (written > 0 && (size_t)written < sizeof(beside) && is_executable_file(beside)) {
            snprintf(lld_path, sizeof(lld_path), "%s", beside);
            found_lld = 1;
        }
    }

    // ② PATH 上的 ld.lld.exe（装了 MSYS2 / LLVM 的机器）。
    if (!found_lld && !find_program_on_path("ld.lld.exe", lld_path, sizeof(lld_path))) return 0;

    char roots[2][RUN_PATH_MAX];
    int root_count = 0;
    char root[RUN_PATH_MAX];
    if (split_parent_directory(lld_path, root, sizeof(root))) {
        snprintf(roots[root_count++], RUN_PATH_MAX, "%s", root);
    }
    // ld.lld 可能来自独立的 LLVM 安装，跟 MinGW 的 lib 目录不在一棵树上，
    // 所以 gcc 所在目录也当一个候选根（探测不到 crt2.o 时自然会被跳过）。
    char gcc_path[RUN_PATH_MAX];
    if (find_program_on_path("gcc.exe", gcc_path, sizeof(gcc_path)) &&
        split_parent_directory(gcc_path, root, sizeof(root))) {
        int duplicate = 0;
        for (int i = 0; i < root_count; i++) {
            if (strcmp(roots[i], root) == 0) duplicate = 1;
        }
        if (!duplicate && root_count < 2) snprintf(roots[root_count++], RUN_PATH_MAX, "%s", root);
    }

    static const char *const layouts[] = {
        "%s/../lib", "%s/../x86_64-w64-mingw32/lib", NULL
    };
    for (int root_index = 0; root_index < root_count; root_index++) {
        for (int layout_index = 0; layouts[layout_index]; layout_index++) {
            char candidate[RUN_PATH_MAX];
            int length = snprintf(candidate, sizeof(candidate), layouts[layout_index],
                                  roots[root_index]);
            if (length <= 0 || (size_t)length >= sizeof(candidate)) continue;

            char crt_path[RUN_PATH_MAX];
            length = snprintf(crt_path, sizeof(crt_path), "%s/crt2.o", candidate);
            if (length <= 0 || (size_t)length >= sizeof(crt_path)) continue;
            if (!runtime_file_exists(crt_path)) continue;

            // 路径里带 .. 也能用，但写进命令和报错信息里很难看，顺手规范化一次。
            char normalized[RUN_PATH_MAX];
            if (_fullpath(normalized, candidate, sizeof(normalized))) {
                snprintf(crt_directory, crt_directory_size, "%s", normalized);
            } else {
                snprintf(crt_directory, crt_directory_size, "%s", candidate);
            }
            snprintf(linker_path, linker_path_size, "%s", lld_path);
            return 1;
        }
    }
    return 0;
}
#endif

#if defined(_WIN32) && !defined(__MINGW32__)
// compiler-rt builtins 里的架构名（库文件名是 clang_rt.builtins-<arch>.lib）。
static const char *msvc_builtins_arch(void) {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_IX86) || defined(__i386__)
    return "i386";
#else
    return "x86_64";
#endif
}

// lld-link 的 /machine 参数。
static const char *msvc_machine_option(void) {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "/machine:arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "/machine:x86";
#else
    return "/machine:x64";
#endif
}

// 在 MSVC 环境下找「能直接调用的 lld-link」和它配套的 compiler-rt builtins。
//
// 为什么要绕开 clang-cl：和 MinGW 那边同理 —— clang-cl 只是个参数翻译器，真正的
// 链接器是 lld-link（或 link.exe）。直接调就少一层依赖，链接行也完全看得见。
//
// 代价是 clang-cl 平时替我们补的默认清单要自己给：libcmt（/MT 的静态 CRT）、
// oldnames（旧名兼容符号）、/subsystem、/machine。i128 的 __divti3 等仍然只能来自
// compiler-rt —— cl.exe 不支持 __int128，Runtime 编不出那四个助手。
//
// builtins 的位置是 <llvm>/lib/clang/<ver>/lib/windows/clang_rt.builtins-<arch>.lib，
// 版本号随 LLVM 版本变，所以从 lld-link 所在目录反推、遍历版本目录。
//
// 查找顺序：先看 tap.exe 旁边（分发包把 lld-link.exe 和 tap.exe 一起放在 bin\，
// builtins 放在 lib\compiler-rt\），再走 PATH + 目录反推。分发包要能在没装 LLVM
// 的机器上工作，这一步不能少。
static int find_msvc_lld_link(char *linker_path, size_t linker_path_size,
                              char *builtins_path, size_t builtins_path_size) {
    char lld_path[RUN_PATH_MAX];
    int found_lld = 0;

    // ① tap.exe 同目录的 lld-link.exe（分发包布局）。
    if (compiler_install_dir[0]) {
        char beside[RUN_PATH_MAX];
        int written = snprintf(beside, sizeof(beside), "%s/lld-link.exe", compiler_install_dir);
        if (written > 0 && (size_t)written < sizeof(beside) && is_executable_file(beside)) {
            snprintf(lld_path, sizeof(lld_path), "%s", beside);
            found_lld = 1;
        }
    }

    // ② PATH 上的 lld-link.exe（装了 LLVM 的机器）。
    if (!found_lld && !find_program_on_path("lld-link.exe", lld_path, sizeof(lld_path))) return 0;

    // 分发包的 builtins 位置是固定的，不必再按 LLVM 版本目录去猜。
    if (compiler_install_dir[0]) {
        char bundled[RUN_PATH_MAX];
        int written = snprintf(bundled, sizeof(bundled),
                               "%s/../lib/compiler-rt/clang_rt.builtins-%s.lib",
                               compiler_install_dir, msvc_builtins_arch());
        if (written > 0 && (size_t)written < sizeof(bundled) && runtime_file_exists(bundled)) {
            if (!_fullpath(builtins_path, bundled, builtins_path_size)) {
                snprintf(builtins_path, builtins_path_size, "%s", bundled);
            }
            snprintf(linker_path, linker_path_size, "%s", lld_path);
            return 1;
        }
    }

    char bin_dir[RUN_PATH_MAX];
    if (!split_parent_directory(lld_path, bin_dir, sizeof(bin_dir))) return 0;

    char clang_root[RUN_PATH_MAX];
    int length = snprintf(clang_root, sizeof(clang_root), "%s/../lib/clang", bin_dir);
    if (length <= 0 || (size_t)length >= sizeof(clang_root)) return 0;

    char pattern[RUN_PATH_MAX];
    length = snprintf(pattern, sizeof(pattern), "%s/*", clang_root);
    if (length <= 0 || (size_t)length >= sizeof(pattern)) return 0;

    WIN32_FIND_DATAA entry;
    HANDLE handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) return 0;

    const char *arch = msvc_builtins_arch();
    int found = 0;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (entry.cFileName[0] == '.') continue;

        char candidate[RUN_PATH_MAX];
        length = snprintf(candidate, sizeof(candidate),
                          "%s/%s/lib/windows/clang_rt.builtins-%s.lib",
                          clang_root, entry.cFileName, arch);
        if (length <= 0 || (size_t)length >= sizeof(candidate)) continue;
        if (!runtime_file_exists(candidate)) continue;

        // 路径里带 .. 也能用，但写进命令和报错信息里很难看，顺手规范化一次。
        char normalized[RUN_PATH_MAX];
        if (_fullpath(normalized, candidate, sizeof(normalized))) {
            snprintf(builtins_path, builtins_path_size, "%s", normalized);
        } else {
            snprintf(builtins_path, builtins_path_size, "%s", candidate);
        }
        found = 1;
        break;
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);

    if (!found) return 0;
    char normalized_lld[RUN_PATH_MAX];
    if (_fullpath(normalized_lld, lld_path, sizeof(normalized_lld))) {
        snprintf(linker_path, linker_path_size, "%s", normalized_lld);
    } else {
        snprintf(linker_path, linker_path_size, "%s", lld_path);
    }
    return 1;
}
#endif

// 链接失败时补一句能照着做的提示。链接器自己只会说 `cannot find -lsqlite3`，
// 不会告诉你下一步该干什么。
static void report_link_failure(int status, const LinkOptions *link_options) {
    fprintf(stderr, "link failed with linker exit code: %d\n", status); // 中文：链接失败，链接器退出码

    if (link_options_is_empty(link_options)) return;

    int has_library = 0;
    for (int i = 0; i < link_options->count; i++) {
        if (link_options->items[i].kind == LINK_ARG_LIBRARY) has_library = 1;
    }
    if (!has_library) return;

    fprintf(stderr, "note: requested libraries:");
    for (int i = 0; i < link_options->count; i++) {
        const LinkArg *argument = &link_options->items[i];
        if (argument->kind == LINK_ARG_LIBRARY) {
            fprintf(stderr, " -l%s", argument->value);
        }
    }
    fprintf(stderr, "\n");
    fprintf(stderr,
        "note: check that the library is installed and that -L points at the directory "
        "containing it\n"); // 中文：检查库是否已安装，-L 是否指向库所在目录
}

// 工具链整个不在 PATH 上时的提示。
//
// 直接 spawn 只会得到 `clang-cl: No such file or directory` —— 从这行看不出缺的是
// 「编译器」还是「链接器」，也看不出该装什么。分发出去的 tap 只带 LLVM-C.dll 和
// Runtime，本来就不含工具链，所以这条提示必须自己说清楚缺什么、怎么补。
static int report_missing_linker(const char *tried, const char *hint) {
    fprintf(stderr, "error: no linker toolchain found (tried: %s)\n", tried); // 中文：找不到可用的链接工具链
    fprintf(stderr, "note: %s\n", hint);
    fprintf(stderr, "note: run with -v to see the full link command\n"); // 中文：加 -v 可查看完整链接命令
    return 1;
}

// 把目标文件和 Runtime 静态库链接成可执行文件。
static int link_object_file(const char *object_file, const char *exe_file, int static_link,
                            const LinkOptions *link_options) {
    if (!runtime_static_path[0]) {
        fprintf(stderr,
            "error: tap Runtime static library not found; set TAP_RUNTIME_PATH\n"); // 中文：找不到 tap Runtime 静态库；请设置 TAP_RUNTIME_PATH
        return 1;
    }

    LinkArgv argv;
    link_argv_init(&argv);

#if defined(_WIN32) && defined(__MINGW32__)
    // MinGW-w64（目标三元组 x86_64-w64-windows-gnu）：优先用 ld.lld 直接链接。
    //
    // 不用 clang-cl —— 它按 MSVC 目标去找 MSVC 的 CRT 和 .lib，和 MinGW 的
    // msvcrt/UCRT 体系对不上。也不首选 gcc 驱动 —— 它会把 libgcc 拖进来，而 Runtime
    // 已经自带 __main / ___chkstk_ms / i128 的除法辅助符号，链接行里根本不需要 -lgcc。
    // 直接调 ld.lld 就少了一层对 gcc 的依赖。
    // 找不到 ld.lld（老工具链、只有 gcc 的环境）时退回 gcc，行为跟以前一致。
    char lld_path[RUN_PATH_MAX];
    char crt_dir[RUN_PATH_MAX];
    if (find_mingw_link_tools(lld_path, sizeof(lld_path), crt_dir, sizeof(crt_dir))) {
        link_argv_push(&argv, lld_path);
#if defined(__x86_64__)
        // ld.lld 默认能按输入文件的机器类型自己判断，显式给出更稳。
        link_argv_push(&argv, "-m");
        link_argv_push(&argv, "i386pep");
#endif
        link_argv_push(&argv, static_link ? "-Bstatic" : "-Bdynamic");
        link_argv_push(&argv, "-o");
        link_argv_push(&argv, exe_file);
        // crt2.o 必须在最前面：它带入口点，并且引用 __main（由 Runtime 提供）。
        link_argv_push_joined(&argv, crt_dir, "/crt2.o");
        link_argv_push_joined(&argv, crt_dir, "/crtbegin.o");
        link_argv_push_joined(&argv, "-L", crt_dir);
        link_argv_push(&argv, object_file);
        link_argv_push(&argv, runtime_static_path);
        // 用户库排在 Runtime 之后、系统库之前：静态链接的符号解析是单向的。
        push_link_options_direct_ld(&argv, link_options);
        // 系统库。ws2_32 提供 winsock 符号；其余是 MinGW 的启动代码和 CRT 例程，
        // 顺序照抄 gcc 的默认链接行。
        link_argv_push(&argv, "-lws2_32");
        link_argv_push(&argv, "-lmingw32");
        link_argv_push(&argv, "-lmingwex");
        link_argv_push(&argv, "-lmsvcrt");
        link_argv_push(&argv, "-lkernel32");
        link_argv_push(&argv, "-lpthread");
        link_argv_push(&argv, "-ladvapi32");
        link_argv_push(&argv, "-lshell32");
        link_argv_push(&argv, "-luser32");
        // 默认清单让生成的 exe 带上清单资源（高 DPI、UCRT 依赖都靠它）；
        // 有些工具链没这个文件，缺了也不影响运行。crtend.o 必须收在所有目标文件之后。
        char manifest[RUN_PATH_MAX];
        int manifest_length = snprintf(manifest, sizeof(manifest), "%s/default-manifest.o", crt_dir);
        if (manifest_length > 0 && (size_t)manifest_length < sizeof(manifest) &&
            runtime_file_exists(manifest)) {
            link_argv_push(&argv, manifest);
        }
        link_argv_push_joined(&argv, crt_dir, "/crtend.o");
    } else {
        // 退回 gcc 驱动：i128 的 __divti3 等符号由 libgcc 提供；MinGW 没有独立的
        // libm（pow 在 CRT 里）。
        const char *linker = "gcc";
        char gcc_path[RUN_PATH_MAX];
        if (!find_program_on_path("gcc.exe", gcc_path, sizeof(gcc_path))) {
            link_argv_free(&argv);
            return report_missing_linker("ld.lld, gcc",
                "install a MinGW-w64 toolchain (it provides gcc and ld) and put its bin "
                "directory on PATH");
        }
        link_argv_push(&argv, linker);
        if (static_link) link_argv_push(&argv, "-static");
        link_argv_push(&argv, object_file);
        link_argv_push(&argv, runtime_static_path);
        link_argv_push(&argv, "-o");
        link_argv_push(&argv, exe_file);
        // 用户库排在 Runtime 之后、系统库之前：静态链接的符号解析是单向的。
        push_link_options(&argv, link_options);
        link_argv_push(&argv, "-lws2_32");
    }
#elif defined(_WIN32)
    // MSVC 目标：优先直接调 lld-link，找不到时退回 clang-cl。
    //
    // 两条路的差别只在「谁拼默认清单」：
    //   - lld-link：我们自己给 /subsystem、/machine、compiler-rt 的 builtins；
    //     CRT 靠 Runtime 里的 .drectve（`/DEFAULTLIB:LIBCMT` / `OLDNAMES`），
    //     所以 /MT 与 /MTd 都不用在这里写死，Debug 构建会自动对
    //   - clang-cl：它替我们补这些，链接器默认是 link.exe（`-fuse-ld=lld` 时是 lld-link）
    (void)static_link;
    char lld_link_path[RUN_PATH_MAX];
    char builtins_path[RUN_PATH_MAX];
    if (find_msvc_lld_link(lld_link_path, sizeof(lld_link_path),
                           builtins_path, sizeof(builtins_path))) {
        char out_option[RUN_PATH_MAX + 8];
        int out_length = snprintf(out_option, sizeof(out_option), "/out:%s", exe_file);
        if (out_length < 0 || (size_t)out_length >= sizeof(out_option)) {
            fprintf(stderr, "output file path is too long\n");
            return 1;
        }
        link_argv_push(&argv, lld_link_path);
        link_argv_push(&argv, "/nologo");
        link_argv_push(&argv, msvc_machine_option());
        // 入口点由 CRT 提供：/subsystem:console 时 lld-link 去默认库里找
        // mainCRTStartup（libcmt 里），和 link.exe 的规则一致。
        link_argv_push(&argv, "/subsystem:console");
        link_argv_push(&argv, out_option);
        link_argv_push(&argv, object_file);
        link_argv_push(&argv, runtime_static_path);
        // ws2_32 提供 Runtime 里 winsock 的符号（WSAStartup / socket / closesocket）。
        link_argv_push(&argv, "ws2_32.lib");
        // 用户库排在 Runtime 之后、builtins 之前：静态链接的符号解析是单向的。
        push_link_options_lld_link(&argv, link_options);
        // i128 的 __divti3 / __udivti3 / __modti3 / __umodti3 只能来自这里：
        // cl.exe 不支持 __int128，Runtime 编不出这四个助手。
        link_argv_push(&argv, builtins_path);
    } else {
        char output_option[RUN_PATH_MAX + 4];
        int output_length = snprintf(output_option, sizeof(output_option), "/Fe%s", exe_file);
        if (output_length < 0 || (size_t)output_length >= sizeof(output_option)) {
            fprintf(stderr, "output file path is too long\n");
            return 1;
        }
        const char *linker = "clang-cl";
        // 走到这里说明 lld-link 那条路没通（PATH 上没有 lld-link.exe，或有它但找不到
        // 配套的 compiler-rt builtins）。clang-cl 也没有就真的是没有工具链了。
        char clang_cl_path[RUN_PATH_MAX];
        if (!find_program_on_path("clang-cl.exe", clang_cl_path, sizeof(clang_cl_path))) {
            link_argv_free(&argv);
            return report_missing_linker("lld-link, clang-cl",
                "tap needs a linker toolchain: install LLVM (provides lld-link) or the MSVC "
                "build tools, then put its bin directory on PATH");
        }
        // clang-cl 默认按 MSVC 目标链接，不会自动加入 compiler-rt 的 builtins。i128 的
        // 除法/取模会生成 __divti3/__modti3，显式选择 compiler-rt 才能让这些符号在 Windows
        // LLVM 安装中被解析；否则普通的 i128 测试会在最终链接阶段失败。
        // ws2_32.lib 提供 Runtime 里 winsock 的符号（WSAStartup / socket / closesocket 等）。
        link_argv_push(&argv, linker);
        link_argv_push(&argv, "/nologo");
        link_argv_push(&argv, "/MT");
        link_argv_push(&argv, "/clang:-rtlib=compiler-rt");
        // 退路也尽量用 lld-link：clang-cl 只做参数翻译，链接器换成 LLVM 的 lld。
        char fallback_lld_path[RUN_PATH_MAX];
        if (find_program_on_path("lld-link.exe", fallback_lld_path, sizeof(fallback_lld_path))) {
            link_argv_push(&argv, "-fuse-ld=lld");
        }
        link_argv_push(&argv, object_file);
        link_argv_push(&argv, runtime_static_path);
        link_argv_push(&argv, output_option);
        link_argv_push(&argv, "ws2_32.lib");
        // 库直接当输入文件；-L 和裸参数由 push_link_options 补 /link 段。
        push_link_options(&argv, link_options);
    }
#elif defined(__APPLE__)
    if (static_link) {
        fprintf(stderr,
            "error: -static is not supported on macOS because libSystem is only available as a dynamic library\n"); // 中文：macOS 不支持完整静态链接
        link_argv_free(&argv);
        return 1;
    }
    // macOS 直接调用 ld 时必须显式传入 SDK、架构和平台版本。
    char sdk_path[RUN_PATH_MAX];
    char sdk_version[64];
    if (macos_sdk_value(
            "TAP_MACOS_SDK_PATH", "xcrun --sdk macosx --show-sdk-path",
            sdk_path, sizeof(sdk_path)) != 0 ||
        macos_sdk_value(
            "TAP_MACOS_SDK_VERSION", "xcrun --sdk macosx --show-sdk-version",
            sdk_version, sizeof(sdk_version)) != 0) {
        link_argv_free(&argv);
        return 1;
    }
#if defined(__aarch64__)
    const char *arch = "arm64";
#elif defined(__x86_64__)
    const char *arch = "x86_64";
#else
    const char *arch = "arm64";
#endif
    link_argv_push(&argv, "ld");
    link_argv_push(&argv, "-o");
    link_argv_push(&argv, exe_file);
    link_argv_push(&argv, object_file);
    link_argv_push(&argv, runtime_static_path);
    push_link_options(&argv, link_options);
    link_argv_push(&argv, "-lSystem");
    link_argv_push(&argv, "-syslibroot");
    link_argv_push(&argv, sdk_path);
    link_argv_push(&argv, "-arch");
    link_argv_push(&argv, arch);
    link_argv_push(&argv, "-platform_version");
    link_argv_push(&argv, "macos");
    link_argv_push(&argv, sdk_version);
    link_argv_push(&argv, sdk_version);
#else
    // 由 C 编译器驱动选择当前架构的 CRT、动态链接器和 compiler runtime。
    // `-lm` 必须放在目标文件和静态库之后：glibc 2.34 起 libm 并入了 libc，但更早的发行版
    // 和 Cygwin 仍需显式链接，否则 Runtime 里的 pow 会报未定义符号。
    // macOS 走上面的 ld 分支，libm 在 libSystem 里，不需要这个标志。
#ifdef __CYGWIN__
    // 用 clang 而不是 `cc`（Cygwin 上 cc 是 GCC 13），让驱动版本和生成 obj 的 LLVM 对齐。
    //
    // 不能加 `-rtlib=compiler-rt`：Cygwin 的 clang 包不随附 compiler-rt，
    // `clang -print-resource-dir` 是 /usr/lib/clang/21，但下面只有 include、没有 lib，
    // 链接时会直接报 `cannot find .../libclang_rt.builtins.a`。
    //
    // i128 除法/取模：LLVM 的 Win64 后端把 `sdiv i128` 降级成 __divti3/__modti3 libcall，
    // 约定是「按 byval 指针传参、XMM0 返回」，只和 compiler-rt 匹配；Cygwin 默认 rtlib 是
    // libgcc，它的 __divti3 是 GCC 约定，对不上 → 一做 i128 除法就段错误（崩在首次输出前，
    // 回归点 tests/run-pass/operators/mixed_signedness.tp）。这里不依赖 libgcc 的那份：
    // Runtime（runtime.c）在 clang 构建的 Windows 下自带了约定匹配的 __divti3/__udivti3/
    // __modti3/__umodti3，链接静态库时优先取我们这份、盖过 libgcc。见 runtime.c 末尾注释。
    //
    // 也不加 `-no-pie`：Cygwin 的 clang 驱动不认这个参数（报 unused），Cygwin 目标本身
    // 就是非 PIE 的。
    const char *linker = "clang";
    const char *pie_option = NULL;
#else
    const char *linker = "cc";
    const char *pie_option = "-no-pie";
#endif
    link_argv_push(&argv, linker);
    if (static_link) link_argv_push(&argv, "-static");
    if (pie_option) link_argv_push(&argv, pie_option);
    link_argv_push(&argv, object_file);
    link_argv_push(&argv, runtime_static_path);
    link_argv_push(&argv, "-o");
    link_argv_push(&argv, exe_file);
    push_link_options(&argv, link_options);
    link_argv_push(&argv, "-lm");
#endif

    int link_status = 1;
    if (!argv.failed) {
        print_command_line("link", argv.items);
        link_status = run_process(argv.items);
    }
    link_argv_free(&argv);

    // argv.failed 说明拼参数时就 OOM 了，报错在那一层已经打过，这里不重复。
    if (argv.failed) return 1;
    if (link_status != 0) {
        report_link_failure(link_status, link_options);
        return 1;
    }
    return 0;
}

// 先写出临时目标文件，再链接为指定可执行文件。
static int compile_with_temp_object(CodeGenContext *context, const char *exe_file,
                                    const char *object_file, int static_link,
                                    const LinkOptions *link_options) {
    // 目标文件由 LLVM 在本进程里直接写出，没有外部编译命令。`-v` 下把路径打出来，
    // 需要 nm / otool / objdump 检查时能直接拿到文件（此时临时目录会保留）。
    if (verbose_commands) {
        fprintf(stderr, "tap: object: %s (emitted in-process, no external command)\n",
                object_file);
    }

    if (write_object_to_file(context, object_file) != 0 ||
        link_object_file(object_file, exe_file, static_link, link_options) != 0) {
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
int compile_to_executable(CodeGenContext *context, const char *exe_file, int static_link,
                          const LinkOptions *link_options) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    int result = compile_with_temp_object(
        context, exe_file, workspace.object_file, static_link, link_options);
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
    print_command_line("run", argv);
    int result = run_process(argv);
    free(argv);
    free(relative_path);
    return result;
}

// 编译当前模块并立即执行生成的程序。
int compile_and_run(CodeGenContext *context, const char *exe_file,
                    int program_argc, char **program_argv, int static_link,
                    const LinkOptions *link_options) {
    TempWorkspace workspace;
    if (create_temp_workspace(&workspace) != 0) return 1;

    const char *run_file = exe_file ? exe_file : workspace.executable_file;
    int compile_result = compile_with_temp_object(
        context, run_file, workspace.object_file, static_link, link_options);
    int result = compile_result;
    if (compile_result == 0) {
        result = execute_file(run_file, program_argc, program_argv);
    }

    // 不删除 exe_file：它是调用方明确要的产物——`-o` 指定的路径，或包模式下的
    // `<包根>/build/<name>`。没给 exe_file 时产物在临时目录里，由下面统一清理。
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
        // LLVM 22 起 lli 的 --load= 更名为 --dlopen=（ORC JIT 改造），
        // 按 LLVM 版本选参数名，21 与 22+ 都能用。
        char load_option[RUN_PATH_MAX + 16];
#if defined(LLVM_VERSION_MAJOR) && LLVM_VERSION_MAJOR >= 22
        snprintf(load_option, sizeof(load_option), "--dlopen=%s", runtime_shared_path);
#else
        snprintf(load_option, sizeof(load_option), "--load=%s", runtime_shared_path);
#endif
        char *const argv[] = {"lli", load_option, workspace.ir_file, NULL};
        print_command_line("lli", argv);
        result = run_process(argv);
    }

    if (cleanup_temp_workspace(&workspace) != 0 && result == 0) result = 1;
    return result;
}
