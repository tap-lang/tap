#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PROGRESS_WIDTH 24

// 测试目录的三种语义：运行成功、运行失败、编译失败。
typedef enum {
    GROUP_RUN_PASS,
    GROUP_RUN_FAIL,
    GROUP_COMPILE_FAIL
} TestGroup;

// 一个简单的拥有型字符串数组，用于保存命令参数、测试文件和期望行。
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

// 从 .tp/.tap 测试块中提取出的期望值。
typedef struct {
    int has_stdout;
    int has_stderr;
    int has_exit;
    int has_args;
    int exit_valid;
    int expected_exit;
    char *stdout_text;
    char *stderr_text;
    StringList args;
} Expectations;

// 一个完整测试用例及其运行时临时文件。
typedef struct {
    char *path;
    char *relative_path;
    TestGroup group;
    Expectations expectations;
    char stdout_path[PATH_MAX];
    char stderr_path[PATH_MAX];
    char object_path[PATH_MAX];
    char executable_path[PATH_MAX];
    int process_status;
    int process_started;
    int failed;
    char *diagnostics;
} TestCase;

typedef struct {
    TestCase *items;
    size_t count;
    size_t capacity;
} TestList;

// 父进程用这个结构等待当前批次中的 tap 子进程。
typedef struct {
    int started;
#ifdef _WIN32
    HANDLE handle;
#else
    pid_t pid;
#endif
} ChildProcess;

// 复制字符串；所有字符串列表都由本文件负责释放。
static char *copy_string(const char *value) {
    size_t length = value ? strlen(value) : 0;
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    if (length) memcpy(copy, value, length);
    copy[length] = '\0';
    return copy;
}

static void fatal_message(const char *message) {
    fprintf(stderr, "run-tests: %s\n", message);
    exit(2);
}

// 统一的内存分配入口，失败时直接终止测试运行器。
static void *checked_malloc(size_t size) {
    void *memory = malloc(size ? size : 1);
    if (!memory) fatal_message("out of memory");
    return memory;
}

static void *checked_realloc(void *memory, size_t size) {
    void *expanded = realloc(memory, size ? size : 1);
    if (!expanded) fatal_message("out of memory");
    return expanded;
}

static void list_init(StringList *list) {
    memset(list, 0, sizeof(*list));
}

static void list_reserve(StringList *list, size_t extra) {
    if (list->count + extra <= list->capacity) return;
    size_t capacity = list->capacity ? list->capacity * 2 : 16;
    while (capacity < list->count + extra) capacity *= 2;
    list->items = checked_realloc(list->items, capacity * sizeof(*list->items));
    list->capacity = capacity;
}

static void list_add_owned(StringList *list, char *value) {
    list_reserve(list, 1);
    list->items[list->count++] = value;
}

static void list_add(StringList *list, const char *value) {
    char *copy = copy_string(value);
    if (!copy) fatal_message("out of memory");
    list_add_owned(list, copy);
}

static void list_free(StringList *list) {
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int compare_strings(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static void list_sort(StringList *list) {
    qsort(list->items, list->count, sizeof(*list->items), compare_strings);
}

static void test_list_add(TestList *list, TestCase *test) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        list->items = checked_realloc(list->items, capacity * sizeof(*list->items));
        list->capacity = capacity;
    }
    list->items[list->count++] = *test;
}

static void append_diagnostic(TestCase *test, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list size_arguments;
    va_copy(size_arguments, arguments);
    int required = vsnprintf(NULL, 0, format, size_arguments);
    va_end(size_arguments);
    if (required < 0) {
        va_end(arguments);
        fatal_message("failed to format diagnostic");
    }

    size_t old_length = test->diagnostics ? strlen(test->diagnostics) : 0;
    char *expanded = checked_realloc(
        test->diagnostics, old_length + (size_t)required + 1);
    vsnprintf(expanded + old_length, (size_t)required + 1, format, arguments);
    va_end(arguments);
    test->diagnostics = expanded;
}

// 以 '/' 拼接路径；Cygwin、Unix 和 Windows 的运行器都接受这种形式。
static int path_join(char *output, size_t output_size,
                     const char *directory, const char *name) {
    size_t directory_length = strlen(directory);
    const char *separator =
        directory_length && (directory[directory_length - 1] == '/' ||
                             directory[directory_length - 1] == '\\')
            ? ""
            : "/";
    int written = snprintf(output, output_size, "%s%s%s",
                           directory, separator, name);
    return written >= 0 && (size_t)written < output_size;
}

