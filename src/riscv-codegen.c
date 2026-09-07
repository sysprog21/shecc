/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Translate IR to target machine code */
#include "defs.h"
#include "globals.c"
#include "riscv.c"

#define RV32_ALIGNMENT 16

/* Explanation: registers preservation/restoration
 *
 * The following table illustrates which registers are caller-saved or
 * callee-saved:
 * +-----------+--------+
 * | Register  | Saver  |
 * | ABI Name  |        |
 * +-----------+--------+
 * | zero,     |        |
 * | gp,       | (None) |
 * | tp        |        |
 * +-----------+--------+
 * | ra        | Caller |
 * +-----------+--------+
 * | sp        | Callee |
 * +-----------+--------+
 * | a0 - a7   | Caller |
 * +-----------+--------+
 * | s0 - s11  | Callee |
 * +-----------+--------+
 * | t0 - t6   | Caller |
 * +-----------+--------+
 *
 * - ra and sp: are properly handled by the code generator.
 *
 * - a0 - a7: are implicitly handled in register allocation phase.
 *
 *   Register allocation use 8 virtual registers (vreg0 - vreg7) to
 *   allocate registers from IR. When encountering a function call,
 *   all virtual registers are spilled properly before the call, and
 *   their contents will also be restored if they are used after the
 *   call.
 *
 *   Since vreg0 - vreg7 map to a0 - a7 directly, these physical
 *   registers are also preserved/restored naturally whan a function
 *   is invoked or returned.
 *
 * - s0 and s1: handling depends on the linking mode.
 *
 *   Static linking:
 *     For s0 register, it is used at the program entry point under
 *     static linking. The program entry point will use s0 to store
 *     the value of sp register and obtain 'argc' and 'argv' via s0
 *     before calling the main function. Then, the content of s0 is
 *     no longer used after calling the main function, so its
 *     preservation and restoration can also be ignored.
 *
 *     s1 is not used under static linking, so it is not necessary to
 *     be handled.
 *
 *   Dynamic linking:
 *     Both registers are used to preserve 'argc' and 'argv' and pass
 *     them to the main function. Therefore, the code generator ensures
 *     that proper instructions are generated to preserve/restore
 *     their contents via stack.
 *
 * - s2 - s11: are not necessary to be preserved or restored.
 *
 *   The current code generator does not use s2 - s11 to generate
 *   instructions, so these registers are not needed to be processed.
 *
 * - t0 - t6: are not necessary to be handled.
 *
 *   These registers are used to store certain temporary values;
 *   however, the code generator ensures that these values do not
 *   persist across function calls.
 */

