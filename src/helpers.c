#include "helpers.h"

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