// 生成临时用例文件名，并统一检查路径长度。
static int format_case_path(char *output, size_t output_size,
                            const char *temporary, size_t case_id,
                            const char *suffix) {
    char name[64];
    int written = snprintf(name, sizeof(name), "%zu%s", case_id, suffix);
    if (written < 0 || (size_t)written >= sizeof(name)) return 0;
    return path_join(output, output_size, temporary, name);
}

static int path_is_absolute(const char *path) {
    if (path[0] == '/' || path[0] == '\\') return 1;
    return isalpha((unsigned char)path[0]) && path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
}

static int path_exists(const char *path) {
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

static int path_is_executable(const char *path) {
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

// 读取整个文件。测试源码、期望文件和子进程输出都通过这个函数读取。
static char *read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    char *data = checked_malloc((size_t)size + 1);
    if (size && fread(data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    data[size] = '\0';
    if (size_out) *size_out = (size_t)size;
    return data;
}

static char *read_file_or_empty(const char *path) {
    size_t size = 0;
    char *data = read_file(path, &size);
    return data ? data : copy_string("");
}

// 复现 shell 版 read_text_file 的规则：去 CR、去每行行尾空格/Tab，
// 并去掉最终换行，方便直接进行字符串比较。
static char *normalize_text(const char *data, size_t length) {
    size_t capacity = length + 1;
    char *output = checked_malloc(capacity ? capacity : 1);
    size_t output_length = 0;
    size_t cursor = 0;

    while (cursor < length) {
        size_t line_start = cursor;
        while (cursor < length && data[cursor] != '\n') cursor++;
        size_t line_end = cursor;
        if (line_end > line_start && data[line_end - 1] == '\r') line_end--;
        while (line_end > line_start &&
               (data[line_end - 1] == ' ' || data[line_end - 1] == '\t')) {
            line_end--;
        }
        size_t line_length = line_end - line_start;
        if (output_length + line_length + 2 > capacity) {
            while (output_length + line_length + 2 > capacity) capacity *= 2;
            output = checked_realloc(output, capacity);
        }
        memcpy(output + output_length, data + line_start, line_length);
        output_length += line_length;
        if (cursor < length && data[cursor] == '\n') {
            output[output_length++] = '\n';
            cursor++;
        }
    }

    while (output_length && output[output_length - 1] == '\n') output_length--;
    output[output_length] = '\0';
    return output;
}

static char *normalize_file(const char *path) {
    size_t size = 0;
    char *data = read_file_or_empty(path);
    size = strlen(data);
    char *normalized = normalize_text(data, size);
    free(data);
    return normalized;
}

static void split_lines(const char *data, StringList *lines) {
    size_t length = strlen(data);
    size_t cursor = 0;
    while (cursor <= length) {
        size_t start = cursor;
        while (cursor < length && data[cursor] != '\n') cursor++;
        size_t end = cursor;
        if (end > start && data[end - 1] == '\r') end--;
        char *line = checked_malloc(end - start + 1);
        memcpy(line, data + start, end - start);
        line[end - start] = '\0';
        list_add_owned(lines, line);
        if (cursor >= length) break;
        cursor++;
    }
}

static char *join_lines(const StringList *lines) {
    size_t size = 0;
    for (size_t i = 0; i < lines->count; i++) size += strlen(lines->items[i]) + 1;
    char *output = checked_malloc(size + 1);
    size_t offset = 0;
    for (size_t i = 0; i < lines->count; i++) {
        size_t length = strlen(lines->items[i]);
        memcpy(output + offset, lines->items[i], length);
        offset += length;
        if (i + 1 < lines->count) output[offset++] = '\n';
    }
    output[offset] = '\0';
    return output;
}

static void trim_trailing_empty_lines(StringList *lines) {
    while (lines->count && lines->items[lines->count - 1][0] == '\0') {
        free(lines->items[--lines->count]);
    }
}

static int section_number(const char *line) {
    if (strcmp(line, "- .stdout") == 0) return 0;
    if (strcmp(line, "- .stderr") == 0) return 1;
    if (strcmp(line, "- .exit") == 0) return 2;
    if (strcmp(line, "- .args") == 0) return 3;
    return -1;
}

static void expectations_init(Expectations *expectations) {
    memset(expectations, 0, sizeof(*expectations));
    list_init(&expectations->args);
    expectations->expected_exit = -1;
}

static void expectations_free(Expectations *expectations) {
    free(expectations->stdout_text);
    free(expectations->stderr_text);
    list_free(&expectations->args);
}

// 单次扫描源码中的 /** -- test ... -- */ 块，同时提取四类期望，
// 替代 shell 版对 stdout/stderr/exit/args 分别启动 awk 的做法。
static void parse_embedded_expectations(const char *source, Expectations *expectations) {
    StringList sections[4];
    for (int i = 0; i < 4; i++) list_init(&sections[i]);

    size_t length = strlen(source);
    size_t cursor = 0;
    int in_test = 0;
    int current = -1;
    while (cursor <= length) {
        size_t start = cursor;
        while (cursor < length && source[cursor] != '\n') cursor++;
        size_t end = cursor;
        if (end > start && source[end - 1] == '\r') end--;
        char *line = checked_malloc(end - start + 1);
        memcpy(line, source + start, end - start);
        line[end - start] = '\0';

        if (strcmp(line, "/** -- test") == 0) {
            in_test = 1;
            current = -1;
        } else if (in_test && strcmp(line, "-- */") == 0) {
            in_test = 0;
            current = -1;
        } else if (in_test) {
            int section = section_number(line);
            if (section >= 0) {
                current = section;
                if (section == 0) expectations->has_stdout = 1;
                if (section == 1) expectations->has_stderr = 1;
                if (section == 2) expectations->has_exit = 1;
                if (section == 3) expectations->has_args = 1;
            } else if (current >= 0) {
                list_add(&sections[current], line);
            }
        }
        free(line);
        if (cursor >= length) break;
        cursor++;
    }

    for (int i = 0; i < 4; i++) trim_trailing_empty_lines(&sections[i]);

    if (expectations->has_stdout) {
        char *joined = join_lines(&sections[0]);
        expectations->stdout_text = normalize_text(joined, strlen(joined));
        free(joined);
    }
    if (expectations->has_stderr) {
        char *joined = join_lines(&sections[1]);
        expectations->stderr_text = normalize_text(joined, strlen(joined));
        free(joined);
    }
    if (expectations->has_exit) {
        char *joined = join_lines(&sections[2]);
        char digits[64];
        size_t count = 0;
        for (size_t i = 0; joined[i] && count + 1 < sizeof(digits); i++) {
            if (!isspace((unsigned char)joined[i])) digits[count++] = joined[i];
        }
        digits[count] = '\0';
        expectations->exit_valid = count > 0;
        for (size_t i = 0; i < count; i++) {
            if (!isdigit((unsigned char)digits[i])) expectations->exit_valid = 0;
        }
        if (expectations->exit_valid) expectations->expected_exit = atoi(digits);
        free(joined);
    }
    if (expectations->has_args) {
        for (size_t i = 0; i < sections[3].count; i++) {
            list_add(&expectations->args, sections[3].items[i]);
        }
    }
    for (int i = 0; i < 4; i++) list_free(&sections[i]);
}

static char *external_expectation_path(const char *source_path, const char *section) {
    const char *slash = strrchr(source_path, '/');
    const char *dot = strrchr(source_path, '.');
    if (!dot || (slash && dot < slash)) dot = source_path + strlen(source_path);
    size_t prefix_length = (size_t)(dot - source_path);
    size_t size = prefix_length + 1 + strlen(section) + 1;
    char *path = checked_malloc(size);
    memcpy(path, source_path, prefix_length);
    snprintf(path + prefix_length, size - prefix_length, ".%s", section);
    return path;
}

static void load_external_expectations(const char *source_path,
                                       Expectations *expectations) {
    if (!expectations->has_stdout) {
        char *path = external_expectation_path(source_path, "stdout");
        if (path_exists(path)) {
            expectations->stdout_text = normalize_file(path);
            expectations->has_stdout = 1;
        }
        free(path);
    }
    if (!expectations->has_stderr) {
        char *path = external_expectation_path(source_path, "stderr");
        if (path_exists(path)) {
            expectations->stderr_text = normalize_file(path);
            expectations->has_stderr = 1;
        }
        free(path);
    }
}

static int has_test_extension(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash)) return 0;
    return strcmp(dot, ".tp") == 0 || strcmp(dot, ".tap") == 0;
}

// 递归扫描一个测试分组目录。排序在调用方完成，以保持原脚本的稳定顺序。
static void collect_directory(const char *directory, StringList *files) {
#ifdef _WIN32
    char pattern[PATH_MAX];
    if (!path_join(pattern, sizeof(pattern), directory, "*")) return;
    struct _finddata_t entry;
    intptr_t handle = _findfirst(pattern, &entry);
    if (handle == -1) return;
    do {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) continue;
        char path[PATH_MAX];
        if (!path_join(path, sizeof(path), directory, entry.name)) continue;
        if (entry.attrib & _A_SUBDIR) {
            collect_directory(path, files);
        } else if (has_test_extension(path)) {
            list_add(files, path);
        }
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
#else
    DIR *handle = opendir(directory);
    if (!handle) return;
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[PATH_MAX];
        if (!path_join(path, sizeof(path), directory, entry->d_name)) continue;
        struct stat info;
        if (stat(path, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            collect_directory(path, files);
        } else if (S_ISREG(info.st_mode) && has_test_extension(path)) {
            list_add(files, path);
        }
    }
    closedir(handle);
#endif
}

// 按当前测试模式组装 tap 命令行：compile-fail 走 -ir，
// fast 走 -run-lli，native 走 run；带 .args 的测试强制走 native。
static int build_test_command(const TestCase *test, const char *compiler,
                              const char *run_mode, StringList *command) {
    list_init(command);
    list_add(command, compiler);
    if (test->group == GROUP_COMPILE_FAIL) {
        list_add(command, "-ir");
        list_add(command, "-o");
        list_add(command, test->object_path);
        list_add(command, test->relative_path);
    } else if (strcmp(run_mode, "native") == 0 ||
               test->expectations.has_args ||
               strcmp(test->relative_path, "tests/run-pass/basics/hello.tp") == 0) {
        list_add(command, "run");
        list_add(command, "-o");
        list_add(command, test->executable_path);
        list_add(command, test->relative_path);
        if (test->expectations.has_args) {
            list_add(command, "--");
            for (size_t i = 0; i < test->expectations.args.count; i++) {
                list_add(command, test->expectations.args.items[i]);
            }
        }
    } else {
        list_add(command, "-run-lli");
        list_add(command, test->relative_path);
    }
    return 0;
}

static char **list_argv(const StringList *list) {
    char **argv = checked_malloc((list->count + 1) * sizeof(*argv));
    for (size_t i = 0; i < list->count; i++) argv[i] = list->items[i];
    argv[list->count] = NULL;
    return argv;
}

#ifdef _WIN32
// Windows CreateProcess 使用一整条命令行，需要按 MSVC 参数规则加引号。
static char *quote_windows_argument(const char *argument) {
    size_t length = strlen(argument);
    char *quoted = checked_malloc(length * 2 + 3);
    char *output = quoted;
    const char *cursor = argument;
    *output++ = '"';
    while (*cursor) {
        size_t slashes = 0;
        while (*cursor == '\\') {
            slashes++;
            cursor++;
        }
        if (*cursor == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) *output++ = '\\';
            *output++ = *cursor++;
        } else if (!*cursor) {
            for (size_t i = 0; i < slashes * 2; i++) *output++ = '\\';
        } else {
            for (size_t i = 0; i < slashes; i++) *output++ = '\\';
            *output++ = *cursor++;
        }
    }
    *output++ = '"';
    *output = '\0';
    return quoted;
}
#endif

