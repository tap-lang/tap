#include "helpers.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *get_basename_no_ext(const char *input_file) {
    const char *filename = input_file;
    const char *slash = strrchr(input_file, '/');
    const char *backslash = strrchr(input_file, '\\');
    if (slash || backslash) {
        const char *separator = !slash || (backslash && backslash > slash) ? backslash : slash;
        filename = separator + 1;
    }

    const char *extension = strrchr(filename, '.');
    size_t length = extension && extension != filename
        ? (size_t)(extension - filename)
        : strlen(filename);

    char *name = malloc(length + 1);
    if (!name) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        return NULL;
    }
    memcpy(name, filename, length);
    name[length] = '\0';
    return name;
}

char *read_source_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "failed to open file: %s\n", filename); // 中文：无法打开文件
        return NULL;
    }

    size_t capacity = 4096;
    size_t length = 0;
    char *buffer = (char *)malloc(capacity + 1);
    if (!buffer) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        fclose(file);
        return NULL;
    }

    for (;;) {
        size_t bytes_read = fread(buffer + length, 1, capacity - length, file);
        length += bytes_read;

        if (length < capacity) break;

        capacity *= 2;
        char *expanded = (char *)realloc(buffer, capacity + 1);
        if (!expanded) {
            fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
            free(buffer);
            fclose(file);
            return NULL;
        }
        buffer = expanded;
    }

    if (ferror(file)) {
        fprintf(stderr, "failed to read file: %s\n", filename); // 中文：无法读取文件
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[length] = '\0';
    fclose(file);
    return buffer;
}

static int decimal_width(int value) {
    int width = 1;
    while (value >= 10) {
        value /= 10;
        width++;
    }
    return width;
}

// 统一输出诊断消息、源码位置，以及错误行上下各三行的代码上下文。
void print_diagnostic(FILE *output, const char *level, const char *filename,
                      int line, int column, const char *format, ...) {
    fprintf(output, "%s: ", level);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(output, format, arguments);
    va_end(arguments);
    fprintf(output, "\n--> %s:%d:%d\n", filename ? filename : "<unknown>", line, column);

    if (filename && line > 0) {
        char *source_code = read_source_file(filename);
        if (source_code) {
            print_code_line(output, source_code, line, 3);
            free(source_code);
        }
    }
}

// 打印代码当前行以及上下各context_lines行
void print_code_line(FILE *output, const char *source_code, int line, int context_lines) {
    if (!output || !source_code || line < 1 || context_lines < 0) return;

    int start_line = line > context_lines ? line - context_lines : 1;
    int end_line = line + context_lines;
    int line_number_width = decimal_width(end_line);
    int current_line = 1;
    const char *line_start = source_code;

    while (*line_start != '\0' && current_line <= end_line) {
        const char *line_end = strchr(line_start, '\n');
        size_t line_length = line_end
            ? (size_t)(line_end - line_start)
            : strlen(line_start);

        if (current_line >= start_line && current_line <= end_line) {
            fprintf(output, "%s %*d  %.*s\n",
                   current_line == line ? "->" : "  ",
                   line_number_width,
                   current_line,
                   (int)line_length,
                   line_start);
        }

        if (!line_end) break;
        line_start = line_end + 1;
        current_line++;
    }
}
