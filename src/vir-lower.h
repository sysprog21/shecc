#pragma once

#include "defs.h"
#include "vir.h"

/* Machine blocks still use the PH2 container consumed by the four emitters;
 * neither source variables nor source instructions participate in lowering.
 * Entry block arguments describe the fixed scalar ABI parameters.
 */
typedef struct {
    basic_block_t *entry;
    int stack_size;
    int block_count;
} vir_machine_function_t;

bool vir_lower_machine(const vir_function_t *function,
                       const vir_call_signature_t *signature,
                       int (*global_offset)(const char *, void *),
                       void *global_context,
                       vir_machine_function_t *output);
