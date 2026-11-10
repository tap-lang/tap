// 子进程的 Runtime 胶水层。
//
// 平台差异全部收在这里：POSIX 用 fork + execvp + pipe + poll，Windows 用
// CreateProcessW + CreatePipe + PeekNamedPipe。`process` 包只负责拼参数表，
// tap 代码里没有平台分支 —— tap 也没有条件编译可用。
//
// 参数表和 env 表都是 '\0' 分隔的扁平 String（C 侧接不了「String 数组」，
// 而且 String 允许内嵌 NUL，必须按 length 走字节）。约定见 process/docs/windows.md。

#include "runtime_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define TAP_PROCESS_MODE_CAPTURE 0
#define TAP_PROCESS_MODE_INHERIT 1

// 程序不存在时两个平台都报 127：POSIX 是子进程 execvp 失败后 _exit(127)，
// Windows 的 CreateProcess 失败发生在父进程，必须手工对齐，否则同一段 tap
// 代码在两平台看到的退出码不一样。
#define TAP_PROCESS_MISSING_STATUS 127

#define TAP_PROCESS_READ_CHUNK 8192

typedef struct TapProcessResult {
    int32_t status;
    char *out_data;
    size_t out_length;
    size_t out_capacity;
    char *err_data;
    size_t err_length;
    size_t err_capacity;
} TapProcessResult;

// ---------------------------------------------------------------------------
// 通用辅助
// ---------------------------------------------------------------------------

static int buffer_append(
    char **data, size_t *length, size_t *capacity, const char *bytes, size_t count) {
    if (count == 0) return 0;
    if (*length + count + 1 > *capacity) {
        size_t next = *capacity ? *capacity : TAP_PROCESS_READ_CHUNK;
        while (next < *length + count + 1) {
            if (next > (size_t)-1 / 2) return -1;
            next *= 2;
        }
        char *grown = realloc(*data, next);
        if (!grown) return -1;
        *data = grown;
        *capacity = next;
    }
    memcpy(*data + *length, bytes, count);
    *length += count;
    (*data)[*length] = '\0';
    return 0;
}

// 把 String 复制成 malloc 的 C 字符串。含内嵌 NUL 时返回 NULL —— 传给系统的
// 字符串只能是零结尾的，截断会让子进程拿到和 tap 看到的不一样的内容。
#ifndef _WIN32
static char *copy_c_string(const String *value) {
    size_t length = (size_t)value->length;
    if (length > 0) {
        if (!value->data) return NULL;
        if (memchr(value->data, 0, length) != NULL) {
            fprintf(stderr, "process argument cannot contain NUL bytes\n");
            return NULL;
        }
    }
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    if (length > 0) memcpy(copy, value->data, length);
    copy[length] = '\0';
    return copy;
}
#endif

// '\0' 分隔的扁平表 → 连续缓冲区 + 指针数组（argv 可直接用）。
// 末尾以分隔符结尾时，最后那段空串不计入元素：tap 侧每追加一个元素就补一个
// '\0'，所以 "a\0" 是一个元素、"a\0\0" 才是两个（第二个是空串）。
typedef struct TapArguments {
    char *buffer;
    char **items;
    size_t count;
} TapArguments;

static void arguments_free(TapArguments *list) {
    free(list->buffer);
    free(list->items);
    list->buffer = NULL;
    list->items = NULL;
    list->count = 0;
}