// 启动一个测试子进程，并把 stdout/stderr 重定向到该用例专属文件。
static int start_process(const StringList *command, const char *stdout_path,
                         const char *stderr_path, ChildProcess *child) {
    char **argv = list_argv(command);
#ifdef _WIN32
    size_t command_length = 1;
    char **quoted = checked_malloc(command->count * sizeof(*quoted));
    for (size_t i = 0; i < command->count; i++) {
        quoted[i] = quote_windows_argument(command->items[i]);
        command_length += strlen(quoted[i]) + 1;
    }
    char *command_line = checked_malloc(command_length);
    command_line[0] = '\0';
    for (size_t i = 0; i < command->count; i++) {
        if (i) strcat(command_line, " ");
        strcat(command_line, quoted[i]);
    }

    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    HANDLE output = CreateFileA(stdout_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE error = CreateFileA(stderr_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
        for (size_t i = 0; i < command->count; i++) free(quoted[i]);
        free(quoted);
        free(command_line);
        free(argv);
        return -1;
    }

    STARTUPINFOA startup;
    PROCESS_INFORMATION information;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&information, sizeof(information));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = error;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    BOOL created = CreateProcessA(NULL, command_line, NULL, NULL, TRUE, 0,
                                  NULL, NULL, &startup, &information);
    CloseHandle(output);
    CloseHandle(error);
    for (size_t i = 0; i < command->count; i++) free(quoted[i]);
    free(quoted);
    free(command_line);
    free(argv);
    if (!created) return -1;
    CloseHandle(information.hThread);
    child->handle = information.hProcess;
    child->started = 1;
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        free(argv);
        return -1;
    }
    if (pid == 0) {
        int output = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int error = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (output < 0 || error < 0) _exit(127);
        dup2(output, STDOUT_FILENO);
        dup2(error, STDERR_FILENO);
        close(output);
        close(error);
        if (strchr(argv[0], '/')) execv(argv[0], argv);
        else execvp(argv[0], argv);
        _exit(127);
    }
    free(argv);
    child->pid = pid;
    child->started = 1;
    return 0;
