#include "tap_runtime.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
// winsock2.h 必须先于 windows.h：windows.h 会拉进老的 winsock.h，两者一起用会冲突。
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
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

// 把 double 渲染成十进制字符串。先用 %.15g，再逐步提高精度直到结果能往返：
// 这样 0.1 输出 "0.1"，而圆周率输出 "3.141592653589793"。
// 精确的十进制展开需要大整数算法，交给 C 库的转换更可靠。
// 非有限值输出 "inf" / "-inf" / "nan"，调用方需要自行处理（JSON 没有这些写法）。
const char *__tap_format_f64(double value) {
    char buffer[64];

    // NaN 的符号位没有意义，而且各平台 0.0/0.0 产生的默认 NaN 符号并不一致
    //（x86-64 给 -nan，ARM64 给 nan）。统一成 "nan"，输出才可移植。
    // 这里用 value != value 判断，避免因为引入 math.h 而牵扯 libm 链接。
    if (value != value) {
        snprintf(buffer, sizeof(buffer), "nan");
    } else {
        for (int precision = 15; precision <= 17; precision++) {
            snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
            if (strtod(buffer, NULL) == value) break;
        }
    }

    size_t length = strlen(buffer);
    char *result = allocate_owned_string(length);
    memcpy(result, buffer, length);
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

// tap 的 struct 是值类型，close() 拿到的是副本，没法把调用方手里的句柄置空；
// 而 fclose 之后指针值本身不变，再读写就是 use-after-close。
// 这张登记表让已关闭（或从未打开）的句柄被判定为无效，把崩溃变成安全的空结果。
#define TAP_OPEN_FILE_CAPACITY 64
static FILE *open_files[TAP_OPEN_FILE_CAPACITY];

static int file_slot_of(FILE *file) {
    if (!file) return -1;
    for (int index = 0; index < TAP_OPEN_FILE_CAPACITY; index++) {
        if (open_files[index] == file) return index;
    }
    return -1;
}

static int file_register(FILE *file) {
    if (!file) return -1;
    for (int index = 0; index < TAP_OPEN_FILE_CAPACITY; index++) {
        if (open_files[index] == NULL) {
            open_files[index] = file;
            return 0;
        }
    }
    return -1;
}

static void file_unregister(FILE *file) {
    int slot = file_slot_of(file);
    if (slot >= 0) open_files[slot] = NULL;
}

// 打开文件；失败返回 NULL。mode 与 C 的 fopen 一致，调用方应带 b
//（"rb" 读 / "wb" 写 / "ab" 追加），否则 Windows 会把 \n 翻译成 \r\n。
void *__tap_file_open(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    FILE *file = fopen(path, mode);
    if (!file) return NULL;
    if (file_register(file) != 0) {
        fclose(file);
        fprintf(stderr, "too many open files (limit %d)\n", TAP_OPEN_FILE_CAPACITY);
        return NULL;
    }
    return file;
}

// 判断句柄是否有效；句柄为 NULL 或已关闭时返回 0。
int32_t __tap_file_is_open(void *handle) {
    return file_slot_of((FILE *)handle) >= 0;
}

// 读取最多 count 个字节，返回 Runtime 管理的字符串。
// 已到末尾、句柄无效或读取失败时返回空字符串，用 __tap_file_eof 区分。
const char *__tap_file_read(void *handle, uint64_t count) {
    FILE *file = (FILE *)handle;
    // 传空串而不是 NULL：长度为 0 时 bytes_to_string 仍会走一次 memcpy。
    if (file_slot_of(file) < 0) return __tap_bytes_to_string((const uint8_t *)"", 0);

    uint8_t *buffer = malloc((size_t)count + 1);
    if (!buffer) {
        fprintf(stderr, "failed to allocate read buffer\n");
        exit(1);
    }
    size_t received = fread(buffer, 1, (size_t)count, file);
    const char *result = __tap_bytes_to_string(buffer, received);
    free(buffer);
    return result;
}

// 写入字符串的全部字节，返回实际写入的字节数；句柄无效或失败返回 -1。
int64_t __tap_file_write(void *handle, const char *data) {
    FILE *file = (FILE *)handle;
    if (file_slot_of(file) < 0 || !data) return -1;

    size_t length = strlen(data);
    if (length == 0) return 0;
    size_t written = fwrite(data, 1, length, file);
    if (written != length) return -1;
    return (int64_t)written;
}

// 关闭文件；成功返回 0，句柄无效或关闭失败返回 -1。
int32_t __tap_file_close(void *handle) {
    FILE *file = (FILE *)handle;
    if (file_slot_of(file) < 0) return -1;
    file_unregister(file);
    return fclose(file) == 0 ? 0 : -1;
}

// 是否已经读到文件末尾；句柄无效时返回 0。
int32_t __tap_file_eof(void *handle) {
    FILE *file = (FILE *)handle;
    if (file_slot_of(file) < 0) return 0;
    return feof(file) != 0;
}

// 删除文件；成功返回 0，失败返回 -1。
int32_t __tap_file_remove(const char *path) {
    if (!path) return -1;
    return remove(path) == 0 ? 0 : -1;
}

// 浮点幂：base 的 exponent 次方。指数可以是小数或负数，靠 C 库的 pow 实现
//（需要 libm，链接时要带 -lm）。定义域外的输入按 C 的规则得到 nan 或 inf。
double __tap_pow_f64(double base, double exponent) {
    return pow(base, exponent);
}

// 格式化输出到标准输出，格式串语义和 printf 完全一致（内部是 vfprintf(stdout, ...)）。
// 返回写出的字符数，出错时为负数。
//
// 和 __tap_eprintf 对称：标准流的 FILE* 只在 C 侧拿得到，而 tap 的变参转发要求「一句纯转发、
// 实参正好是具名形参」，中间插不进一个取流句柄的调用，所以流固定在 Runtime 侧。
//
// 之所以不直接让 tap 绑 libc 的 printf：std.io 模块要导出名为 printf 的公开函数，而同一个
// 模块里不能再有一个同名的 extern 声明（全局函数名唯一）。用 __tap_ 前缀两边都干净。
int32_t __tap_printf(const char *format, ...) {
    if (!format) return -1;
    va_list arguments;
    va_start(arguments, format);
    int written = vfprintf(stdout, format, arguments);
    va_end(arguments);
    return written;
}

// 格式化输出到标准错误，格式串语义和 printf 完全一致（内部是 vfprintf(stderr, ...)）。
// 返回写出的字符数，出错时为负数。
//
// 单独放在 Runtime 而不是让 tap 侧直接绑 libc 的 fprintf：stderr 是 libc 的 FILE* 全局量，
// tap 里拿不到；而 tap 的变参转发要求「一句纯转发、实参正好是具名形参」，没法在中间插一个
// 取流句柄的调用。这里把流固定在 Runtime 侧，tap 侧就只剩一句干净的转发。
//
// stderr 按 C 标准默认不带全缓冲，所以不需要额外 flush。
int32_t __tap_eprintf(const char *format, ...) {
    if (!format) return -1;
    va_list arguments;
    va_start(arguments, format);
    int written = vfprintf(stderr, format, arguments);
    va_end(arguments);
    return written;
}

// 打印错误信息并以状态 1 终止程序，用于调用方无法合理恢复的情况。
// 前缀用 `panic: ` 和编译期诊断的 `error: ` 区分开。
// 函数不会返回；返回值只是为了让 tap 侧能把它当普通调用使用。
int32_t __tap_panic(const char *message) {
    fprintf(stderr, "panic: %s\n", message ? message : "(no message)");
    exit(1);
    return 0;
}

// 以调用方给的状态码终止程序。和 __tap_panic 的区别是这里不打印任何东西，
// 退出码也由调用方决定。走 libc 的 exit，所以标准库缓冲区会被正常刷新——
// 用 _exit 的话已经写进缓冲区但还没落盘的输出会丢。
// 函数不会返回；返回值只是为了让 tap 侧能把它当普通调用使用。
int32_t __tap_exit(int32_t code) {
    exit((int)code);
    return 0;
}

// ---------------------------------------------------------------------------
// TCP 套接字（IPv4）
//
// 这一层存在的唯一理由是跨平台：POSIX 直接把 int fd 交给 libc 的 BSD socket；
// Windows 的 winsock 必须先 WSAStartup、句柄是 64 位 SOCKET、关闭要用 closesocket。
// tap 没有条件编译，所以平台差异只能落在这里。
//
// 句柄统一成 int64：POSIX 的 fd 原样放进去（负值即无效），Windows 的 SOCKET 也是
// 小整数，而 INVALID_SOCKET 恰好等于 -1，所以「负值 = 无效」两边都成立。

#ifdef _WIN32
// winsock 必须先初始化。惰性做一次，避免给不用网络的程序加启动开销。
static int winsock_ready = 0;

static int ensure_winsock(void) {
    if (winsock_ready) return 0;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
    winsock_ready = 1;
    return 0;
}

// Windows 的 recv / send 长度参数是 int，超长时截断到 INT32_MAX。
static int clamp_length(uint64_t length) {
    return length > (uint64_t)INT32_MAX ? INT32_MAX : (int)length;
}
#endif

int64_t __tap_socket_create(void) {
#ifdef _WIN32
    if (ensure_winsock() != 0) return -1;
    SOCKET handle = socket(AF_INET, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET) return -1;
    return (int64_t)handle;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return fd < 0 ? -1 : (int64_t)fd;
#endif
}

int32_t __tap_socket_bind(int64_t handle, const uint8_t *address, uint32_t length) {
    if (handle < 0 || !address) return -1;
#ifdef _WIN32
    return bind((SOCKET)handle, (const struct sockaddr *)address, (int)length) == 0 ? 0 : -1;
#else
    return bind((int)handle, (const struct sockaddr *)address, (socklen_t)length) == 0 ? 0 : -1;
#endif
}

int32_t __tap_socket_listen(int64_t handle, int32_t backlog) {
    if (handle < 0) return -1;
#ifdef _WIN32
    return listen((SOCKET)handle, backlog) == 0 ? 0 : -1;
#else
    return listen((int)handle, backlog) == 0 ? 0 : -1;
#endif
}

int64_t __tap_socket_accept(int64_t handle) {
    if (handle < 0) return -1;
#ifdef _WIN32
    SOCKET peer = accept((SOCKET)handle, NULL, NULL);
    return peer == INVALID_SOCKET ? -1 : (int64_t)peer;
#else
    int fd = accept((int)handle, NULL, NULL);
    return fd < 0 ? -1 : (int64_t)fd;
#endif
}

int32_t __tap_socket_connect(int64_t handle, const uint8_t *address, uint32_t length) {
    if (handle < 0 || !address) return -1;
#ifdef _WIN32
    return connect((SOCKET)handle, (const struct sockaddr *)address, (int)length) == 0 ? 0 : -1;
#else
    return connect((int)handle, (const struct sockaddr *)address, (socklen_t)length) == 0 ? 0 : -1;
#endif
}

int32_t __tap_socket_getsockname(int64_t handle, uint8_t *address, uint32_t *length) {
    if (handle < 0 || !address || !length) return -1;
#ifdef _WIN32
    int native_length = (int)*length;
    if (getsockname((SOCKET)handle, (struct sockaddr *)address, &native_length) != 0) return -1;
    *length = (uint32_t)native_length;
    return 0;
#else
    socklen_t native_length = (socklen_t)*length;
    if (getsockname((int)handle, (struct sockaddr *)address, &native_length) != 0) return -1;
    *length = (uint32_t)native_length;
    return 0;
#endif
}

int64_t __tap_socket_recv(int64_t handle, uint8_t *buffer, uint64_t length) {
    if (handle < 0 || !buffer) return -1;
#ifdef _WIN32
    int received = recv((SOCKET)handle, (char *)buffer, clamp_length(length), 0);
    return received < 0 ? -1 : (int64_t)received;
#else
    ssize_t received = recv((int)handle, buffer, (size_t)length, 0);
    return received < 0 ? -1 : (int64_t)received;
#endif
}

int64_t __tap_socket_send(int64_t handle, const uint8_t *buffer, uint64_t length) {
    if (handle < 0 || !buffer) return -1;
#ifdef _WIN32
    int sent = send((SOCKET)handle, (const char *)buffer, clamp_length(length), 0);
    return sent < 0 ? -1 : (int64_t)sent;
#else
    ssize_t sent = send((int)handle, buffer, (size_t)length, 0);
    return sent < 0 ? -1 : (int64_t)sent;
#endif
}

// 发送字符串的全部字节（不含结尾 NUL）；返回实际发出的字节数，失败返回 -1。
int64_t __tap_socket_send_text(int64_t handle, const char *data) {
    if (handle < 0 || !data) return -1;
    return __tap_socket_send(handle, (const uint8_t *)data, (uint64_t)strlen(data));
}

int32_t __tap_socket_close(int64_t handle) {
    if (handle < 0) return -1;
#ifdef _WIN32
    return closesocket((SOCKET)handle) == 0 ? 0 : -1;
#else
    return close((int)handle) == 0 ? 0 : -1;
#endif
}
