#ifndef RUN_H
#define RUN_H

#include "codegen.h"

// 用户从命令行传下来的链接参数，按出现顺序保存。
//
// 顺序有语义：静态链接的符号解析是单向的，`-l` 必须排在目标文件和 Runtime
// 静态库之后，`-L` 也必须排在依赖它的 `-l` 之前。所以这里不能用集合，只能保序。
typedef enum {
    LINK_ARG_LIBRARY,     // -l<名字>，只存库名，不含 -l 前缀
    LINK_ARG_SEARCH_DIR,  // -L<目录>，只存目录
    LINK_ARG_RAW          // -Wl,<参数>，只存 -Wl, 之后的部分，原样交给链接器
} LinkArgKind;

typedef struct {
    LinkArgKind kind;
    char *value;
} LinkArg;

typedef struct {
    LinkArg *items;
    int count;
    int capacity;
} LinkOptions;

void link_options_init(LinkOptions *options);
int link_options_add(LinkOptions *options, LinkArgKind kind, const char *value);
int link_options_is_empty(const LinkOptions *options);
void link_options_free(LinkOptions *options);

// `-v`：把 tap 实际执行的外部命令（链接、lli、运行产物）回显到 stderr。
//
// 用全局开关而不是逐层传参 —— 只有三个调用点需要它，而它们都在这个文件里。
// 输出走 stderr 是硬要求：`tap run -v prog.tp > out.txt` 时 out.txt 里必须只有
// 程序自己的输出。
void set_verbose_commands(int enabled);

// 根据编译器路径或 TAP_RUNTIME_PATH 解析 Runtime 库位置。
void configure_runtime(const char *compiler_path);
int compile_to_executable(CodeGenContext *context, const char *exe_file, int static_link,
                          const LinkOptions *link_options);
int compile_and_run(CodeGenContext *context, const char *exe_file,
                    int program_argc, char **program_argv, int static_link,
                    const LinkOptions *link_options);
int run_with_lli(CodeGenContext *context);

// 把 `tap new ...` 转发给 tapm。forwarded_argv[0] 是 "new"，其后是原样透传的参数。
// 返回 tapm 的退出码；找不到 tapm 时打印提示并返回 1。
int run_tapm(const char *compiler_path, int forwarded_argc, char **forwarded_argv);

#endif
