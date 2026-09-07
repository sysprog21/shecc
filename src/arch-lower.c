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

/* Mark detached conditional branches so codegen can decide between short/long
 * forms without re-deriving CFG shape.
 *
 * Only the ARM backend reads 'is_branch_detached'; RISC-V and x86-64 ignore it,
 * so the pass runs for ARM alone.
 */
void arch_lower(void)
{
#if ELF_MACHINE == ELF_MACHINE_ARM32
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                /* In SSA, we index 'else_bb' first, and then 'then_bb' */
                if (insn->op == OP_branch)
                    insn->is_branch_detached = (insn->else_bb != bb->rpo_next);
            }
        }
    }
#endif
}