static int arguments_parse(const String *value, TapArguments *list) {
    memset(list, 0, sizeof(*list));
    size_t length = value ? (size_t)value->length : 0;
    if (length == 0) return 0;
    if (!value->data) return -1;
    if (length > (size_t)-1 - 1) return -1;

    list->buffer = malloc(length + 1);
    if (!list->buffer) return -1;
    memcpy(list->buffer, value->data, length);
    list->buffer[length] = '\0';

    size_t count = 1;
    for (size_t i = 0; i < length; i++) {
        if (list->buffer[i] == '\0') count++;
    }
    if (list->buffer[length - 1] == '\0') count--;

    list->items = malloc(sizeof(char *) * (count + 1));
    if (!list->items) {
        arguments_free(list);
        return -1;
    }
    size_t index = 0;
    if (count > 0) list->items[index++] = list->buffer;
    for (size_t i = 0; i < length && index < count; i++) {
        if (list->buffer[i] == '\0') list->items[index++] = list->buffer + i + 1;
    }
    list->items[count] = NULL;
    list->count = count;
    return 0;
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

#ifdef _WIN32

// 可增长的宽字符串缓冲区（命令行、环境块都用它拼）。
typedef struct WideBuffer {
    wchar_t *data;
    size_t length;
    size_t capacity;
} WideBuffer;

static int wide_append(WideBuffer *buffer, const wchar_t *text, size_t count) {
    if (count == 0) return 0;
    if (buffer->length + count + 1 > buffer->capacity) {
        size_t next = buffer->capacity ? buffer->capacity : 256;
        while (next < buffer->length + count + 1) {
            if (next > (size_t)-1 / 2) return -1;
            next *= 2;
        }
        wchar_t *grown = realloc(buffer->data, sizeof(wchar_t) * next);
        if (!grown) return -1;
        buffer->data = grown;
        buffer->capacity = next;
    }
    memcpy(buffer->data + buffer->length, text, sizeof(wchar_t) * count);
    buffer->length += count;
    buffer->data[buffer->length] = L'\0';
    return 0;
}

// 按 MSVCRT 的规则给一个参数加引号：反斜杠只在「引号前」才需要加倍。
// 和 src/run.c 里的 quote_windows_argument 是同一套算法，但那份是编译器内部
// 的 static 函数，链接不到 Runtime，所以这里单独实现一份。
static int wide_append_quoted(WideBuffer *buffer, const wchar_t *argument) {
    if (wide_append(buffer, L"\"", 1) != 0) return -1;
    size_t backslashes = 0;
    for (const wchar_t *cursor = argument;; cursor++) {
        if (*cursor == L'\\') {
            backslashes++;
            continue;
        }
        if (*cursor == L'"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++) {
                if (wide_append(buffer, L"\\", 1) != 0) return -1;
            }
            if (wide_append(buffer, L"\"", 1) != 0) return -1;
        } else if (*cursor == L'\0') {
            for (size_t i = 0; i < backslashes * 2; i++) {
                if (wide_append(buffer, L"\\", 1) != 0) return -1;
            }
            break;
        } else {
            for (size_t i = 0; i < backslashes; i++) {
                if (wide_append(buffer, L"\\", 1) != 0) return -1;
            }
            if (wide_append(buffer, cursor, 1) != 0) return -1;
        }
        backslashes = 0;
    }
    return wide_append(buffer, L"\"", 1);
}

// 拼 "KEY=VALUE\0...\0\0" 形式的环境块：父进程的条目原样保留，同名（不区分
// 大小写）的被覆盖项去掉后追加在末尾。没有任何覆盖项时返回 NULL，表示直接
// 继承父进程环境 —— 这也是 CreateProcessW 的默认行为。
static wchar_t *build_environment(const TapArguments *vars) {
    if (vars->count == 0) return NULL;

    wchar_t **overrides = calloc(vars->count, sizeof(wchar_t *));
    if (!overrides) return NULL;
    for (size_t i = 0; i < vars->count; i++) {
        overrides[i] = tap_wide_from_utf8(vars->items[i], strlen(vars->items[i]));
        if (!overrides[i]) {
            for (size_t j = 0; j < i; j++) free(overrides[j]);
            free(overrides);
            return NULL;
        }
    }

    WideBuffer buffer = {NULL, 0, 0};
    wchar_t *parent = GetEnvironmentStringsW();
    int failed = 0;
    if (parent) {
        for (wchar_t *entry = parent; *entry && !failed; entry += wcslen(entry) + 1) {
            // "=C:=C:\path" 这类是每个驱动器的当前目录，不必传给子进程。
            if (entry[0] == L'=') continue;
            size_t name_length = wcscspn(entry, L"=");
            int overridden = 0;
            for (size_t i = 0; i < vars->count; i++) {
                if (_wcsnicmp(entry, overrides[i], name_length) == 0 &&
                    overrides[i][name_length] == L'=') {
                    overridden = 1;
                    break;
                }
            }
            if (overridden) continue;
            if (wide_append(&buffer, entry, wcslen(entry)) != 0 ||
                wide_append(&buffer, L"\0", 1) != 0) {
                failed = 1;
            }
        }
        FreeEnvironmentStringsW(parent);
    }
    for (size_t i = 0; i < vars->count && !failed; i++) {
        if (wide_append(&buffer, overrides[i], wcslen(overrides[i])) != 0 ||
            wide_append(&buffer, L"\0", 1) != 0) {
            failed = 1;
        }
    }
    if (!failed) {
        // 环境块以两个 NUL 结束：最后一条后面的 '\0' 加上这个。
        if (wide_append(&buffer, L"\0", 1) != 0) failed = 1;
    }

    for (size_t i = 0; i < vars->count; i++) free(overrides[i]);
    free(overrides);
    if (failed) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

// 读空一路管道。返回 1 表示遇到 EOF（写端全部关闭），0 表示暂时没数据。
static int drain_pipe(
    HANDLE pipe, char **data, size_t *length, size_t *capacity) {
    char buffer[TAP_PROCESS_READ_CHUNK];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) return 1;
        if (available == 0) return 0;
        DWORD chunk = available > (DWORD)sizeof(buffer)
                          ? (DWORD)sizeof(buffer)
                          : available;
        DWORD received = 0;
        if (!ReadFile(pipe, buffer, chunk, &received, NULL)) return 1;
        if (received == 0) return 1;
        if (buffer_append(data, length, capacity, buffer, (size_t)received) != 0) {
            return 1;
        }
    }
}