#endif
}

// 等待单个子进程并统一转换成 shell 习惯的退出码。
static int wait_process(ChildProcess *child) {
    if (!child->started) return 127;
#ifdef _WIN32
    WaitForSingleObject(child->handle, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(child->handle, &status);
    CloseHandle(child->handle);
    child->started = 0;
    return (int)status;
#else
    int status = 1;
    while (waitpid(child->pid, &status, 0) < 0) {
        if (errno != EINTR) {
            child->started = 0;
            return 127;
        }
    }
    child->started = 0;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 127;
#endif
}

// 把意外输出加入测试诊断，而不是直接打印，保证最后仍按测试文件顺序汇总。
static void append_indented_diagnostic(TestCase *test, const char *text) {
    const char *cursor = text;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        append_diagnostic(test, "    %.*s\n", (int)length, cursor);
        if (!end) break;
        cursor = end + 1;
    }
}

// 绘制固定宽度的进度条，避免启动 C 运行器后再依赖 shell 格式化。
static void print_progress(size_t completed, size_t total) {
    size_t filled = total ? completed * PROGRESS_WIDTH / total : 0;
    char bar[PROGRESS_WIDTH + 1];
    for (size_t i = 0; i < PROGRESS_WIDTH; i++) {
        bar[i] = i < filled ? '#' : '.';
    }
    bar[PROGRESS_WIDTH] = '\0';
    printf("Testing [%s] %zu/%zu (%zu%%)\n", bar, completed, total,
           total ? completed * 100 / total : 0);
}

