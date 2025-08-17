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

                    /* Perform truncation based on size */
                    if (insn->sz == 1) {
                        /* Truncate to 8 bits */
                        result = value & 0xFF;
                    } else if (insn->sz == 2) {
                        /* Truncate to 16 bits */
                        result = value & 0xFFFF;
                    } else if (insn->sz == 4) {
                        /* No truncation needed for 32-bit */
                        result = value;
                    } else {
                        /* Invalid size, skip */
                        break;
                    }

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

                    /* Perform sign extension based on source size */
                    if (insn->sz == 1) {
                        /* Sign extend from 8 bits */
                        result = (value & 0x80) ? (value | 0xFFFFFF00)
                                                : (value & 0xFF);
                    } else if (insn->sz == 2) {
                        /* Sign extend from 16 bits */
                        result = (value & 0x8000) ? (value | 0xFFFF0000)
                                                  : (value & 0xFFFF);
                    } else if (insn->sz == 4) {
                        /* No sign extension needed for 32-bit */
                        result = value;
                    } else {
                        /* Invalid size, skip */
                        break;
                    }

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
                /* Fold binary operations with constants */
                if (insn->rs1 && insn->rs1->is_const && insn->rs2 &&
                    insn->rs2->is_const &&
                    !insn->rd->is_global) { /* Don't modify globals */
                    int result = 0;
                    int l = insn->rs1->init_val, r = insn->rs2->init_val;

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

            case OP_eq:
            case OP_neq:
            case OP_lt:
            case OP_leq:
            case OP_gt:
            case OP_geq:
                /* Fold comparison operations */
                if (insn->rs1 && insn->rs1->is_const && insn->rs2 &&
                    insn->rs2->is_const &&
                    !insn->rd->is_global) { /* Don't modify globals */
                    int result = 0;
                    int l = insn->rs1->init_val;
                    int r = insn->rs2->init_val;

                    switch (insn->opcode) {
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

                /* Perform truncation based on size */
                if (next_insn->sz == 1) {
                    /* Truncate to 8 bits */
                    result = value & 0xFF;
                } else if (next_insn->sz == 2) {
                    /* Truncate to 16 bits */
                    result = value & 0xFFFF;
                } else if (next_insn->sz == 4) {
                    /* No truncation needed for 32-bit */
                    result = value;
                } else {
                    /* Invalid size, skip */
                    continue;
                }

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
