/*
 * shecc - Architecture-specific IR lowering stage
 *
 * Introduces a minimal arch-lowering boundary that applies target-specific
 * tweaks to phase-2 IR (ph2_ir) before final code generation. This keeps
 * backends simpler by moving decisions that depend on CFG shape or target
 * quirks out of emit-time where possible.
 */

#include "../config"
#include "defs.h"

/* ARM-specific lowering:
 * - Mark detached conditional branches so codegen can decide between
 *   short/long forms without re-deriving CFG shape.
 */
void arm_lower(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                /* Mark branches that don't fall through to next block */
                if (insn->op == OP_branch) {
                    /* In SSA, we index 'else_bb' first, and then 'then_bb' */
                    insn->is_branch_detached = (insn->else_bb != bb->rpo_next);
                }
            }
        }
    }
}

/* RISC-V-specific lowering:
 * - Mark detached conditional branches
 * - Future: prepare for RISC-V specific patterns
 */
void riscv_lower(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                /* Mark branches that don't fall through to next block */
                if (insn->op == OP_branch)
                    insn->is_branch_detached = (insn->else_bb != bb->rpo_next);
            }
        }
    }
}

/* Entry point: dispatch to the active architecture. */
void arch_lower(void)
{
#if ELF_MACHINE == 0x28 /* ARM */
    arm_lower();
#elif ELF_MACHINE == 0xf3 /* RISC-V */
    riscv_lower();
#else
    /* Unknown architecture: keep behavior as-is. */
    (void) 0;
#endif
}