static void check_test(TestCase *test) {
    char *actual_stdout = normalize_file(test->stdout_path);
    char *actual_stderr = normalize_file(test->stderr_path);

    if (!test->expectations.has_exit) {
        append_diagnostic(test, "missing embedded .exit expectation\n");
    } else if (!test->expectations.exit_valid) {
        append_diagnostic(test, "invalid embedded .exit expectation\n");
    } else if (test->process_status != test->expectations.expected_exit) {
        append_diagnostic(test, "exit status %d, expected %d\n",
                          test->process_status, test->expectations.expected_exit);
    }

    if (test->expectations.has_stdout) {
        if (strcmp(actual_stdout, test->expectations.stdout_text) != 0) {
            append_diagnostic(test, "stdout does not match .stdout expectation\n");
            append_diagnostic(test, "expected:\n%s\nactual:\n%s\n",
                              test->expectations.stdout_text, actual_stdout);
        }
    } else if (actual_stdout[0]) {
        append_diagnostic(test, "unexpected stdout\n");
        append_indented_diagnostic(test, actual_stdout);
    }

    if (test->group == GROUP_COMPILE_FAIL) {
        if (!test->expectations.has_stderr) {
            append_diagnostic(test, "missing .stderr expectation\n");
        } else {
            StringList lines;
            list_init(&lines);
            split_lines(test->expectations.stderr_text, &lines);
            for (size_t i = 0; i < lines.count; i++) {
                if (!lines.items[i][0]) continue;
                if (!strstr(actual_stderr, lines.items[i])) {
                    append_diagnostic(test, "missing stderr text: %s\n", lines.items[i]);
                }
            }
            list_free(&lines);
        }
    } else if (test->expectations.has_stderr) {
        if (strcmp(actual_stderr, test->expectations.stderr_text) != 0) {
            append_diagnostic(test, "stderr does not match .stderr expectation\n");
            append_diagnostic(test, "expected:\n%s\nactual:\n%s\n",
                              test->expectations.stderr_text, actual_stderr);
        }
    } else if (actual_stderr[0]) {
        append_diagnostic(test, "unexpected stderr\n");
        append_indented_diagnostic(test, actual_stderr);
    }

    test->failed = test->diagnostics != NULL;
    free(actual_stdout);
    free(actual_stderr);
}

