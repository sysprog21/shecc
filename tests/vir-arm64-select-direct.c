#define main compiler_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void diamond(int bad)
{
    func_t f = {0};
    basic_block_t b = {0}, t = {0}, e = {0}, j = {0};
    bb_connection_t tp[2] = {{0}}, ep[1] = {{0}};
    ph2_ir_t cmp = {0}, branch = {0}, a = {0}, c = {0};
    ph2_ir_t tj = {0}, ej = {0}, extra = {0};
    f.bbs = &b;
    b.rpo_next = &t;
    t.rpo_next = &e;
    e.rpo_next = &j;
    tp[0].bb = ep[0].bb = &b;
    t.prev = tp;
    e.prev = ep;
    t.prev_idx = e.prev_idx = 1;
    cmp.op = OP_lt;
    cmp.src0 = 0;
    cmp.src1 = 1;
    cmp.dest = 2;
    cmp.next = &branch;
    branch.op = OP_branch;
    branch.src0 = 2;
    branch.then_bb = &t;
    branch.else_bb = &e;
    b.ph2_ir_list.head = &cmp;
    b.ph2_ir_list.tail = &branch;
    a.op = OP_add;
    a.src0 = 3;
    a.src1 = 4;
    a.dest = 5;
    c.op = OP_sub;
    c.src0 = 3;
    c.src1 = 4;
    c.dest = 5;
    a.next = &tj;
    c.next = &ej;
    tj.op = ej.op = OP_jump;
    tj.next_bb = ej.next_bb = &j;
    t.ph2_ir_list.head = &a;
    t.ph2_ir_list.tail = &tj;
    e.ph2_ir_list.head = &c;
    e.ph2_ir_list.tail = &ej;
    t.machine_liveout = e.machine_liveout = 1U << 5;
    if (bad == -1) {
        extra.op = OP_bit_xor;
        extra.dest = 6;
        extra.src0 = 0;
        extra.src1 = 1;
        extra.next = &a;
        t.ph2_ir_list.head = &extra;
        a.src0 = 6;
    }
    if (bad == 1)
        a.op = OP_read;
    if (bad == 2)
        a.op = OP_write;
    if (bad == 3)
        a.op = OP_call;
    if (bad == 4) {
        t.machine_liveout |= 1U << 6;
        extra.op = OP_add;
        extra.dest = 6;
        extra.src0 = 0;
        extra.src1 = 1;
        extra.next = &a;
        t.ph2_ir_list.head = &extra;
    }
    if (bad == 5)
        tj.op = OP_branch;
    if (bad == 6) {
        b.machine_liveout = 1U << 2;
    }
    if (bad == 7) {
        tp[1].bb = &j;
        t.prev_idx = 2;
    }
    if (bad == 8) {
        extra.op = OP_indirect;
        j.ph2_ir_list.head = j.ph2_ir_list.tail = &extra;
    }
    if (bad == 9) {
        extra.op = OP_load_func;
        j.ph2_ir_list.head = j.ph2_ir_list.tail = &extra;
    }
    if (bad == 10)
        c.dest = 6;
    if (bad == 11) {
        extra.op = OP_add;
        extra.dest = 6;
        extra.src0 = 0;
        extra.src1 = 1;
        extra.next = &a;
        t.ph2_ir_list.head = &extra;
        a.src0 = 6;
        a.next = &c;
        c.src0 = 6;
        c.next = &tj;
    }
    a64_select_diamonds(&f);
    assert((branch.op == OP_jump) == (bad <= 0));
    if (bad <= 0) {
        assert(cmp.src2 == 1);
        ph2_ir_t *x = cmp.next;
        if (bad == -1) {
            assert(x->dest == REG_CNT && x->op == OP_bit_xor);
            x = x->next;
            assert(x->src0 == REG_CNT);
        }
        assert(x->dest == REG_CNT && x->op == OP_add);
        x = x->next;
        assert(x->dest == REG_CNT + 1 && x->op == OP_sub);
        assert(x->next->op == OP_ternary && x->next->dest == 5);
        assert(!t.ph2_ir_list.head && !e.ph2_ir_list.head);
    }
}

static void masks(void)
{
    opcode_t ops[] = {OP_bit_and, OP_bit_or, OP_bit_xor};
    for (int wide = 0; wide <= 1; wide++)
        for (unsigned op = 0; op < sizeof(ops) / sizeof(ops[0]); op++)
            for (int count = 0; count <= (wide ? 64 : 32) + 1; count++) {
                func_t f = {0};
                basic_block_t b = {0};
                ph2_ir_t literal = {0}, use = {0};
                unsigned long long mask = count == 0    ? 0
                                          : count >= 64 ? ~0ULL
                                                        : (1ULL << count) - 1;
                if (count == (wide ? 64 : 32) + 1)
                    mask = 5;
                literal.op = OP_load_constant;
                literal.dest = 1;
                literal.size_bytes = wide ? 8 : 4;
                literal.is_unsigned = true;
                literal.src0 = (int) mask;
                literal.src1 = (int) (mask >> 32);
                literal.next = &use;
                use.op = ops[op];
                use.src0 = 0;
                use.src1 = 1;
                use.dest = 2;
                use.size_bytes = wide ? 8 : 4;
                f.bbs = &b;
                b.ph2_ir_list.head = &literal;
                b.ph2_ir_list.tail = &use;
                arm64_fold_immediates(&f);
                bool folded = count > 0 && count < (wide ? 64 : 32);
                assert((b.ph2_ir_list.head == &use) == folded);
                assert(use.src2 == (folded ? count : 0));
                if (folded) {
                    int opcode = op == 0   ? 0x12000000
                                 : op == 1 ? 0x32000000
                                           : 0x52000000;
                    printf("%08x\n", (unsigned) a64_logic_imm_insn(
                                         opcode, wide, 2, 0, count));
                }
            }
}

int main(void)
{
    BB_ARENA = arena_init(4096);
    for (int bad = -1; bad <= 11; bad++)
        diamond(bad);
    masks();
    return 0;
}
