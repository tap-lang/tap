#include <stdio.h>

#include "version.h"

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        const char *version = get_version();
        printf("4yue version %s\n", version);
    }
    // printf("Hello 4yue\n");
    return 0;
}
