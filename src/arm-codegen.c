/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
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
        if (ph2_ir->dest_hi >= 0) {
            if (ph2_ir->src1 < 0)
                elf_offset += 12;
            else if (ph2_ir->src1 > 255)
                elf_offset += 8;
            else
                elf_offset += 4;
        }
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
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->dest_hi != ph2_ir->src0_hi)
            elf_offset += 4;
        return;
    case OP_load:
    case OP_global_load:
        /* LDR/LDRB use a 12-bit offset, but LDRSB uses eight bits. A larger
         * signed-byte offset is materialized with MOVW/MOVT/ADD before the
         * load, exactly as an offset beyond the general load range is.
         */
        if (ph2_ir->src0 > 4095 || (ph2_ir->size_bytes == 1 &&
                                    !ph2_ir->is_unsigned && ph2_ir->src0 > 255))
            if (ph2_ir->dest_hi >= 0)
                elf_offset += 20;
            else
                elf_offset += 16;
        else if (ph2_ir->src0 >= 0)
            if (ph2_ir->dest_hi >= 0)
                elf_offset += 8;
            else
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
            if (ph2_ir->src0_hi >= 0)
                elf_offset += 20;
            else
                elf_offset += 16;
        else if (ph2_ir->src1 >= 0)
            if (ph2_ir->src0_hi >= 0)
                elf_offset += 8;
            else
                elf_offset += 4;
        else
            abort();
        return;
    case OP_read:
    case OP_write:
    case OP_jump:
    case OP_load_func:
    case OP_indirect:
    case OP_lshift:
    case OP_rshift:
        elf_offset += ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 ? 48 : 4;
        return;
    case OP_mul:
        elf_offset +=
            ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0
                ? 20
                : 4;
        return;
    case OP_negate:
        elf_offset += 4;
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0)
            elf_offset += 4;
        return;
    case OP_add:
    case OP_sub:
        elf_offset += 4;
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0)
            elf_offset += 4;
        return;
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
        elf_offset += 4;
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0)
            elf_offset += 4;
        return;
    case OP_bit_not:
        elf_offset += 4;
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0)
            elf_offset += 4;
        return;
    case OP_call:
        func = find_func(ph2_ir->func_name);
        if (func->bbs)
            elf_offset += 4;
        else if (dynlink) {
            /* When calling external functions in dynamic linking mode, the
             * following instructions are required:
             * - movw + movt: set r8 to 'elf_data_start'
             * - ldr: load a word from the address 'elf_data_start' into r12.
             *        (restore the global stack pointer.)
             *
             * Therefore, the total offset is 16 bytes (4 instructions).
             */
            elf_offset += 16;
        } else {
            printf("The '%s' function is not implemented\n", ph2_ir->func_name);
            fflush(stdout); /* see fatal() */
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
        elf_offset += 124;
        return;
    case OP_load_data_address:
    case OP_load_rodata_address:
        elf_offset += 8;
        return;
    case OP_address_of_func:
    case OP_log_not:
        elf_offset += 12;
        return;
    case OP_gt:
    case OP_lt:
        elf_offset += ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0 ? 24 : 12;
        return;
    case OP_geq:
    case OP_leq:
        elf_offset += ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0 ? 28 : 12;
        return;
    case OP_eq:
    case OP_neq:
        elf_offset += ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0 ? 16 : 12;
        return;
    case OP_branch:
        if (ph2_ir->is_branch_detached)
            elf_offset += 12;
        else
            elf_offset += 8;
        return;
    case OP_return:
        elf_offset += 24;
        if (ph2_ir->src0_hi >= 0)
            elf_offset += 4;
        return;
    case OP_trunc:
        /* Unsigned byte/half truncation uses a logical shift pair; signed
         * narrowing uses the single-instruction SXTB/SXTH forms.
         */
        if (ph2_ir->is_unsigned && (ph2_ir->src1 == 1 || ph2_ir->src1 == 2))
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_sign_ext:
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi < 0) {
            int source_size = (ph2_ir->src1 >> 16) & 0xFFFF;
            elf_offset +=
                (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer) &&
                        (source_size == 1 || source_size == 2)
                    ? 12
                    : 8;
        } else if (ph2_ir->src0_is_unsigned)
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_cast:
        elf_offset +=
            ph2_ir->dest_hi >= 0 &&
                    (ph2_ir->src0_hi < 0 || ph2_ir->dest_hi != ph2_ir->src0_hi)
                ? 8
                : 4;
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
            100; /* offset of __libc_start_main + main_wrapper in codegen */
    else {
        func = find_func("__syscall");
        func->bbs->elf_offset = 32; /* offset of start + branch in codegen */
        elf_offset = 92; /* offset of start + branch + syscall in codegen */
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
        flatten_ir->func_name = intern_string(func->return_def.var_name);

        /* The actual offset of the top of the local stack is the sum of:
         * - 36 bytes (pushing registers r4-r11 and lr onto the stack)
         * - 4 bytes
         *   (to ensure 8-byte alignment after pushing the 9 registers)
         * - ALIGN_UP(func->stack_size, 8)
         *
         * Note that func->stack_size does not include the 36 + 4 bytes, so an
         * additional 40 bytes should be added to ALIGN_UP(func->stack_size, 8).
         */
        int stack_top_ofs = ALIGN_UP(func->stack_size, MIN_ALIGNMENT) + 40;

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            bb->elf_offset = elf_offset;

            if (bb == func->bbs) {
                /* retrieve the global stack pointer and save ra, sp */
                elf_offset += 28;
            }

            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                /* For the instructions whose ofs_based_on_stack_top is set,
                 * recalculate the operand's offset by adding stack_top_ofs.
                 */
                if (insn->ofs_based_on_stack_top) {
                    switch (insn->op) {
                    case OP_load:
                    case OP_address_of:
                        insn->src0 = insn->src0 + stack_top_ofs;
                        break;
                    case OP_store:
                        insn->src1 = insn->src1 + stack_top_ofs;
                        break;
                    default:
                        /* Ignore opcodes with the ofs_based_on_stack_top flag
                         * set since only the three opcodes above needs to
                         * access a variable's address.
                         */
                        break;
                    }
                }
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
    int store_offset;
    bool is_external_call = false;

    /* Prepare this variable to reuse code for:
     * 1. division and modulo operations
     * 2. load and store operations
     * 3. address-of operations
     */
    arm_reg interm;

    switch (ph2_ir->op) {
    case OP_define:
        /* We should handle the function entry point carefully due to the
         * following constraints:
         * - according to AAPCS, the callee must preserve r4-r11 for the caller,
         *   and the stack must always be 8-byte aligned.
         * - lr must be pushed because it may be modified when the function
         *   calls another function.
         * - since external functions may call internal functions, r12 may not
         *   hold the global stack pointer.
         *
         * Therefore, we perform the following operations:
         * 1. use a __stmdb instruction to push r4-r11 and lr onto the stack
         *    first.
         * 2. retrieve the global stack pointer from the 4-byte global object
         *    located at 'elf_data_start' to ensure correct access to the global
         *    stack.
         * 3. set ofs to align(ph2_ir->src0, 8) + 4, and prepare a local
         *    stack for the callee by subtracting ofs from sp.
         */
        emit(__stmdb(__AL, 1, __sp, 0x4FF0));
        emit(__movw(__AL, __r8, elf_data_start));
        emit(__movt(__AL, __r8, elf_data_start));
        emit(__lw(__AL, __r12, __r8, 0));
        ofs = ALIGN_UP(ph2_ir->src0, MIN_ALIGNMENT) + 4;
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
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
        if (ph2_ir->dest_hi >= 0) {
            if (ph2_ir->src1 < 0) {
                emit(__movw(__AL, __r8, -ph2_ir->src1));
                emit(__movt(__AL, __r8, -ph2_ir->src1));
                emit(__rsb_i(__AL, ph2_ir->dest_hi, 0, __r8));
            } else if (ph2_ir->src1 > 255) {
                emit(__movw(__AL, ph2_ir->dest_hi, ph2_ir->src1));
                emit(__movt(__AL, ph2_ir->dest_hi, ph2_ir->src1));
            } else
                emit(__mov_i(__AL, ph2_ir->dest_hi, ph2_ir->src1));
        }
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
        /* update_elf_offset() reserves no space for a self-assignment, so
         * emitting one here would push every later address out by four bytes
         * and leave the data segment's p_offset and p_vaddr incongruent.
         */
        if (rd != rn)
            emit(__mov_r(__AL, rd, rn));
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->dest_hi != ph2_ir->src0_hi)
            emit(__mov_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi));
        return;
    case OP_load:
    case OP_global_load:
        interm = ph2_ir->op == OP_load ? __sp : __r12;
        if (ph2_ir->dest_hi >= 0) {
            if (ph2_ir->src0 > 4095) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, __r8, interm, __r8));
                emit(__lw(__AL, rd, __r8, 0));
                emit(__lw(__AL, ph2_ir->dest_hi, __r8, 4));
            } else {
                emit(__lw(__AL, rd, interm, ph2_ir->src0));
                emit(__lw(__AL, ph2_ir->dest_hi, interm, ph2_ir->src0 + 4));
            }
            return;
        }

        /* LDRSB's immediate field is only eight bits, unlike LDRB's 12-bit
         * field. Materialize a larger signed-byte offset before loading.
         */
        if (ph2_ir->src0 > 4095 ||
            (ph2_ir->size_bytes == 1 && !ph2_ir->is_unsigned &&
             ph2_ir->src0 > 255)) {
            emit(__movw(__AL, __r8, ph2_ir->src0));
            emit(__movt(__AL, __r8, ph2_ir->src0));
            emit(__add_r(__AL, __r8, interm, __r8));
            if (ph2_ir->size_bytes == 1)
                emit(ph2_ir->is_unsigned ? __lb(__AL, rd, __r8, 0)
                                         : __lsb(__AL, rd, __r8, 0));
            else if (ph2_ir->size_bytes == 2)
                emit(ph2_ir->is_unsigned ? __lhu(__AL, rd, __r8, 0)
                                         : __lh(__AL, rd, __r8, 0));
            else
                emit(__lw(__AL, rd, __r8, 0));
        } else if (ph2_ir->size_bytes == 1)
            emit(ph2_ir->is_unsigned ? __lb(__AL, rd, interm, ph2_ir->src0)
                                     : __lsb(__AL, rd, interm, ph2_ir->src0));
        else if (ph2_ir->size_bytes == 2)
            emit(ph2_ir->is_unsigned ? __lhu(__AL, rd, interm, ph2_ir->src0)
                                     : __lh(__AL, rd, interm, ph2_ir->src0));
        else
            emit(__lw(__AL, rd, interm, ph2_ir->src0));
        return;
    case OP_store:
    case OP_global_store:
        interm = ph2_ir->op == OP_store ? __sp : __r12;
        store_offset = ph2_ir->src1;
        if (ph2_ir->src0_hi >= 0) {
            if (store_offset > 4095) {
                emit(__movw(__AL, __r8, store_offset));
                emit(__movt(__AL, __r8, store_offset));
                emit(__add_r(__AL, __r8, interm, __r8));
                emit(__sw(__AL, rn, __r8, 0));
                emit(__sw(__AL, ph2_ir->src0_hi, __r8, 4));
            } else {
                emit(__sw(__AL, rn, interm, store_offset));
                emit(__sw(__AL, ph2_ir->src0_hi, interm, store_offset + 4));
            }
            return;
        }

        /* STRH has an 8-bit split immediate while STR/STRB accept 12 bits.
         * Materialize larger halfword offsets before selecting the width.
         */
        if (store_offset > (ph2_ir->size_bytes == 2 ? 255 : 4095)) {
            emit(__movw(__AL, __r8, store_offset));
            emit(__movt(__AL, __r8, store_offset));
            emit(__add_r(__AL, __r8, interm, __r8));
            interm = __r8;
            store_offset = 0;
        }

        /* Global aggregate initialization can address a byte or halfword field
         * directly. Treating every OP_global_store as a word store clobbers
         * neighbouring designated members on ARM. Ordinary stack slots remain
         * word-sized unless their IR says otherwise.
         */
        if (ph2_ir->size_bytes == 1)
            emit(__sb(__AL, rn, interm, store_offset));
        else if (ph2_ir->size_bytes == 2)
            emit(__sh(__AL, rn, interm, store_offset));
        else
            emit(__sw(__AL, rn, interm, store_offset));
        return;
    case OP_read:
        if (ph2_ir->src1 == 1)
            emit(ph2_ir->is_unsigned ? __lb(__AL, rd, rn, 0)
                                     : __lsb(__AL, rd, rn, 0));
        else if (ph2_ir->src1 == 2)
            emit(ph2_ir->is_unsigned ? __lhu(__AL, rd, rn, 0)
                                     : __lh(__AL, rd, rn, 0));
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
                  (elf_code_start + elf_code->size);
            is_external_call = true;
        } else {
            printf("The '%s' function is not implemented\n", ph2_ir->func_name);
            fflush(stdout); /* see fatal() */
            abort();
        }

        /* When calling external functions in dynamic linking mode, the
         * following instructions are required:
         * - movw + movt: set r8 to 'elf_data_start'
         * - ldr: load a word from the address 'elf_data_start' to r12.
         *        (restore the global stack pointer.)
         *
         * Since shecc uses r12 to store a global stack pointer and external
         * functions can freely modify r12, causing internal functions to access
         * global variables incorrectly, additional instructions are needed to
         * restore r12 from the global object after the external function
         * returns.
         *
         * Otherwise, only a 'bl' instruction is generated to call internal
         * functions because shecc guarantees they do not modify r12.
         */
        emit(__bl(__AL, ofs));
        if (is_external_call) {
            emit(__movw(__AL, __r8, elf_data_start));
            emit(__movt(__AL, __r8, elf_data_start));
            emit(__lw(__AL, __r12, __r8, 0));
        }
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
            fflush(stdout); /* see fatal() */
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
        if (ph2_ir->src0_hi >= 0)
            emit(__mov_r(__AL, __r1, ph2_ir->src0_hi));

        /* When calling a function, the following operations are performed:
         * 1. push r4-r11 and lr onto the stack.
         * 2. retrieve the global stack pointer from the 4-byte global object.
         * 3. decrement the stack by ALIGN_UP(stack_size, 8) + 4.
         *
         * Except for step 2, the reversed operations should be performed to
         * upon returning to restore the stack and the contents of r4-r11 and
         * lr.
         */
        ofs = ALIGN_UP(ph2_ir->src1, MIN_ALIGNMENT) + 4;
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
        emit(__add_r(__AL, __sp, __sp, __r8));
        emit(__ldm(__AL, 1, __sp, 0x4FF0));
        emit(__bx(__AL, __lr));
        return;
    case OP_add:
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0) {
            emit(__adds_r(__AL, rd, rn, rm));
            emit(__adc_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi,
                         ph2_ir->src1_hi));
        } else
            emit(__add_r(__AL, rd, rn, rm));
        return;
    case OP_sub:
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0) {
            emit(__subs_r(__AL, rd, rn, rm));
            emit(__sbc_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi,
                         ph2_ir->src1_hi));
        } else
            emit(__sub_r(__AL, rd, rn, rm));
        return;
    case OP_mul:
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0) {
            emit(__umull(__AL, rd, ph2_ir->dest_hi, rn, rm));
            emit(__mul(__AL, __r8, rn, ph2_ir->src1_hi));
            emit(__mul(__AL, __r9, ph2_ir->src0_hi, rm));
            emit(__add_r(__AL, ph2_ir->dest_hi, ph2_ir->dest_hi, __r8));
            emit(__add_r(__AL, ph2_ir->dest_hi, ph2_ir->dest_hi, __r9));
        } else
            emit(__mul(__AL, rd, rn, rm));
        return;
    case OP_div:
    case OP_mod:
        if (hard_mul_div) {
            if (ph2_ir->op == OP_div)
                emit(ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned
                         ? __udiv(__AL, rd, rm, rn)
                         : __div(__AL, rd, rm, rn));
            else {
                emit(ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned
                         ? __udiv(__AL, __r8, rm, rn)
                         : __div(__AL, __r8, rm, rn));
                emit(__mul(__AL, __r8, rm, __r8));
                emit(__sub_r(__AL, rd, rn, __r8));
            }
            return;
        }
        interm = __r8;
        /* div/mod emulation: preserve the dividend and the divisor */
        emit(__stmdb(__AL, 1, __sp, (1 << rn) | (1 << rm)));

        /* Unsigned operands already are magnitudes. Keep the signed path's
         * instruction count so the fixed branch displacements remain valid.
         */
        if (ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned) {
            emit(__zero(__r8));
            emit(__mov_r(__AL, __r8, __r8));
            emit(__mov_r(__AL, __r8, __r8));
            emit(__zero(__r9));
            emit(__mov_r(__AL, __r9, __r9));
            emit(__mov_r(__AL, __r9, __r9));
        } else {
            emit(__srl_amt(__AL, 0, arith_rs, __r8, rn, 31));
            emit(__add_r(__AL, rn, rn, __r8));
            emit(__eor_r(__AL, rn, rn, __r8));
            emit(__srl_amt(__AL, 0, arith_rs, __r9, rm, 31));
            emit(__add_r(__AL, rm, rm, __r9));
            emit(__eor_r(__AL, rm, rm, __r9));
        }
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

        /* Scale until the divisor reaches the dividend or the *next* shift
         * would overflow. A divisor such as 0xc0000000 is still valid for a
         * 0xffffffff dividend; testing carry after shifting would lose it. Test
         * bit 31 before the shift instead.
         */
        emit(__cmp_i(__CC, rm, 0x80000000));
        emit(__b(__CS, 16));
        emit(__sll_amt(__AL, 0, logic_ls, rm, rm, 1));
        emit(__sll_amt(__AL, 0, logic_ls, __r9, __r9, 1));
        emit(__b(__AL, -20));
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
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0) {
            emit(__cmp_i(__AL, rm, 32));
            emit(__b(__CS, 32));
            emit(__mov_r(__AL, __r8, rn));
            emit(__sll(__AL, rd, rn, rm));
            emit(__sll(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi, rm));
            emit(__rsb_i(__AL, __r9, 32, rm));
            emit(__srl(__AL, __r8, __r8, __r9));
            emit(__or_r(__AL, ph2_ir->dest_hi, ph2_ir->dest_hi, __r8));
            emit(__b(__AL, 16));
            emit(__add_i(__AL, __r9, rm, -32));
            emit(__sll(__AL, ph2_ir->dest_hi, rn, __r9));
            emit(__zero(rd));
            return;
        }
        emit(__sll(__AL, rd, rn, rm));
        return;
    case OP_rshift:
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0) {
            int shift_kind = ph2_ir->src0_is_unsigned ? logic_rs : arith_rs;

            emit(__cmp_i(__AL, rm, 32));
            emit(__b(__CS, 32));
            emit(__mov_r(__AL, __r8, ph2_ir->src0_hi));
            emit(shift_kind == logic_rs ? __srl(__AL, rd, rn, rm)
                                        : __sra(__AL, rd, rn, rm));
            emit(__rsb_i(__AL, __r9, 32, rm));
            emit(__sll(__AL, __r8, __r8, __r9));
            emit(__or_r(__AL, rd, rd, __r8));
            emit(shift_kind == logic_rs
                     ? __srl(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi, rm)
                     : __sra(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi, rm));
            emit(__b(__AL, 16));
            emit(__add_i(__AL, __r9, rm, -32));
            emit(shift_kind == logic_rs
                     ? __srl(__AL, rd, ph2_ir->src0_hi, __r9)
                     : __sra(__AL, rd, ph2_ir->src0_hi, __r9));
            if (shift_kind == logic_rs)
                emit(__zero(ph2_ir->dest_hi));
            else
                emit(__srl_amt(__AL, 0, arith_rs, ph2_ir->dest_hi,
                               ph2_ir->src0_hi, 31));
            return;
        }
        emit(ph2_ir->src0_is_unsigned ? __srl(__AL, rd, rn, rm)
                                      : __sra(__AL, rd, rn, rm));
        return;
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
        if (ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0) {
            bool unsigned_cmp =
                ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned;
            arm_cond_t true_cond = arm_get_cond(ph2_ir->op, unsigned_cmp);
            arm_cond_t false_cond;

            switch (ph2_ir->op) {
            case OP_lt:
                false_cond = unsigned_cmp ? __CS : __GE;
                emit(__zero(rd));
                break;
            case OP_gt:
                false_cond = unsigned_cmp ? __LS : __LE;
                emit(__zero(rd));
                break;
            case OP_geq:
                false_cond = unsigned_cmp ? __CC : __LT;
                emit(__mov_i(__AL, rd, 1));
                break;
            default: /* OP_leq */
                false_cond = unsigned_cmp ? __HI : __GT;
                emit(__mov_i(__AL, rd, 1));
                break;
            }
            emit(__cmp_r(__AL, ph2_ir->src0_hi, ph2_ir->src1_hi));
            emit(__mov_i(true_cond, rd, 1));
            emit(__mov_i(false_cond, rd, 0));
            emit(__cmp_r(__EQ, rn, rm));
            if (ph2_ir->op == OP_geq || ph2_ir->op == OP_leq)
                emit(__mov_i(__EQ, rd, 0));
            emit(__mov_i(true_cond, rd, 1));
            return;
        }
        emit(__cmp_r(__AL, rn, rm));
        if ((ph2_ir->op == OP_eq || ph2_ir->op == OP_neq) &&
            ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0)
            emit(__cmp_r(__EQ, ph2_ir->src0_hi, ph2_ir->src1_hi));
        emit(__zero(rd));
        emit(__mov_i(arm_get_cond(ph2_ir->op, ph2_ir->src0_is_unsigned ||
                                                  ph2_ir->src1_is_unsigned),
                     rd, 1));
        return;
    case OP_negate:
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0) {
            emit(__rsbs_i(__AL, rd, 0, rn));
            emit(__rsc_i(__AL, ph2_ir->dest_hi, 0, ph2_ir->src0_hi));
        } else
            emit(__rsb_i(__AL, rd, 0, rn));
        return;
    case OP_bit_not:
        emit(__mvn_r(__AL, rd, rn));
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0)
            emit(__mvn_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi));
        return;
    case OP_bit_and:
        emit(__and_r(__AL, rd, rn, rm));
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0)
            emit(__and_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi,
                         ph2_ir->src1_hi));
        return;
    case OP_bit_or:
        emit(__or_r(__AL, rd, rn, rm));
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0)
            emit(__or_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi,
                        ph2_ir->src1_hi));
        return;
    case OP_bit_xor:
        emit(__eor_r(__AL, rd, rn, rm));
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi >= 0 &&
            ph2_ir->src1_hi >= 0)
            emit(__eor_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi,
                         ph2_ir->src1_hi));
        return;
    case OP_log_not:
        emit(__cmp_i(__AL, rn, 0));
        emit(__mov_i(__NE, rd, 0));
        emit(__mov_i(__EQ, rd, 1));
        return;
    case OP_trunc:
        if (ph2_ir->is_unsigned && (rm == 1 || rm == 2)) {
            int shift = rm == 1 ? 24 : 16;

            emit(__sll_amt(__AL, 0, logic_ls, rd, rn, shift));
            emit(__srl_amt(__AL, 0, logic_rs, rd, rd, shift));
        } else if (rm == 1) {
            emit(__sxtb(__AL, rd, rn, 0));
        } else if (rm == 2) {
            emit(__sxth(__AL, rd, rn, 0));
        } else if (rm == 4) {
            emit(__mov_r(__AL, rd, rn));
        } else {
            fatal("Unsupported truncation operation with invalid target size");
        }
        return;
    case OP_sign_ext: {
        /* Decode source size from upper 16 bits */
        int source_size = (rm >> 16) & 0xFFFF;
        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi < 0) {
            if (source_size == 1) {
                if (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer) {
                    emit(__sll_amt(__AL, 0, logic_ls, rd, rn, 24));
                    emit(__srl_amt(__AL, 0, logic_rs, rd, rd, 24));
                } else
                    emit(__sxtb(__AL, rd, rn, 0));
            } else if (source_size == 2) {
                if (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer) {
                    emit(__sll_amt(__AL, 0, logic_ls, rd, rn, 16));
                    emit(__srl_amt(__AL, 0, logic_rs, rd, rd, 16));
                } else
                    emit(__sxth(__AL, rd, rn, 0));
            } else
                emit(__mov_r(__AL, rd, rn));
            if (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer)
                emit(__zero(ph2_ir->dest_hi));
            else
                emit(__srl_amt(__AL, 0, arith_rs, ph2_ir->dest_hi, rd, 31));
            return;
        }
        if (ph2_ir->src0_is_unsigned) {
            int shift = source_size == 2 ? 16 : 24;
            emit(__sll_amt(__AL, 0, logic_ls, rd, rn, shift));
            emit(__srl_amt(__AL, 0, logic_rs, rd, rd, shift));
            return;
        }
        if (source_size == 2) {
            emit(__sxth(__AL, rd, rn, 0));
        } else {
            /* For other cases, use byte extension (original behavior) */
            emit(__sxtb(__AL, rd, rn, 0));
        }
    }
        return;
    case OP_cast:
        /* A 32-bit source widened to a direct wide scalar needs an explicit
         * high word. Copy an existing pair unchanged, otherwise sign- or
         * zero-extend the source according to its original type.
         */
        emit(__mov_r(__AL, rd, rn));
        if (ph2_ir->dest_hi >= 0) {
            if (ph2_ir->src0_hi >= 0) {
                if (ph2_ir->dest_hi != ph2_ir->src0_hi)
                    emit(__mov_r(__AL, ph2_ir->dest_hi, ph2_ir->src0_hi));
            } else if (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer)
                emit(__zero(ph2_ir->dest_hi));
            else
                emit(__srl_amt(__AL, 0, arith_rs, ph2_ir->dest_hi, rn, 31));
        }
        return;
    default:
        fatal("Unknown opcode");
    }
}

