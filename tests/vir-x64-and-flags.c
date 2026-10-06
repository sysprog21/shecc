#define main compiler_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void check_flags(int width, int mode, int mask)
{
    basic_block_t bb = {0}, other = {0};
    ph2_ir_t producer = {0}, gap = {0}, compare = {0}, branch = {0};
    producer.op = mode == 6 ? OP_bit_or : OP_bit_and;
    producer.dest = 0;
    producer.src0 = 2;
    producer.src1 = 3;
    producer.size_bytes = mode == 3 ? (width == 4 ? 8 : 4) : width;
    producer.src0_hi = producer.src1_hi = -1;
    gap.op = mode == 2 ? OP_assign : OP_load_constant;
    gap.dest = mode == 4 ? 0 : mode == 2 ? 6 : 1;
    gap.src0 = mode == 2 ? 7 : 0;
    gap.size_bytes = 4;
    gap.src0_hi = gap.src1_hi = -1;
    compare.op = OP_eq;
    compare.dest = 4;
    compare.src0 = 0;
    compare.src1 = mode == 4 ? 0 : 1;
    compare.size_bytes = width;
    compare.src0_hi = compare.src1_hi = -1;
    branch.op = OP_branch;
    branch.src0 = 4;
    bool has_gap = mode == 1 || mode == 2 || mode == 4;
    ph2_ir_t *irs[] = {&producer, has_gap ? &gap : &compare,
                       has_gap ? &compare : &branch, &branch};
    basic_block_t *bbs[] = {mode == 5 ? &other : &bb, &bb, &bb, &bb};
    PH2_IR_FLATTEN = irs;
    ph2_ir_idx = has_gap ? 4 : 3;
    instruction_to_bb = bbs;
    instruction_to_bb_capacity = ph2_ir_idx;
    elf_code = strbuf_create(128);
    folds_off = false;
    frame_mirror_reset();
    const_track_reset();
    shift_cache_reset();
    for (int i = 0; i < MAX_SKIP_IR; i++)
        skip_ir_index[i] = -1;
    addr_fold = addr_sib = fused_cc_pending = false;
    gaddr_pending_ir = src0_override = cmp_mem_slot = -1;
    const_reg_valid[1] = const_reg_valid[3] = true;
    const_reg_val[1] = 0;
    const_reg_val[3] = mask;
    emit_ir_index = 0;
    emit_next_ir = irs[1];
    emit_ph2_ir(&producer);
    if (has_gap) {
        emit_ir_index = 1;
        emit_next_ir = &compare;
        emit_ph2_ir(&gap);
    }
    if (mode == 7)
        cmp_mem_slot = 16;
    if (mode == 8)
        folds_off = true;
    emit_ir_index = has_gap ? 2 : 1;
    emit_next_ir = &branch;
    int before = elf_code->size;
    emit_ph2_ir(&compare);
    assert(fused_cc_pending && fused_cc == 0x84);
    assert((elf_code->size == before) == (mode == 0 || mode == 1));
    assert(!cmp_imm_known);
    strbuf_free(elf_code);
}

int main(void)
{
    for (int width = 4; width <= 8; width += 4)
        for (int mode = 0; mode < 9; mode++) {
            check_flags(width, mode, 0);
            check_flags(width, mode, 255);
            check_flags(width, mode, -1);
        }
    return 0;
}
