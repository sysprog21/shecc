/* Native parser graph construction and lifecycle. */
#pragma once

#include "defs.h"
#include "vir.h"

extern int vir_frontend_opt_level;

const vir_function_t *vir_frontend_function(const func_t *func);
bool vir_frontend_fold_i32(opcode_t opcode, int left, int right, int *result);
void vir_frontend_finish_globals(void);
void vir_frontend_release(void);

void vir_frontend_begin(func_t *func);
void vir_frontend_note_insn(basic_block_t *bb,
                            opcode_t opcode,
                            var_t *rd,
                            var_t *rs1,
                            var_t *rs2,
                            int size_bytes,
                            const char *symbol);
void vir_frontend_note_edge(basic_block_t *pred, basic_block_t *succ);
void vir_frontend_end(func_t *func);

const vir_call_signature_t *vir_frontend_signature(const func_t *func);
void vir_frontend_layout_globals(void);
int vir_frontend_global_offset(const char *name, void *context);
