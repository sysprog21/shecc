/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* Translate IR to target machine code */

#include "arm.c"
#include "defs.h"
#include "globals.c"

void update_elf_offset(ph2_ir_t *ph2_ir)
{
    func_t *func;
    switch (ph2_ir->op) {
    case OP_load_constant:
        /* ARMv7 uses 12 bits to encode immediate value, but the higher 4 bits
         * are for rotation. See A5.2.4 "Modified immediate constants in ARM
         * instructions" in ARMv7-A manual.
         */
        if (ph2_ir->src0 < 0)
            elf_offset += 12;
        else if (ph2_ir->src0 > 255)
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_address_of:
    case OP_global_address_of:
        /* ARMv7 uses 12 bits to encode immediate value, but the higher 4 bits
         * are for rotation. See A5.2.4 "Modified immediate constants in ARM
         * instructions" in ARMv7-A manual.
         */
        if (ph2_ir->src0 > 255)
            elf_offset += 12;
        else if (ph2_ir->src0 >= 0)
            elf_offset += 4;
        else
            abort();
        return;
    case OP_assign:
        if (ph2_ir->dest != ph2_ir->src0)
            elf_offset += 4;
        return;
    case OP_load:
    case OP_global_load:
        /* ARMv7 straight uses 12 bits to encode the offset of load instruction
         * (no rotation).
         */
        if (ph2_ir->src0 > 4095)
            elf_offset += 16;
        else if (ph2_ir->src0 >= 0)
            elf_offset += 4;
        else
            abort();
        return;
    case OP_store:
    case OP_global_store:
        /* ARMv7 straight uses 12 bits to encode the offset of store instruction
         * (no rotation).
         */
        if (ph2_ir->src1 > 4095)
            elf_offset += 16;
        else if (ph2_ir->src1 >= 0)
            elf_offset += 4;
        else
            abort();
        return;
    case OP_read:
    case OP_write:
    case OP_jump:
    case OP_load_func:
    case OP_indirect:
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_negate:
    case OP_bit_not:
        elf_offset += 4;
        return;
    case OP_call:
        func = find_func(ph2_ir->func_name);
        if (func->bbs)
            elf_offset += 4;
        else if (dynlink)
            elf_offset += 12;
        else {
            printf("The '%s' function is not implemented\n", ph2_ir->func_name);
            abort();
        }
        return;
    case OP_div:
    case OP_mod:
        if (hard_mul_div) {
            if (ph2_ir->op == OP_div)
                elf_offset += 4;
            else
                elf_offset += 12;
            return;
        }
        /* div/mod emulation's offset */
        elf_offset += 116;
        return;
    case OP_load_data_address:
    case OP_load_rodata_address:
        elf_offset += 8;
        return;
    case OP_address_of_func:
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
    case OP_log_not:
        elf_offset += 12;
        return;
    case OP_branch:
        if (ph2_ir->is_branch_detached)
            elf_offset += 12;
        else
            elf_offset += 8;
        return;
    case OP_return:
        elf_offset += 24;
        return;
    case OP_trunc:
        if (ph2_ir->src1 == 2)
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_sign_ext:
        elf_offset += 4;
        return;
    case OP_cast:
        elf_offset += 4;
        return;
    default:
        fatal("Unknown opcode");
    }
}

void cfg_flatten(void)
{
    func_t *func;

    if (dynlink)
        elf_offset =
            88; /* offset of __libc_start_main + main_wrapper in codegen */
    else {
        func = find_func("__syscall");
        func->bbs->elf_offset = 48; /* offset of start + exit in codegen */
        elf_offset =
            84; /* offset of start + branch + exit + syscall in codegen */
    }

    GLOBAL_FUNC->bbs->elf_offset = elf_offset;

    for (ph2_ir_t *ph2_ir = GLOBAL_FUNC->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next) {
        update_elf_offset(ph2_ir);
    }

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    if (dynlink)
        elf_offset += 28;
    else
        elf_offset += 32; /* 6 insns for main call + 2 for exit */

    for (func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        /* reserve stack */
        ph2_ir_t *flatten_ir = add_ph2_ir(OP_define);
        flatten_ir->src0 = func->stack_size;
        strncpy(flatten_ir->func_name, func->return_def.var_name, MAX_VAR_LEN);

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            bb->elf_offset = elf_offset;

            if (bb == func->bbs) {
                /* save ra, sp */
                elf_offset += 16;
            }

            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                flatten_ir = add_existed_ph2_ir(insn);

                if (insn->op == OP_return) {
                    /* restore sp */
                    flatten_ir->src1 = bb->belong_to->stack_size;
                }

                /* Branch detachment is determined in the arch-lowering stage */

                update_elf_offset(flatten_ir);
            }
        }
    }
}

