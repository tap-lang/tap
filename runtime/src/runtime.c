#include "tap_runtime.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
// winsock2.h 必须先于 windows.h：windows.h 会拉进老的 winsock.h，两者一起用会冲突。
#include <winsock2.h>
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

// 构造一个不拥有缓冲区的 String 视图。NULL 统一规整为空串，避免空 String 携带空指针。
static String string_view(const char *data, size_t length) {
    String result = {data ? data : "", (uint64_t)length};
    return result;
}

// 把 C 的零结尾字符串借用为 String。这里只有 C 边界允许调用 strlen；进入 String 世界后
// 所有操作都必须使用随值携带的 length，才能正确处理内嵌 NUL。
static String string_from_c(const char *data) {
    return string_view(data, data ? strlen(data) : 0);
}

// 空字符串使用静态字面量作为 data，满足“即使长度为 0，data 也可读且以 NUL 结尾”的约定。
static String empty_string(void) {
    return string_view("", 0);
}

String __tap_string_from_c(const char *value) {
    return string_from_c(value);
}

// 系统 API 仍然接收 C 字符串。tap 字符串保留尾部 NUL，但内嵌 NUL 必须拒绝，
// 否则系统调用看到的内容会和 tap 的 length 不一致。
static const char *require_c_string(String value, const char *purpose) {
    if (!value.data || value.length > SIZE_MAX ||
        memchr(value.data, 0, (size_t)value.length) != NULL) {
        fprintf(stderr, "%s cannot contain NUL bytes\n", purpose);
        return NULL;
    }
    return value.data;
}

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

