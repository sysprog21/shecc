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

/* Whether a load or store materializes its frame or data offset with
 * MOVW/MOVT/ADD because no immediate form reaches it. LDR, LDRB, STR and STRB
 * take 12 bits, LDRSB, LDRH, LDRSH and STRH eight, and a register pair also
 * addresses its high word four bytes further up. The size estimator and the
 * emitter both ask this, so the two cannot disagree.
 */
bool arm_offset_needs_movw(const vir_mop_t *mop)
{
    bool load = mop->op == OP_load || mop->op == OP_global_load;
    int offset = load ? mop->src0 : mop->src1;
    bool eight_bits = mop->size_bytes == 2 ||
                      (load && mop->size_bytes == 1 && !mop->is_unsigned);

    if ((load ? mop->dest_hi : mop->src0_hi) >= 0)
        return offset > 4091;
    return offset > (eight_bits ? 255 : 4095);
}

void update_elf_offset(vir_mop_t *mop)
{
    elf_offset += mop_count_words(mop) * 4;
}

static mop_frame_layout_t arm_frame_layout(func_t *func, vir_mop_t *define)
{
    /* Eight-byte alignment follows the nine saved words and one padding word.
     */
    return (mop_frame_layout_t) {ALIGN_UP(func->stack_size, MIN_ALIGNMENT) + 40,
                                 mop_count_words(define) * 4, 0, false};
}

/* Mark the edges that fall into the block laid out next, before offsets are
 * planned: a jump there emits nothing, and a branch tests for one edge alone. A
 * branch keeps 1 when its else block follows, 2 when its then block does, and 0
 * when neither does, so both edges need a branch.
 */
static void arm_mark_fallthrough(void)
{
    FOR_EACH_FUNCTION_BODY(func)
    {
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (vir_mop_t *mop = bb->mops.head; mop; mop = mop->next) {
                if (mop->op == OP_branch)
                    mop->src2 = mop->else_bb == bb->rpo_next   ? 1
                                : mop->then_bb == bb->rpo_next ? 2
                                                               : 0;
                else if (mop->op == OP_jump && !mop->next)
                    mop->src2 = mop->next_bb == bb->rpo_next;
            }
        }
    }
}

void cfg_flatten(void)
{
    func_t *func;
    mop_flat_prepare(false);
    arm_mark_fallthrough();

    if (dynlink)
        elf_offset = 132; /* __libc_start_main call, main_wrapper, and the
                           * global stack clearing loop in codegen
                           */
    else {
        func = find_func("__syscall");
        func->bbs->elf_offset = 32; /* offset of start + branch in codegen */
        elf_offset = 92; /* offset of start + branch + syscall in codegen */
    }

    GLOBAL_FUNC->bbs->elf_offset = elf_offset;

    mop_visit_globals(update_elf_offset);

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    if (dynlink)
        elf_offset += 28;
    else
        elf_offset += 32; /* 6 insns for main call + 2 for exit */

    mop_flatten_functions(arm_frame_layout, update_elf_offset);
}

void emit(int code)
{
    if (mop_count_only)
        ++mop_counted_words;
    else
        elf_write_int(elf_code, code);
}

static int arm_bitwise_insn(opcode_t op,
                            arm_reg dest,
                            arm_reg left,
                            arm_reg right)
{
    switch (op) {
    case OP_bit_and:
        return __and_r(__AL, dest, left, right);
    case OP_bit_or:
        return __or_r(__AL, dest, left, right);
    case OP_bit_xor:
        return __eor_r(__AL, dest, left, right);
    default:
        fatal("unsupported ARM bitwise operation");
    }
    return 0;
}

