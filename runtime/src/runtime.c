#include "tap_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifdef _WIN32
extern char **_environ;
#define TAP_ENVIRON _environ
#else
extern char **environ;
#define TAP_ENVIRON environ
#endif

#ifndef _WIN32
// POSIX terminals stay in raw mode between polls and are restored at process exit.
static struct termios original_terminal;
static int terminal_is_raw = 0;

static void restore_terminal(void) {
    if (terminal_is_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
        terminal_is_raw = 0;
    }
}

static void restore_terminal_on_signal(int signal_number) {
    // Restore terminal state before forwarding termination to the default handler.
    restore_terminal();
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static int configure_terminal(void) {
    if (terminal_is_raw) return 0;
    if (!isatty(STDIN_FILENO) ||
        tcgetattr(STDIN_FILENO, &original_terminal) != 0) {
        return -1;
    }

    // Disable canonical input and echo so keys remain available between game frames.
    struct termios raw = original_terminal;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
    terminal_is_raw = 1;
    if (atexit(restore_terminal) != 0) {
        restore_terminal();
        return -1;
    }
    // Interactive termination must not leave the user's terminal in raw mode.
    if (signal(SIGINT, restore_terminal_on_signal) == SIG_ERR ||
        signal(SIGTERM, restore_terminal_on_signal) == SIG_ERR) {
        restore_terminal();
        return -1;
    }
    return 0;
}

static int read_terminal_byte_with_timeout(int timeout_ms) {
    fd_set input;
    FD_ZERO(&input);
    FD_SET(STDIN_FILENO, &input);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (long)(timeout_ms % 1000) * 1000L
    };
    if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) <= 0) return -1;

    unsigned char key = 0;
    return read(STDIN_FILENO, &key, 1) == 1 ? (int)key : -1;
}

static int read_terminal_byte(void) {
    return read_terminal_byte_with_timeout(0);
}

static int read_escape_sequence_key(void) {
    int marker = read_terminal_byte_with_timeout(10);
    if (marker != '[' && marker != 'O') return 27;

    int key = read_terminal_byte_with_timeout(10);
    if (key == 'A' || key == 'B' || key == 'C' || key == 'D') return key;
    return 27;
}
#endif

static int32_t saved_argc = 0;
static char **saved_argv = NULL;

// Runtime 生成的字符串串成单向链表，在进程退出时统一释放。
typedef struct TapOwnedString {
    struct TapOwnedString *next;
    char value[];
} TapOwnedString;

static TapOwnedString *owned_strings = NULL;
static int owned_string_cleanup_registered = 0;

// 释放 slice、ByteVec.to_string 等 Runtime API 创建的全部字符串。
static void free_owned_strings(void) {
    while (owned_strings) {
        TapOwnedString *next = owned_strings->next;
        free(owned_strings);
        owned_strings = next;
    }
}

// 分配带链表节点的字符串缓冲区，并只注册一次退出清理函数。
static char *allocate_owned_string(size_t length) {
    TapOwnedString *allocation = malloc(sizeof(*allocation) + length + 1);
    if (!allocation) {
        fprintf(stderr, "failed to allocate runtime string\n");
        exit(1);
    }
    if (!owned_string_cleanup_registered) {
        if (atexit(free_owned_strings) != 0) {
            free(allocation);
            fprintf(stderr, "failed to register string cleanup\n");
            exit(1);
        }
        owned_string_cleanup_registered = 1;
    }
    allocation->next = owned_strings;
    owned_strings = allocation;
    return allocation->value;
}

// void __tap_init_console(void) {
// #ifdef _WIN32
//     // 控制台默认使用系统代码页（简体中文为 936），而 tap 源码里的字符串是 UTF-8 字节，
//     // 直接写出去会被按 GBK 解释成乱码。输出和输入代码页一并切到 UTF-8。
//     // 输出被重定向到文件或管道时本调用无副作用，写出的仍是原始 UTF-8 字节。
//     SetConsoleOutputCP(CP_UTF8);
//     SetConsoleCP(CP_UTF8);
// #endif
// }

void __tap_init_args(int32_t argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
}

int32_t __tap_argc(void) {
    return saved_argc;
}

const char *__tap_arg(int32_t index) {
    if (index < 0 || index >= saved_argc || !saved_argv || !saved_argv[index]) {
        return "";
    }
    return saved_argv[index];
}

// 获取指定名称的环境变量；不存在时返回空字符串，方便 tap 侧直接当 string 使用。
const char *__tap_env_var(const char *name) {
    if (!name || !name[0]) return "";
    const char *value = getenv(name);
    return value ? value : "";
}

// 返回当前进程环境变量数量。
int32_t __tap_envc(void) {
    int32_t count = 0;
    if (!TAP_ENVIRON) return 0;
    while (TAP_ENVIRON[count]) {
        count++;
    }
    return count;
}

