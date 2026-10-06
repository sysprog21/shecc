#define main compiler_main
#include "../src/main.c"
#undef main
#include <assert.h>
static void check_branch_fallthrough(void)
{
    for (int condition = 0x80; condition <= 0x8f; condition++)
        for (int fallthrough = 0; fallthrough < 3; fallthrough++) {
            ph2_ir_t then_ir = {0}, else_ir = {0};
            then_ir.op = else_ir.op = OP_return;
            basic_block_t then_bb = {0}, else_bb = {0};
            then_bb.ph2_ir_list.head = &then_ir;
            else_bb.ph2_ir_list.head = &else_ir;
            emit_next_ir = fallthrough == 1   ? &then_ir
                           : fallthrough == 2 ? &else_ir
                                              : NULL;
            elf_code = strbuf_create(32);
            emit_fell_through = true;
            emit_conditional_edges(condition, &then_bb, &else_bb);
            assert(elf_code->size == (fallthrough ? 6 : 11));
            assert((unsigned char) elf_code->elements[1] ==
                   (condition ^ (fallthrough == 1)));
            assert(emit_fell_through == (fallthrough != 0));
            strbuf_free(elf_code);
        }
}

int main(void)
{
    check_branch_fallthrough();
    basic_block_t bb = {0};
    ph2_ir_t sum = {0}, read = {0}, ret = {0};
    sum.op = OP_add;
    sum.src0 = 0;
    sum.src1 = 1;
    sum.dest = 2;
    read.op = OP_read;
    read.src0 = 2;
    read.dest = 3;
    ret.op = OP_return;
    ret.src0 = -1;
    ph2_ir_t *irs[] = {&sum, &read, &ret};
    basic_block_t *bbs[] = {&bb, &bb, &bb};
    PH2_IR_FLATTEN = irs;
    ph2_ir_idx = 3;
    instruction_to_bb = bbs;
    instruction_to_bb_capacity = 3;
    emit_ir_index = 0;
    int effective = 13, second = map_ir_reg(1);
    assert(try_fold_sib_add(&sum, effective, second));
    assert(addr_sib_base == effective);
    assert(addr_sib_index == second);
    addr_sib = false;
    effective = 4;
    assert(try_fold_sib_add(&sum, effective, second));
    assert(addr_sib_index == second);
    assert(addr_sib_base == 4);
    addr_sib = false;
    assert(try_fold_sib_add(&sum, 13, 12));
    assert(addr_sib_base == 12 && addr_sib_index == 13);
    addr_sib = false;
    assert(!try_fold_sib_add(&sum, 13, 13));
    sib_address_t a;
    assert(sib_add_address(&sum, &bb, 0, sum.src1, 13, 12, &a));
    assert(a.base == 15 && a.index == 13);
    assert(sib_add_address(&sum, &bb, 0, sum.src0, 13, 11, &a));
    assert(a.base == 15 && a.index == 11);
    assert(!sib_add_address(&sum, &bb, 0, sum.src1, 4, 12, &a));
    for (int i = 0; i < MAX_SKIP_IR; i++)
        skip_ir_index[i] = -1;
    sum.size_bytes = 8;
    ph2_ir_t load = {0};
    load.op = OP_load;
    load.dest = 0;
    load.src0 = 32;
    load.size_bytes = 8;
    ph2_ir_t *pipeline[] = {&load, &sum, &read, &ret};
    basic_block_t *pipeline_bbs[] = {&bb, &bb, &bb, &bb};
    sum.src0_hi = sum.src1_hi = read.src0_hi = read.src1_hi = ret.src0_hi =
        ret.src1_hi = load.src0_hi = load.src1_hi = -1;
    PH2_IR_FLATTEN = pipeline;
    ph2_ir_idx = 4;
    instruction_to_bb = pipeline_bbs;
    instruction_to_bb_capacity = 4;
    elf_code = strbuf_create(128);
    frame_mirror_reset();
    reg_mirror_valid[9] = true;
    reg_mirror_slot[9] = 32;
    reg_mirror_size[9] = 8;
    reg_mirror_sext[9] = false;
    gaddr_pending_ir = -1;
    src0_override = -1;
    emit_ir_index = 0;
    emit_next_ir = &sum;
    emit_ph2_ir(&load);
    assert(src0_override == map_ir_reg(9));
    emit_ir_index = 1;
    emit_next_ir = &read;
    emit_ph2_ir(&sum);
    assert(addr_sib && addr_sib_base == map_ir_reg(9) &&
           addr_sib_index == map_ir_reg(1));
    strbuf_free(elf_code);
    return 0;
}
