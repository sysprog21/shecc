/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* Constant cast optimization pass.
 *
 * Folds the 'load_constant' + 'trunc' pattern into a single constant load.
 * General constant propagation and folding live in const_folding() in ssa.c,
 * which runs over the same IR immediately afterwards.
 */

/* Narrow a constant to 'size' bytes, keeping its sign. Every integer type in
 * this language is signed -- there is no 'unsigned' keyword -- and widening
 * sign-extends, so masking alone would make "char c = -1" compare as 255.
 * A size the caller does not narrow is returned unchanged.
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
        if (!bb)
            continue;

        for (insn_t *insn = bb->insn_list.head; insn && insn->next;
             insn = insn->next) {
            insn_t *next_insn = insn->next;

            /* Look for pattern: const %.tX, VALUE followed by
             * %.tY = trunc %.tX, SIZE
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

                /* Optimize: Replace both instructions with single const */
                insn->rd = next_insn->rd; /* Update dest to final target */
                insn->rd->is_const = true;
                insn->rd->init_val = result;

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