void plt_generate(void);
void code_generate(void)
{
    int ofs;

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
         * returns.
         */
        emit(__mov_i(__AL, __r0, 127));
        emit(__mov_i(__AL, __r7, 1));
        emit(__svc());

        /* If the compiled program is dynamic linking, the starting point of
         * 'main_wrapper' is located here.
         *
         * Push the contents of r4-r11 and lr onto stack. Preserve 'argc' and
         * 'argv' for the 'main' function.
         */
        emit(__stmdb(__AL, 1, __sp, 0x4FF0));
        emit(__mov_r(__AL, __r9, __r0));
        emit(__mov_r(__AL, __r10, __r1));
    }

    /* For both static and dynamic linking, we need to set up the stack and call
     * the main function.
     *
     * To ensure that the stack remains 8-byte aligned after adjustment, 'ofs'
     * is to align(GLOBAL_FUNC->stack_size, 8) to allocate space for the global
     * stack.
     *
     * In dynamic linking mode, since the preceding __stmdb instruction pushes 9
     * registers onto stack, 'ofs' must be increased by 4 to prevent the stack
     * from becoming misaligned.
     */
    ofs = ALIGN_UP(GLOBAL_FUNC->stack_size, MIN_ALIGNMENT);
    if (dynlink)
        ofs += 4;
    emit(__movw(__AL, __r8, ofs));
    emit(__movt(__AL, __r8, ofs));
    emit(__sub_r(__AL, __sp, __sp, __r8));
    emit(__mov_r(__AL, __r12, __sp));

    /* The first object in the .data section is used to store the global stack
     * pointer. Therefore, store r12 at the address 'elf_data_start' after the
     * global stack has been prepared.
     */
    emit(__movw(__AL, __r8, elf_data_start));
    emit(__movt(__AL, __r8, elf_data_start));
    emit(__sw(__AL, __r12, __r8, 0));

    if (!dynlink) {
        /* Jump directly to the main preparation and then execute the main
         * function.
         *
         * In static linking mode, when the main function completes its
         * execution, it will invoke the '_exit' syscall to terminate the
         * program.
         *
         * That is, the execution flow is:
         *
         *               +------------------+
         *               | movw r8 <ofs>    |
         * 'start'       | ...              |
         *               | b <global init>  | (1) jump to global init --+
         *               +------------------+                           |
         *               | push {r4 ... r7} |                           |
         * '__syscall'   | ...              |                           |
         *               | bx lr            |                           |
         *               +------------------+                           |
         *               | ...              | (2) global init    <------+
         *               | (global init)    |
         *               | ...              |
         * global init   | movw r8 <ofs>    |
         *     +         | movt r8 <ofs>    |
         * call main()   | ...              |
         *               | bl <main func>   | (3) call main()
         *               | mov r7 #1        |
         *               | svc 0x00000000   | (4) call '_exit' after main()
         *               +------------------+     returns
         */
        emit(__b(__AL, GLOBAL_FUNC->bbs->elf_offset - elf_code->size));

        /* __syscall - only for static linking
         *
         * If the number of arguments is greater than 4, the additional
         * arguments need to be retrieved from the stack. However, this process
         * must modify the contents of registers r4-r7.
         *
         * Therefore, __syscall needs to preserve the contents of these
         * registers before invoking a syscall, and restore them after the
         * syscall has completed.
         */
        emit(__stmdb(__AL, 1, __sp, 0x00F0));
        emit(__lw(__AL, __r4, __sp, 16));
        emit(__lw(__AL, __r5, __sp, 20));
        emit(__lw(__AL, __r6, __sp, 24));
        emit(__lw(__AL, __r7, __sp, 28));
        emit(__mov_r(__AL, __r7, __r0));
        emit(__mov_r(__AL, __r0, __r1));
        emit(__mov_r(__AL, __r1, __r2));
        emit(__mov_r(__AL, __r2, __r3));
        emit(__mov_r(__AL, __r3, __r4));
        emit(__mov_r(__AL, __r4, __r5));
        emit(__mov_r(__AL, __r5, __r6));
        emit(__svc());
        emit(__ldm(__AL, 1, __sp, 0x00F0));
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
             * After the main function returns, the following instructions
             * restore the registers r4-r11 and return control to
             * __libc_start_main via the preserved lr.
             */
            emit(__bl(__AL, MAIN_BB->elf_offset - elf_code->size));
            emit(__movw(__AL, __r8, ofs));
            emit(__movt(__AL, __r8, ofs));
            emit(__add_r(__AL, __sp, __sp, __r8));
            emit(__ldm(__AL, 1, __sp, 0x8FF0));
        } else {
            emit(__movw(__AL, __r8, ofs));
            emit(__movt(__AL, __r8, ofs));
            emit(__add_r(__AL, __r8, __r12, __r8));
            emit(__lw(__AL, __r0, __r8, 0));
            emit(__add_i(__AL, __r1, __r8, 4));

            /* Call main function, and call '_exit' syscall to terminate the
             * program.
             */
            emit(__bl(__AL, MAIN_BB->elf_offset - elf_code->size));

            /* exit with main's return value - r0 already has the return value
             */
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
    /* - PLT code generation explanation -
     *
     * As described in ARM's Platform Standard, PLT code should make register ip
     * address the corresponding GOT entry on SVr4-like (Linux-like) platforms.
     *
     * Therefore, PLT[1] ~ PLT[N] use r12 (ip) to load the address of the GOT
     * entry and jump to the function entry via the GOT value.
     *
     * PLT[0] is used to call the resolver, which requires:
     * - [sp] contains the return address from the original function call.
     * - ip contains the address of the GOT entry.
     * - lr points to the address of GOT[2].
     *
     * The second requirement is alreadly handled by PLT[1] - PLT[N], so PLT[0]
     * must take care of the other two. The first one can be achieved by a
     * 'push' instruction; for the third, we use r10 to store the address of
     * GOT[2] and then move the value to lr.
     *
     * - Reason for using r10 in PLT[0] -
     *
     * The register allocation assumes 8 available registers, so the ARM code
     * generator primarily uses r0-r7 for code generation. These registers
     * cannot be modified arbitrarily; otherwise, the program may fail if any of
     * them are changed by PLT[0].
     *
     * However, r8-r11 can be freely used as temporary registers during code
     * generation, so PLT[0] arbitrarily chooses r10 to perform the required
     * operation.
     */
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
