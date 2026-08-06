#include "run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

extern int debug;
#ifndef _WIN32
extern char **environ;
#endif

int compile_to_executable(CodeGenContext *context, const char *exe_file) {
    const char *temp_ir_file = "temp_output.ll";

    if (write_ir_to_file(context, temp_ir_file) != 0) {
        fprintf(stderr, "写入临时IR文件失败\n");
        return 1;
    }

    int result = compile_ir_to_exe(temp_ir_file, exe_file);
    remove(temp_ir_file);

    if (result != 0) {
        fprintf(stderr, "生成可执行文件失败\n");
        return 1;
    }

#ifndef _WIN32
    if (chmod(exe_file, 0755) != 0) {
        fprintf(stderr, "设置可执行权限失败: %s\n", exe_file);
        return 1;
    }
#endif

    if (debug) printf("可执行文件已生成: %s\n", exe_file);
    return 0;
}

static int execute_file(const char *exe_file) {
    fflush(NULL);

#ifdef _WIN32
    int result = system(exe_file);
    return result == -1 ? 1 : result;
#else
    char *relative_path = NULL;
    const char *exec_path = exe_file;

    if (!strchr(exe_file, '/')) {
        size_t path_size = strlen(exe_file) + 3;
        relative_path = malloc(path_size);
        if (!relative_path) {
            fprintf(stderr, "内存分配失败\n");
            return 1;
        }
        snprintf(relative_path, path_size, "./%s", exe_file);
        exec_path = relative_path;
    }

    pid_t pid;
    char *const child_argv[] = {(char *)exec_path, NULL};
    int spawn_result = posix_spawn(&pid, exec_path, NULL, NULL, child_argv, environ);
    if (spawn_result != 0) {
        fprintf(stderr, "运行可执行文件失败: %s: %s\n", exec_path, strerror(spawn_result));
        free(relative_path);
        return 1;
    }

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            fprintf(stderr, "等待程序结束失败: %s\n", strerror(errno));
            free(relative_path);
            return 1;
        }
    }
    free(relative_path);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

int compile_and_run(CodeGenContext *context, const char *exe_file) {
    int result = compile_to_executable(context, exe_file);
    if (result != 0) return result;

    result = execute_file(exe_file);
    if (remove(exe_file) != 0) {
        fprintf(stderr, "删除临时可执行文件失败: %s\n", exe_file);
        if (result == 0) result = 1;
    }

    return result;
}

void run_with_lli(CodeGenContext *context) {
    const char *temp_ir_file = "temp_output.ll";

    if (debug) printf("执行程序...\n");

    if (write_ir_to_file(context, temp_ir_file) != 0) {
        fprintf(stderr, "写入临时IR文件失败\n");
        return;
    }

    int result = system("lli temp_output.ll");
#ifndef _WIN32
    if (debug) printf("程序执行完毕，返回值: %d\n", WEXITSTATUS(result));
#else
    if (debug) printf("程序执行完毕，返回值: %d\n", result);
#endif

    remove(temp_ir_file);
}
