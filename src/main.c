/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
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

/* C language lexical analyzer */
#include "lexer.c"

/* C language syntactic analyzer */
#include "parser.c"

/* architecture-independent middle-end */
#include "ssa.c"

/* Register allocator */
#include "reg-alloc.c"

/* Peephole optimization */
#include "peephole.c"

/* Arch-specific IR lowering boundary */
#include "arch-lower.c"

/* Machine code generation. support ARMv7-A and RV32I */
#include "codegen.c"

/* inlined libc */
#include "../out/libc.inc"

int main(int argc, char *argv[])
{
    bool libc = true;
    char *out = NULL;
    char *in = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-ir"))
            dump_ir = true;
        else if (!strcmp(argv[i], "+m"))
            hard_mul_div = true;
        else if (!strcmp(argv[i], "--no-libc"))
            libc = false;
        else if (!strcmp(argv[i], "--dynlink"))
            dynlink = true;
        else if (!strcmp(argv[i], "-o")) {
            if (i + 1 < argc) {
                out = argv[i + 1];
                i++;
            } else
                /* unsupported options */
                abort();
        } else if (argv[i][0] == '-') {
            fatal("Unidentified option");
        } else
            in = argv[i];
    }

    if (!in) {
        printf("Missing source file!\n");
        printf(
            "Usage: shecc [-o output] [+m] [--dump-ir] [--no-libc] [--dynlink] "
            "<input.c>\n");
        return -1;
    }

    /* initialize global objects */
    global_init();

    /* include libc */
    if (libc)
        libc_generate();

    /* load and parse source code into IR */
    parse(in);

    /* Compact arenas after parsing to free temporary parse structures */
    compact_all_arenas();

    ssa_build();

    /* dump first phase IR */
    if (dump_ir)
        dump_insn();

    /* SSA-based optimization */
    optimize();

    /* Compact arenas after SSA optimization to free temporary SSA structures */
    compact_all_arenas();

    /* SSA-based liveness analyses */
    liveness_analysis();

    /* Compact after liveness analysis - mainly traversal args in GENERAL_ARENA
     */
    compact_arenas_selective(COMPACT_ARENA_GENERAL);

    /* allocate register from IR */
    reg_alloc();

    /* Compact after register allocation - mainly INSN and BB arenas */
    compact_arenas_selective(COMPACT_ARENA_INSN | COMPACT_ARENA_BB);

    peephole();

    /* Apply arch-specific IR tweaks before final codegen */
    arch_lower();

    /* flatten CFG to linear instruction */
    cfg_flatten();

    /* Compact after CFG flattening - BB and GENERAL no longer needed */
    compact_arenas_selective(COMPACT_ARENA_BB | COMPACT_ARENA_GENERAL);

    /* dump second phase IR */
    if (dump_ir)
        dump_ph2_ir();

    /*
     * ELF preprocess:
     * 1. generate all sections except for .text section.
     * 2. calculate the starting addresses of certain sections.
     */
    elf_preprocess();

    /* generate code from IR */
    code_generate();

    /* ELF postprocess: generate all ELF headers */
    elf_postprocess();

    /* output code in ELF */
    elf_generate(out);

    /* release allocated objects */
    global_release();

    exit(0);
}
