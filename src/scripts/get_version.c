#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <direct.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#define strdup _strdup
#endif

static const char *PLACEHOLDER = "@GIT_COMMIT_ID";
static const char *DATE_PLACEHOLDER = "@VERSION_DATE";

// 输出错误信息，并附带当前 errno 对应的系统错误原因。
static void print_errno(const char *message, const char *path) {
    fprintf(stderr, "%s: %s: %s\n", message, path, strerror(errno));
}

// 读取命令输出的第一行；失败或无输出时返回 NULL。
static char *read_command_line(const char *command) {
    FILE *pipe = popen(command, "r");
    if (!pipe) return NULL;

    char buffer[128];
    char *line = NULL;
    if (fgets(buffer, sizeof(buffer), pipe)) {
        buffer[strcspn(buffer, "\r\n")] = '\0';
        if (buffer[0]) line = strdup(buffer);
    }

    int status = pclose(pipe);
    if (status != 0) {
        free(line);
        return NULL;
    }
    return line;
}

// 获取当前 Git 短提交号；非 Git 环境或未安装 Git 时回退为 dev。
static char *git_commit_id(void) {
#ifdef _WIN32
    char *commit = read_command_line("git log -1 --pretty=format:%h 2>nul");
#else
    char *commit = read_command_line("git log -1 --pretty=format:%h 2>/dev/null");
#endif
    if (commit) return commit;
    return strdup("dev");
}

// 获取当前本地日期，格式为 YYYY-MM-DD。
static char *version_date(void) {
    time_t now = time(NULL);
    struct tm local_time;
#ifdef _WIN32
    if (localtime_s(&local_time, &now) != 0) return strdup("unknown");
#else
    if (!localtime_r(&now, &local_time)) return strdup("unknown");
#endif

    char buffer[16];
    if (strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local_time) == 0) {
        return strdup("unknown");
    }
    return strdup(buffer);
}

// 读取整个文本文件，调用者负责释放返回缓冲区。
static char *read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        print_errno("failed to open input file", path); // 中文：打开输入文件失败
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        print_errno("failed to seek input file", path); // 中文：定位输入文件失败
        fclose(file);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0) {
        print_errno("failed to get input file size", path); // 中文：获取输入文件大小失败
        fclose(file);
        return NULL;
    }
    rewind(file);

    char *data = malloc((size_t)size + 1);
    if (!data) {
        fprintf(stderr, "out of memory\n"); // 中文：内存不足
        fclose(file);
        return NULL;
    }

    if (size > 0 && fread(data, 1, (size_t)size, file) != (size_t)size) {
        print_errno("failed to read input file", path); // 中文：读取输入文件失败
        free(data);
        fclose(file);
        return NULL;
    }
    data[size] = '\0';
    fclose(file);

    *size_out = (size_t)size;
    return data;
}

// 将文本写入目标文件。
static int write_file(const char *path, const char *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        print_errno("failed to open output file", path); // 中文：打开输出文件失败
        return 1;
    }

    if (fwrite(data, 1, size, file) != size) {
        print_errno("failed to write output file", path); // 中文：写入输出文件失败
        fclose(file);
        return 1;
    }

    if (fclose(file) != 0) {
        print_errno("failed to close output file", path); // 中文：关闭输出文件失败
        return 1;
    }
    return 0;
}

// 计算模板中指定占位符出现次数。
static size_t count_placeholders(const char *data, const char *placeholder) {
    size_t count = 0;
    size_t placeholder_length = strlen(placeholder);
    const char *cursor = data;
    while ((cursor = strstr(cursor, placeholder))) {
        count++;
        cursor += placeholder_length;
    }
    return count;
}

// 替换模板里的一个占位符。
static char *replace_placeholder(
    const char *template_data, size_t template_size, const char *placeholder,
    const char *value, size_t *size_out) {
    size_t placeholder_length = strlen(placeholder);
    size_t value_length = strlen(value);
    size_t placeholder_count = count_placeholders(template_data, placeholder);
    size_t output_size = template_size -
        placeholder_count * placeholder_length +
        placeholder_count * value_length;

    char *output = malloc(output_size + 1);
    if (!output) {
        fprintf(stderr, "out of memory\n"); // 中文：内存不足
        return NULL;
    }

    const char *src = template_data;
    char *dst = output;
    const char *match = NULL;
    while ((match = strstr(src, placeholder))) {
        size_t prefix_length = (size_t)(match - src);
        memcpy(dst, src, prefix_length);
        dst += prefix_length;
        memcpy(dst, value, value_length);
        dst += value_length;
        src = match + placeholder_length;
    }
    size_t tail_length = strlen(src);
    memcpy(dst, src, tail_length);
    dst += tail_length;
    *dst = '\0';

    *size_out = output_size;
    return output;
}

// 把模板里的版本占位符替换为提交号和日期。
static char *render_version_header(
    const char *template_data, size_t template_size, const char *commit,
    const char *date, size_t *size_out) {
    size_t commit_size = 0;
    char *with_commit = replace_placeholder(
        template_data, template_size, PLACEHOLDER, commit, &commit_size);
    if (!with_commit) return NULL;

    char *with_date = replace_placeholder(
        with_commit, commit_size, DATE_PLACEHOLDER, date, size_out);
    free(with_commit);
    return with_date;
}

// 根据 version.h.ini 生成 version.h。
int main(int argc, char **argv) {
    const char *input_path = argc > 1 ? argv[1] : "src/version.h.ini";
    const char *output_path = argc > 2 ? argv[2] : "src/version.h";

    if (argc > 3) {
        fprintf(stderr, "usage: get_version [version.h.ini] [version.h]\n"); // 中文：get_version 用法
        return 2;
    }

    char *commit = git_commit_id();
    if (!commit) {
        fprintf(stderr, "failed to determine git commit id\n"); // 中文：无法确定 Git 提交 ID
        return 1;
    }
    char *date = version_date();
    if (!date) {
        fprintf(stderr, "failed to determine version date\n"); // 中文：无法确定版本日期
        free(commit);
        return 1;
    }

    size_t template_size = 0;
    char *template_data = read_file(input_path, &template_size);
    if (!template_data) {
        free(date);
        free(commit);
        return 1;
    }

    size_t output_size = 0;
    char *output = render_version_header(
        template_data, template_size, commit, date, &output_size);
    if (!output) {
        free(template_data);
        free(date);
        free(commit);
        return 1;
    }

    int result = write_file(output_path, output, output_size);
    free(output);
    free(template_data);
    free(date);
    free(commit);
    return result;
}