void update_elf_offset(ph2_ir_t *ph2_ir)
{
    switch (ph2_ir->op) {
    case OP_load_constant:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_address_of:
    case OP_global_address_of:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
            elf_offset += 12;
        else
            elf_offset += 4;
        return;
    case OP_assign:
        elf_offset += 4;
        return;
    case OP_load:
    case OP_global_load:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
            elf_offset += 16;
        else
            elf_offset += 4;
        return;
    case OP_store:
    case OP_global_store:
        if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047)
            elf_offset += 16;
        else
            elf_offset += 4;
        return;
    case OP_read:
    case OP_write:
    case OP_jump:
    case OP_call:
    case OP_load_func:
    case OP_indirect:
    case OP_add:
    case OP_sub:
    case OP_lshift:
    case OP_rshift:
    case OP_gt:
    case OP_lt:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_negate:
    case OP_bit_not:
        elf_offset += 4;
        return;
    case OP_mul:
        if (hard_mul_div)
            elf_offset += 4;
        else
            elf_offset += 52;
        return;
    case OP_div:
    case OP_mod:
        if (hard_mul_div)
            elf_offset += 4;
        else
            elf_offset += 108;
        return;
    case OP_load_data_address:
    case OP_load_rodata_address:
    case OP_neq:
    case OP_geq:
    case OP_leq:
    case OP_log_not:
        elf_offset += 8;
        return;
    case OP_address_of_func:
    case OP_eq:
        elf_offset += 12;
        return;
    case OP_branch:
        elf_offset += 20;
        return;
    case OP_return:
        elf_offset += 24;
        return;
    case OP_trunc:
        /* A byte and a short each need a shift pair to keep the sign. */
        if (ph2_ir->src1 == 1 || ph2_ir->src1 == 2)
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_sign_ext: {
        /* Decode source size from upper 16 bits */
        int source_size = (ph2_ir->src1 >> 16) & 0xFFFF;
        if (source_size == 2)
            elf_offset += 8; /* short extension: 2 instructions */
        else
            elf_offset += 12; /* byte extension: 3 instructions */
        return;
    }
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

    if (dynlink) {
        /* When using dynamic linking, 20 instructions are generated at the
         * program entry point to perform the following operations:
         * - prepare arguments and call __libc_start_main()
         * - preserve a0 ('argc'), a1 ('argv') and sp.
         * - allocate a global stack and jump to global init function.
         */
        elf_offset = 80;
    } else {
        /* Under static linking, "__syscall" must be generated to allow the
         * program to invoke system calls.
         *
         * "__syscall" consists of 9 instructions, preceded by 6 initial
         * instructions. Consequently, the elf offset for "__syscall" is is 24
         * bytes, and the offset for the subsequent function (GLOBAL_FUNC) is 60
         * bytes.
         */
        func = find_func("__syscall");
        func->bbs->elf_offset = 24;
        elf_offset = 60;
    }

    GLOBAL_FUNC->bbs->elf_offset = elf_offset;

    for (ph2_ir_t *ph2_ir = GLOBAL_FUNC->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next) {
        update_elf_offset(ph2_ir);
    }

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    if (dynlink)
        elf_offset += 44;
    else
        elf_offset += 24;

    for (func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        /* reserve stack */
        ph2_ir_t *flatten_ir = add_ph2_ir(OP_define);
        flatten_ir->src0 = func->stack_size;
        flatten_ir->func_name = intern_string(func->return_def.var_name);

        /* Except for local variables, it must allocate additional space to
         * preserve the content of ra at each function entry point.
         *
         * 'stack_size' doesn't include the additional space, so an extra number
         * '4' is added to 'stack_size'.
         */
        int stack_top_ofs = ALIGN_UP(func->stack_size + 4, RV32_ALIGNMENT);

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            bb->elf_offset = elf_offset;

            if (bb == func->bbs) {
                /* save ra, sp */
                elf_offset += 16;
            }

            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
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
    int rd = ph2_ir->dest + 10;
    int rs1 = ph2_ir->src0 + 10;
    int rs2 = ph2_ir->src1 + 10;
    int ofs;

    /* Prepare the variables to reuse the same code for the instruction sequence
     * of
     * 1. division and modulo.
     * 2. load and store operations.
     * 3. address-of operations.
     */
    rv_reg interm, divisor_mask = __t1;

    switch (ph2_ir->op) {
    case OP_define:
        ofs = ALIGN_UP(ph2_ir->src0 + 4, RV32_ALIGNMENT);
        emit(__sw(__ra, __sp, -4));
        emit(__lui(__t0, rv_hi(ofs)));
        emit(__addi(__t0, __t0, rv_lo(ofs)));
        emit(__sub(__sp, __sp, __t0));
        return;
    case OP_load_constant:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit(__lui(rd, rv_hi(ph2_ir->src0)));
            emit(__addi(rd, rd, rv_lo(ph2_ir->src0)));

        } else
            emit(__addi(rd, __zero, ph2_ir->src0));
        return;
    case OP_address_of:
    case OP_global_address_of:
        interm = ph2_ir->op == OP_address_of ? __sp : __gp;
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit(__lui(__t0, rv_hi(ph2_ir->src0)));
            emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
            emit(__add(rd, interm, __t0));
        } else
            emit(__addi(rd, interm, ph2_ir->src0));
        return;
    case OP_assign:
        emit(__addi(rd, rs1, 0));
        return;
    case OP_load:
    case OP_global_load:
        interm = ph2_ir->op == OP_load ? __sp : __gp;
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit(__lui(__t0, rv_hi(ph2_ir->src0)));
            emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
            emit(__add(__t0, interm, __t0));
            emit(__lw(rd, __t0, 0));
        } else
            emit(__lw(rd, interm, ph2_ir->src0));
        return;
    case OP_store:
    case OP_global_store:
        interm = ph2_ir->op == OP_store ? __sp : __gp;
        if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047) {
            emit(__lui(__t0, rv_hi(ph2_ir->src1)));
            emit(__addi(__t0, __t0, rv_lo(ph2_ir->src1)));
            emit(__add(__t0, interm, __t0));
            emit(__sw(rs1, __t0, 0));
        } else
            emit(__sw(rs1, interm, ph2_ir->src1));
        return;
    case OP_read:
        if (ph2_ir->src1 == 1)
            emit(__lb(rd, rs1, 0));
        else if (ph2_ir->src1 == 2)
            emit(__lh(rd, rs1, 0));
        else if (ph2_ir->src1 == 4)
            emit(__lw(rd, rs1, 0));
        else
            abort();
        return;
    case OP_write:
        if (ph2_ir->dest == 1)
            emit(__sb(rs2, rs1, 0));
        else if (ph2_ir->dest == 2)
            emit(__sh(rs2, rs1, 0));
        else if (ph2_ir->dest == 4)
            emit(__sw(rs2, rs1, 0));
        else
            abort();
        return;
    case OP_branch:
        ofs = elf_code_start + ph2_ir->then_bb->elf_offset;
        emit(__lui(__t0, rv_hi(ofs)));
        emit(__addi(__t0, __t0, rv_lo(ofs)));
        emit(__beq(rs1, __zero, 8));
        emit(__jalr(__zero, __t0, 0));
        emit(__jal(__zero, ph2_ir->else_bb->elf_offset - elf_code->size));
        return;
    case OP_jump:
        emit(__jal(__zero, ph2_ir->next_bb->elf_offset - elf_code->size));
        return;
    case OP_call:
        func = find_func(ph2_ir->func_name);
        if (func->bbs)
            ofs = func->bbs->elf_offset - elf_code->size;
        else if (dynlink) {
            ofs = (dynamic_sections.elf_plt_start + func->plt_offset) -
                  (elf_code_start + elf_code->size);
        } else {
            printf("The '%s' function is not implemented\n", ph2_ir->func_name);
            abort();
        }
        emit(__jal(__ra, ofs));
        return;
    case OP_load_data_address:
        emit(__lui(rd, rv_hi(elf_data_start + ph2_ir->src0)));
        emit(__addi(rd, rd, rv_lo(elf_data_start + ph2_ir->src0)));
        return;
    case OP_load_rodata_address:
        emit(__lui(rd, rv_hi(elf_rodata_start + ph2_ir->src0)));
        emit(__addi(rd, rd, rv_lo(elf_rodata_start + ph2_ir->src0)));
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
        emit(__lui(__t0, rv_hi(ofs)));
        emit(__addi(__t0, __t0, rv_lo(ofs)));
        emit(__sw(__t0, rs1, 0));
        return;
    case OP_load_func:
        emit(__addi(__t0, rs1, 0));
        return;
    case OP_indirect:
        emit(__jalr(__ra, __t0, 0));
        return;
    case OP_return:
        if (ph2_ir->src0 == -1)
            emit(__addi(__zero, __zero, 0));
        else
            emit(__addi(__a0, rs1, 0));
        ofs = ALIGN_UP(ph2_ir->src1 + 4, RV32_ALIGNMENT);
        emit(__lui(__t0, rv_hi(ofs)));
        emit(__addi(__t0, __t0, rv_lo(ofs)));
        emit(__add(__sp, __sp, __t0));
        emit(__lw(__ra, __sp, -4));
        emit(__jalr(__zero, __ra, 0));
        return;
    case OP_add:
        emit(__add(rd, rs1, rs2));
        return;
    case OP_sub:
        emit(__sub(rd, rs1, rs2));
        return;
    case OP_mul:
        if (hard_mul_div)
            emit(__mul(rd, rs1, rs2));
        else {
            emit(__addi(__t0, __zero, 0));
            emit(__addi(__t1, __zero, 0));
            emit(__addi(__t3, rs1, 0));
            emit(__addi(__t4, rs2, 0));
            emit(__beq(__t3, __zero, 32));
            emit(__beq(__t4, __zero, 28));
            emit(__andi(__t1, __t4, 1));
            emit(__beq(__t1, __zero, 8));
            emit(__add(__t0, __t0, __t3));
            emit(__slli(__t3, __t3, 1));
            emit(__srli(__t4, __t4, 1));
            emit(__jal(__zero, -28));
            emit(__addi(rd, __t0, 0));
        }
        return;
    case OP_div:
    case OP_mod:
        if (hard_mul_div) {
            if (ph2_ir->op == OP_div)
                emit(__div(rd, rs1, rs2));
            else
                emit(__mod(rd, rs1, rs2));
            return;
        }
        interm = __t0;
        /* div/mod emulation */
        if (ph2_ir->op == OP_mod) {
            /* If the requested operation is modulo, the result will be stored
             * in __t2. The sign of the divisor is irrelevant for determining
             * the result's sign.
             */
            interm = __t2;
            divisor_mask = __zero;
        }
        /* Obtain absolute values of the dividend and divisor */
        emit(__addi(__t2, rs1, 0));
        emit(__addi(__t3, rs2, 0));
        emit(__srai(__t0, __t2, 31));
        emit(__add(__t2, __t2, __t0));
        emit(__xor(__t2, __t2, __t0));
        emit(__srai(__t1, __t3, 31));
        emit(__add(__t3, __t3, __t1));
        emit(__xor(__t3, __t3, __t1));
        emit(__xor(__t5, __t0, divisor_mask));
        /* Unsigned integer division */
        emit(__addi(__t0, __zero, 0));
        emit(__addi(__t1, __zero, 1));
        emit(__beq(__t3, __zero, 52));
        emit(__beq(__t2, __zero, 48));
        emit(__beq(__t2, __t3, 20));
        emit(__bltu(__t2, __t3, 16));
        emit(__slli(__t3, __t3, 1));
        emit(__slli(__t1, __t1, 1));
        emit(__jal(__zero, -16));
        emit(__bltu(__t2, __t3, 12));
        emit(__sub(__t2, __t2, __t3));
        emit(__add(__t0, __t0, __t1));
        emit(__srli(__t1, __t1, 1));
        emit(__srli(__t3, __t3, 1));
        emit(__bne(__t1, __zero, -20));
        emit(__addi(rd, interm, 0));
        /* Handle the correct sign for the quotient or remainder */
        emit(__beq(__t5, __zero, 8));
        emit(__sub(rd, __zero, rd));
        return;
    case OP_lshift:
        emit(__sll(rd, rs1, rs2));
        return;
    case OP_rshift:
        emit(__sra(rd, rs1, rs2));
        return;
    case OP_eq:
        emit(__sub(rd, rs1, rs2));
        emit(__sltu(rd, __zero, rd));
        emit(__xori(rd, rd, 1));
        return;
    case OP_neq:
        emit(__sub(rd, rs1, rs2));
        emit(__sltu(rd, __zero, rd));
        return;
    case OP_gt:
        emit(__slt(rd, rs2, rs1));
        return;
    case OP_geq:
        emit(__slt(rd, rs1, rs2));
        emit(__xori(rd, rd, 1));
        return;
    case OP_lt:
        emit(__slt(rd, rs1, rs2));
        return;
    case OP_leq:
        emit(__slt(rd, rs2, rs1));
        emit(__xori(rd, rd, 1));
        return;
    case OP_negate:
        emit(__sub(rd, __zero, rs1));
        return;
    case OP_bit_not:
        emit(__xori(rd, rs1, -1));
        return;
    case OP_bit_and:
        emit(__and(rd, rs1, rs2));
        return;
    case OP_bit_or:
        emit(__or(rd, rs1, rs2));
        return;
    case OP_bit_xor:
        emit(__xor(rd, rs1, rs2));
        return;
    case OP_log_not:
        emit(__sltu(rd, __zero, rs1));
        emit(__xori(rd, rd, 1));
        return;
    case OP_trunc:
        /* Narrowing keeps the sign: there are no unsigned types. */
        if (ph2_ir->src1 == 1) {
            emit(__slli(rd, rs1, 24));
            emit(__srai(rd, rd, 24));
        } else if (ph2_ir->src1 == 2) {
            emit(__slli(rd, rs1, 16));
            emit(__srai(rd, rd, 16));
        } else if (ph2_ir->src1 == 4) {
            /* No truncation needed for 32-bit values */
            emit(__add(rd, rs1, __zero));
        } else {
            fatal("Unsupported truncation operation with invalid target size");
        }
        return;
    case OP_sign_ext: {
        /* Decode size information: Lower 16 bits: target size Upper 16 bits:
         * source size
         */
        int target_size = ph2_ir->src1 & 0xFFFF;
        int source_size = (ph2_ir->src1 >> 16) & 0xFFFF;

        /* Calculate shift amount based on target and source sizes */
        int shift_amount = (target_size - source_size) * 8;

        if (source_size == 2) {
            /* Sign extend from short to word (16-bit shift) For 16-bit sign
             * extension, use only shift operations since 0xFFFF is too large
             * for RISC-V immediate field
             */
            emit(__slli(rd, rs1, shift_amount));
            emit(__srai(rd, rd, shift_amount));
        } else {
            /* Fallback for other sizes */
            emit(__andi(rd, rs1, 0xFF));
            emit(__slli(rd, rd, shift_amount));
            emit(__srai(rd, rd, shift_amount));
        }
        return;
    }
    case OP_cast:
        /* Generic cast operation - for now, just move the value */
        emit(__addi(rd, rs1, 0));
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

        /* - Initial stack layout when the program starts:
         *
         *      +----------------+ (high address)
         *      | ...            |
         *      +----------------+
         *      | argv[argc - 1] |
         *      +----------------+
         *      | ...            |
         *      +----------------+
         *      | argv[0]        |
         *      +----------------+
         *      | argc           |
         *      +----------------+ <- sp points to this location.
         *
         * - At the program entry point, it must call __libc_start_main()
         *   under dynamic linking. The function prototype is as follows:
         *
         *   int __libc_start_main(int (*main) (int, char **, char **),
         *                         int argc, char **argv,
         *                         void (*init) (void),
         *                         void (*fini) (void),
         *                         void (*rtld_fini) (void),
         *                         void (*stack_end));
         *
         * Currently, to execute a dynamically linked program with the minimal
         * effort required, we perform the following call: ->
         * __libc_start_main(main_wrapper, argc, argv, NULL,
         *                      NULL, NULL, stack_end)
         */
        emit(__lui(__a0, rv_hi(elf_code_start + 36)));
        emit(__addi(__a0, __a0, rv_lo(elf_code_start + 36)));
        emit(__lw(__a1, __sp, 0));
        emit(__addi(__a2, __sp, 4));
        emit(__addi(__a3, __zero, 0));
        emit(__addi(__a4, __zero, 0));
        emit(__addi(__a5, __zero, 0));
        emit(__addi(__a6, __sp, 0));

        /* Call __libc_start_main() via PLT[1] */
        ofs = (dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE) -
              (elf_code_start + elf_code->size);
        emit(__jal(__ra, ofs));

        /* The main wrapper is located here under the dynamic linking mode
         *
         * Use s0 and s1 registers to temporarily store 'argc' and 'argv', while
         * preserving ra on the stack.
         *
         * After the main function completes its execution, it must use the
         * original content of ra to transfer control back to
         * __libc_start_main().
         */
        emit(__addi(__sp, __sp, -12));
        emit(__sw(__ra, __sp, 8));
        emit(__sw(__s1, __sp, 4));   /* callee-saved */
        emit(__sw(__s0, __sp, 0));   /* callee-saved */
        emit(__addi(__s0, __a0, 0)); /* argc */
        emit(__addi(__s1, __a1, 0)); /* argv */
        ofs = ALIGN_UP(GLOBAL_FUNC->stack_size, RV32_ALIGNMENT) + 4;
    } else {
        /* When using static linking, the starting address of the main wrapper
         * is here.
         *
         * Save original sp in s0 first.
         */
        ofs = ALIGN_UP(GLOBAL_FUNC->stack_size, RV32_ALIGNMENT);
        emit(__addi(__s0, __sp, 0));
    }

    /* Next, the main wrapper performs:
     *   1. allocate global stack
     *   2. jump to global init function
     *   3. call the main function
     */
    emit(__lui(__t0, rv_hi(ofs)));
    emit(__addi(__t0, __t0, rv_lo(ofs)));
    emit(__sub(__sp, __sp, __t0));
    emit(__addi(__gp, __sp, 0)); /* Set up global pointer */
    emit(__jal(__ra, GLOBAL_FUNC->bbs->elf_offset - elf_code->size));

    if (!dynlink) {
        /* syscall trampoline for __syscall */
        emit(__addi(__a7, __a0, 0));
        emit(__addi(__a0, __a1, 0));
        emit(__addi(__a1, __a2, 0));
        emit(__addi(__a2, __a3, 0));
        emit(__addi(__a3, __a4, 0));
        emit(__addi(__a4, __a5, 0));
        emit(__addi(__a5, __a6, 0));
        emit(__ecall());
        emit(__jalr(__zero, __ra, 0));
    }

    ph2_ir_t *ph2_ir;
    for (ph2_ir = GLOBAL_FUNC->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next)
        emit_ph2_ir(ph2_ir);

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    if (MAIN_BB) {
        if (dynlink) {
            emit(__addi(__a0, __s0, 0));
            emit(__addi(__a1, __s1, 0));
            emit(__jal(__ra, MAIN_BB->elf_offset - elf_code->size));

            /* - Restore sp, s0 and s1.
             * - Transfer control back to __libc_start_main() using
             *   the preserved ra.
             */
            emit(__lui(__t0, rv_hi(ofs)));
            emit(__addi(__t0, __t0, rv_lo(ofs)));
            emit(__add(__sp, __sp, __t0));
            emit(__lw(__ra, __sp, 8));
            emit(__lw(__s1, __sp, 4));
            emit(__lw(__s0, __sp, 0));
            emit(__addi(__sp, __sp, 12));
            emit(__jalr(__zero, __ra, 0));
        } else {
            /* use original sp saved in s0 to get argc/argv */
            emit(__addi(__t0, __s0, 0));
            emit(__lw(__a0, __t0, 0));
            emit(__addi(__a1, __t0, 4));
            emit(__jal(__ra, MAIN_BB->elf_offset - elf_code->size));

            /* exit with main's return value in a0 */
            emit(__addi(__a7, __zero, 93));
            emit(__ecall());
        }
    }

    for (int i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = PH2_IR_FLATTEN[i];
        emit_ph2_ir(ph2_ir);
    }
}