// 复制精确的 length 个字节并补尾部 NUL。length 独立保存，所以中间出现 NUL 不会截断内容。
static String allocate_string_copy(const void *data, size_t length) {
    char *result = allocate_owned_string(length);
    if (length > 0) memcpy(result, data, length);
    result[length] = '\0';
    return string_view(result, length);
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

String __tap_arg(int32_t index) {
    if (index < 0 || index >= saved_argc || !saved_argv || !saved_argv[index]) {
        return empty_string();
    }
    return string_from_c(saved_argv[index]);
}

// 获取指定名称的环境变量；不存在时返回空字符串，方便 tap 侧直接当 String 使用。
String __tap_env_var(String name) {
    const char *name_data = require_c_string(name, "environment variable name");
    if (!name_data || name.length == 0) return empty_string();
    const char *value = getenv(name_data);
    return value ? string_from_c(value) : empty_string();
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
String __tap_env(int32_t index) {
    if (index < 0 || !TAP_ENVIRON) return empty_string();
    for (int32_t current = 0; current <= index; current++) {
        if (!TAP_ENVIRON[current]) return empty_string();
    }
    return string_from_c(TAP_ENVIRON[index]);
}

// 按 UTF-8 原始字节读取；负数或超过 length 的索引均为错误。
uint8_t __tap_string_byte_at(String value, int64_t index) {
    if (index < 0 || (uint64_t)index >= value.length) {
        fprintf(stderr, "String byte index out of bounds: index=%lld, length=%llu\n",
                (long long)index, (unsigned long long)value.length);
        exit(1);
    }
    return (uint8_t)(unsigned char)value.data[index];
}

// 统计 UTF-8 码点数量。遇到格式错误的字节时把它当作一个独立字符，
// 这样即使字符串来自未经校验的原始字节，也不会越界或陷入死循环。
uint64_t __tap_string_char_count(String value) {
    const unsigned char *bytes = (const unsigned char *)value.data;
    size_t length = (size_t)value.length;
    size_t offset = 0;
    uint64_t count = 0;
    while (offset < length) {
        unsigned char first = bytes[offset];
        size_t sequence_length = 1;
        int valid = 1;
        if (first >= 0xC2 && first <= 0xDF) {
            sequence_length = 2;
            valid = sequence_length <= length - offset &&
                    bytes[offset + 1] >= 0x80 && bytes[offset + 1] <= 0xBF;
        } else if (first >= 0xE0 && first <= 0xEF) {
            sequence_length = 3;
            valid = sequence_length <= length - offset &&
                    bytes[offset + 1] >= 0x80 && bytes[offset + 1] <= 0xBF &&
                    bytes[offset + 2] >= 0x80 && bytes[offset + 2] <= 0xBF;
            // E0 80..9F is overlong; ED A0..BF encodes UTF-16 surrogates.
            if (first == 0xE0 && valid) valid = bytes[offset + 1] >= 0xA0;
            if (first == 0xED && valid) valid = bytes[offset + 1] <= 0x9F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            sequence_length = 4;
            valid = sequence_length <= length - offset &&
                    bytes[offset + 1] >= 0x80 && bytes[offset + 1] <= 0xBF &&
                    bytes[offset + 2] >= 0x80 && bytes[offset + 2] <= 0xBF &&
                    bytes[offset + 3] >= 0x80 && bytes[offset + 3] <= 0xBF;
            // F0 80..8F is overlong; F4 90..BF exceeds U+10FFFF.
            if (first == 0xF0 && valid) valid = bytes[offset + 1] >= 0x90;
            if (first == 0xF4 && valid) valid = bytes[offset + 1] <= 0x8F;
        } else if (first >= 0x80) {
            // Continuation bytes, C0/C1, and F5..FF are invalid standalone bytes.
            valid = 0;
        }
        offset += valid ? sequence_length : 1;
        count++;
    }
    return count;
}

// 复制半开区间 [start, end)，结果由 Runtime 持有到进程退出。
String __tap_string_slice(String value, int64_t start, int64_t end) {
    if (start < 0 || end < start || (uint64_t)end > value.length) {
        fprintf(stderr,
                "String slice out of bounds: start=%lld, end=%lld, length=%llu\n",
                (long long)start, (long long)end, (unsigned long long)value.length);
        exit(1);
    }

    size_t slice_length = (size_t)(end - start);
    return allocate_string_copy(value.data + start, slice_length);
}

// 按无符号 UTF-8 字节序比较；共同前缀相同时短串排在前面。
int32_t __tap_string_compare(String left, String right) {
    size_t common = (size_t)(left.length < right.length ? left.length : right.length);
    int result = common > 0 ? memcmp(left.data, right.data, common) : 0;
    if (result == 0) {
        result = left.length < right.length ? -1 : left.length > right.length ? 1 : 0;
    }
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

// 拼接两个以 NUL 结尾的字符串；结果由 Runtime 持有到进程退出。
String __tap_string_concat(String left, String right) {
    size_t left_length = (size_t)left.length;
    size_t right_length = (size_t)right.length;
    if (left_length > SIZE_MAX - 1 ||
        right_length > SIZE_MAX - 1 - left_length) {
        fprintf(stderr, "string concatenation is too long\n");
        exit(1);
    }

    size_t length = left_length + right_length;
    char *result = allocate_owned_string(length);
    memcpy(result, left.data, left_length);
    memcpy(result + left_length, right.data, right_length);
    result[length] = '\0';
    return string_view(result, length);
}

// 判断 value 是否包含 needle。needle 为空串时总返回 1。
// 按字节匹配，不涉及 UTF-8 码点边界。
int32_t __tap_string_contains(String value, String needle) {
    if (needle.length == 0) return 1;
    if (needle.length > value.length) return 0;
    size_t limit = (size_t)(value.length - needle.length);
    for (size_t i = 0; i <= limit; i++) {
        if (memcmp(value.data + i, needle.data, (size_t)needle.length) == 0)
            return 1;
    }
    return 0;
}

// 判断 value 是否以 prefix 开头。
int32_t __tap_string_starts_with(String value, String prefix) {
    if (prefix.length > value.length) return 0;
    if (prefix.length == 0) return 1;
    return memcmp(value.data, prefix.data, (size_t)prefix.length) == 0 ? 1 : 0;
}

// 判断 value 是否以 suffix 结尾。
int32_t __tap_string_ends_with(String value, String suffix) {
    if (suffix.length > value.length) return 0;
    if (suffix.length == 0) return 1;
    return memcmp(value.data + (value.length - suffix.length),
                  suffix.data, (size_t)suffix.length) == 0 ? 1 : 0;
}

// 把 value 中所有 from 的出现替换为 to。from 为空串时返回 value 的副本。
// 结果由 Runtime 持有到进程退出。
String __tap_string_replace(String value, String from, String to) {
    // from 为空串：直接复制 value，避免无限循环。
    if (from.length == 0)
        return allocate_string_copy(value.data, (size_t)value.length);

    // 先扫描一遍统计替换次数，算出结果长度，避免反复 realloc。
    size_t from_len = (size_t)from.length;
    size_t to_len = (size_t)to.length;
    size_t match_count = 0;
    size_t pos = 0;
    while (pos + from_len <= (size_t)value.length) {
        if (memcmp(value.data + pos, from.data, from_len) == 0) {
            match_count++;
            pos += from_len;
        } else {
            pos++;
        }
    }

    size_t result_length = (size_t)value.length + match_count * (to_len - from_len);
    char *result = allocate_owned_string(result_length);
    size_t out = 0;
    pos = 0;
    while (pos < (size_t)value.length) {
        if (pos + from_len <= (size_t)value.length &&
            memcmp(value.data + pos, from.data, from_len) == 0) {
            memcpy(result + out, to.data, to_len);
            out += to_len;
            pos += from_len;
        } else {
            result[out++] = value.data[pos++];
        }
    }
    result[result_length] = '\0';
    return string_view(result, result_length);
}

// 把有符号整数格式化为 Runtime 管理的十进制字符串。
String __tap_format_i64(int64_t value) {
    char buffer[32];
    int length = snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        fprintf(stderr, "failed to format integer\n");
        exit(1);
    }
    return allocate_string_copy(buffer, (size_t)length);
}

// 把无符号整数格式化为 Runtime 管理的十进制字符串。
String __tap_format_u64(uint64_t value) {
    char buffer[32];
    int length = snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        fprintf(stderr, "failed to format integer\n");
        exit(1);
    }
    return allocate_string_copy(buffer, (size_t)length);
}

// 128 位无符号数的四个 32 位小端 limb。手工实现十进制除法和乘法，
// 不依赖编译器的 __int128 扩展，因此 MSVC、MinGW 和 Cygwin 都使用同一份代码。
static void u128_from_halves(uint64_t low, uint64_t high, uint32_t limbs[4]) {
    limbs[0] = (uint32_t)low;
    limbs[1] = (uint32_t)(low >> 32);
    limbs[2] = (uint32_t)high;
    limbs[3] = (uint32_t)(high >> 32);
}

static void u128_to_halves(const uint32_t limbs[4], uint64_t *low, uint64_t *high) {
    if (low) *low = ((uint64_t)limbs[1] << 32) | limbs[0];
    if (high) *high = ((uint64_t)limbs[3] << 32) | limbs[2];
}

// limbs /= 10，返回余数。
static uint32_t u128_div10(uint32_t limbs[4]) {
    uint64_t remainder = 0;
    for (int index = 3; index >= 0; index--) {
        uint64_t current = (remainder << 32) | limbs[index];
        limbs[index] = (uint32_t)(current / 10);
        remainder = current % 10;
    }
    return (uint32_t)remainder;
}

// limbs = limbs * 10 + digit，返回是否溢出 128 位。
static int u128_mul10_add(uint32_t limbs[4], uint32_t digit) {
    uint64_t carry = digit;
    for (int index = 0; index < 4; index++) {
        uint64_t current = (uint64_t)limbs[index] * 10 + carry;
        limbs[index] = (uint32_t)current;
        carry = current >> 32;
    }
    return carry != 0;
}

static int u128_is_zero(const uint32_t limbs[4]) {
    return limbs[0] == 0 && limbs[1] == 0 && limbs[2] == 0 && limbs[3] == 0;
}

static int u128_compare(const uint32_t left[4], const uint32_t right[4]) {
    for (int index = 3; index >= 0; index--) {
        if (left[index] != right[index]) return left[index] > right[index] ? 1 : -1;
    }
    return 0;
}

static void u128_twos_complement(uint32_t limbs[4]) {
    uint64_t carry = 1;
    for (int index = 0; index < 4; index++) {
        uint64_t current = (uint64_t)(~limbs[index]) + carry;
        limbs[index] = (uint32_t)current;
        carry = current >> 32;
    }
}

static String format_u128_limbs(const uint32_t input[4], int negative) {
    uint32_t limbs[4];
    memcpy(limbs, input, sizeof(limbs));
    char reversed[40]; // ceil(log10(2^128)) == 39
    size_t length = 0;
    do {
        reversed[length++] = (char)('0' + u128_div10(limbs));
    } while (!u128_is_zero(limbs));

    size_t total = length + (negative ? 1 : 0);
    char *result = allocate_owned_string(total);
    size_t offset = 0;
    if (negative) result[offset++] = '-';
    while (length > 0) result[offset++] = reversed[--length];
    result[offset] = '\0';
    return string_view(result, total);
}

String __tap_format_u128(uint64_t low, uint64_t high) {
    uint32_t limbs[4];
    u128_from_halves(low, high, limbs);
    return format_u128_limbs(limbs, 0);
}

String __tap_format_i128(uint64_t low, uint64_t high) {
    uint32_t limbs[4];
    u128_from_halves(low, high, limbs);
    int negative = (limbs[3] & UINT32_C(0x80000000)) != 0;
    if (negative) u128_twos_complement(limbs);
    return format_u128_limbs(limbs, negative);
}

// 把布尔值格式化为 `true` 或 `false`。
String __tap_format_bool(int32_t value) {
    return value ? string_view("true", 4) : string_view("false", 5);
}

// 把 Unicode 码点编码为 Runtime 管理的 UTF-8 字符串。
String __tap_format_char(uint32_t value) {
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        fprintf(stderr, "invalid Unicode code point U+%04X\n", value);
        exit(1);
    }
    size_t length = value <= 0x7F ? 1 : value <= 0x7FF ? 2 : value <= 0xFFFF ? 3 : 4;
    char *result = allocate_owned_string(length);
    if (length == 1) {
        result[0] = (char)value;
    } else if (length == 2) {
        result[0] = (char)(0xC0 | (value >> 6));
        result[1] = (char)(0x80 | (value & 0x3F));
    } else if (length == 3) {
        result[0] = (char)(0xE0 | (value >> 12));
        result[1] = (char)(0x80 | ((value >> 6) & 0x3F));
        result[2] = (char)(0x80 | (value & 0x3F));
    } else {
        result[0] = (char)(0xF0 | (value >> 18));
        result[1] = (char)(0x80 | ((value >> 12) & 0x3F));
        result[2] = (char)(0x80 | ((value >> 6) & 0x3F));
        result[3] = (char)(0x80 | (value & 0x3F));
    }
    result[length] = '\0';
    return string_view(result, length);
}

