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
