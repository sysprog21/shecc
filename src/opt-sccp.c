/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* SCCP (Sparse Conditional Constant Propagation) Optimization Pass
 *
 * This optimization pass performs:
 * - Constant propagation through assignments
 * - Constant folding for arithmetic and comparison operations
 * - Branch folding when conditions are compile-time constants
 * - Dead code elimination through unreachable branch removal
 */

/* Simple constant propagation within basic blocks */
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

bool simple_sccp(func_t *func)
{
    if (!func || !func->bbs)
        return false;

    bool changed = false;

    /* Iterate through basic blocks */
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        /* Process instructions in the block */
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            /* Skip if no destination */
            if (!insn->rd)
                continue;

            /* Handle simple constant propagation */
            switch (insn->opcode) {
            case OP_assign:
                /* Propagate constants through assignments */
                if (insn->rs1 && insn->rs1->is_const && !insn->rd->is_const) {
                    insn->rd->is_const = true;
                    insn->rd->init_val = insn->rs1->init_val;
                    insn->opcode = OP_load_constant;
                    insn->rs1 = NULL;
                    changed = true;
                }
                break;

            case OP_trunc:
                /* Constant truncation optimization integrated into SCCP */
                if (insn->rs1 && insn->rs1->is_const && !insn->rd->is_global &&
                    insn->sz > 0) {
                    int value = insn->rs1->init_val;
                    int result = value;

                    if (insn->sz != 1 && insn->sz != 2 && insn->sz != 4)
                        break; /* not a width we narrow to */
                    result = sign_extend_const(value, insn->sz);

                    /* Convert to constant load */
                    insn->opcode = OP_load_constant;
                    insn->rd->is_const = true;
                    insn->rd->init_val = result;
                    insn->rs1 = NULL;
                    insn->sz = 0;
                    changed = true;
                }
                break;

            case OP_sign_ext:
                /* Constant sign extension optimization integrated into SCCP */
                if (insn->rs1 && insn->rs1->is_const && !insn->rd->is_global &&
                    insn->sz > 0) {
                    int value = insn->rs1->init_val;
                    int result = value;

                    if (insn->sz != 1 && insn->sz != 2 && insn->sz != 4)
                        break; /* not a width we extend from */
                    result = sign_extend_const(value, insn->sz);

                    /* Convert to constant load */
                    insn->opcode = OP_load_constant;
                    insn->rd->is_const = true;
                    insn->rd->init_val = result;
                    insn->rs1 = NULL;
                    insn->sz = 0;
                    changed = true;
                }
                break;

            case OP_add:
            case OP_sub:
            case OP_mul:
            case OP_eq:
            case OP_neq:
            case OP_lt:
            case OP_leq:
            case OP_gt:
            case OP_geq:
                /* Unified constant folding for binary and comparison ops */
                if (insn->rs1 && insn->rs1->is_const && insn->rs2 &&
                    insn->rs2->is_const && !insn->rd->is_global) {
                    int result = 0;
                    const int l = insn->rs1->init_val, r = insn->rs2->init_val;

                    /* Compute result based on operation type */
                    switch (insn->opcode) {
                    case OP_add:
                        result = l + r;
                        break;
                    case OP_sub:
                        result = l - r;
                        break;
                    case OP_mul:
                        result = l * r;
                        break;
                    case OP_eq:
                        result = (l == r);
                        break;
                    case OP_neq:
                        result = (l != r);
                        break;
                    case OP_lt:
                        result = (l < r);
                        break;
                    case OP_leq:
                        result = (l <= r);
                        break;
                    case OP_gt:
                        result = (l > r);
                        break;
                    case OP_geq:
                        result = (l >= r);
                        break;
                    default:
                        continue;
                    }

                    /* Convert to constant load */
                    insn->opcode = OP_load_constant;
                    insn->rd->is_const = true;
                    insn->rd->init_val = result;
                    insn->rs1 = NULL;
                    insn->rs2 = NULL;
                    changed = true;
                }
                break;

            default:
                /* Other opcodes - no optimization */
                break;
            }
        }

        /* Simple constant branch folding */
        insn_t *last = bb->insn_list.tail;
        if (last && last->opcode == OP_branch) {
            if (last->rs1 && last->rs1->is_const) {
                /* Convert to unconditional jump */
                last->opcode = OP_jump;

                if (last->rs1->init_val != 0) {
                    /* Take then branch */
                    bb->else_ = NULL;
                } else {
                    /* Take else branch */
                    bb->then_ = bb->else_;
                    bb->else_ = NULL;
                }

                last->rs1 = NULL;
                changed = true;
            }
        }
    }

    return changed;
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