static void invalid_numeric_string(const char *kind, String value) {
    fprintf(stderr, "invalid %s string '%.*s'\n", kind,
            value.length > INT_MAX ? INT_MAX : (int)value.length,
            value.data ? value.data : "<null>");
    exit(1);
}

// 手工解析无符号十进制整数，避免 strtoull 在不同平台上的 errno 行为差异。
uint64_t __tap_parse_u64(String value) {
    const char *text = require_c_string(value, "numeric string");
    if (!text || value.length == 0) invalid_numeric_string("unsigned integer", value);
    const char *cursor = text;
    if (*cursor == '+') cursor++;
    if (!*cursor) invalid_numeric_string("unsigned integer", value);

    uint64_t result = 0;
    for (; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            invalid_numeric_string("unsigned integer", value);
        }
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            invalid_numeric_string("unsigned integer", value);
        }
        result = result * 10 + digit;
    }
    return result;
}

// 手工解析有符号十进制整数，同时接受 i64 的最小值。
int64_t __tap_parse_i64(String value) {
    const char *text = require_c_string(value, "numeric string");
    if (!text || value.length == 0) invalid_numeric_string("integer", value);
    const char *cursor = text;
    int negative = 0;
    if (*cursor == '-' || *cursor == '+') {
        negative = *cursor == '-';
        cursor++;
    }
    if (!*cursor) invalid_numeric_string("integer", value);

    const uint64_t positive_limit = UINT64_C(9223372036854775807);
    const uint64_t negative_limit = UINT64_C(9223372036854775808);
    const uint64_t limit = negative ? negative_limit : positive_limit;
    uint64_t magnitude = 0;
    for (; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            invalid_numeric_string("integer", value);
        }
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (magnitude > (limit - digit) / 10) {
            invalid_numeric_string("integer", value);
        }
        magnitude = magnitude * 10 + digit;
    }
    if (!negative) return (int64_t)magnitude;
    if (magnitude == negative_limit) return INT64_MIN;
    return -(int64_t)magnitude;
}

