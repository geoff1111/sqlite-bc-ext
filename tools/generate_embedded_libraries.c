#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int level; const char *file; const char *symbol; } Lib;
static const Lib libs[] = {
    {1, "lib.bc-lite", "sc_embedded_library_1_source"},
    {2, "lib2.bc-lite", "sc_embedded_library_2_source"},
    {3, "lib3.bc-lite", "sc_embedded_library_3_source"}
};

static int emit(FILE *out, const char *path, const char *symbol) {
    FILE *in = fopen(path, "rb"); unsigned char buf[4096]; size_t n, col = 0;
    if (!in) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return 0; }
    fprintf(out, "const unsigned char %s[] = {\n", symbol);
    while ((n = fread(buf, 1, sizeof(buf), in)) != 0) {
        for (size_t i = 0; i < n; ++i) {
            if (col == 0) fputs("    ", out);
            fprintf(out, "0x%02x,", buf[i]);
            col++;
            if (col == 12) { fputc('\n', out); col = 0; } else fputc(' ', out);
        }
    }
    if (ferror(in)) { fclose(in); return 0; }
    fclose(in);
    if (col == 0) fputs("    ", out);
    fputs("0x00,\n};\n", out);
    fprintf(out, "const size_t %s_len = sizeof(%s) - 1u;\n", symbol, symbol);
    return !ferror(out);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "lib";
    const char *header = argc > 2 ? argv[2] : "embedded_libraries.h";
    const char *source = argc > 3 ? argv[3] : "embedded_libraries.c";
    FILE *h = fopen(header, "wb"), *c = fopen(source, "wb");
    if (!h || !c) { fprintf(stderr, "cannot open generated outputs\n"); return 1; }
    fputs("#ifndef SC_EMBEDDED_LIBRARIES_H\n#define SC_EMBEDDED_LIBRARIES_H\n\n#include <stddef.h>\n\n#ifndef SC_EMBEDDED_LIBRARY_LEVEL\n#define SC_EMBEDDED_LIBRARY_LEVEL 3\n#endif\n\n#if SC_EMBEDDED_LIBRARY_LEVEL < 0 || SC_EMBEDDED_LIBRARY_LEVEL > 3\n#error \"SC_EMBEDDED_LIBRARY_LEVEL must be between 0 and 3\"\n#endif\n\n", h);
    fputs("#include \"embedded_libraries.h\"\n\n", c);
    for (size_t i = 0; i < sizeof(libs)/sizeof(libs[0]); ++i) {
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/%s", dir, libs[i].file) >= (int)sizeof(path)) return 1;
        fprintf(h, "#if SC_EMBEDDED_LIBRARY_LEVEL >= %d\nextern const unsigned char %s[];\nextern const size_t %s_len;\n#endif\n\n", libs[i].level, libs[i].symbol, libs[i].symbol);
        fprintf(c, "#if SC_EMBEDDED_LIBRARY_LEVEL >= %d\n", libs[i].level);
        if (!emit(c, path, libs[i].symbol)) return 1;
        fputs("#endif\n\n", c);
    }
    fputs("#endif\n", h);
    if (fclose(h) || fclose(c)) return 1;
    return 0;
}