void *__tap_process_run(const String *program, const String *arguments,
                        const String *env, const String *working_dir,
                        int32_t mode) {
    if (!program || program->length == 0) return NULL;

    TapArguments args;
    if (arguments_parse(arguments, &args) != 0) return NULL;
    TapArguments vars;
    if (arguments_parse(env, &vars) != 0) {
        arguments_free(&args);
        return NULL;
    }

    void *result_handle = NULL;
    wchar_t *program_wide = NULL;
    wchar_t *cwd_wide = NULL;
    wchar_t *environment = NULL;
    WideBuffer command_line = {NULL, 0, 0};
    HANDLE out_read = NULL, out_write = NULL, err_read = NULL, err_write = NULL;

    program_wide = tap_wide_from_utf8(program->data, (size_t)program->length);
    if (!program_wide) goto cleanup;

    if (working_dir && working_dir->length > 0) {
        cwd_wide = tap_wide_from_utf8(
            working_dir->data ? working_dir->data : "", (size_t)working_dir->length);
        if (!cwd_wide) goto cleanup;
        // CreateProcessW 的 lpCurrentDirectory 只接受绝对路径（相对路径会被当成无效
        // 目录），而 POSIX 的 chdir 接受相对路径。按同样的基准解析成绝对路径，
        // 两边语义才一致 —— git、tar 这类工具全靠它。
        DWORD needed = GetFullPathNameW(cwd_wide, 0, NULL, NULL);
        if (needed > 0) {
            wchar_t *absolute = malloc(sizeof(wchar_t) * (size_t)needed);
            if (!absolute) goto cleanup;
            if (GetFullPathNameW(cwd_wide, needed, absolute, NULL) == 0) {
                free(absolute);
                goto cleanup;
            }
            free(cwd_wide);
            cwd_wide = absolute;
        }
    }

    if (wide_append_quoted(&command_line, program_wide) != 0) goto cleanup;
    for (size_t i = 0; i < args.count; i++) {
        wchar_t *argument = tap_wide_from_utf8(args.items[i], strlen(args.items[i]));
        if (!argument) goto cleanup;
        int failed = wide_append(&command_line, L" ", 1) != 0 ||
                     wide_append_quoted(&command_line, argument) != 0;
        free(argument);
        if (failed) goto cleanup;
    }

    environment = build_environment(&vars);

    if (mode == TAP_PROCESS_MODE_CAPTURE) {
        SECURITY_ATTRIBUTES security;
        security.nLength = sizeof(security);
        security.lpSecurityDescriptor = NULL;
        security.bInheritHandle = TRUE;
        if (!CreatePipe(&out_read, &out_write, &security, 0)) goto cleanup;
        // 读端只留给父进程：否则子进程也会持有读端，EOF 永远等不到。
        if (!SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0)) goto cleanup;
        if (!CreatePipe(&err_read, &err_write, &security, 0)) goto cleanup;
        if (!SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0)) goto cleanup;
    }

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    if (mode == TAP_PROCESS_MODE_CAPTURE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = out_write;
        startup.hStdError = err_write;
    }

    PROCESS_INFORMATION information;
    memset(&information, 0, sizeof(information));
    // 传了宽字符环境块就必须带 CREATE_UNICODE_ENVIRONMENT：不带的话 Win32 会把
    // 它当成 ANSI 块解释（每个宽字符被当成 1 字节），环境块立刻变成畸形数据。
    DWORD creation_flags = environment ? CREATE_UNICODE_ENVIRONMENT : 0;
    // lpApplicationName 传 NULL、命令行首段是程序名 → 和 execvp 一样按 PATH 搜索。
    BOOL started = CreateProcessW(
        NULL, command_line.data, NULL, NULL, TRUE, creation_flags, environment,
        cwd_wide, &startup, &information);
    DWORD start_error = GetLastError();

    // 父进程必须尽早关掉写端，否则读循环等不到 EOF。
    if (out_write) CloseHandle(out_write);
    if (err_write) CloseHandle(err_write);
    out_write = NULL;
    err_write = NULL;

    TapProcessResult *result = calloc(1, sizeof(TapProcessResult));
    if (!result) goto cleanup;

    if (!started) {
        // 程序不存在也按 127 报告（和 POSIX 的 _exit(127) 对齐）；
        // 工作目录无效同样归到这里 —— POSIX 侧是在子进程里 chdir 失败后 _exit(127)。
        // 只有真正的资源类失败才返回 NULL 让调用方看到 Err。
        if (start_error == ERROR_FILE_NOT_FOUND ||
            start_error == ERROR_PATH_NOT_FOUND ||
            start_error == ERROR_BAD_EXE_FORMAT ||
            start_error == ERROR_DIRECTORY) {
            result->status = TAP_PROCESS_MISSING_STATUS;
            result_handle = result;
        } else {
            fprintf(stderr, "failed to start process (Win32 error %lu)\n",
                    (unsigned long)start_error);
            free(result);
        }
        goto cleanup;
    }

    if (mode == TAP_PROCESS_MODE_CAPTURE) {
        int out_eof = 0;
        int err_eof = 0;
        while (!out_eof || !err_eof) {
            size_t out_before = result->out_length;
            size_t err_before = result->err_length;
            if (!out_eof) {
                out_eof = drain_pipe(
                    out_read, &result->out_data, &result->out_length,
                    &result->out_capacity);
            }
            if (!err_eof) {
                err_eof = drain_pipe(
                    err_read, &result->err_data, &result->err_length,
                    &result->err_capacity);
            }
            // 两路都没进展时让出 CPU：PeekNamedPipe 是非阻塞轮询。
            if (result->out_length == out_before && result->err_length == err_before) {
                Sleep(1);
            }
        }
    }

    WaitForSingleObject(information.hProcess, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(information.hProcess, &code)) {
        // 0xC0000000 以上是 NTSTATUS（除零、Ctrl-C 之类），归一化成 -1，
        // 和 POSIX 的「被信号终止」保持一致。
        result->status = code >= 0xC0000000u ? -1 : (int32_t)code;
    } else {
        result->status = -1;
    }
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
    result_handle = result;

