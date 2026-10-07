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
#include "vir-frontend.h"

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

/* Native value IR construction and optimization. */
#include "vir-frontend.c"
#include "vir.c"

/* Register allocator */
#include "vir-lower.c"

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


static void check_native_pass(const char *name, int result)
{
    if (result < 0) {
        fprintf(stderr, "VIR %s failed in %s\n", name,
                fatal_function_context ? fatal_function_context : "globals");
        fatal("VIR optimization failed");
    }
}

static void optimize_native_graph(vir_function_t *graph)
{
    if (!vir_remove_unreachable(graph))
        fatal("Invalid VIR reachability");
    check_native_pass("sccp", vir_sccp(graph, vir_frontend_opt_level));
    check_native_pass("cfg", vir_simplify_cfg(graph, vir_frontend_opt_level));
    check_native_pass("cse", vir_local_cse(graph, vir_frontend_opt_level));
    if (vir_frontend_opt_level == VIR_OPT_O2)
        check_native_pass("gvn", vir_gvn_with_stats(graph, VIR_OPT_O2, NULL));
    check_native_pass("licm", vir_licm(graph, vir_frontend_opt_level));
    int strengthened = vir_strength_reduce(graph, vir_frontend_opt_level);
    check_native_pass("strength", strengthened);
    check_native_pass("cse", vir_local_cse(graph, vir_frontend_opt_level));
    check_native_pass("dce", vir_dce(graph, vir_frontend_opt_level));
    check_native_pass("cfg", vir_simplify_cfg(graph, vir_frontend_opt_level));
    check_native_pass("final-cse",
                      vir_local_cse(graph, vir_frontend_opt_level));
    check_native_pass("final-dce", vir_dce(graph, vir_frontend_opt_level));
    char *error = NULL;
    if (!vir_verify(graph, &error)) {
        fprintf(stderr, "Invalid optimized VIR in %s: %s\n",
                fatal_function_context ? fatal_function_context : "globals",
                error ? error : "verification failed");
        free(error);
        fatal("Invalid optimized VIR");
    }
}

/* Functions marked used but not yet optimized. Each enters once, when it is
 * first marked, so reachability costs one visit per function.
 */
static func_t **native_worklist;
static int native_worklist_count;

static void mark_native_used(func_t *func)
{
    if (!func || func->is_used)
        return;
    func->is_used = true;
    native_worklist[native_worklist_count++] = func;
}

static void mark_native_references(const vir_function_t *graph)
{
    for (const vir_block_t *block = graph->blocks; block; block = block->next) {
        for (const vir_effect_t *effect = block->effects; effect;
             effect = effect->next)
            if (effect->kind == VIR_EFFECT_CALL && effect->callee)
                mark_native_used(find_func((char *) effect->callee));
        for (const vir_value_t *value = block->head; value; value = value->next)
            if (value->opcode == VIR_OP_FUNC_ADDR)
                mark_native_used(find_func((char *) value->address_name));
    }
}

static void lower_native_function(func_t *func, bool global)
{
    vir_function_t *graph = (vir_function_t *) vir_frontend_function(func);
    vir_machine_function_t machine = {0};
    int global_bytes = GLOBAL_FUNC->stack_size;
    if (!vir_lower_machine(graph, vir_frontend_signature(func),
                           vir_frontend_global_offset, NULL, &machine))
        fatal("VIR machine lowering failed");
    func->bbs = machine.entry;
    if (!global) {
        func->stack_size = machine.stack_size;
        if (!strcmp(func->return_def.var_name, "main"))
            MAIN_BB = machine.entry;
    } else {
        int scratch_base =
            machine.stack_size ? (global_bytes + 7) & -8 : global_bytes;
        if (machine.block_count != 1)
            fatal("Unexpected control flow in global initializer");
        ph2_ir_t *previous = NULL;
        for (ph2_ir_t *ir = machine.entry->ph2_ir_list.head; ir;
             ir = ir->next) {
            if (ir->op == OP_load) {
                ir->op = OP_global_load;
                ir->src0 += scratch_base;
            } else if (ir->op == OP_store) {
                ir->op = OP_global_store;
                ir->src1 += scratch_base;
            } else if (ir->op == OP_address_of) {
                ir->op = OP_global_address_of;
                ir->src0 += scratch_base;
            } else if (ir->op == OP_return) {
                if (previous)
                    previous->next = NULL;
                else
                    machine.entry->ph2_ir_list.head = NULL;
                machine.entry->ph2_ir_list.tail = previous;
                break;
            }
            previous = ir;
        }
        func->stack_size = scratch_base + machine.stack_size;
    }
    for (basic_block_t *block = machine.entry; block; block = block->rpo_next)
        block->belong_to = func;
}