static void parse_u128_limbs(String value, uint32_t limbs[4], int is_signed) {
    const char *text = require_c_string(value, "numeric string");
    if (!text || value.length == 0) invalid_numeric_string("integer", value);
    const char *cursor = text;
    int negative = 0;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        cursor++;
    }
    if (!*cursor || (negative && !is_signed)) {
        invalid_numeric_string(is_signed ? "integer" : "unsigned integer", value);
    }

    memset(limbs, 0, sizeof(uint32_t) * 4);
    for (; *cursor; cursor++) {
        if (*cursor < '0' || *cursor > '9' || u128_mul10_add(limbs, (uint32_t)(*cursor - '0'))) {
            invalid_numeric_string(is_signed ? "integer" : "unsigned integer", value);
        }
    }

    // 有符号数的正数上限是 2^127-1，负数允许到 -2^127；无符号数上限是 2^128-1。
    uint32_t limit[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    if (is_signed) {
        limit[3] = negative ? UINT32_C(0x80000000) : UINT32_C(0x7fffffff);
        if (u128_compare(limbs, limit) > 0) {
            invalid_numeric_string("integer", value);
        }
    }
    if (negative) u128_twos_complement(limbs);
}

void __tap_parse_u128(String value, uint64_t *low, uint64_t *high) {
    uint32_t limbs[4];
    parse_u128_limbs(value, limbs, 0);
    u128_to_halves(limbs, low, high);
}