cleanup:
    if (out_read) CloseHandle(out_read);
    if (err_read) CloseHandle(err_read);
    if (out_write) CloseHandle(out_write);
    if (err_write) CloseHandle(err_write);
    free(command_line.data);
    free(environment);
    free(cwd_wide);
    free(program_wide);
    arguments_free(&vars);
    arguments_free(&args);
    return result_handle;
}

// ---------------------------------------------------------------------------
// POSIX
// ---------------------------------------------------------------------------

#else // !_WIN32

static void decode_wait_status(TapProcessResult *result, int status) {
    if (WIFEXITED(status)) {
        result->status = WEXITSTATUS(status);
    } else {
        // 被信号杀掉或状态无效，tap 没有 Option，统一用 -1。
        result->status = -1;
    }
}

// 从两路管道同时读到 EOF。顺序读会死锁：子进程写 stderr 写满管道缓冲后会阻塞，
// 父进程却还在等 stdout，两边都不动。
static void capture_pipes(
    TapProcessResult *result, int out_fd, int err_fd, pid_t pid, int *out_status) {
    char buffer[TAP_PROCESS_READ_CHUNK];
    int out_open = 1;
    int err_open = 1;
    int reaped = 0;
    int status = 0;

    while (out_open || err_open) {
        struct pollfd fds[2];
        fds[0].fd = out_open ? out_fd : -1;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = err_open ? err_fd : -1;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int ready = poll(fds, 2, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) {
            // 超时只是「暂时没输出」，先看子进程还在不在：已经退出但管道被孙进程
            // 继承着没关时不能无限等下去，这也是旧实现 1 秒超时的本意。
            if (!reaped && waitpid(pid, &status, WNOHANG) == pid) {
                reaped = 1;
                continue;
            }
            if (reaped) break;
            continue;
        }

        if (out_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t received = read(out_fd, buffer, sizeof(buffer));
            if (received > 0) {
                buffer_append(&result->out_data, &result->out_length,
                              &result->out_capacity, buffer, (size_t)received);
            } else if (received == 0) {
                out_open = 0;
                close(out_fd);
                out_fd = -1;
            } else if (errno != EINTR && errno != EAGAIN) {
                out_open = 0;
                close(out_fd);
                out_fd = -1;
            }
        }
        if (err_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t received = read(err_fd, buffer, sizeof(buffer));
            if (received > 0) {
                buffer_append(&result->err_data, &result->err_length,
                              &result->err_capacity, buffer, (size_t)received);
            } else if (received == 0) {
                err_open = 0;
                close(err_fd);
                err_fd = -1;
            } else if (errno != EINTR && errno != EAGAIN) {
                err_open = 0;
                close(err_fd);
                err_fd = -1;
            }
        }
    }

    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
    if (!reaped) waitpid(pid, &status, 0);
    *out_status = status;
}