static void free_test(TestCase *test) {
    free(test->path);
    free(test->relative_path);
    expectations_free(&test->expectations);
    free(test->diagnostics);
}

static int make_temp_directory(char *output, size_t output_size) {
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(base), base);
    if (!length || length >= sizeof(base)) return -1;
    DWORD pid = GetCurrentProcessId();
    for (unsigned int attempt = 0; attempt < 128; attempt++) {
        int written = snprintf(output, output_size, "%stap-tests-%lu-%u",
                               base, (unsigned long)pid, attempt);
        if (written < 0 || (size_t)written >= output_size) return -1;
        if (CreateDirectoryA(output, NULL)) return 0;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    }
    return -1;
#else
    const char *temporary = getenv("TMPDIR");
    if (!temporary || !temporary[0]) temporary = "/tmp";
    int written = snprintf(output, output_size, "%s/tap-tests.XXXXXX", temporary);
    if (written < 0 || (size_t)written >= output_size) return -1;
    return mkdtemp(output) ? 0 : -1;
#endif
}

static void cleanup_temp_directory(const char *directory, TestList *tests) {
    for (size_t i = 0; i < tests->count; i++) {
        remove(tests->items[i].stdout_path);
        remove(tests->items[i].stderr_path);
        remove(tests->items[i].object_path);
        remove(tests->items[i].executable_path);
    }
#ifdef _WIN32
    RemoveDirectoryA(directory);
#else
    rmdir(directory);
#endif
}

static int parse_jobs(void) {
    const char *value = getenv("TEST_JOBS");
    if (!value || !value[0]) return 4;
    char *end = NULL;
    long jobs = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || jobs <= 0 || jobs > 1024) {
        fprintf(stderr, "TEST_JOBS must be a positive integer: %s\n", value);
        exit(2);
    }
    return (int)jobs;
}

static const char *parse_mode(void) {
    const char *mode = getenv("TEST_RUN_MODE");
    if (!mode || !mode[0]) mode = "fast";
    if (strcmp(mode, "fast") != 0 && strcmp(mode, "native") != 0) {
        fprintf(stderr, "TEST_RUN_MODE must be fast or native: %s\n", mode);
        exit(2);
    }
    return mode;
}

// 获取项目根目录。Makefile 从项目根调用运行器，因此这里保持与 shell 入口一致。
static int current_working_directory(char *output, size_t output_size) {
#ifdef _WIN32
    return _getcwd(output, (int)output_size) != NULL;
#else
    return getcwd(output, output_size) != NULL;
#endif
}

#if defined(__APPLE__)
// macOS 原脚本会缓存 xcrun 查询结果，避免每个 native 用例重复启动 xcrun。
static char *read_command_line(const char *command) {
    FILE *pipe = popen(command, "r");
    if (!pipe) return NULL;
    char buffer[PATH_MAX];
    char *result = NULL;
    if (fgets(buffer, sizeof(buffer), pipe)) {
        buffer[strcspn(buffer, "\r\n")] = '\0';
        if (buffer[0]) result = copy_string(buffer);
    }
    int status = pclose(pipe);
    if (status != 0) {
        free(result);
        return NULL;
    }
    return result;
}