void emit(int code)
{
    elf_write_int(elf_code, code);
}

void emit_ph2_ir(ph2_ir_t *ph2_ir)
{
    func_t *func;
    const int rd = ph2_ir->dest;
    const int rn = ph2_ir->src0;
    int rm = ph2_ir->src1; /* Not const because OP_trunc modifies it */
    int ofs;
    bool is_external_call = false;

    /* Prepare this variable to reuse code for:
     * 1. division and modulo operations
     * 2. load and store operations
     * 3. address-of operations
     */
    arm_reg interm;

    switch (ph2_ir->op) {
    case OP_define:
        emit(__sw(__AL, __lr, __sp, -4));
        emit(__movw(__AL, __r8, ph2_ir->src0 + 4));
        emit(__movt(__AL, __r8, ph2_ir->src0 + 4));
        emit(__sub_r(__AL, __sp, __sp, __r8));
        return;
    case OP_load_constant:
        if (ph2_ir->src0 < 0) {
            emit(__movw(__AL, __r8, -ph2_ir->src0));
            emit(__movt(__AL, __r8, -ph2_ir->src0));
            emit(__rsb_i(__AL, rd, 0, __r8));
        } else if (ph2_ir->src0 > 255) {
            emit(__movw(__AL, rd, ph2_ir->src0));
            emit(__movt(__AL, rd, ph2_ir->src0));
        } else
            emit(__mov_i(__AL, rd, ph2_ir->src0));
        return;
    case OP_address_of:
    case OP_global_address_of:
        interm = ph2_ir->op == OP_address_of ? __sp : __r12;
        if (ph2_ir->src0 > 255) {
            emit(__movw(__AL, __r8, ph2_ir->src0));
            emit(__movt(__AL, __r8, ph2_ir->src0));
            emit(__add_r(__AL, rd, interm, __r8));
        } else
            emit(__add_i(__AL, rd, interm, ph2_ir->src0));
        return;
    case OP_assign:
        emit(__mov_r(__AL, rd, rn));
        return;
    case OP_load:
    case OP_global_load:
        interm = ph2_ir->op == OP_load ? __sp : __r12;
        if (ph2_ir->src0 > 4095) {
            emit(__movw(__AL, __r8, ph2_ir->src0));
            emit(__movt(__AL, __r8, ph2_ir->src0));
            emit(__add_r(__AL, __r8, interm, __r8));
            emit(__lw(__AL, rd, __r8, 0));
        } else
            emit(__lw(__AL, rd, interm, ph2_ir->src0));
        return;
    case OP_store:
    case OP_global_store:
        interm = ph2_ir->op == OP_store ? __sp : __r12;
        if (ph2_ir->src1 > 4095) {
            emit(__movw(__AL, __r8, ph2_ir->src1));
            emit(__movt(__AL, __r8, ph2_ir->src1));
            emit(__add_r(__AL, __r8, interm, __r8));
            emit(__sw(__AL, rn, __r8, 0));
        } else
            emit(__sw(__AL, rn, interm, ph2_ir->src1));
        return;
    case OP_read:
        if (ph2_ir->src1 == 1)
            emit(__lb(__AL, rd, rn, 0));
        else if (ph2_ir->src1 == 2)
            emit(__lh(__AL, rd, rn, 0));
        else if (ph2_ir->src1 == 4)
            emit(__lw(__AL, rd, rn, 0));
        else
            abort();
        return;
    case OP_write:
        if (ph2_ir->dest == 1)
            emit(__sb(__AL, rm, rn, 0));
        else if (ph2_ir->dest == 2)
            emit(__sh(__AL, rm, rn, 0));
        else if (ph2_ir->dest == 4)
            emit(__sw(__AL, rm, rn, 0));
        else
            abort();
        return;
    case OP_branch:
        emit(__teq(rn));
        if (ph2_ir->is_branch_detached) {
            emit(__b(__NE, 8));
            emit(__b(__AL, ph2_ir->else_bb->elf_offset - elf_code->size));
        } else
            emit(__b(__NE, ph2_ir->then_bb->elf_offset - elf_code->size));
        return;
    case OP_jump:
        emit(__b(__AL, ph2_ir->next_bb->elf_offset - elf_code->size));
        return;
    case OP_call:
        func = find_func(ph2_ir->func_name);
        if (func->bbs)
            ofs = func->bbs->elf_offset - elf_code->size;
        else if (dynlink) {
            ofs = (dynamic_sections.elf_plt_start + func->plt_offset) -
                  (elf_code_start + elf_code->size + 4);
            is_external_call = true;
        } else {
            printf("The '%s' function is not implemented\n", ph2_ir->func_name);
            abort();
        }

        /* If the callee is external, save __r12 at [sp + 16] and
         * restore it after the function returns.
         */
        if (is_external_call)
            emit(__sw(__AL, __r12, __sp, 16));
        emit(__bl(__AL, ofs));
        if (is_external_call)
            emit(__lw(__AL, __r12, __sp, 16));
        return;
    case OP_load_data_address:
        emit(__movw(__AL, rd, ph2_ir->src0 + elf_data_start));
        emit(__movt(__AL, rd, ph2_ir->src0 + elf_data_start));
        return;
    case OP_load_rodata_address:
        emit(__movw(__AL, rd, ph2_ir->src0 + elf_rodata_start));
        emit(__movt(__AL, rd, ph2_ir->src0 + elf_rodata_start));
        return;
    case OP_address_of_func:
        func = find_func(ph2_ir->func_name);
        if (func->bbs)
            ofs = elf_code_start + func->bbs->elf_offset;
        else if (dynlink)
            ofs = dynamic_sections.elf_plt_start + func->plt_offset;
        else {
            printf("The '%s' function is not implemented\n", ph2_ir->func_name);
            abort();
        }
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
        emit(__sw(__AL, __r8, rn, 0));
        return;
    case OP_load_func:
        emit(__mov_r(__AL, __r8, rn));
        return;
    case OP_indirect:
        emit(__blx(__AL, __r8));
        return;
    case OP_return:
        if (ph2_ir->src0 == -1)
            emit(__mov_r(__AL, __r0, __r0));
        else
            emit(__mov_r(__AL, __r0, rn));
        emit(__movw(__AL, __r8, ph2_ir->src1 + 4));
        emit(__movt(__AL, __r8, ph2_ir->src1 + 4));
        emit(__add_r(__AL, __sp, __sp, __r8));
        emit(__lw(__AL, __lr, __sp, -4));
        emit(__bx(__AL, __lr));
        return;
    case OP_add:
        emit(__add_r(__AL, rd, rn, rm));
        return;
    case OP_sub:
        emit(__sub_r(__AL, rd, rn, rm));
        return;
    case OP_mul:
        emit(__mul(__AL, rd, rn, rm));
        return;
    case OP_div:
    case OP_mod:
        if (hard_mul_div) {
            if (ph2_ir->op == OP_div)
                emit(__div(__AL, rd, rm, rn));
            else {
                emit(__div(__AL, __r8, rm, rn));
                emit(__mul(__AL, __r8, rm, __r8));
                emit(__sub_r(__AL, rd, rn, __r8));
            }
            return;
        }
        interm = __r8;
        /* div/mod emulation */
        /* Preserve the values of the dividend and divisor */
        emit(__stmdb(__AL, 1, __sp, (1 << rn) | (1 << rm)));
        /* Obtain absolute values of the dividend and divisor */
        emit(__srl_amt(__AL, 0, arith_rs, __r8, rn, 31));
        emit(__add_r(__AL, rn, rn, __r8));
        emit(__eor_r(__AL, rn, rn, __r8));
        emit(__srl_amt(__AL, 0, arith_rs, __r9, rm, 31));
        emit(__add_r(__AL, rm, rm, __r9));
        emit(__eor_r(__AL, rm, rm, __r9));
        if (ph2_ir->op == OP_div)
            emit(__eor_r(__AL, __r10, __r8, __r9));
        else {
            /* If the requested operation is modulo, the result will be stored
             * in __r9. The sign of the divisor is irrelevant for determining
             * the result's sign.
             */
            interm = __r9;
            emit(__mov_r(__AL, __r10, __r8));
        }
        /* Unsigned integer division */
        emit(__zero(__r8));
        emit(__mov_i(__AL, __r9, 1));
        emit(__cmp_i(__AL, rm, 0));
        emit(__b(__EQ, 52));
        emit(__cmp_i(__AL, rn, 0));
        emit(__b(__EQ, 44));
        emit(__cmp_r(__AL, rm, rn));
        emit(__sll_amt(__CC, 0, logic_ls, rm, rm, 1));
        emit(__sll_amt(__CC, 0, logic_ls, __r9, __r9, 1));
        emit(__b(__CC, -12));
        emit(__cmp_r(__AL, rn, rm));
        emit(__sub_r(__CS, rn, rn, rm));
        emit(__add_r(__CS, __r8, __r8, __r9));
        emit(__srl_amt(__AL, 1, logic_rs, __r9, __r9, 1));
        emit(__srl_amt(__CC, 0, logic_rs, rm, rm, 1));
        emit(__b(__CC, -20));
        /* After completing the emulation, the quotient and remainder will be
         * stored in __r8 and __r9, respectively.
         *
         * The original values of the dividend and divisor will be restored in
         * rn and rm.
         *
         * Finally, the result (quotient or remainder) will be stored in rd.
         */
        emit(__mov_r(__AL, __r9, rn));
        emit(__ldm(__AL, 1, __sp, (1 << rn) | (1 << rm)));
        emit(__mov_r(__AL, rd, interm));
        /* Handle the correct sign for the quotient or remainder */
        emit(__cmp_i(__AL, __r10, 0));
        emit(__rsb_i(__NE, rd, 0, rd));
        return;
    case OP_lshift:
        emit(__sll(__AL, rd, rn, rm));
        return;
    case OP_rshift:
        emit(__sra(__AL, rd, rn, rm));
        return;
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
        emit(__cmp_r(__AL, rn, rm));
        emit(__zero(rd));
        emit(__mov_i(arm_get_cond(ph2_ir->op), rd, 1));
        return;
    case OP_negate:
        emit(__rsb_i(__AL, rd, 0, rn));
        return;
    case OP_bit_not:
        emit(__mvn_r(__AL, rd, rn));
        return;
    case OP_bit_and:
        emit(__and_r(__AL, rd, rn, rm));
        return;
    case OP_bit_or:
        emit(__or_r(__AL, rd, rn, rm));
        return;
    case OP_bit_xor:
        emit(__eor_r(__AL, rd, rn, rm));
        return;
    case OP_log_not:
        emit(__cmp_i(__AL, rn, 0));
        emit(__mov_i(__NE, rd, 0));
        emit(__mov_i(__EQ, rd, 1));
        return;
    case OP_trunc:
        if (rm == 1) {
            emit(__and_i(__AL, rd, rn, 0xFF));
        } else if (rm == 2) {
            emit(__sll_amt(__AL, 0, logic_ls, rd, rn, 16));
            emit(__sll_amt(__AL, 0, logic_rs, rd, rd, 16));
        } else if (rm == 4) {
            emit(__mov_r(__AL, rd, rn));
        } else {
            fatal("Unsupported truncation operation with invalid target size");
        }
        return;
    case OP_sign_ext: {
        /* Decode source size from upper 16 bits */
        int source_size = (rm >> 16) & 0xFFFF;
        if (source_size == 2) {
            emit(__sxth(__AL, rd, rn, 0));
        } else {
            /* For other cases, use byte extension (original behavior) */
            emit(__sxtb(__AL, rd, rn, 0));
        }
    }
        return;
    case OP_cast:
        /* Generic cast operation - for now, just move the value */
        emit(__mov_r(__AL, rd, rn));
        return;
    default:
        fatal("Unknown opcode");
    }
}

