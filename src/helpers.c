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

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *buffer = (char *)malloc((size_t)file_size + 1);
    if (!buffer) {
        fprintf(stderr, "Out of memory\n"); // 中文：内存分配失败
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
    buffer[bytes_read] = '\0';
    fclose(file);
    return buffer;
}