void __tap_parse_i128(String value, uint64_t *low, uint64_t *high) {
    uint32_t limbs[4];
    parse_u128_limbs(value, limbs, 1);
    u128_to_halves(limbs, low, high);
}

// 解析布尔文本；同时接受 true/false 和 1/0，比较大小写敏感。
int32_t __tap_parse_bool(String value) {
    if ((value.length == 4 && memcmp(value.data, "true", 4) == 0) ||
        (value.length == 1 && value.data[0] == '1')) return 1;
    if ((value.length == 5 && memcmp(value.data, "false", 5) == 0) ||
        (value.length == 1 && value.data[0] == '0')) return 0;
    invalid_numeric_string("boolean", value);
    return 0;
}

// ByteVec.extend 的批量复制入口；调用方保证源长度和目标容量均足够。
int32_t __tap_string_copy_bytes(
    String value, uint8_t *destination, size_t length) {
    if ((uint64_t)length > value.length) return -1;
    memcpy(destination, value.data, length);
    return 0;
}

// 复制 ByteVec 的有效字节并补兼容 C API 的尾部 NUL；length 仍保留内嵌 NUL。
String __tap_bytes_to_string(const uint8_t *data, size_t length) {
    return allocate_string_copy(data, length);
}

// 把 double 渲染成十进制字符串。先用 %.15g，再逐步提高精度直到结果能往返：
// 这样 0.1 输出 "0.1"，而圆周率输出 "3.141592653589793"。
// 精确的十进制展开需要大整数算法，交给 C 库的转换更可靠。
// 非有限值输出 "inf" / "-inf" / "nan"，调用方需要自行处理（JSON 没有这些写法）。
String __tap_format_f64(double value) {
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
    return allocate_string_copy(buffer, length);
}

