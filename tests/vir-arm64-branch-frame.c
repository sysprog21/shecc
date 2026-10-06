#define main compiler_main
#include "../src/main.c"
#undef main
#include <assert.h>

int main(void)
{
    opcode_t ops[] = {OP_eq, OP_neq, OP_lt, OP_geq, OP_gt, OP_leq};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        for (int width = 4; width <= 8; width += 4)
            for (int uns = 0; uns < 2; uns++)
                for (int layout = 0; layout < 3; layout++) {
                    basic_block_t bb = {0}, yes = {0}, no = {0};
                    ph2_ir_t compare = {0}, branch = {0};
                    compare.op = ops[i];
                    compare.src0 = 1;
                    compare.src1 = 2;
                    compare.dest = 3;
                    compare.size_bytes = width;
                    compare.src0_is_unsigned = uns;
                    compare.next = &branch;
                    branch.op = OP_branch;
                    branch.src0 = 3;
                    branch.src2 = layout;
                    branch.then_bb = &yes;
                    branch.else_bb = &no;
                    yes.elf_offset = 64;
                    no.elf_offset = 128;
                    assert(a64_branch_only_compare(&bb, &compare));
                    bb.machine_liveout = 1U << 3;
                    assert(!a64_branch_only_compare(&bb, &compare));
                    bb.machine_liveout = 0;
                    branch.src0 = 4;
                    assert(!a64_branch_only_compare(&bb, &compare));
                    branch.src0 = 3;
                    compare.src2 = 1;
                    branch.src1 = a64_cond(ops[i], uns) + 1;
                    elf_code = strbuf_create(32);
                    emit_ph2_ir(&compare);
                    assert(elf_code->size == 4);
                    emit_ph2_ir(&branch);
                    assert(elf_code->size == (layout ? 8 : 12));
                    unsigned instruction = 0;
                    memcpy(&instruction, elf_code->elements + 4, 4);
                    assert((instruction & 0xff000010U) == 0x54000000U);
                    assert((instruction & 15) ==
                           (unsigned) (a64_cond(ops[i], uns) ^ (layout == 2)));
                    strbuf_free(elf_code);
                }
    int frames[] = {0, 16, 4080, 4096, 65536};
    for (unsigned i = 0; i < sizeof(frames) / sizeof(frames[0]); i++)
        for (int subtract = 0; subtract < 2; subtract++) {
            elf_code = strbuf_create(32);
            a64_adjust_frame(frames[i], subtract);
            assert(elf_code->size == (frames[i] == 0     ? 0
                                      : frames[i] < 4096 ? 4
                                                         : 12));
            strbuf_free(elf_code);
        }
    for (int used = 0; used <= CALLEE_SAVED_REGS; used++)
        for (int gp = 0; gp <= 1; gp++) {
            int saved = used | (gp ? A64_SAVE_GP : 0);
            assert(a64_save_bytes(saved) == 16 + ((used + gp + 1) / 2) * 16);
            for (int index = 0; index < used; index++)
                assert(a64_saved_reg(saved, index) == 20 + index);
            assert(a64_saved_reg(saved, used) == (gp ? A64_GP : A64_ZR));
            assert(a64_saved_reg(saved, used + 1) == A64_ZR);
        }
    return 0;
}
