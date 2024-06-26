#include <stdio.h>
#include <stdlib.h> // 为了使用 malloc 和 free

#include "version.h"

const char *get_version()
{
    return VERSION;
}

const char *get_version_from_file()
{
    FILE *fp;
    char *version = NULL;
    long length;

    fp = fopen("./VERSION", "r");
    if (fp == NULL)
    {
        perror("Error to get version");
        return NULL;
    }

    // 获取文件长度
    fseek(fp, 0, SEEK_END);
    length = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // 分配内存来存储文件内容
    version = (char *)malloc(length + 1); // 加1是为了容纳字符串结束符 '\0'
    if (version == NULL)
    {
        perror("Memory allocation failed");
        fclose(fp);
        return NULL;
    }

    // 读取文件内容到内存中
    fread(version, 1, length, fp);
    version[length] = '\0'; // 添加字符串结束符

    // 关闭文件
    fclose(fp);

    return version;
}

