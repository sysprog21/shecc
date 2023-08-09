/* shecc - Self-Hosting and Educational C Compiler */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Define target machine */
#include "../config"

/* The inclusion must follow the fixed order, otherwise it fails to build. */
#include "defs.h"

/* Initialize global objects */
#include "globals.c"

/* ELF manipulation */
#include "elf.c"

/* C language front-end */
#include "cfront.c"

/* Machine code generation. support ARMv7-A and RISC-V32I */
#include "codegen.c"

/* inlined libc */
#include "../out/libc.inc"

int main(int argc, char *argv[])
{
    int libc = 1;
    char *out = NULL, *in = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-ir"))
            dump_ir = 1;
        else if (!strcmp(argv[i], "--no-libc"))
            libc = 0;
        else if (!strcmp(argv[i], "-o")) {
            if (i < argc + 1) {
                out = argv[i + 1];
                i++;
            } else
                /* unsupported options */
                abort();
        } else
            in = argv[i];
    }

    if (!in) {
        printf("Missing source file!\n");
        printf("Usage: shecc [-o output] [--dump-ir] [--no-libc] <input.c>\n");
        return -1;
    }

    /* initialize global objects */
    global_init();

    /* include libc */
    if (libc)
        libc_generate();

    /* load and parse source code into IR */
    parse(in);

    /* generate code from IR */
    code_generate();

    /* output code in ELF */
    elf_generate(out);

    return 0;
}
