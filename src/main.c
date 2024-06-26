#include <stdio.h>

#include "version.h"
#include "options.h"

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        const char *version = get_version();
        printf("4yue lang version %s\n\n", version);
        printf("Usage: 4yue [OPTIONS] INPUT\n\n%s", get_display_options());
        return 0;
    }
    // printf("Hello 4yue\n");
    return 0;
}
