#define main compiler_main
#include "../src/main.c"
#undef main
#include <assert.h>

int main(int argc, char **argv)
{
    FILE *oracle = argc > 1 ? fopen(argv[1], "wb") : NULL;
    if (argc > 1)
        assert(oracle);
    unsigned int loads[4][2] = {{0x38a06800U, 0x38606800U},
                                {0x78a06800U, 0x78606800U},
                                {0xb8a06800U, 0xb8606800U},
                                {0xf8606800U, 0xf8606800U}};
    for (int b = 0; b < 4; b++)
        for (int read = 0; read < 2; read++)
            for (int u = 0; u < 2; u++)
                for (int scaled = 0; scaled < 2; scaled++)
                    for (int word_index = 0; word_index < 2; word_index++) {
                        basic_block_t bb = {0};
                        func_t f = {0};
                        f.bbs = &bb;
                        ph2_ir_t extend = {0}, add = {0}, mem = {0};
                        add.op = OP_add;
                        add.src0 = 0;
                        add.src1 = 23;
                        add.dest = 2;
                        add.src0_hi = add.src1_hi = add.dest_hi = -1;
                        add.size_bytes = 8;
                        add.src2 = scaled ? -(b + 1) : 0;
                        mem.op = read ? OP_read : OP_write;
                        mem.src0 = 2;
                        mem.src1 = read ? (1 << b) : 3;
                        mem.dest = read ? 3 : (1 << b);
                        mem.is_unsigned = u;
                        mem.src0_hi = mem.src1_hi = mem.dest_hi = -1;
                        mem.is_volatile = true;
                        add.next = &mem;
                        bb.ph2_ir_list.head = &add;
                        bb.ph2_ir_list.tail = &mem;
                        if (word_index) {
                            extend.op = OP_sign_ext;
                            extend.src0 = 22;
                            extend.dest = 23;
                            extend.src1 = (4 << 16) | 8;
                            extend.src0_is_unsigned = true;
                            extend.src0_hi = extend.src1_hi = extend.dest_hi =
                                -1;
                            extend.next = &add;
                            bb.ph2_ir_list.head = &extend;
                        }
                        arm64_fold_immediates(&f);
                        assert(bb.ph2_ir_list.head == &mem && mem.src0 == 0);
                        assert(mem.src0_hi == (word_index ? 22 : 23) &&
                               mem.is_volatile);
                        assert(ir_reads_reg(&mem, word_index ? 22 : 23));
                        assert(func_highest_used_reg(&f, A64_FIRST_CALLEE) ==
                               (word_index ? 22 : 23));
                        elf_code = strbuf_create(32);
                        assert(elf_code);
                        emit_ph2_ir(&mem);
                        unsigned int word = 0;
                        memcpy(&word, elf_code->elements, 4);
                        unsigned int expected =
                            (read ? loads[b][u]
                                  : 0x38206800U | ((unsigned int) b << 30)) |
                            ((word_index ? 27U : 28U) << 16) |
                            ((scaled && b ? 1U : 0U) << 12) | 3U;
                        if (word_index)
                            expected &= ~(1U << 13);
                        assert(elf_code->size == 4 && word == expected);
                        if (oracle)
                            assert(fwrite(&word, 4, 1, oracle) == 1);
                        free(elf_code->elements);
                        free(elf_code);
                        elf_code = NULL;
                    }
    for (int offset = 0; offset < 2; offset++) {
        basic_block_t bb = {0};
        func_t f = {0};
        f.bbs = &bb;
        ph2_ir_t add = {0}, mem = {0};
        add.op = OP_add;
        add.src0 = 0;
        add.src1 = 1;
        add.dest = 2;
        add.size_bytes = 8;
        add.src0_hi = add.src1_hi = add.dest_hi = -1;
        mem.op = OP_read;
        mem.src0 = 2;
        mem.src1 = 8;
        mem.dest = 3;
        mem.dest_hi = mem.src1_hi = -1;
        mem.src0_hi = offset ? -1 : 4;
        mem.src2 = offset ? 8 : -1;
        add.next = &mem;
        bb.ph2_ir_list.head = &add;
        arm64_fold_immediates(&f);
        assert(bb.ph2_ir_list.head == &add);
    }
    basic_block_t bb = {0};
    func_t f = {0};
    f.bbs = &bb;
    ph2_ir_t mem = {0};
    mem.op = OP_read;
    mem.dest = 0;
    mem.src0 = 0;
    mem.src1 = 8;
    mem.dest_hi = mem.src0_hi = mem.src1_hi = REG_CNT;
    bb.ph2_ir_list.head = &mem;
    assert(func_highest_used_reg(&f, A64_FIRST_CALLEE) == A64_FIRST_CALLEE - 1);
    for (int reject = 0; reject < 5; reject++) {
        basic_block_t block = {0};
        func_t func = {0};
        func.bbs = &block;
        ph2_ir_t extend = {0}, memory = {0};
        extend.op = OP_sign_ext;
        extend.src0 = 1;
        extend.dest = 2;
        extend.src1 = (4 << 16) | 8;
        extend.src0_is_unsigned = reject != 0;
        extend.is_pointer = reject == 1;
        extend.dest_hi = extend.src0_hi = extend.src1_hi = -1;
        memory.op = reject == 3 ? OP_write : OP_read;
        memory.dest = reject == 3 ? 8 : 3;
        memory.src0 = reject == 2 ? 2 : 0;
        memory.src1 = reject == 3 ? 2 : 8;
        memory.src0_hi = 2;
        memory.src1_hi = memory.dest_hi = -1;
        memory.src2 = -4;
        extend.next = &memory;
        block.ph2_ir_list.head = &extend;
        if (reject == 4)
            block.machine_liveout = 1U << 2;
        arm64_fold_immediates(&func);
        assert(block.ph2_ir_list.head == &extend && memory.src2 == -4);
    }
    basic_block_t loop = {0};
    func_t loop_func = {0};
    loop_func.bbs = &loop;
    ph2_ir_t literal = {0}, indexed = {0};
    literal.op = OP_load_constant;
    literal.dest = 0;
    literal.src0 = 3;
    literal.size_bytes = 8;
    literal.dest_hi = literal.src0_hi = literal.src1_hi = -1;
    indexed.op = OP_read;
    indexed.dest = 2;
    indexed.src0 = 1;
    indexed.src1 = 4;
    indexed.src0_hi = 0;
    indexed.src2 = -3;
    indexed.dest_hi = indexed.src1_hi = -1;
    literal.next = &indexed;
    loop.ph2_ir_list.head = &literal;
    loop.ph2_ir_list.tail = &indexed;
    loop.next = &loop;
    BB_ARENA = arena_init(SMALL_ARENA_SIZE);
    a64_hoist_constants(&loop_func);
    assert(loop.ph2_ir_list.head->op == OP_load_constant);
    assert(indexed.src0_hi == loop.ph2_ir_list.head->dest);
    assert(indexed.src0_hi >= A64_FIRST_CALLEE);
    if (oracle)
        assert(fclose(oracle) == 0);
    return 0;
}