static void compile_native_vir(const char *output)
{
    int function_count = 0;
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        func->is_used = false;
        function_count++;
    }
    native_worklist = malloc((function_count + 1) * sizeof(func_t *));
    if (!native_worklist)
        fatal("Out of memory");
    vir_frontend_layout_globals();
    vir_function_t *global =
        (vir_function_t *) vir_frontend_function(GLOBAL_FUNC);
    if (global) {
        optimize_native_graph(global);
        mark_native_references(global);
    }
    func_t *main_func = find_func("main");
    if (main_func)
        mark_native_used(main_func);
    else
        for (func_t *func = FUNC_LIST.head; func; func = func->next)
            if (vir_frontend_function(func))
                mark_native_used(func);
    while (native_worklist_count) {
        func_t *func = native_worklist[--native_worklist_count];
        vir_function_t *graph = (vir_function_t *) vir_frontend_function(func);
        if (!graph)
            continue;
        fatal_function_context = func->return_def.var_name;
        optimize_native_graph(graph);
        mark_native_references(graph);
    }
    free(native_worklist);
    FILE *dot = dump_dot ? fopen(output, "w") : NULL;
    if (dump_dot && !dot)
        fatal("Cannot open DOT output");
    if (dot)
        fprintf(dot, "digraph VIR {\n");
    int function_index = 0;
    for (func_t *func = FUNC_LIST.head; func;
         func = func->next, function_index++) {
        vir_function_t *graph = (vir_function_t *) vir_frontend_function(func);
        if (func->is_used && !graph) {
            char message[MAX_LINE_LEN];
            if (func->is_static || !dynlink) {
                snprintf(message, MAX_LINE_LEN, "undefined %sfunction '%s'",
                         func->is_static ? "static " : "",
                         function_source_name(func));
                error_at(message, NULL);
            }
            if (func->returns_aggregate) {
                snprintf(message, MAX_LINE_LEN,
                         "aggregate-return function '%s' has its address taken "
                         "but is not defined",
                         func->return_def.var_name);
                error_at(message, NULL);
            }
        }
        if (!func->is_used || !graph) {
            func->bbs = NULL;
            continue;
        }
        if (dump_ir || dump_vir) {
            FILE *stream = dump_ir ? stdout : stderr;
            fprintf(stream, "function %s\n", func->return_def.var_name);
            vir_print(graph, stream);
        }
        if (dot) {
            for (vir_block_t *block = graph->blocks; block;
                 block = block->next) {
                fprintf(dot, "f%d_b%d [label=\"%s block %d\"];\n",
                        function_index, block->id, func->return_def.var_name,
                        block->id);
                for (vir_edge_t *edge = block->outgoing; edge;
                     edge = edge->next_outgoing)
                    fprintf(dot, "f%d_b%d -> f%d_b%d;\n", function_index,
                            block->id, function_index, edge->to->id);
            }
        } else
            lower_native_function(func, false);
    }
    if (dot) {
        fprintf(dot, "}\n");
        fclose(dot);
    } else if (global)
        lower_native_function(GLOBAL_FUNC, true);
    dump_stats_phase("vir");
}

