/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
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

/* C language pre-processor */
#include "preprocessor.c"

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

char *last_char(char *text, char needle)
{
    char *last = NULL;

    for (int i = 0; text[i]; i++) {
        if (text[i] == needle)
            last = text + i;
    }

    return last;
}

/* Derive lacc-style DOT output when the caller did not specify -o. */
char *dot_output_name(char *input)
{
    const char *suffix = last_char(input, '.');
    char *slash = last_char(input, '/');
    const char *base = input;

    if (slash)
        base = slash + 1;

    /* A dot only introduces a suffix when something in the same path component
     * precedes it. That rules out a directory's dot ("dir.d/file") and a
     * dotfile's leading one, which would reduce ".bashrc" to ".dot".
     */
    if (suffix && suffix <= base)
        suffix = NULL;

    /* Not a ternary: "suffix - input" is ptrdiff_t and strlen() is size_t, and
     * mixing them in one ?: makes the whole expression unsigned.
     */
    int base_len = strlen(input);

    if (suffix)
        base_len = suffix - input;

    /* strlen, not sizeof: shecc types a string literal as a pointer, so
     * sizeof(".dot") is 1 once the compiler is compiling itself.
     */
    char *output = malloc(base_len + strlen(".dot") + 1);

    if (!output)
        fatal("Unable to allocate DOT output name");

    memcpy(output, input, base_len);
    strcpy(output + base_len, ".dot");
    return output;
}

int main(int argc, char *argv[])
{
    char *out = NULL;
    char *in = NULL;
    token_stream_t *libc_token_stream = NULL, *token_stream;
    token_t *tk;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump-ir"))
            dump_ir = true;
        else if (!strcmp(argv[i], "--dot"))
            dump_dot = true;
        else if (!strcmp(argv[i], "+m"))
            hard_mul_div = true;
        else if (!strcmp(argv[i], "--no-libc"))
            libc = false;
        else if (!strcmp(argv[i], "--dynlink"))
            dynlink = true;
        else if (!strcmp(argv[i], "-z")) {
            if (i + 1 >= argc)
                usage_error("-z requires \"lazy\" or \"now\"");

            if (!strcmp(argv[i + 1], "lazy"))
                imm_binding = false;
            else if (!strcmp(argv[i + 1], "now"))
                imm_binding = true;
            else
                usage_error("-z requires \"lazy\" or \"now\"");
        } else if (!strcmp(argv[i], "-E"))
            expand_only = true;
        else if (!strcmp(argv[i], "-o")) {
            if (i + 1 < argc) {
                out = argv[i + 1];
                i++;
            } else
                usage_error("-o requires an output file name");
        } else if (argv[i][0] == '-') {
            usage_error("Unidentified option");
        } else
            in = argv[i];
    }

    if (dynlink) {
        switch (ELF_MACHINE) {
        /* The following 64-bit targets have no lazy-resolution path, so
         * immediate binding must be used.
         */
        case ELF_MACHINE_X86_64:
        case ELF_MACHINE_AARCH64:
            imm_binding = true;
        }
    }

    if (!in) {
        printf(
            "Usage: shecc [-o output] [+m] [--dot] [--dump-ir] [--no-libc] "
            "[--dynlink] [-z <lazy | now>] [-E] <input.c>\n");
        usage_error("Missing source file");
    }

    /* --dot stops the pipeline at a different phase than these do. */
    if (dump_dot && expand_only)
        usage_error("--dot cannot be combined with -E");

    if (dump_dot && dump_ir)
        usage_error("--dot cannot be combined with --dump-ir");

    if (dump_dot && !out)
        out = dot_output_name(in);

    /* The graph is written by truncating its output, so naming the input
     * destroys the source. That happens both when -o names it outright and when
     * an input already ending in .dot derives its own name.
     */
    if (dump_dot && !strcmp(out, in))
        usage_error("--dot would overwrite the input; name another output");

    /* initialize global objects */
    global_init();

    /* include libc */
    if (libc) {
        libc_decl();
        if (!dynlink)
            libc_impl();
        libc_token_stream = gen_libc_token_stream();
    }

    token_stream = gen_file_token_stream(in);

    /* concat libc's and input file's token stream */
    if (libc_token_stream) {
        libc_token_stream->tail->next = token_stream->head;
        token_stream = libc_token_stream;
    }

    tk = preprocess(token_stream->head);

    if (expand_only) {
        emit_preprocessed_token(tk);
        exit(0);
    }

    /* load and parse source code into IR */
    parse(pp_strip_layout(tk));

    /* Tokens, macros and the source buffers they point into are dead once
     * parsing finishes; releasing them here keeps 17 MiB out of the peak.
     */
    release_token_arena();

    /* Compact arenas after parsing to free temporary parse structures */
    compact_all_arenas();

    ssa_build();

    /* Like lacc's -dot target, visualize the SSA CFG directly rather than
     * lowering it into an executable. Prune first: every compile prepends the
     * whole of lib/c.c, and the functions nothing reaches are noise in the
     * picture -- two thirds of the graph on tests/fib.c.
     */
    if (dump_dot) {
        prune_unused_funcs();
        dump_cfg(out);
        global_release();
        exit(0);
    }

    unwind_phi();

    /* Copy small helpers into their callers before anything else looks at them,
     * so the optimizer sees one body rather than a call boundary.
     */
    inline_calls();

    /* dump first phase IR */
    if (dump_ir)
        dump_insn();

    /* Nothing beyond this point should spend time on functions no call can
     * reach; on a small input, almost all of the embedded libc is that.
     */
    prune_unused_funcs();

    /* SSA-based optimization */
    optimize();

    /* Flatten unpredictable ifs into branchless selects.
     *
     * After the optimizer rather than inside ssa_build(): a select reads a
     * third operand, and the passes in optimize() walk instructions two sources
     * at a time. One of them would rewrite a copy feeding that third operand
     * and leave the select reading a value nothing defines.
     */
    if_convert();

    /* Send short-circuit arms straight to their destination. */
    thread_const_branches();

    /* Both passes above rewired the CFG the dominator tree was built from, and
     * everything below asks that tree which edges close a loop.
     */
    rebuild_dom();

    /* Walk arrays with a pointer rather than recomputing addresses. */
    strength_reduce();

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

    /* ELF preprocess:
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
