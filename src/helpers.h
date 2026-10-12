#ifndef HELPERS_H
#define HELPERS_H

#include <stddef.h>
#include <stdio.h>

char *get_basename_no_ext(const char *input_file);
int tap_executable_path(char *buffer, size_t size);
char *read_source_file(const char *filename);
void print_diagnostic(FILE *output, const char *level, const char *filename,
                      int line, int column, const char *format, ...);
void print_code_line(FILE *output, const char *source_code, int line, int context_lines);

#endif