// 与 f64 版本相同，但按 float 的有效精度寻找最短往返表示，避免
// `3.14 as String` 先提升成 double 后暴露出 f32 的二进制尾数。
String __tap_format_f32(float value) {
    char buffer[64];
    if (value != value) {
        snprintf(buffer, sizeof(buffer), "nan");
    } else {
        for (int precision = 6; precision <= 9; precision++) {
            snprintf(buffer, sizeof(buffer), "%.*g", precision, (double)value);
            if (strtof(buffer, NULL) == value) break;
        }
    }
    size_t length = strlen(buffer);
    return allocate_string_copy(buffer, length);
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
void *__tap_file_open(String path, String mode) {
    const char *path_data = require_c_string(path, "file path");
    const char *mode_data = require_c_string(mode, "file mode");
    if (!path_data || !mode_data) return NULL;
    FILE *file = fopen(path_data, mode_data);
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
String __tap_file_read(void *handle, uint64_t count) {
    FILE *file = (FILE *)handle;
    // 传空串而不是 NULL：长度为 0 时 bytes_to_string 仍会走一次 memcpy。
    if (file_slot_of(file) < 0) return __tap_bytes_to_string((const uint8_t *)"", 0);

    uint8_t *buffer = malloc((size_t)count + 1);
    if (!buffer) {
        fprintf(stderr, "failed to allocate read buffer\n");
        exit(1);
    }
    size_t received = fread(buffer, 1, (size_t)count, file);
    String result = __tap_bytes_to_string(buffer, received);
    free(buffer);
    return result;
}

// 写入字符串的全部字节，返回实际写入的字节数；句柄无效或失败返回 -1。
int64_t __tap_file_write(void *handle, String data) {
    FILE *file = (FILE *)handle;
    if (file_slot_of(file) < 0 || !data.data) return -1;

    size_t length = (size_t)data.length;
    if (length == 0) return 0;
    size_t written = fwrite(data.data, 1, length, file);
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
int32_t __tap_file_remove(String path) {
    const char *path_data = require_c_string(path, "file path");
    if (!path_data) return -1;
    return remove(path_data) == 0 ? 0 : -1;
}

// popen / pclose 在严格 POSIX 模式下（glibc 不设 _POSIX_C_SOURCE 时）不会通过 <stdio.h>
// 暴露。Windows 使用带下划线的 CRT 名称；统一映射到下面的 Runtime 实现，避免把平台
// 差异泄露给 tap 标准库。
#ifdef _WIN32
#define TAP_POPEN _popen
#define TAP_PCLOSE _pclose
#else
// 直接声明一次即可覆盖严格的 feature-test 宏组合；和 libc 的声明保持相同签名，
// 不需要为了暴露这两个 POSIX/XSI 符号而修改全局宏。
extern FILE *popen(const char *command, const char *mode);
extern int pclose(FILE *stream);
#define TAP_POPEN popen
#define TAP_PCLOSE pclose
#endif

// 运行一条 shell 命令，返回 wait status。0 表示命令成功退出；非 0 表示命令不存在、
// 非零退出或被信号打断。调用方判断「成不成功」用 `status != 0`。
// （POSIX 下是被 << 8 编码过的 wait status，Windows 下是直接退出码。）
int32_t __tap_system(String command) {
    const char *command_data = require_c_string(command, "system command");
    if (!command_data) return -1;
    return (int32_t)system(command_data);
}

// 打开管道，返回不透明句柄（只读 "r" / 只写 "w"）；失败返回 NULL。
void *__tap_popen(String command, String mode) {
    const char *command_data = require_c_string(command, "popen command");
    const char *mode_data = require_c_string(mode, "popen mode");
    if (!command_data || !mode_data) return NULL;
    return TAP_POPEN(command_data, mode_data);
}

// 关闭管道并等待子进程，返回 wait status（语义同 __tap_system）。
int32_t __tap_pclose(void *stream) {
    if (!stream) return -1;
    return (int32_t)TAP_PCLOSE((FILE *)stream);
}

// 从一个已打开的流读取最多 count 字节，返回 Runtime 管理的字符串。
// 和 __tap_file_read 的区别：不做 open_files 表登记校验，因为 popen 返回的
// FILE* 没进过表；POSIX/Cygwin 直接对底层描述符做一次 read，Windows 用 fread，
// 管道读取专用。读取失败或到末尾时返回空串。
String __tap_stream_read(void *stream, uint64_t count) {
    FILE *file = (FILE *)stream;
    if (!file) return __tap_bytes_to_string((const uint8_t *)"", 0);

    // fread() is allowed to wait until all requested elements are available.
    // That behavior is particularly troublesome for Cygwin pipes: a command
    // that has produced a short line can leave a large fread request waiting
    // for EOF. Use the descriptor directly on POSIX platforms so this API
    // really means "read once" and returns whatever the pipe currently gives
    // us. The stream has not been touched through stdio yet, so mixing the
    // descriptor read with pclose/fclose is safe here.

    uint8_t *buffer = malloc((size_t)count + 1);
    if (!buffer) {
        fprintf(stderr, "failed to allocate read buffer\n");
        exit(1);
    }
    size_t received = 0;
#ifdef _WIN32
    received = fread(buffer, 1, (size_t)count, file);
#else
    int descriptor = fileno(file);
    if (descriptor >= 0) {
        ssize_t result = read(descriptor, buffer, (size_t)count);
        if (result > 0) received = (size_t)result;
    }
#endif
    String result = __tap_bytes_to_string(buffer, received);
    free(buffer);
    return result;
}

// 句柄是否为空指针：NULL 返回 1，否则 0。popen 失败时调用方用它判断。
int32_t __tap_ptr_is_null(void *pointer) {
    return pointer == NULL ? 1 : 0;
}

// 把 system / pclose 返回的状态码转换成跨平台的退出码。
// POSIX 状态码由 wait macros 解码；Windows 的 _pclose 直接返回退出码。
int32_t __tap_process_exit_code(int32_t status) {
    if (status < 0) return -1;
#ifdef _WIN32
    return status;
#else
    if (!WIFEXITED(status)) return -1;
    return (int32_t)WEXITSTATUS(status);
#endif
}

// 判断子进程是否因信号终止。system / pclose 失败时返回 0，退出码由上面的函数返回 -1。
int32_t __tap_process_was_signaled(int32_t status) {
    if (status < 0) return 0;
#ifdef _WIN32
    (void)status;
    return 0;
#else
    return WIFSIGNALED(status) ? 1 : 0;
#endif
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

// ---------------------------------------------------------------------------
// 目录遍历
//
// 打开时把条目名一次性读进内存并排序：readdir 的顺序是未定义的，排序之后调用方
// 拿到的结果才稳定（包管理这类工具要靠它做可复现的输出）。
// 句柄就是下面这个结构体，通过不透明指针传给 tap 侧。
// ---------------------------------------------------------------------------

typedef struct {
    char **names;
    int32_t count;
    int32_t index;
} TapDirectory;

static int compare_directory_names(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

// 释放目录句柄。keep_names 为真时保留条目名本身：它们已经通过 __tap_dir_next
// 交给了 tap 侧（可能被存进 Vec），释放会让调用方拿到悬空指针。
// 和 tap 其他 Runtime 字符串一样，这些名字活到进程结束。
static void free_directory(TapDirectory *directory, int keep_names) {
    if (!directory) return;
    if (!keep_names) {
        for (int32_t index = 0; index < directory->count; index++) {
            free(directory->names[index]);
        }
    }
    free(directory->names);
    free(directory);
}

static int directory_append(
    TapDirectory *directory, int32_t *capacity, const char *name) {
    if (directory->count == *capacity) {
        int32_t next = (*capacity) * 2;
        char **grown = realloc(directory->names, sizeof(char *) * (size_t)next);
        if (!grown) return -1;
        directory->names = grown;
        *capacity = next;
    }

    size_t length = strlen(name);
    char *copy = malloc(length + 1);
    if (!copy) return -1;
    memcpy(copy, name, length + 1);
    directory->names[directory->count++] = copy;
    return 0;
}

// 打开目录准备遍历；失败返回 NULL。条目里不含 "." 和 ".."。
void *__tap_dir_open(String path) {
    const char *path_data = require_c_string(path, "directory path");
    if (!path_data) return NULL;

    TapDirectory *directory = calloc(1, sizeof(TapDirectory));
    if (!directory) return NULL;

    int32_t capacity = 8;
    directory->names = malloc(sizeof(char *) * (size_t)capacity);
    if (!directory->names) {
        free(directory);
        return NULL;
    }

#ifdef _WIN32
    size_t path_length = (size_t)path.length;
    char *pattern = malloc(path_length + 3);
    if (!pattern) {
        free_directory(directory, 0);
        return NULL;
    }
    memcpy(pattern, path_data, path_length);
    pattern[path_length] = '\\';
    pattern[path_length + 1] = '*';
    pattern[path_length + 2] = '\0';

    struct _finddata_t entry;
    intptr_t handle = _findfirst(pattern, &entry);
    free(pattern);
    if (handle == -1) {
        free_directory(directory, 0);
        return NULL;
    }
    do {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) continue;
        if (directory_append(directory, &capacity, entry.name) != 0) {
            _findclose(handle);
            free_directory(directory, 0);
            return NULL;
        }
    } while (_findnext(handle, &entry) == 0);
    _findclose(handle);
#else
    DIR *handle = opendir(path_data);
    if (!handle) {
        free_directory(directory, 0);
        return NULL;
    }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (directory_append(directory, &capacity, entry->d_name) != 0) {
            closedir(handle);
            free_directory(directory, 0);
            return NULL;
        }
    }
    closedir(handle);
#endif

    if (directory->count > 1) {
        qsort(directory->names, (size_t)directory->count, sizeof(char *),
              compare_directory_names);
    }
    return directory;
}

// 取下一条目名；已到末尾返回空字符串。名字由 Runtime 持有，调用方不要释放。
String __tap_dir_next(void *handle) {
    TapDirectory *directory = (TapDirectory *)handle;
    if (!directory || directory->index >= directory->count) return empty_string();
    return string_from_c(directory->names[directory->index++]);
}

// 句柄是否有效。tap 没有空指针字面量，有效性只能由 Runtime 判断。
int32_t __tap_dir_is_open(void *handle) {
    return handle != NULL ? 1 : 0;
}

// 关闭目录句柄并释放条目名；成功返回 0。
int32_t __tap_dir_close(void *handle) {
    if (!handle) return -1;
    free_directory((TapDirectory *)handle, 1);
    return 0;
}

// path 是不是目录；不存在或不是目录都返回 0。
int32_t __tap_path_is_directory(String path) {
    const char *path_data = require_c_string(path, "path");
    if (!path_data) return 0;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path_data);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? 1 : 0;
#else
    struct stat info;
    if (stat(path_data, &info) != 0) return 0;
    return S_ISDIR(info.st_mode) ? 1 : 0;
#endif
}

// 建符号链接：link_path 指向 target。成功返回 0。
// Windows 上需要管理员权限或开启开发者模式，失败时调用方应回退到复制。
int32_t __tap_symlink(String target, String link_path) {
    const char *target_data = require_c_string(target, "symlink target");
    const char *link_data = require_c_string(link_path, "symlink path");
    if (!target_data || !link_data) return -1;
#ifdef _WIN32
    // Windows 的参数顺序和 POSIX 相反，目录链接还要额外标志。
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (GetFileAttributesA(target_data) != INVALID_FILE_ATTRIBUTES &&
        (GetFileAttributesA(target_data) & FILE_ATTRIBUTE_DIRECTORY)) {
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    }
    if (CreateSymbolicLinkA(link_data, target_data, flags)) return 0;
    // 老系统不认 ALLOW_UNPRIVILEGED_CREATE，退回不带该标志再试一次。
    return CreateSymbolicLinkA(link_data, target_data,
               flags & ~SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ? 0 : -1;
#else
    return symlink(target_data, link_data) == 0 ? 0 : -1;
#endif
}

// 建目录（已存在算成功）。成功返回 0。
int32_t __tap_make_directory(String path) {
    const char *path_data = require_c_string(path, "directory path");
    if (!path_data) return -1;
#ifdef _WIN32
    if (_mkdir(path_data) == 0) return 0;
#else
    if (mkdir(path_data, 0755) == 0) return 0;
#endif
    return errno == EEXIST ? 0 : -1;
}

// 删除一个条目：文件用 remove，空目录也能删掉。成功返回 0。
int32_t __tap_remove_entry(String path) {
    const char *path_data = require_c_string(path, "path");
    if (!path_data) return -1;
#ifdef _WIN32
    if (RemoveDirectoryA(path_data)) return 0;
#endif
    return remove(path_data) == 0 ? 0 : -1;
}

// 打印错误信息并以状态 1 终止程序，用于调用方无法合理恢复的情况。
// 前缀用 `panic: ` 和编译期诊断的 `error: ` 区分开。
// 函数不会返回；返回值只是为了让 tap 侧能把它当普通调用使用。
int32_t __tap_panic(String message) {
    fprintf(stderr, "panic: %.*s\n",
            message.length > INT_MAX ? INT_MAX : (int)message.length,
            message.data ? message.data : "(no message)");
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
int64_t __tap_socket_send_text(int64_t handle, String data) {
    if (handle < 0 || !data.data) return -1;
    return __tap_socket_send(handle, (const uint8_t *)data.data, data.length);
}

int32_t __tap_socket_close(int64_t handle) {
    if (handle < 0) return -1;
#ifdef _WIN32
    return closesocket((SOCKET)handle) == 0 ? 0 : -1;
#else
    return close((int)handle) == 0 ? 0 : -1;
#endif
}
