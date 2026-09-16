/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Constant cast optimization pass.
 *
 * Folds the 'load_constant' + 'trunc' pattern into a single constant load.
 * General constant propagation and folding live in const_folding() in ssa.c,
 * which runs over the same IR immediately afterwards.
 */

/* Narrow a constant to 'size' bytes, keeping its sign. A signed narrow type
 * sign-extends when widened, so masking alone would make "char c = -1" compare
 * as 255. A size the caller does not narrow is returned unchanged.
 */
int sign_extend_const(int value, int size)
{
    if (size == 1) {
        value = value & 0xFF;
        if (value & 0x80)
            value = value | ~0xFF;
    } else if (size == 2) {
        value = value & 0xFFFF;
        if (value & 0x8000)
            value = value | ~0xFFFF;
    }
    return value;
}

/* Targeted constant truncation peephole optimization */
bool optimize_constant_casts(func_t *func)
{
    if (!func || !func->bbs)
        return false;

    bool changed = false;

    /* Simple peephole optimization: const + trunc pattern */
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        for (insn_t *insn = bb->insn_list.head; insn && insn->next;
             insn = insn->next) {
            insn_t *next_insn = insn->next;

            /* Look for pattern: const %.tX, VALUE followed by %.tY = trunc
             * %.tX, SIZE
             */
            if (insn->opcode == OP_load_constant &&
                next_insn->opcode == OP_trunc && insn->rd && next_insn->rs1 &&
                insn->rd == next_insn->rs1 && next_insn->sz > 0 &&
                !next_insn->rd->is_global) {
                int value = insn->rd->init_val;
                int result = value;

                if (next_insn->sz != 1 && next_insn->sz != 2 &&
                    next_insn->sz != 4)
                    continue; /* not a width we narrow to */
                result = sign_extend_const(value, next_insn->sz);

                /* An unsigned narrow type zero-extends instead: folding
                 * "unsigned short x = 0xfffe" to -2 made (int) x compare as -2.
                 */
                bool is_unsigned = next_insn->rd->type &&
                                   next_insn->rd->type->is_unsigned &&
                                   !next_insn->rd->ptr_level;
                if (is_unsigned && next_insn->sz < 4)
                    result = value & ((1 << (next_insn->sz * 8)) - 1);

                /* Optimize: Replace both instructions with single const */
                insn->rd = next_insn->rd; /* Update dest to final target */
                insn->rd->is_const = true;
                insn->rd->init_val = result;

                /* The high word belongs to the narrowed value too. A 64-bit
                 * target loads an argument constant at full width, and a zero
                 * high word made "f(0xe231fffab478ULL)" pass its int parameter
                 * as 0xfffab478 rather than -347016.
                 */
                insn->rd->init_val_hi = result < 0 && !is_unsigned ? -1 : 0;

                /* Remove the truncation instruction by converting it to
                 * NOP-like
                 */
                next_insn->opcode = OP_load_constant;
                next_insn->rd->is_const = true;
                next_insn->rd->init_val = result;
                next_insn->rs1 = NULL;
                next_insn->sz = 0;

                changed = true;
            }
        }
    }

    return changed;
}
