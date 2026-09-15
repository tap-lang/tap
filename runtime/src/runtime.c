#include "4yue_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#define FOUR_YUE_ENVIRON _environ
#else
extern char **environ;
#define FOUR_YUE_ENVIRON environ
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

void __4yue_init_args(int32_t argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
}

int32_t __4yue_argc(void) {
    return saved_argc;
}

const char *__4yue_arg(int32_t index) {
    if (index < 0 || index >= saved_argc || !saved_argv || !saved_argv[index]) {
        return "";
    }
    return saved_argv[index];
}

// 获取指定名称的环境变量；不存在时返回空字符串，方便 4yue 侧直接当 string 使用。
const char *__4yue_env_var(const char *name) {
    if (!name || !name[0]) return "";
    const char *value = getenv(name);
    return value ? value : "";
}

// 返回当前进程环境变量数量。
int32_t __4yue_envc(void) {
    int32_t count = 0;
    if (!FOUR_YUE_ENVIRON) return 0;
    while (FOUR_YUE_ENVIRON[count]) {
        count++;
    }
    return count;
}

// 按索引返回环境变量原始条目，格式为 NAME=VALUE；越界时返回空字符串。
const char *__4yue_env(int32_t index) {
    if (index < 0 || !FOUR_YUE_ENVIRON) return "";
    for (int32_t current = 0; current <= index; current++) {
        if (!FOUR_YUE_ENVIRON[current]) return "";
    }
    return FOUR_YUE_ENVIRON[index];
}

int32_t __4yue_read_key(void) {
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

int32_t __4yue_sleep_ms(int32_t milliseconds) {
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

int32_t __4yue_clear_screen(void) {
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

int32_t __4yue_random(int32_t maximum) {
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
