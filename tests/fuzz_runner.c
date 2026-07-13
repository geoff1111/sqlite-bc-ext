#include "compiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static int read_exact(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    while (n != 0u) {
        size_t got = fread(p, 1u, n, stdin);
        if (got == 0u) return feof(stdin) ? 0 : -1;
        p += got;
        n -= got;
    }
    return 1;
}

static int run_stream(void)
{
    unsigned char hdr[4];
    uint8_t *buf = NULL;
    size_t cap = 0u;
    for (;;) {
        uint32_t len;
        int rr = read_exact(hdr, sizeof(hdr));
        if (rr == 0) break;
        if (rr < 0) { free(buf); return 2; }
        len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
              ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        if (len > 65536u) { free(buf); return 3; }
        if ((size_t)len > cap) {
            uint8_t *p = (uint8_t *)realloc(buf, len ? (size_t)len : 1u);
            if (p == NULL) { free(buf); return 2; }
            buf = p;
            cap = len;
        }
        if (len != 0u && read_exact(buf, len) != 1) { free(buf); return 2; }
        (void)LLVMFuzzerTestOneInput(buf, len);
        if (putchar(0) == EOF || fflush(stdout) != 0) { free(buf); return 2; }
    }
    free(buf);
    return 0;
}

static int run_one(void)
{
    uint8_t *buf = NULL;
    size_t len = 0u, cap = 0u;
    int ch;
    while ((ch = getchar()) != EOF) {
        if (len == cap) {
            size_t next = cap ? cap * 2u : 4096u;
            uint8_t *p;
            if (next > 65536u) next = 65536u;
            if (len == 65536u) break;
            p = (uint8_t *)realloc(buf, next);
            if (p == NULL) { free(buf); return 2; }
            buf = p;
            cap = next;
        }
        buf[len++] = (uint8_t)ch;
    }
    (void)LLVMFuzzerTestOneInput(buf, len);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && argv[1][0] == '-' && argv[1][1] == '-' &&
        argv[1][2] == 's' && argv[1][3] == 't' && argv[1][4] == 'r' &&
        argv[1][5] == 'e' && argv[1][6] == 'a' && argv[1][7] == 'm' &&
        argv[1][8] == '\0') return run_stream();
    return run_one();
}
