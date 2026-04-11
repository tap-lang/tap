#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
/* map POSIX names to MSVC/_CRT names when necessary */
#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif
#ifndef strdup
#define strdup _strdup
#endif
#endif

static const char *PLACEHOLDER = "@GIT_COMMIT_ID";

/* Try to obtain the git commit id (short). Returns a malloc'd string which must be freed. */
char *get_git_commit_id(void) {
    FILE *fp = NULL;
    char buf[128];
    char *ret = NULL;

#ifdef _WIN32
    fp = _popen("git log -1 --pretty=format:%h 2>nul", "r");
#else
    fp = popen("git log -1 --pretty=format:%h 2>/dev/null", "r");
#endif
    if (!fp) {
        ret = strdup("dev");
        return ret;
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        /* no output */
        buf[0] = '\0';
    }

#ifdef _WIN32
    _pclose(fp);
#else
    pclose(fp);
#endif

    if (buf[0] == '\0') {
        ret = strdup("dev");
    } else {
        /* trim newline */
        size_t n = strlen(buf);
        if (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[n-1] = '\0';
        ret = strdup(buf);
    }
    return ret;
}

int main(void) {
    const char *inpath = "src/version.h.ini";
    const char *outpath = "src/version.h";
    FILE *f = NULL;
    long size;
    char *data = NULL;
    char *out = NULL;
    char *commit = NULL;
    size_t placeholder_len = strlen(PLACEHOLDER);

    commit = get_git_commit_id();
    if (!commit) {
        fprintf(stderr, "failed to determine git commit id\n");
        return 2;
    }

    f = fopen(inpath, "rb");
    if (!f) {
        fprintf(stderr, "failed to open input file '%s'\n", inpath);
        free(commit);
        return 3;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "fseek failed\n");
        fclose(f);
        free(commit);
        return 4;
    }
    size = ftell(f);
    if (size < 0) size = 0;
    rewind(f);

    data = (char*)malloc((size_t)size + 1);
    if (!data) {
        fprintf(stderr, "out of memory\n");
        fclose(f);
        free(commit);
        return 5;
    }

    if (size > 0) {
        if (fread(data, 1, (size_t)size, f) != (size_t)size) {
            fprintf(stderr, "failed to read input file\n");
            free(data);
            fclose(f);
            free(commit);
            return 6;
        }
    }
    data[size] = '\0';
    fclose(f);

    /* Count occurrences of placeholder */
    size_t count = 0;
    char *p = data;
    while ((p = strstr(p, PLACEHOLDER)) != NULL) {
        count++;
        p += placeholder_len;
    }

    if (count == 0) {
        /* No placeholder: just copy the file */
        FILE *outf = fopen(outpath, "wb");
        if (!outf) {
            fprintf(stderr, "failed to open output file '%s' for writing\n", outpath);
            free(data);
            free(commit);
            return 7;
        }
        if (fwrite(data, 1, (size_t)size, outf) != (size_t)size) {
            fprintf(stderr, "failed to write output file\n");
            fclose(outf);
            free(data);
            free(commit);
            return 8;
        }
        fclose(outf);
        free(data);
        free(commit);
        return 0;
    }

    /* Build replaced output */
    size_t commit_len = strlen(commit);
    size_t new_size = (size_t)size + count * (commit_len - placeholder_len);
    out = (char*)malloc(new_size + 1);
    if (!out) {
        fprintf(stderr, "out of memory (output buffer)\n");
        free(data);
        free(commit);
        return 9;
    }

    char *dst = out;
    const char *src = data;
    while ((p = strstr(src, PLACEHOLDER)) != NULL) {
        size_t n = (size_t)(p - src);
        memcpy(dst, src, n);
        dst += n;
        memcpy(dst, commit, commit_len);
        dst += commit_len;
        src = p + placeholder_len;
    }
    /* copy remainder */
    strcpy(dst, src);

    /* write out */
    FILE *outf = fopen(outpath, "wb");
    if (!outf) {
        fprintf(stderr, "failed to open output file '%s' for writing\n", outpath);
        free(data);
        free(out);
        free(commit);
        return 10;
    }
    if (fwrite(out, 1, new_size, outf) != new_size) {
        fprintf(stderr, "failed to write output file\n");
        fclose(outf);
        free(data);
        free(out);
        free(commit);
        return 11;
    }
    fclose(outf);

    free(data);
    free(out);
    free(commit);

    return 0;
}