void emit_mop(vir_mop_t *mop)
{
    func_t *func;
    const int rd = mop->dest;
    const int rn = mop->src0;
    int rm = mop->src1; /* Not const because OP_trunc modifies it */
    int ofs;
    int store_offset;
    bool is_external_call = false;

    /* Prepare this variable to reuse code for:
     * 1. division and modulo operations
     * 2. load and store operations
     * 3. address-of operations
     */
    arm_reg interm;

    switch (mop->op) {
    case OP_define:
        fatal_function_context = mop->func_name;

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
         * 3. set ofs to align(mop->src0, 8) + 4, and prepare a local
         *    stack for the callee by subtracting ofs from sp.
         */
        emit(__stmdb(__AL, 1, __sp, 0x4FF0));
        emit(__movw(__AL, __r8, elf_data_start));
        emit(__movt(__AL, __r8, elf_data_start));
        emit(__lw(__AL, __r12, __r8, 0));
        ofs = ALIGN_UP(mop->src0, MIN_ALIGNMENT) + 4;
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
        emit(__sub_r(__AL, __sp, __sp, __r8));
        return;
    case OP_load_constant:
        if (mop->src0 < 0) {
            emit(__movw(__AL, __r8, -mop->src0));
            emit(__movt(__AL, __r8, -mop->src0));
            emit(__rsb_i(__AL, rd, 0, __r8));
        } else if (mop->src0 > 255) {
            emit(__movw(__AL, rd, mop->src0));
            emit(__movt(__AL, rd, mop->src0));
        } else
            emit(__mov_i(__AL, rd, mop->src0));
        if (mop->dest_hi >= 0) {
            if (mop->src1 < 0) {
                emit(__movw(__AL, __r8, -mop->src1));
                emit(__movt(__AL, __r8, -mop->src1));
                emit(__rsb_i(__AL, mop->dest_hi, 0, __r8));
            } else if (mop->src1 > 255) {
                emit(__movw(__AL, mop->dest_hi, mop->src1));
                emit(__movt(__AL, mop->dest_hi, mop->src1));
            } else
                emit(__mov_i(__AL, mop->dest_hi, mop->src1));
        }
        return;
    case OP_address_of:
    case OP_global_address_of:
        interm = mop->op == OP_address_of ? __sp : __r12;
        if (mop->src0 > 255) {
            emit(__movw(__AL, __r8, mop->src0));
            emit(__movt(__AL, __r8, mop->src0));
            emit(__add_r(__AL, rd, interm, __r8));
        } else
            emit(__add_i(__AL, rd, interm, mop->src0));
        return;
    case OP_assign:
        /* update_elf_offset() reserves no space for a self-assignment, so
         * emitting one here would push every later address out by four bytes
         * and leave the data segment's p_offset and p_vaddr incongruent.
         */
        if (MOP_PAIR_DEST_SRC0(mop) && rd == mop->src0_hi) {
            /* A pair moved one register over, as a slot round trip collapsed
             * into a move can leave it: the low destination is the high source,
             * so move the high word first, through r8 when the two registers
             * trade places.
             */
            if (mop->dest_hi == rn) {
                emit(__mov_r(__AL, __r8, rn));
                emit(__mov_r(__AL, rd, mop->src0_hi));
                emit(__mov_r(__AL, mop->dest_hi, __r8));
            } else {
                emit(__mov_r(__AL, mop->dest_hi, mop->src0_hi));
                emit(__mov_r(__AL, rd, rn));
            }
            return;
        }
        if (rd != rn)
            emit(__mov_r(__AL, rd, rn));
        if (MOP_PAIR_DEST_SRC0(mop) && mop->dest_hi != mop->src0_hi)
            emit(__mov_r(__AL, mop->dest_hi, mop->src0_hi));
        return;
    case OP_load:
    case OP_global_load:
        interm = mop->op == OP_load ? __sp : __r12;
        if (mop->dest_hi >= 0) {
            if (arm_offset_needs_movw(mop)) {
                emit(__movw(__AL, __r8, mop->src0));
                emit(__movt(__AL, __r8, mop->src0));
                emit(__add_r(__AL, __r8, interm, __r8));
                emit(__lw(__AL, rd, __r8, 0));
                emit(__lw(__AL, mop->dest_hi, __r8, 4));
            } else {
                emit(__lw(__AL, rd, interm, mop->src0));
                emit(__lw(__AL, mop->dest_hi, interm, mop->src0 + 4));
            }
            return;
        }

        /* LDRSB, LDRH and LDRSH have only eight immediate bits, unlike LDRB's
         * 12-bit field. Materialize a larger offset before loading.
         */
        if (arm_offset_needs_movw(mop)) {
            emit(__movw(__AL, __r8, mop->src0));
            emit(__movt(__AL, __r8, mop->src0));
            emit(__add_r(__AL, __r8, interm, __r8));
            if (mop->size_bytes == 1)
                emit(mop->is_unsigned ? __lb(__AL, rd, __r8, 0)
                                      : __lsb(__AL, rd, __r8, 0));
            else if (mop->size_bytes == 2)
                emit(mop->is_unsigned ? __lhu(__AL, rd, __r8, 0)
                                      : __lh(__AL, rd, __r8, 0));
            else
                emit(__lw(__AL, rd, __r8, 0));
        } else if (mop->size_bytes == 1)
            emit(mop->is_unsigned ? __lb(__AL, rd, interm, mop->src0)
                                  : __lsb(__AL, rd, interm, mop->src0));
        else if (mop->size_bytes == 2)
            emit(mop->is_unsigned ? __lhu(__AL, rd, interm, mop->src0)
                                  : __lh(__AL, rd, interm, mop->src0));
        else
            emit(__lw(__AL, rd, interm, mop->src0));
        return;
    case OP_store:
    case OP_global_store:
        interm = mop->op == OP_store ? __sp : __r12;
        store_offset = mop->src1;
        if (mop->src0_hi >= 0) {
            if (arm_offset_needs_movw(mop)) {
                emit(__movw(__AL, __r8, store_offset));
                emit(__movt(__AL, __r8, store_offset));
                emit(__add_r(__AL, __r8, interm, __r8));
                emit(__sw(__AL, rn, __r8, 0));
                emit(__sw(__AL, mop->src0_hi, __r8, 4));
            } else {
                emit(__sw(__AL, rn, interm, store_offset));
                emit(__sw(__AL, mop->src0_hi, interm, store_offset + 4));
            }
            return;
        }

        /* STRH has an 8-bit split immediate while STR/STRB accept 12 bits.
         * Materialize larger halfword offsets before selecting the width.
         */
        if (arm_offset_needs_movw(mop)) {
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
        if (mop->size_bytes == 1)
            emit(__sb(__AL, rn, interm, store_offset));
        else if (mop->size_bytes == 2)
            emit(__sh(__AL, rn, interm, store_offset));
        else
            emit(__sw(__AL, rn, interm, store_offset));
        return;
    case OP_read:
        if (mop->dest_hi >= 0) {
            if (mop->src1 != 8)
                fatal("unsupported Arm pair load width");
#define ARM_WORD_LOAD(dst, base, offset) emit(__lw(__AL, dst, base, offset))
            EMIT_PAIR_LOAD(ARM_WORD_LOAD, rd, mop->dest_hi, rn);
#undef ARM_WORD_LOAD
            return;
        }
        if (mop->src1 == 1)
            emit(mop->is_unsigned ? __lb(__AL, rd, rn, 0)
                                  : __lsb(__AL, rd, rn, 0));
        else if (mop->src1 == 2)
            emit(mop->is_unsigned ? __lhu(__AL, rd, rn, 0)
                                  : __lh(__AL, rd, rn, 0));
        else if (mop->src1 == 4)
            emit(__lw(__AL, rd, rn, 0));
        else
            fatal("unsupported Arm load width");
        return;
    case OP_write:
        if (mop->src1_hi >= 0) {
            if (mop->dest != 8)
                fatal("unsupported Arm pair store width");
            emit(__sw(__AL, rm, rn, 0));
            emit(__sw(__AL, mop->src1_hi, rn, 4));
            return;
        }
        if (mop->dest == 1)
            emit(__sb(__AL, rm, rn, 0));
        else if (mop->dest == 2)
            emit(__sh(__AL, rm, rn, 0));
        else if (mop->dest == 4)
            emit(__sw(__AL, rm, rn, 0));
        else
            fatal("unsupported Arm store width");
        return;
    case OP_branch:
        /* A pair is true when either word is nonzero. */
        if (mop->src0_hi >= 0) {
            emit(__or_r(__AL, __r8, rn, mop->src0_hi));
            emit(__teq(__r8));
        } else
            emit(__teq(rn));
        if (mop->src2 == 2)
            /* The then block follows: only the else edge needs a branch. */
            emit(__b(__EQ, mop->else_bb->elf_offset - elf_code->size));
        else if (!mop->src2) {
            /* The else block does not follow, and nothing says the then block
             * does either: a loop's back edge lands behind this one. Jump to
             * both explicitly rather than skipping over the else jump into
             * whatever was laid out next.
             */
            emit(__b(__NE, mop->then_bb->elf_offset - elf_code->size));
            emit(__b(__AL, mop->else_bb->elf_offset - elf_code->size));
        } else
            emit(__b(__NE, mop->then_bb->elf_offset - elf_code->size));
        return;
    case OP_jump:
        if (!mop->src2)
            emit(__b(__AL, mop->next_bb->elf_offset - elf_code->size));
        return;
    case OP_call:
        func = find_func(mop->func_name);
        if (func->bbs)
            ofs = func->bbs->elf_offset - elf_code->size;
        else if (dynlink) {
            ofs = (dynamic_sections.elf_plt_start + func->plt_offset) -
                  (elf_code_start + elf_code->size);
            is_external_call = true;
        } else {
            printf("The '%s' function is not implemented\n", mop->func_name);
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
        emit(__movw(__AL, rd, mop->src0 + elf_data_start));
        emit(__movt(__AL, rd, mop->src0 + elf_data_start));
        return;
    case OP_load_rodata_address:
        emit(__movw(__AL, rd, mop->src0 + elf_rodata_start));
        emit(__movt(__AL, rd, mop->src0 + elf_rodata_start));
        return;
    case OP_address_of_func:
        ofs = function_entry_address(mop->func_name);
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
        emit(__sw(__AL, __r8, rn, 0));
        return;
    case OP_load_func:
        emit(__mov_r(__AL, __r8, rn));
        return;
    case OP_indirect:
        emit(__blx(__AL, __r8));

        /* The pointer may name a libc function, reached through its PLT entry,
         * which loads the GOT slot through r12. Restore the global stack
         * pointer as OP_call does after an external call.
         */
        if (dynlink) {
            emit(__movw(__AL, __r8, elf_data_start));
            emit(__movt(__AL, __r8, elf_data_start));
            emit(__lw(__AL, __r12, __r8, 0));
        }
        return;
    case OP_return:
        if (mop->src0 == -1)
            emit(__mov_r(__AL, __r0, __r0));
        else
            emit(__mov_r(__AL, __r0, rn));
        if (mop->src0_hi >= 0)
            emit(__mov_r(__AL, __r1, mop->src0_hi));

        /* When calling a function, the following operations are performed:
         * 1. push r4-r11 and lr onto the stack.
         * 2. retrieve the global stack pointer from the 4-byte global object.
         * 3. decrement the stack by ALIGN_UP(stack_size, 8) + 4.
         *
         * Except for step 2, the reversed operations should be performed to
         * upon returning to restore the stack and the contents of r4-r11 and
         * lr.
         */
        ofs = ALIGN_UP(mop->src1, MIN_ALIGNMENT) + 4;
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
        emit(__add_r(__AL, __sp, __sp, __r8));
        emit(__ldm(__AL, 1, __sp, 0x4FF0));
        emit(__bx(__AL, __lr));
        return;
    case OP_add:
        if (MOP_PAIR_BINARY(mop)) {
            emit(__adds_r(__AL, rd, rn, rm));
            emit(__adc_r(__AL, mop->dest_hi, mop->src0_hi, mop->src1_hi));
        } else
            emit(__add_r(__AL, rd, rn, rm));
        return;
    case OP_sub:
        if (MOP_PAIR_BINARY(mop)) {
            emit(__subs_r(__AL, rd, rn, rm));
            emit(__sbc_r(__AL, mop->dest_hi, mop->src0_hi, mop->src1_hi));
        } else
            emit(__sub_r(__AL, rd, rn, rm));
        return;
    case OP_mul:
        if (MOP_PAIR_BINARY(mop)) {
            /* Sum the cross terms before UMULL writes the result pair, so the
             * product stays right even if that pair shares a register with an
             * operand. The allocator does not hand out such a pair today, and
             * this order costs nothing over the other.
             */
            emit(__mul(__AL, __r8, rn, mop->src1_hi));
            emit(__mul(__AL, __r9, mop->src0_hi, rm));
            emit(__add_r(__AL, __r8, __r8, __r9));
            emit(__umull(__AL, rd, mop->dest_hi, rn, rm));
            emit(__add_r(__AL, mop->dest_hi, mop->dest_hi, __r8));
        } else
            emit(__mul(__AL, rd, rn, rm));
        return;
    case OP_div:
    case OP_mod:
        if (MOP_PAIR_BINARY(mop)) {
            bool is_unsigned = mop->src0_is_unsigned || mop->src1_is_unsigned;

            /* Stack words: dividend (0,4), divisor (8,12), remainder (16,20),
             * quotient (24,28), counter (32), and the incoming dividend bit
             * (36). Signed pairs use (40,44) for sign masks. r8-r10 are codegen
             * scratch registers.
             */
            emit(__add_i(__AL, __sp, __sp, is_unsigned ? -40 : -48));
            emit(__sw(__AL, rn, __sp, 0));
            emit(__sw(__AL, mop->src0_hi, __sp, 4));
            emit(__sw(__AL, rm, __sp, 8));
            emit(__sw(__AL, mop->src1_hi, __sp, 12));
            if (!is_unsigned) {
                /* x ^ sign + -sign produces its unsigned magnitude. */
                emit(__lw(__AL, __r8, __sp, 4));
                emit(__srl_amt(__AL, 0, arith_rs, __r10, __r8, 31));
                emit(__sw(__AL, __r10, __sp, 40));
                emit(__lw(__AL, __r8, __sp, 0));
                emit(__eor_r(__AL, __r8, __r8, __r10));
                emit(__rsbs_i(__AL, __r9, 0, __r10));
                emit(__adds_r(__AL, __r8, __r8, __r9));
                emit(__sw(__AL, __r8, __sp, 0));
                emit(__lw(__AL, __r8, __sp, 4));
                emit(__eor_r(__AL, __r8, __r8, __r10));
                emit(__zero(__r9));
                emit(__adc_r(__AL, __r8, __r8, __r9));
                emit(__sw(__AL, __r8, __sp, 4));
                emit(__lw(__AL, __r8, __sp, 12));
                emit(__srl_amt(__AL, 0, arith_rs, __r10, __r8, 31));
                emit(__sw(__AL, __r10, __sp, 44));
                emit(__lw(__AL, __r8, __sp, 8));
                emit(__eor_r(__AL, __r8, __r8, __r10));
                emit(__rsbs_i(__AL, __r9, 0, __r10));
                emit(__adds_r(__AL, __r8, __r8, __r9));
                emit(__sw(__AL, __r8, __sp, 8));
                emit(__lw(__AL, __r8, __sp, 12));
                emit(__eor_r(__AL, __r8, __r8, __r10));
                emit(__zero(__r9));
                emit(__adc_r(__AL, __r8, __r8, __r9));
                emit(__sw(__AL, __r8, __sp, 12));
            }
            emit(__zero(__r8));
            emit(__sw(__AL, __r8, __sp, 16));
            emit(__sw(__AL, __r8, __sp, 20));
            emit(__sw(__AL, __r8, __sp, 24));
            emit(__sw(__AL, __r8, __sp, 28));
            emit(__mov_i(__AL, __r8, 64));
            emit(__sw(__AL, __r8, __sp, 32));

            /* Shift the remainder, dividend, and quotient left by one. */
            emit(__lw(__AL, __r8, __sp, 4));
            emit(__srl_amt(__AL, 0, logic_rs, __r10, __r8, 31));
            emit(__sw(__AL, __r10, __sp, 36));
            emit(__lw(__AL, __r8, __sp, 16));
            emit(__srl_amt(__AL, 0, logic_rs, __r9, __r8, 31));
            emit(__sll_amt(__AL, 0, logic_ls, __r8, __r8, 1));
            emit(__sw(__AL, __r8, __sp, 16));
            emit(__lw(__AL, __r8, __sp, 20));
            emit(__sll_amt(__AL, 0, logic_ls, __r8, __r8, 1));
            emit(__or_r(__AL, __r8, __r8, __r9));
            emit(__sw(__AL, __r8, __sp, 20));
            emit(__lw(__AL, __r10, __sp, 36));
            emit(__lw(__AL, __r8, __sp, 16));
            emit(__or_r(__AL, __r8, __r8, __r10));
            emit(__sw(__AL, __r8, __sp, 16));
            emit(__lw(__AL, __r8, __sp, 0));
            emit(__srl_amt(__AL, 0, logic_rs, __r9, __r8, 31));
            emit(__sll_amt(__AL, 0, logic_ls, __r8, __r8, 1));
            emit(__sw(__AL, __r8, __sp, 0));
            emit(__lw(__AL, __r8, __sp, 4));
            emit(__sll_amt(__AL, 0, logic_ls, __r8, __r8, 1));
            emit(__or_r(__AL, __r8, __r8, __r9));
            emit(__sw(__AL, __r8, __sp, 4));
            emit(__lw(__AL, __r8, __sp, 24));
            emit(__srl_amt(__AL, 0, logic_rs, __r9, __r8, 31));
            emit(__sll_amt(__AL, 0, logic_ls, __r8, __r8, 1));
            emit(__sw(__AL, __r8, __sp, 24));
            emit(__lw(__AL, __r8, __sp, 28));
            emit(__sll_amt(__AL, 0, logic_ls, __r8, __r8, 1));
            emit(__or_r(__AL, __r8, __r8, __r9));
            emit(__sw(__AL, __r8, __sp, 28));

            /* Subtract the divisor when the two-word remainder permits it. */
            emit(__lw(__AL, __r8, __sp, 20));
            emit(__lw(__AL, __r9, __sp, 12));
            emit(__cmp_r(__AL, __r8, __r9));
            emit(__b(__CC, 68));
            emit(__b(__HI, 20));
            emit(__lw(__AL, __r8, __sp, 16));
            emit(__lw(__AL, __r9, __sp, 8));
            emit(__cmp_r(__AL, __r8, __r9));
            emit(__b(__CC, 48));
            emit(__lw(__AL, __r8, __sp, 16));
            emit(__lw(__AL, __r9, __sp, 8));
            emit(__subs_r(__AL, __r8, __r8, __r9));
            emit(__sw(__AL, __r8, __sp, 16));
            emit(__lw(__AL, __r8, __sp, 20));
            emit(__lw(__AL, __r9, __sp, 12));
            emit(__sbc_r(__AL, __r8, __r8, __r9));
            emit(__sw(__AL, __r8, __sp, 20));
            emit(__lw(__AL, __r8, __sp, 24));
            emit(__add_i(__AL, __r8, __r8, 1));
            emit(__sw(__AL, __r8, __sp, 24));
            emit(__lw(__AL, __r8, __sp, 32));
            emit(__add_i(__AL, __r8, __r8, -1));
            emit(__sw(__AL, __r8, __sp, 32));
            emit(__cmp_i(__AL, __r8, 0));
            emit(__b(__NE, -220));

            emit(__lw(__AL, rd, __sp, mop->op == OP_mod ? 16 : 24));
            emit(__lw(__AL, mop->dest_hi, __sp, mop->op == OP_mod ? 20 : 28));
            if (!is_unsigned) {
                emit(__lw(__AL, __r10, __sp, 40));
                if (mop->op == OP_div) {
                    emit(__lw(__AL, __r8, __sp, 44));
                    emit(__eor_r(__AL, __r10, __r10, __r8));
                }
                emit(__eor_r(__AL, rd, rd, __r10));
                emit(__eor_r(__AL, mop->dest_hi, mop->dest_hi, __r10));
                emit(__rsbs_i(__AL, __r8, 0, __r10));
                emit(__adds_r(__AL, rd, rd, __r8));
                emit(__zero(__r8));
                emit(__adc_r(__AL, mop->dest_hi, mop->dest_hi, __r8));
            }
            emit(__add_i(__AL, __sp, __sp, is_unsigned ? 40 : 48));
            return;
        }
        if (hard_mul_div) {
            if (mop->op == OP_div)
                emit(mop->src0_is_unsigned || mop->src1_is_unsigned
                         ? __udiv(__AL, rd, rm, rn)
                         : __div(__AL, rd, rm, rn));
            else {
                emit(mop->src0_is_unsigned || mop->src1_is_unsigned
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
        if (mop->src0_is_unsigned || mop->src1_is_unsigned) {
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
        if (mop->op == OP_div)
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
        if (MOP_PAIR_DEST_SRC0(mop)) {
            emit(__cmp_i(__AL, rm, 32));
            emit(__b(__CS, 32));
            emit(__mov_r(__AL, __r8, rn));
            emit(__sll(__AL, rd, rn, rm));
            emit(__sll(__AL, mop->dest_hi, mop->src0_hi, rm));
            emit(__rsb_i(__AL, __r9, 32, rm));
            emit(__srl(__AL, __r8, __r8, __r9));
            emit(__or_r(__AL, mop->dest_hi, mop->dest_hi, __r8));
            emit(__b(__AL, 16));
            emit(__add_i(__AL, __r9, rm, -32));
            emit(__sll(__AL, mop->dest_hi, rn, __r9));
            emit(__zero(rd));
            return;
        }
        emit(__sll(__AL, rd, rn, rm));
        return;
    case OP_rshift:
        if (MOP_PAIR_DEST_SRC0(mop)) {
            int shift_kind = mop->src0_is_unsigned ? logic_rs : arith_rs;

            emit(__cmp_i(__AL, rm, 32));
            emit(__b(__CS, 32));
            emit(__mov_r(__AL, __r8, mop->src0_hi));

            /* The sign lives in the high word. The low word's vacated top bits
             * are the high word's low bits, ORed in below, so shifting in the
             * low word's own bit 31 would set bits the OR cannot clear.
             */
            emit(__srl(__AL, rd, rn, rm));
            emit(__rsb_i(__AL, __r9, 32, rm));
            emit(__sll(__AL, __r8, __r8, __r9));
            emit(__or_r(__AL, rd, rd, __r8));
            emit(shift_kind == logic_rs
                     ? __srl(__AL, mop->dest_hi, mop->src0_hi, rm)
                     : __sra(__AL, mop->dest_hi, mop->src0_hi, rm));
            emit(__b(__AL, 16));
            emit(__add_i(__AL, __r9, rm, -32));
            emit(shift_kind == logic_rs ? __srl(__AL, rd, mop->src0_hi, __r9)
                                        : __sra(__AL, rd, mop->src0_hi, __r9));
            if (shift_kind == logic_rs)
                emit(__zero(mop->dest_hi));
            else
                emit(__srl_amt(__AL, 0, arith_rs, mop->dest_hi, mop->src0_hi,
                               31));
            return;
        }
        emit(mop->src0_is_unsigned ? __srl(__AL, rd, rn, rm)
                                   : __sra(__AL, rd, rn, rm));
        return;
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
        /* Wide ordering subtracts the pairs, the low words with CMP and the
         * high words with SBCS, so the flags describe the whole 64-bit
         * difference: N and V order signed operands and C unsigned ones. The
         * low words are unsigned whatever the type, which a signed test of them
         * got wrong. Swapping the operands turns > and <= into < and >=. The
         * result register is written only once the flags are set, so it may be
         * any operand register. Equality is left to the word-by-word sequence
         * below, since Z is not valid after SBCS.
         */
        if (mop->op != OP_eq && mop->op != OP_neq && mop->src0_hi >= 0 &&
            mop->src1_hi >= 0) {
            bool unsigned_cmp = mop->src0_is_unsigned || mop->src1_is_unsigned;
            bool swap = mop->op == OP_gt || mop->op == OP_leq;
            bool below = mop->op == OP_lt || mop->op == OP_gt;
            arm_cond_t cond = below ? (unsigned_cmp ? __CC : __LT)
                                    : (unsigned_cmp ? __CS : __GE);

            emit(__cmp_r(__AL, swap ? rm : rn, swap ? rn : rm));
            emit(__sbcs_r(__AL, __r8, swap ? mop->src1_hi : mop->src0_hi,
                          swap ? mop->src0_hi : mop->src1_hi));
            emit(__zero(rd));
            emit(__mov_i(cond, rd, 1));
            return;
        }
        emit(__cmp_r(__AL, rn, rm));
        if ((mop->op == OP_eq || mop->op == OP_neq) && mop->src0_hi >= 0 &&
            mop->src1_hi >= 0)
            emit(__cmp_r(__EQ, mop->src0_hi, mop->src1_hi));
        emit(__zero(rd));
        emit(__mov_i(arm_get_cond(mop->op, mop->src0_is_unsigned ||
                                               mop->src1_is_unsigned),
                     rd, 1));
        return;
    case OP_negate:
        if (MOP_PAIR_DEST_SRC0(mop)) {
            emit(__rsbs_i(__AL, rd, 0, rn));
            emit(__rsc_i(__AL, mop->dest_hi, 0, mop->src0_hi));
        } else
            emit(__rsb_i(__AL, rd, 0, rn));
        return;
    case OP_bit_not:
        emit(__mvn_r(__AL, rd, rn));
        if (MOP_PAIR_DEST_SRC0(mop))
            emit(__mvn_r(__AL, mop->dest_hi, mop->src0_hi));
        return;
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
        emit(arm_bitwise_insn(mop->op, rd, rn, rm));
        if (MOP_PAIR_BINARY(mop))
            emit(arm_bitwise_insn(mop->op, mop->dest_hi, mop->src0_hi,
                                  mop->src1_hi));
        return;
    case OP_log_not:
        if (mop->src0_hi >= 0) {
            emit(__or_r(__AL, __r8, rn, mop->src0_hi));
            emit(__cmp_i(__AL, __r8, 0));
        } else
            emit(__cmp_i(__AL, rn, 0));
        emit(__mov_i(__NE, rd, 0));
        emit(__mov_i(__EQ, rd, 1));
        return;
    case OP_trunc:
        if (mop->is_unsigned && (rm == 1 || rm == 2)) {
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
        if (mop->dest_hi >= 0 && mop->src0_hi < 0) {
            if (source_size == 1) {
                if (mop->src0_is_unsigned || mop->src0_is_pointer) {
                    emit(__sll_amt(__AL, 0, logic_ls, rd, rn, 24));
                    emit(__srl_amt(__AL, 0, logic_rs, rd, rd, 24));
                } else
                    emit(__sxtb(__AL, rd, rn, 0));
            } else if (source_size == 2) {
                if (mop->src0_is_unsigned || mop->src0_is_pointer) {
                    emit(__sll_amt(__AL, 0, logic_ls, rd, rn, 16));
                    emit(__srl_amt(__AL, 0, logic_rs, rd, rd, 16));
                } else
                    emit(__sxth(__AL, rd, rn, 0));
            } else
                emit(__mov_r(__AL, rd, rn));
            if (mop->src0_is_unsigned || mop->src0_is_pointer)
                emit(__zero(mop->dest_hi));
            else
                emit(__srl_amt(__AL, 0, arith_rs, mop->dest_hi, rd, 31));
            return;
        }
        if (mop->src0_is_unsigned) {
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
        if (mop->dest_hi >= 0) {
            if (mop->src0_hi >= 0) {
                if (mop->dest_hi != mop->src0_hi)
                    emit(__mov_r(__AL, mop->dest_hi, mop->src0_hi));
            } else if (mop->src0_is_unsigned || mop->src0_is_pointer)
                emit(__zero(mop->dest_hi));
            else
                emit(__srl_amt(__AL, 0, arith_rs, mop->dest_hi, rn, 31));
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

    /* Clearing the global stack is the dynamic build's job alone. A static
     * image is the first thing to run, so the stack below the entry SP is
     * untouched anonymous memory and already reads as zero. A dynamic one has
     * had the loader and glibc's startup run over that same memory first, so a
     * global with no initializer would otherwise begin life holding their
     * leftovers. The loop stores zero from the top word down; 'ofs' is a
     * nonzero multiple of four here. No libc call is involved, so this holds
     * with --no-libc as well.
     */
    if (dynlink) {
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
        emit(__mov_i(__AL, __r0, 0));
        emit(__add_i(__AL, __r8, __r8, -4));
        emit(__add_r(__AL, __r3, __r12, __r8));
        emit(__sw(__AL, __r0, __r3, 0));
        emit(__cmp_i(__AL, __r8, 0));
        emit(__b(__NE, -16));
    }

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

    mop_visit_globals(emit_mop);

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

    for (int i = 0; i < mop_flat_count; i++) {
        vir_mop_t *mop = MOP_FLAT[i];
        emit_mop(mop);
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