void plt_generate()
{
    int addr_of_plt = dynamic_sections.elf_plt_start;
    int addr_of_got = dynamic_sections.elf_got_start;
    int end = dynamic_sections.plt_size - PLT_FIXUP_SIZE;
    int ofs, pcrel_hi, pcrel_lo;

    ofs = addr_of_got - addr_of_plt;
    pcrel_hi = ofs & ~0xFFF;
    pcrel_lo = ofs & 0xFFF;
    if (pcrel_lo > 2047) {
        pcrel_hi += 0x1000;
        pcrel_lo -= 0x1000;
    }

    /* Accroding the RISC-V ABI specification, the first PLT entry should
     * contains the following instructions:
     *
     * 1: auipc t2, %pcrel_hi(.got)
     *    sub    t1, t1, t3
     *    lw     t3, %pcrel_lo(1b)(t2)
     *    addi   t1, t1 -(PLT0_SIZE + 12)    # PLT0_SIZE is 32 bytes.
     *    addi   t0, t2, %pcrel_lo(1b)
     *    srli   t1, t1, log2(16 / PTRSIZE)  # PTRSIZE is 4 bytes.
     *    lw     t0, PTRSIZE(t0)
     *    jr     t3
     *
     * +-----------------------------------+-----------------------+
     * | Instruction                       | Contents of registers |
     * +-----------------------------------+-----------------------+
     * | auipc  t2, %pcrel_hi(.got)        | t0: <dont' care>      |
     * |                                   | t1: &PLT[N] + 12      |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: &PLT[0]           |
     * +-----------------------------------+-----------------------+
     * | sub    t1, t1, t3                 | t0: <dont' care>      |
     * |                                   | t1: (N - 1) * 16 +    |
     * |                                   |     32 + 12           |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: &PLT[0]           |
     * +-----------------------------------+-----------------------+
     * | lw     t3, %pcrel_lo(1b)(t2)      | t0: <dont' care>      |
     * |                                   | t1: (N - 1) * 16 +    |
     * |                                   |     32 + 12           |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | addi   t1, t1 -(PLT0_SIZE + 12)   | t0: <dont'care>       |
     * |                                   | t1: (N - 1) * 16      |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | addi   t0, t2, %pcrel_lo(1b)      | t0: &GOT[0]           |
     * |                                   | t1: (N - 1) * 16      |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | srli   t1, t1, log2(16 / PTRSIZE) | t0: &GOT[0]           |
     * |                                   | t1: (N - 1) * 4       |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | lw     t0, PTRSIZE(t0)            | t0: GOT[1]            |
     * |                                   | t1: (N - 1) * 4       |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | jr     t3                         | t0: GOT[1]            |
     * |                                   | t1: (N - 1) * 4       |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     *
     * Note:
     * - N >= 1, and it means the N-th external function.
     * - &PLT[N] - &PLT[0] = size of PLT[0] + size of several PLT stubs.
     *                     = 32             + (N - 1) * 16
     */
    elf_write_int(dynamic_sections.elf_plt, __auipc(__t2, pcrel_hi));
    elf_write_int(dynamic_sections.elf_plt, __sub(__t1, __t1, __t3));
    elf_write_int(dynamic_sections.elf_plt, __lw(__t3, __t2, pcrel_lo));
    elf_write_int(dynamic_sections.elf_plt, __addi(__t1, __t1, -44));
    elf_write_int(dynamic_sections.elf_plt, __addi(__t0, __t2, pcrel_lo));
    elf_write_int(dynamic_sections.elf_plt, __srli(__t1, __t1, 2));
    elf_write_int(dynamic_sections.elf_plt, __lw(__t0, __t0, 4));
    elf_write_int(dynamic_sections.elf_plt, __jalr(__zero, __t3, 0));
    for (int i = 0; i * PLT_ENT_SIZE < end; i++) {
        /* elf_generate() ensures that the .got section is placed a higher
         * memory address than the plt section. As a result, 'ofs' must always
         * be positive.
         *
         * addr_of_plt: the starting address of PLT[N]. (N >= 1) addr_of_got:
         * the starting address of GOT[N + 1].
         */
        addr_of_plt =
            dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE + PLT_ENT_SIZE * i;
        addr_of_got = dynamic_sections.elf_got_start + PTR_SIZE * (i + 2);
        ofs = addr_of_got - addr_of_plt;

        /* In RISC-V ABI, a PLT stub takes up 4 instructions to load GOT[N + 2]:
         *
         * 1: auipc t3, %pcrel_hi(function@.got)
         *    lw     t3, %pcrel_lo(1b)(t3)
         *    jalr   t1, t3
         *    nop
         *
         * Each PLT stub uses auipc and lw instructions to perform a PC-relative
         * addressing to obtain GOT[N + 1], and then perform an unconditional
         * jump.
         *
         * +-------------------------------------+----------------------------+
         * | Instruction                         | Contents of registers      |
         * +-------------------------------------+----------------------------+
         * | auipc  t3, %pcrel_hi(function@.got) | t1: <dont'care>            |
         * |                                     | t3: pcrel_hi(function%got) |
         * +-------------------------------------+----------------------------+
         * | lw     t3, %pcrel_lo(1b)(t3)        | t1: <dont'care>            |
         * |                                     | t3: GOT[N + 1]             |
         * +-------------------------------------+----------------------------+
         * | jalr   t1, t3                       | t1: addr of nop            |
         * |                                     | t3: GOT[N + 1]             |
         * +-------------------------------------+----------------------------+
         */
        pcrel_hi = ofs & ~0xFFF;
        pcrel_lo = ofs & 0xFFF;
        if (pcrel_lo > 2047) {
            pcrel_hi += 0x1000;
            pcrel_lo -= 0x1000;
        }

        elf_write_int(dynamic_sections.elf_plt, __auipc(__t3, pcrel_hi));
        elf_write_int(dynamic_sections.elf_plt, __lw(__t3, __t3, pcrel_lo));
        elf_write_int(dynamic_sections.elf_plt, __jalr(__t1, __t3, 0));
        elf_write_int(dynamic_sections.elf_plt, __addi(__zero, __zero, 0));
    }
}
