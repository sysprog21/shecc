/* shecc - Self-Hosting and Educational C Compiler */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The inclusion must follow the fixed order, otherwise it fails to build. */
#include "defs.h"

/* Initialize global objects */
#include "globals.c"

/* ELF manipulation */
#include "elf.c"

/* C language front-end */
#include "cfront.c"

/* Machine code generation. ARMv7-A is the only target */
#include "codegen.c"

/* inlined libc */
#include "../out/libc.inc"

void load_source_file(char *file)
{
    char buffer[MAX_LINE_LEN];

    FILE *f = fopen(file, "rb");
    for (;;) {
        if (fgets(buffer, MAX_LINE_LEN, f) == NULL) {
            fclose(f);
            return;
        }
        if ((strncmp(buffer, "#include ", 9) == 0) && (buffer[9] == '"')) {
            char path[MAX_LINE_LEN];
            int c = strlen(file) - 1;
            while (c > 0 && file[c] != '/')
                c--;
            if (c) {
                /* prepend directory name */
                strncpy(path, file, c + 1);
                c++;
            }
            path[c] = 0;
            buffer[strlen(buffer) - 2] = 0;
            strcpy(path + c, buffer + 10);
            load_source_file(path);
        } else {
            strcpy(SOURCE + source_idx, buffer);
            source_idx += strlen(buffer);
        }
    }
    fclose(f);
}

int main(int argc, char *argv[])
{
    int libc = 1;
    char *out = NULL, *in = NULL;
    int i;
    dump_ir = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-ir") == 0)
            dump_ir = 1;
        else if (strcmp(argv[i], "--no-libc") == 0)
            libc = 0;
        else if (strcmp(argv[i], "-o") == 0) {
            if (i < argc + 1) {
                out = argv[i + 1];
                i++;
            } else
                /* unsupported options */
                abort();
        } else
            in = argv[i];
    }

    if (in == NULL) {
        printf("Missing source file!\n");
        printf("Usage: shecc [-o output] [--dump-ir] [--no-libc] <input.c>\n");
        return -1;
    }

    /* initialize global objects */
    global_init();

    /* include libc */
    if (libc)
        libc_generate();

    /* load source code */
    load_source_file(in);

    /* parse source into IR */
    parse();

    /* generate code from IR */
    code_generate();

    /* output code in ELF */
    elf_generate(out);

    return 0;
}
