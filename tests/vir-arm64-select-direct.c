#define main compiler_main
#include "../src/main.c"
#undef main
#include <assert.h>

static void diamond(int bad)
{
    func_t f = {0};
    basic_block_t b = {0}, t = {0}, e = {0}, j = {0};
    bb_connection_t tp[2] = {{0}}, ep[1] = {{0}};
    vir_mop_t cmp = {0}, branch = {0}, a = {0}, c = {0};
    vir_mop_t tj = {0}, ej = {0}, extra = {0};
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
    b.mops.head = &cmp;
    b.mops.tail = &branch;
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
    t.mops.head = &a;
    t.mops.tail = &tj;
    e.mops.head = &c;
    e.mops.tail = &ej;
    t.machine_liveout = e.machine_liveout = 1U << 5;
    if (bad == -1) {
        extra.op = OP_bit_xor;
        extra.dest = 6;
        extra.src0 = 0;
        extra.src1 = 1;
        extra.next = &a;
        t.mops.head = &extra;
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
        t.mops.head = &extra;
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
        j.mops.head = j.mops.tail = &extra;
    }
    if (bad == 9) {
        extra.op = OP_load_func;
        j.mops.head = j.mops.tail = &extra;
    }
    if (bad == 10)
        c.dest = 6;
    if (bad == 11) {
        extra.op = OP_add;
        extra.dest = 6;
        extra.src0 = 0;
        extra.src1 = 1;
        extra.next = &a;
        t.mops.head = &extra;
        a.src0 = 6;
        a.next = &c;
        c.src0 = 6;
        c.next = &tj;
    }
    a64_select_diamonds(&f);
    assert((branch.op == OP_jump) == (bad <= 0));
    if (bad <= 0) {
        assert(cmp.src2 == 1);
        vir_mop_t *x = cmp.next;
        if (bad == -1) {
            assert(x->dest == REG_CNT && x->op == OP_bit_xor);
            x = x->next;
            assert(x->src0 == REG_CNT);
        }
        assert(x->dest == REG_CNT && x->op == OP_add);
        x = x->next;
        assert(x->dest == REG_CNT + 1 && x->op == OP_sub);
        assert(x->next->op == OP_ternary && x->next->dest == 5);
        assert(!t.mops.head && !e.mops.head);
    }
}

/* Fold @mask as the literal operand of each logical operation at the width
 * @wide, and print the instruction for each one that folds.
 */
static void mask_fold(int wide, unsigned long long mask, bool encodable)
{
    opcode_t ops[] = {OP_bit_and, OP_bit_or, OP_bit_xor};
    for (unsigned op = 0; op < sizeof(ops) / sizeof(ops[0]); op++) {
        func_t f = {0};
        basic_block_t b = {0};
        vir_mop_t literal = {0}, use = {0};
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
        b.mops.head = &literal;
        b.mops.tail = &use;
        arm64_fold_immediates(&f);
        assert((b.mops.head == &use) == encodable);
        assert(encodable ? use.src2 > 0 && use.src1 == -1 : use.src2 == 0);
        if (encodable) {
            int opcode = op == 0   ? 0x12000000
                         : op == 1 ? 0x32000000
                                   : 0x52000000;
            printf("%08x\n", (unsigned) a64_logic_imm_insn(opcode, wide, 2, 0,
                                                           use.src2 - 1));
        }
    }
}

/* Every run of low ones, then patterns that only the general bitmask encoding
 * reaches: single bits, rotated runs and repeated elements. tests/
 * vir-arm64-logical-gas.py assembles the same list in the same order.
 */
static void masks(void)
{
    static const unsigned long long w_patterns[] = {
        0x8, 0xff00, 0x80000001, 0xfffffff8, 0x0f0f0f0f, 0xaaaaaaaa, 0x7ffffffe,
    };
    static const unsigned long long x_patterns[] = {
        0x8,
        0xff00,
        0x8000000000000001ULL,
        0xfffffffffffffff8ULL,
        0x5555555555555555ULL,
        0xffff0000ffff0000ULL,
        0x7ffffffffffffffeULL,
        0x00000000fffffff0ULL,
    };
    for (int wide = 0; wide <= 1; wide++) {
        int bits = wide ? 64 : 32;
        for (int count = 0; count <= bits + 1; count++) {
            unsigned long long mask = count == 0    ? 0
                                      : count >= 64 ? ~0ULL
                                                    : (1ULL << count) - 1;
            if (count == bits + 1)
                mask = 5;
            mask_fold(wide, mask, count > 0 && count < bits);
        }
        const unsigned long long *patterns = wide ? x_patterns : w_patterns;
        int n = wide ? sizeof(x_patterns) / sizeof(x_patterns[0])
                     : sizeof(w_patterns) / sizeof(w_patterns[0]);
        for (int i = 0; i < n; i++)
            mask_fold(wide, patterns[i], true);
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