void plt_generate(void);
void code_generate(void)
{
    if (dynlink) {
        plt_generate();
        /* Call __libc_start_main() */
        emit(__mov_i(__AL, __r11, 0));
        emit(__mov_i(__AL, __lr, 0));
        emit(__pop_word(__AL, __r1));
        emit(__mov_r(__AL, __r2, __sp));
        emit(__push_reg(__AL, __r2));
        emit(__push_reg(__AL, __r0));
        emit(__mov_i(__AL, __r12, 0));
        emit(__push_reg(__AL, __r12));

        int main_wrapper_offset = elf_code->size + 28;
        emit(__movw(__AL, __r0, elf_code_start + main_wrapper_offset));
        emit(__movt(__AL, __r0, elf_code_start + main_wrapper_offset));
        emit(__mov_i(__AL, __r3, 0));
        emit(__bl(__AL, (dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE) -
                            (elf_code_start + elf_code->size)));
        /* Call '_exit' (syscall) to terminate the program if __libc_start_main
         * returns. */
        emit(__mov_i(__AL, __r0, 127));
        emit(__mov_i(__AL, __r7, 1));
        emit(__svc());

        /* If the compiled program is dynamic linking, the starting
         * point of 'main_wrapper' is located here.
         *
         * Push the contents of r4-r11 and lr onto stack.
         * Preserve 'argc' and 'argv' for the 'main' function.
         */
        emit(__stmdb(__AL, 1, __sp, 0x4FF0));
        emit(__mov_r(__AL, __r9, __r0));
        emit(__mov_r(__AL, __r10, __r1));
    }
    /* For both static and dynamic linking, we need to set up the stack
     * and call the main function.
     */
    emit(__movw(__AL, __r8, GLOBAL_FUNC->stack_size));
    emit(__movt(__AL, __r8, GLOBAL_FUNC->stack_size));
    emit(__sub_r(__AL, __sp, __sp, __r8));
    emit(__mov_r(__AL, __r12, __sp));

    if (!dynlink) {
        emit(__bl(__AL, GLOBAL_FUNC->bbs->elf_offset - elf_code->size));
        /* After global init, jump to main preparation */
        emit(__b(__AL,
                 56)); /* PC+8: skip exit (24) + syscall (36) + ret (4) - 8 */

        /* exit - only for static linking */
        emit(__movw(__AL, __r8, GLOBAL_FUNC->stack_size));
        emit(__movt(__AL, __r8, GLOBAL_FUNC->stack_size));
        emit(__add_r(__AL, __sp, __sp, __r8));
        emit(__mov_r(__AL, __r0, __r0));
        emit(__mov_i(__AL, __r7, 1));
        emit(__svc());

        /* syscall */
        emit(__mov_r(__AL, __r7, __r0));
        emit(__mov_r(__AL, __r0, __r1));
        emit(__mov_r(__AL, __r1, __r2));
        emit(__mov_r(__AL, __r2, __r3));
        emit(__mov_r(__AL, __r3, __r4));
        emit(__mov_r(__AL, __r4, __r5));
        emit(__mov_r(__AL, __r5, __r6));
        emit(__svc());
        emit(__bx(__AL, __lr));
    }

    ph2_ir_t *ph2_ir;
    for (ph2_ir = GLOBAL_FUNC->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next)
        emit_ph2_ir(ph2_ir);

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    if (MAIN_BB) {
        if (dynlink) {
            emit(__mov_r(__AL, __r0, __r9));
            emit(__mov_r(__AL, __r1, __r10));
            /* Call the main function.
             *
             * After the main function returns, the following
             * instructions restore the registers r4-r11 and
             * return control to __libc_start_main via the
             * preserved lr.
             */
            emit(__bl(__AL, MAIN_BB->elf_offset - elf_code->size));
            emit(__movw(__AL, __r8, GLOBAL_FUNC->stack_size));
            emit(__movt(__AL, __r8, GLOBAL_FUNC->stack_size));
            emit(__add_r(__AL, __sp, __sp, __r8));
            emit(__ldm(__AL, 1, __sp, 0x8FF0));
        } else {
            emit(__movw(__AL, __r8, GLOBAL_FUNC->stack_size));
            emit(__movt(__AL, __r8, GLOBAL_FUNC->stack_size));
            emit(__add_r(__AL, __r8, __r12, __r8));
            emit(__lw(__AL, __r0, __r8, 0));
            emit(__add_i(__AL, __r1, __r8, 4));

            /* Call main function, and call '_exit' syscall to
             * terminate the program. */
            emit(__bl(__AL, MAIN_BB->elf_offset - elf_code->size));

            /* exit with main's return value - r0 already has the
             * return value */
            emit(__mov_i(__AL, __r7, 1));
            emit(__svc());
        }
    }

    for (int i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = PH2_IR_FLATTEN[i];
        emit_ph2_ir(ph2_ir);
    }
}

void plt_generate(void)
{
    int addr_of_got = dynamic_sections.elf_got_start + PTR_SIZE * 2;
    int end = dynamic_sections.plt_size - PLT_FIXUP_SIZE;
    elf_write_int(dynamic_sections.elf_plt, __push_reg(__AL, __lr));
    elf_write_int(dynamic_sections.elf_plt, __movw(__AL, __r10, addr_of_got));
    elf_write_int(dynamic_sections.elf_plt, __movt(__AL, __r10, addr_of_got));
    elf_write_int(dynamic_sections.elf_plt, __mov_r(__AL, __lr, __r10));
    elf_write_int(dynamic_sections.elf_plt, __lw(__AL, __pc, __lr, 0));
    for (int i = 0; i * PLT_ENT_SIZE < end; i++) {
        addr_of_got = dynamic_sections.elf_got_start + PTR_SIZE * (i + 3);
        elf_write_int(dynamic_sections.elf_plt,
                      __movw(__AL, __r12, addr_of_got));
        elf_write_int(dynamic_sections.elf_plt,
                      __movt(__AL, __r12, addr_of_got));
        elf_write_int(dynamic_sections.elf_plt, __lw(__AL, __pc, __r12, 0));
    }
}
