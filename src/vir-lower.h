#pragma once

#include "defs.h"
#include "vir.h"

/* Machine blocks hold the VIR machine operations (vir_mop_t) the four emitters
 * consume; neither source variables nor source instructions participate in
 * lowering. Entry block arguments describe the fixed scalar ABI parameters.
 */
typedef struct {
    basic_block_t *entry;
    int stack_size;
    int block_count;

    /* Whether a call passes arguments on the stack, which then sit at the
     * bottom of the frame where the callee expects them.
     */
    bool passes_stack_args;
} vir_machine_function_t;

bool vir_lower_machine(const vir_function_t *function,
                       const vir_call_signature_t *signature,
                       int (*global_offset)(const char *, void *),
                       void *global_context,
                       vir_machine_function_t *output);

/* VIR machine operation: one allocated instruction the shared lowerer produces
 * and a backend emits.
 */
struct vir_mop {
    /* Grouped by width so the struct carries no interior padding: mixed in
     * declaration order it was 72 bytes for 63 bytes of fields, on all ~101k of
     * them a self-compile emits. Callee / definition name, interned in
     * GENERAL_ARENA rather than copied. A MAX_VAR_LEN array here was 128 of
     * this struct's 192 bytes while only OP_define, OP_call and
     * OP_address_of_func ever name anything. NULL when unused.
     */
    char *func_name;
    basic_block_t *next_bb;
    basic_block_t *then_bb;
    basic_block_t *else_bb;
    struct vir_mop *next;

    opcode_t op;
    int src0;
    int src1;

    /* Backend metadata. On OP_define and OP_return, the saved-register set the
     * frame keeps. On OP_jump and OP_branch, how the edge is encoded: on ARM
     * and ARM64 whether the jump falls into the next block or which branch
     * successor does; on RISC-V the chosen branch or jump form.
     */
    int src2;
    int dest;

    /* A 32-bit target represents a wide integer as low/high register pairs. -1
     * means this instruction uses the existing single-register form.
     */
    int src0_hi;
    int src1_hi;
    int dest_hi;
    /* Type information for LP64 support */
    int size_bytes; /* Size in bytes for load/store/read/write operations */

    /* When an instruction uses a variable that its offset is based on the top
     * of the stack, this instruction's flag is also set to indicate the
     * compiler to recalculate the offset after the function's stack size has
     * been determined.
     *
     * Currently, only OP_load, OP_store and OP_address_of need this flag to
     * recompute the offset.
     */
    bool ofs_based_on_stack_top;
    bool is_pointer; /* True if this operation involves a pointer type */
    /* Scalar signedness accompanies register values independently of their
     * storage width. Comparisons inspect their sources; arithmetic and loads
     * inspect the result.
     */
    bool is_unsigned;

    /* Operand provenance is required by LP64 backends: pointer arithmetic keeps
     * the address operand wide but sign-extends an int index.
     */
    bool src0_is_pointer;
    bool src1_is_pointer;
    bool src0_is_unsigned;
    bool src1_is_unsigned;

    /* The load, read or store accesses a volatile object, which is a side
     * effect whether or not its value is used (C99 6.7.3p6), so no rewrite may
     * drop it, replace a load with a copy of a value read earlier, or drop a
     * store for writing what the object already holds.
     */
    bool is_volatile;
};

#define MOP_PAIR_DEST_SRC0(ir) ((ir)->dest_hi >= 0 && (ir)->src0_hi >= 0)
#define MOP_PAIR_BINARY(ir) (MOP_PAIR_DEST_SRC0(ir) && (ir)->src1_hi >= 0)

int mop_flatten_function(func_t *func,
                         int stack_top_ofs,
                         int prologue_bytes,
                         int saved_regs,
                         bool restore_saved_regs,
                         void (*update_offset)(vir_mop_t *));
vir_mop_t *mop_flatten_insn(vir_mop_t *mop,
                            int stack_top_ofs,
                            int stack_size,
                            int saved_regs,
                            bool restore_saved_regs);
int func_highest_used_reg(func_t *func, int first_callee_saved);
unsigned int func_used_reg_mask(func_t *func);
bool func_has_op(func_t *func, opcode_t a, opcode_t b);
bool func_makes_call(func_t *func);
bool func_has_frame_slots(func_t *func);
bool func_is_frameless_leaf(func_t *func);
void emit_mop(vir_mop_t *mop);
int mop_count_words(vir_mop_t *mop);
unsigned int mop_defs(vir_mop_t *ir);
unsigned int mop_reads(vir_mop_t *ir);
void mop_function_liveness(func_t *func);
void mop_compute_liveness(void);