void *__tap_process_run(const String *program, const String *arguments,
                        const String *env, const String *working_dir,
                        int32_t mode) {
    if (!program || program->length == 0) return NULL;

    TapArguments args;
    if (arguments_parse(arguments, &args) != 0) return NULL;
    TapArguments vars;
    if (arguments_parse(env, &vars) != 0) {
        arguments_free(&args);
        return NULL;
    }

    char *program_data = NULL;
    char *cwd = NULL;
    char **argv = NULL;
    TapProcessResult *result = NULL;
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};

    program_data = copy_c_string(program);
    if (!program_data) goto cleanup;
    if (working_dir && working_dir->length > 0) {
        cwd = copy_c_string(working_dir);
        if (!cwd) goto cleanup;
    }

    argv = malloc(sizeof(char *) * (args.count + 2));
    if (!argv) goto cleanup;
    argv[0] = program_data;
    for (size_t i = 0; i < args.count; i++) argv[i + 1] = args.items[i];
    argv[args.count + 1] = NULL;

    if (mode == TAP_PROCESS_MODE_CAPTURE) {
        if (pipe(out_pipe) != 0) goto cleanup;
        if (pipe(err_pipe) != 0) goto cleanup;
    }

    pid_t pid = fork();
    if (pid < 0) goto cleanup;

    if (pid == 0) {
        // 子进程：所有 C 字符串都已经在 fork 之前建好，这里只做系统调用。
        if (mode == TAP_PROCESS_MODE_CAPTURE) {
            close(out_pipe[0]);
            close(err_pipe[0]);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            close(out_pipe[1]);
            close(err_pipe[1]);
        }
        if (cwd && chdir(cwd) != 0) _exit(TAP_PROCESS_MISSING_STATUS);
        for (size_t i = 0; i < vars.count; i++) {
            char *entry = vars.items[i];
            char *equals = strchr(entry, '=');
            if (!equals) continue;
            *equals = '\0';
            setenv(entry, equals + 1, 1);
            *equals = '=';
        }
        execvp(program_data, argv);
        _exit(TAP_PROCESS_MISSING_STATUS);
    }

    // 父进程
    if (out_pipe[1] >= 0) {
        close(out_pipe[1]);
        out_pipe[1] = -1;
    }
    if (err_pipe[1] >= 0) {
        close(err_pipe[1]);
        err_pipe[1] = -1;
    }

    result = calloc(1, sizeof(TapProcessResult));
    if (!result) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        goto cleanup;
    }

    int status = 0;
    if (mode == TAP_PROCESS_MODE_CAPTURE) {
        capture_pipes(result, out_pipe[0], err_pipe[0], pid, &status);
        out_pipe[0] = -1;
        err_pipe[0] = -1;
    } else {
        waitpid(pid, &status, 0);
    }
    decode_wait_status(result, status);

cleanup:
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    free(argv);
    free(cwd);
    free(program_data);
    arguments_free(&vars);
    arguments_free(&args);
    return result;
}

#endif // _WIN32

// ---------------------------------------------------------------------------
// 结果句柄的访问器（两个平台共用）
// ---------------------------------------------------------------------------

int32_t __tap_process_status(void *handle) {
    TapProcessResult *result = (TapProcessResult *)handle;
    return result ? result->status : -1;
}

void __tap_process_stdout(String *out, void *handle) {
    TapProcessResult *result = (TapProcessResult *)handle;
    if (!result) {
        __tap_bytes_to_string(out, (const uint8_t *)"", 0);
        return;
    }
    *out = tap_allocate_string_copy(result->out_data, result->out_length);
}

void __tap_process_stderr(String *out, void *handle) {
    TapProcessResult *result = (TapProcessResult *)handle;
    if (!result) {
        __tap_bytes_to_string(out, (const uint8_t *)"", 0);
        return;
    }
    *out = tap_allocate_string_copy(result->err_data, result->err_length);
}

int32_t __tap_process_free(void *handle) {
    TapProcessResult *result = (TapProcessResult *)handle;
    if (!result) return -1;
    free(result->out_data);
    free(result->err_data);
    free(result);
    return 0;
}