static void setup_macos_sdk(void) {
    const char *sdk_path = getenv("TAP_MACOS_SDK_PATH");
    const char *sdk_version = getenv("TAP_MACOS_SDK_VERSION");
    if (!sdk_path || !sdk_path[0]) {
        char *value = read_command_line("xcrun --sdk macosx --show-sdk-path");
        if (!value || setenv("TAP_MACOS_SDK_PATH", value, 1) != 0) {
            free(value);
            fatal_message("failed to resolve macOS SDK path");
        }
        free(value);
    }
    if (!sdk_version || !sdk_version[0]) {
        char *value = read_command_line("xcrun --sdk macosx --show-sdk-version");
        if (!value || setenv("TAP_MACOS_SDK_VERSION", value, 1) != 0) {
            free(value);
            fatal_message("failed to resolve macOS SDK version");
        }
        free(value);
    }
}
#endif

// 主流程：解析参数、发现并发运行测试、最后按文件顺序比较并汇总结果。
int main(int argc, char **argv) {
    int jobs = parse_jobs();
    const char *run_mode = parse_mode();
    const char *filter = getenv("TEST_FILTER");
    if (!filter || !filter[0]) filter = argc > 2 ? argv[2] : "";
    const char *compiler_argument = argc > 1 ? argv[1] : "build/tap";

    char project_root[PATH_MAX];
    if (!current_working_directory(project_root, sizeof(project_root))) {
        fatal_message("failed to get project root");
    }

    char compiler[PATH_MAX];
    if (path_is_absolute(compiler_argument)) {
        snprintf(compiler, sizeof(compiler), "%s", compiler_argument);
    } else if (!path_join(compiler, sizeof(compiler), project_root, compiler_argument)) {
        fatal_message("compiler path is too long");
    }
    if (!path_is_executable(compiler)) {
        char with_extension[PATH_MAX];
        size_t compiler_length = strlen(compiler);
        if (compiler_length + 4 < sizeof(with_extension)) {
            memcpy(with_extension, compiler, compiler_length);
            memcpy(with_extension + compiler_length, ".exe", 5);
            if (path_is_executable(with_extension)) {
                snprintf(compiler, sizeof(compiler), "%s", with_extension);
            }
        }
    }
    if (!path_is_executable(compiler)) {
        fprintf(stderr, "test compiler is not executable: %s\n", compiler);
        return 2;
    }

    char test_root[PATH_MAX];
    if (!path_join(test_root, sizeof(test_root), project_root, "tests")) {
        fatal_message("test root path is too long");
    }
    char module_path[PATH_MAX * 2];
    char fixtures[PATH_MAX];
    char fixtures_extra[PATH_MAX];
    path_join(fixtures, sizeof(fixtures), test_root, "fixtures");
    path_join(fixtures_extra, sizeof(fixtures_extra), test_root, "fixtures-extra");
#ifdef _WIN32
    snprintf(module_path, sizeof(module_path), "%s;%s", fixtures, fixtures_extra);
#else
    snprintf(module_path, sizeof(module_path), "%s:%s", fixtures, fixtures_extra);
#endif
    char std_path[PATH_MAX];
    path_join(std_path, sizeof(std_path), project_root, "std");
#ifdef _WIN32
    _putenv_s("TAP_MODULE_PATH", module_path);
    _putenv_s("TAP_STD_PATH", std_path);
#else
    setenv("TAP_MODULE_PATH", module_path, 1);
    setenv("TAP_STD_PATH", std_path, 1);
#endif
#if defined(__APPLE__)
    setup_macos_sdk();
#endif

    char temporary[PATH_MAX];
    if (make_temp_directory(temporary, sizeof(temporary)) != 0) {
        fatal_message("failed to create temporary directory");
    }

    TestList tests = {0};
    const char *group_names[] = {"run-pass", "run-fail", "compile-fail"};
    TestGroup groups[] = {GROUP_RUN_PASS, GROUP_RUN_FAIL, GROUP_COMPILE_FAIL};
    for (size_t group_index = 0; group_index < 3; group_index++) {
        char directory[PATH_MAX];
        path_join(directory, sizeof(directory), test_root, group_names[group_index]);
        StringList files;
        list_init(&files);
        collect_directory(directory, &files);
        list_sort(&files);
        for (size_t i = 0; i < files.count; i++) {
            const char *relative = files.items[i] + strlen(project_root);
            while (*relative == '/' || *relative == '\\') relative++;
            if (filter[0] && !strstr(relative, filter)) continue;

            TestCase test;
            memset(&test, 0, sizeof(test));
            test.path = copy_string(files.items[i]);
            test.relative_path = copy_string(relative);
            test.group = groups[group_index];
            expectations_init(&test.expectations);
            char *source = read_file(test.path, NULL);
            if (!source) fatal_message("failed to read test source");
            parse_embedded_expectations(source, &test.expectations);
            load_external_expectations(test.path, &test.expectations);
            free(source);
            test_list_add(&tests, &test);
        }
        list_free(&files);
    }

    if (!tests.count) {
        fprintf(stderr, "no tests matched filter: %s\n", filter);
        cleanup_temp_directory(temporary, &tests);
        free(tests.items);
        return 2;
    }

    for (size_t i = 0; i < tests.count; i++) {
        size_t id = i + 1;
        if (!format_case_path(tests.items[i].stdout_path, sizeof(tests.items[i].stdout_path), temporary, id, ".stdout") ||
            !format_case_path(tests.items[i].stderr_path, sizeof(tests.items[i].stderr_path), temporary, id, ".stderr") ||
            !format_case_path(tests.items[i].object_path, sizeof(tests.items[i].object_path), temporary, id, ".ll") ||
            !format_case_path(tests.items[i].executable_path, sizeof(tests.items[i].executable_path), temporary, id, "-program"
#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
                 ".exe"
#else
                 ""
#endif
            )) {
            fatal_message("temporary test path is too long");
        }
    }

    printf("Running %zu tests with %d workers\n", tests.count, jobs);
    size_t completed = 0;
    for (size_t batch_start = 0; batch_start < tests.count; batch_start += (size_t)jobs) {
        size_t batch_count = tests.count - batch_start;
        if (batch_count > (size_t)jobs) batch_count = (size_t)jobs;
        ChildProcess *children = checked_malloc(batch_count * sizeof(*children));
        StringList *commands = checked_malloc(batch_count * sizeof(*commands));
        memset(children, 0, batch_count * sizeof(*children));

        printf("------------------------------------\n");
        for (size_t i = 0; i < (size_t)jobs; i++) {
            if (i < batch_count) printf("worker %zu running: %s\n", i + 1, tests.items[batch_start + i].relative_path);
            else printf("worker %zu idle\n", i + 1);
        }
        print_progress(completed, tests.count);
        fflush(stdout);

        for (size_t i = 0; i < batch_count; i++) {
            TestCase *test = &tests.items[batch_start + i];
            build_test_command(test, compiler, run_mode, &commands[i]);
            if (start_process(&commands[i], test->stdout_path, test->stderr_path, &children[i]) != 0) {
                test->process_status = 127;
                test->process_started = 0;
                append_diagnostic(test, "failed to start test process\n");
            } else {
                test->process_started = 1;
            }
        }
        for (size_t i = 0; i < batch_count; i++) {
            if (children[i].started) {
                tests.items[batch_start + i].process_status = wait_process(&children[i]);
            }
        }
        completed += batch_count;
        for (size_t i = 0; i < batch_count; i++) list_free(&commands[i]);
        free(commands);
        free(children);
        printf("------------------------------------\n");
        for (size_t i = 0; i < (size_t)jobs; i++) printf("worker %zu idle\n", i + 1);
        print_progress(completed, tests.count);
        fflush(stdout);
    }

    int passed = 0;
    int failed = 0;
    for (size_t i = 0; i < tests.count; i++) {
        check_test(&tests.items[i]);
        if (tests.items[i].failed) {
            if (tests.items[i].diagnostics) fputs(tests.items[i].diagnostics, stderr);
            fprintf(stderr, "FAIL %s\n", tests.items[i].relative_path);
            failed++;
        } else {
            printf("PASS %s\n", tests.items[i].relative_path);
            passed++;
        }
    }
    printf("\n%zu tests: %d passed, %d failed\n", tests.count, passed, failed);

    cleanup_temp_directory(temporary, &tests);
    for (size_t i = 0; i < tests.count; i++) free_test(&tests.items[i]);
    free(tests.items);
    return failed ? 1 : 0;
}