// 按索引返回环境变量原始条目，格式为 NAME=VALUE；越界时返回空字符串。
const char *__tap_env(int32_t index) {
    if (index < 0 || !TAP_ENVIRON) return "";
    for (int32_t current = 0; current <= index; current++) {
        if (!TAP_ENVIRON[current]) return "";
    }
    return TAP_ENVIRON[index];
}

// 按 UTF-8 原始字节读取；负数或超过 strlen(value) 的索引均为错误。
uint8_t __tap_string_byte_at(const char *value, int64_t index) {
    size_t length = strlen(value);
    if (index < 0 || (uint64_t)index >= (uint64_t)length) {
        fprintf(stderr, "String byte index out of bounds: index=%lld, length=%llu\n",
                (long long)index, (unsigned long long)length);
        exit(1);
    }
    return (uint8_t)(unsigned char)value[index];
}

// 复制半开区间 [start, end)，结果由 Runtime 持有到进程退出。
const char *__tap_string_slice(const char *value, int64_t start, int64_t end) {
    size_t length = strlen(value);
    if (start < 0 || end < start || (uint64_t)end > (uint64_t)length) {
        fprintf(stderr,
                "String slice out of bounds: start=%lld, end=%lld, length=%llu\n",
                (long long)start, (long long)end, (unsigned long long)length);
        exit(1);
    }

    size_t slice_length = (size_t)(end - start);
    char *result = allocate_owned_string(slice_length);
    memcpy(result, value + start, slice_length);
    result[slice_length] = '\0';
    return result;
}

// 把 strcmp 的结果归一化，避免向 tap 暴露平台相关的具体返回值。
int32_t __tap_string_compare(const char *left, const char *right) {
    int result = strcmp(left, right);
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

// ByteVec.extend 的批量复制入口；调用方保证源长度和目标容量均足够。
int32_t __tap_string_copy_bytes(
    const char *value, uint8_t *destination, size_t length) {
    memcpy(destination, value, length);
    return 0;
}

// 复制 ByteVec 的有效字节并补 NUL；内部 NUL 无法用 tap string 表示。
const char *__tap_bytes_to_string(const uint8_t *data, size_t length) {
    if (memchr(data, 0, length)) {
        fprintf(stderr, "ByteVec cannot convert bytes containing NUL to string\n");
        exit(1);
    }
    char *result = allocate_owned_string(length);
    memcpy(result, data, length);
    result[length] = '\0';
    return result;
}

// 分配堆内存；size 为 0 时仍申请 1 字节，避免不同 C 库对 malloc(0) 的差异。
void *__tap_malloc(size_t size) {
    return malloc(size == 0 ? 1 : size);
}

// 调整堆内存大小；size 为 0 时仍保留 1 字节，调用者可用返回 NULL 判断失败。
void *__tap_realloc(void *pointer, size_t size) {
    return realloc(pointer, size == 0 ? 1 : size);
}

// 释放由 tap Runtime 分配的堆内存；NULL 指针安全无操作，返回 0 方便 tap 调用。
int32_t __tap_free(void *pointer) {
    free(pointer);
    return 0;
}

int32_t __tap_read_key(void) {
#ifdef _WIN32
    // Windows arrow and function keys use a prefix byte followed by a scan code.
    if (!_kbhit()) return -1;
    int key = _getch();
    if (key != 0 && key != 224) return (int32_t)key;

    return (int32_t)_getch();
#else
    // Non-interactive stdin has no terminal state and therefore no key event.
    if (configure_terminal() != 0) return -1;

    int key = read_terminal_byte();
    if (key != 27) return (int32_t)key;
    return (int32_t)read_escape_sequence_key();
#endif
}

int32_t __tap_sleep_ms(int32_t milliseconds) {
    if (milliseconds < 0) return -1;

#ifdef _WIN32
    // Sleep accepts milliseconds directly on Windows.
    Sleep((DWORD)milliseconds);
#else
    // Retry nanosleep when a signal interrupts the requested delay.
    struct timespec remaining = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) return -1;
    }
#endif
    return 0;
}

int32_t __tap_clear_screen(void) {
#ifdef _WIN32
    // Enable ANSI escape processing for modern Windows terminals when possible.
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif

    // ANSI clear-screen and cursor-home sequences also work on POSIX terminals.
    if (fputs("\x1b[2J\x1b[H", stdout) == EOF) return -1;
    return fflush(stdout) == 0 ? 0 : -1;
}

int32_t __tap_random(int32_t maximum) {
    static uint32_t state = 0;
    if (maximum <= 0) return 0;

    // Seed once, then use xorshift32 to avoid platform-specific libc random symbols.
    if (state == 0) {
        state = (uint32_t)time(NULL) ^ (uint32_t)clock() ^ UINT32_C(0x9e3779b9);
        if (state == 0) state = UINT32_C(0x6d2b79f5);
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (int32_t)(state % (uint32_t)maximum);
}