int main(int argc, char *argv[])
{
    static const struct {
        const char *name;
        bool *setting;
        bool value;
    } flags[] = {
        {"--dump-ir", &dump_ir, true},
        {"--stats", &dump_stats, true},
        {"--dump-vir", &dump_vir, true},
        {"--warn-string-literals", &warn_string_literals, true},
        {"--std=c99", &strict_c99, true},
        {"--dot", &dump_dot, true},
        {"+m", &hard_mul_div, true},
        {"--no-libc", &libc, false},
        {"--dynlink", &dynlink, true},
        {"-E", &expand_only, true},
    };
    char *out = NULL;
    char **inputs;
    int input_count = 0;
    token_stream_t *libc_token_stream = NULL, *token_stream;
    token_t *input_tokens = NULL, *input_tail = NULL;

    inputs = malloc((size_t) argc * sizeof(*inputs));
    if (!inputs)
        fatal("Unable to allocate input file list");

    for (int i = 1; i < argc; i++) {
        size_t flag = 0;

        while (flag < sizeof(flags) / sizeof(*flags) &&
               strcmp(argv[i], flags[flag].name))
            flag++;
        if (flag < sizeof(flags) / sizeof(*flags)) {
            *flags[flag].setting = flags[flag].value;
            continue;
        }
        if (!strcmp(argv[i], "--no-opt"))
            vir_frontend_opt_level = VIR_OPT_O0;
        else if (!strcmp(argv[i], "-I")) {
            if (i + 1 >= argc)
                usage_error("-I requires an include directory");
            if (include_dirs_idx == MAX_INCLUDE_DIRS)
                usage_error("Too many include directories");
            include_dirs[include_dirs_idx++] = argv[++i];
        } else if (!strncmp(argv[i], "-I", 2) && argv[i][2]) {
            if (include_dirs_idx == MAX_INCLUDE_DIRS)
                usage_error("Too many include directories");
            include_dirs[include_dirs_idx++] = argv[i] + 2;
        } else if (!strcmp(argv[i], "-o")) {
            if (i + 1 < argc) {
                out = argv[i + 1];
                i++;
            } else
                usage_error("-o requires an output file name");
        } else if (argv[i][0] == '-') {
            usage_error("Unidentified option");
        } else
            inputs[input_count++] = argv[i];
    }

    if (!input_count) {
        printf(
            "Usage: shecc [-I directory] [-o output] [+m] [--dot] [--dump-ir] "
            "[--dump-vir] "
            "[--no-opt] "
            "[--warn-string-literals] [--std=c99] [--no-libc] "
            "[--dynlink] [-E] <input.c> [input.c ...]\n");
        usage_error("Missing source file");
    }

    /* --dot stops the pipeline at a different phase than these do. */
    if (dump_dot && expand_only)
        usage_error("--dot cannot be combined with -E");

    if (dump_dot && dump_ir)
        usage_error("--dot cannot be combined with --dump-ir");

    if (dump_dot && !out) {
        if (input_count != 1)
            usage_error("--dot requires -o with multiple input files");
        out = dot_output_name(inputs[0]);
    }

    /* The graph is written by truncating its output, so naming the input
     * destroys the source. That happens both when -o names it outright and when
     * an input already ending in .dot derives its own name.
     */
    if (dump_dot)
        for (int i = 0; i < input_count; i++)
            if (!strcmp(out, inputs[i]))
                usage_error(
                    "--dot would overwrite an input; name another output");

    /* initialize global objects */
    global_init();
    dump_stats_layout();

    /* include libc */
    if (libc) {
        libc_decl();
        if (!dynlink)
            libc_impl();
        libc_token_stream = gen_libc_token_stream();
    }

    /* Preprocess each input independently so macro definitions and #pragma once
     * state do not leak between translation units. The existing implicit libc
     * prelude remains attached to the first input only.
     */
    for (int i = 0; i < input_count; i++) {
        token_t *unit_tokens;

        token_stream = gen_file_token_stream(inputs[i]);
        if (i == 0 && libc_token_stream) {
            libc_token_stream->tail->next = token_stream->head;
            token_stream = libc_token_stream;
        }
        unit_tokens = preprocess(token_stream->head);
        if (!unit_tokens)
            fatal("Preprocessor produced no input tokens");
        if (input_tail) {
            /* An EOF ends preprocessing for one input, but not compilation of
             * the next input. Reuse it as an explicit parser-visible boundary
             * that also preserves the line separator emitted by -E.
             */
            input_tail->kind = T_translation_unit;
            input_tail->next = unit_tokens;
        } else {
            input_tokens = unit_tokens;
        }
        input_tail = unit_tokens;
        while (input_tail->kind != T_eof && input_tail->next)
            input_tail = input_tail->next;
        if (input_tail->kind != T_eof)
            fatal("Preprocessor output has no end-of-file token");
    }

    if (expand_only) {
        emit_preprocessed_token(input_tokens);
        free(inputs);
        exit(0);
    }

    /* load and parse source code into IR; each input is one unit */
    translation_unit_count = input_count;
    parse(pp_strip_layout(input_tokens));
    free(inputs);
    dump_stats_phase("parse");

    /* Tokens, macros and the source buffers they point into are dead once
     * parsing finishes; releasing them here keeps 17 MiB out of the peak.
     */
    release_token_arena();

    /* Finalize graphs after all declarations and initializers are known. */
    vir_frontend_finish_globals();
    compile_native_vir(out);
    if (dump_dot) {
        vir_frontend_release();
        global_release();
        exit(0);
    }

    ph2_compute_liveness();
    peephole();

    /* Apply arch-specific IR tweaks before final codegen */
    arch_lower();
    ph2_compute_liveness();

    /* flatten CFG to linear instruction */
    cfg_flatten();
    dump_stats_phase("lower");
    vir_frontend_release();

    /* Compact after CFG flattening - BB and GENERAL no longer needed */
    compact_arenas_selective(COMPACT_ARENA_BB | COMPACT_ARENA_GENERAL);



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
