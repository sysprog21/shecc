#ifdef SHECC_VIR_LOWER_TEST
static void vir_lower_test_hook(void);
#endif
#define main compiler_main
#ifdef SHECC_VIR_LOWER_TEST
#include VIR_LOWER_TEST_MAIN
#else
#include "../src/main.c"
#endif
#undef main
#include <assert.h>
#include <stdint.h>

static bool install_machine;

static void install_function(const char *name, vir_machine_function_t *machine)
{
    if (!install_machine)
        return;
    func_t *function = find_func((char *) name);
    assert(function);
    bool listed = false;
    for (func_t *item = FUNC_LIST.head; item; item = item->next)
        if (item == function)
            listed = true;
    if (!listed) {
        FUNC_LIST.tail->next = function;
        FUNC_LIST.tail = function;
        function->next = NULL;
    }
    function->bbs = machine->entry;
    function->stack_size = machine->stack_size;
    for (basic_block_t *block = machine->entry; block; block = block->rpo_next)
        block->belong_to = function;
}

static unsigned long long registers[REG_CNT];
static unsigned char memory[65536];

static unsigned long long machine_value(int low, int high)
{
    return high < 0 ? registers[low]
                    : (unsigned int) registers[low] |
                          ((unsigned long long) (unsigned int) registers[high]
                           << 32);
}

static void machine_write(int low, int high, unsigned long long value)
{
    registers[low] = high < 0 ? value : (unsigned int) value;
    if (high >= 0)
        registers[high] = (unsigned int) (value >> 32);
}

static unsigned long long argument_word(int word)
{
    unsigned long long value = 0;
    if (word < MAX_ARGS_IN_REG)
        return registers[word];
    memcpy(&value, memory + (word - MAX_ARGS_IN_REG) * PTR_SIZE, PTR_SIZE);
    return value;
}

static unsigned long long run_machine(vir_machine_function_t *function)
{
    basic_block_t *block = function->entry;
    int steps = 0;
    while (block && ++steps < 1000) {
        for (ph2_ir_t *instruction = block->ph2_ir_list.head; instruction;
             instruction = instruction->next) {
            unsigned long long left =
                instruction->src0 >= 0 && instruction->src0 < REG_CNT
                    ? machine_value(instruction->src0, instruction->src0_hi)
                    : 0;
            unsigned long long right =
                instruction->src1 >= 0 && instruction->src1 < REG_CNT
                    ? machine_value(instruction->src1, instruction->src1_hi)
                    : 0;
            unsigned long long result = 0;
            int width = instruction->size_bytes;
            switch (instruction->op) {
            case OP_load_constant:
                result = (unsigned int) instruction->src0 |
                         ((unsigned long long) (unsigned int) instruction->src1
                          << 32);
                break;
            case OP_add:
                result = left + right;
                break;
            case OP_sub:
                result = left - right;
                break;
            case OP_mul:
                result = left * right;
                break;
            case OP_eq:
                result = left == right;
                break;
            case OP_lt:
                result = instruction->src0_is_unsigned ? left < right
                         : width == 8 ? (long long) left < (long long) right
                                      : (int) left < (int) right;
                break;
            case OP_assign:
                result = left;
                break;
            case OP_address_of:
                result = 4096 + instruction->src0;
                break;
            case OP_load:
                assert(instruction->src0 >= 0 &&
                       instruction->src0 + width <= (int) sizeof(memory));
                memcpy(&result, memory + instruction->src0, width);
                break;
            case OP_store:
                assert(instruction->src1 >= 0 &&
                       instruction->src1 + width <= (int) sizeof(memory));
                memcpy(memory + instruction->src1, &left, width);
                continue;
            case OP_read:
                memcpy(&result, memory + (left - 4096), instruction->src1);
                break;
            case OP_write:
                memcpy(memory + (left - 4096), &right, width);
                continue;
            case OP_sign_ext: {
                int source = instruction->src1 >> 16;
                if (instruction->src0_is_unsigned)
                    result = left &
                             (source == 8 ? ~0ull : (1ull << (source * 8)) - 1);
                else if (source == 1)
                    result = (signed char) left;
                else if (source == 2)
                    result = (short) left;
                else if (source == 4)
                    result = (int) left;
                else
                    result = left;
                break;
            }
            case OP_trunc:
                result = left & ((1ull << (instruction->src1 * 8)) - 1);
                break;
            case OP_call:
                if (!strcmp(instruction->func_name, "test_add"))
                    result = machine_value(0, PTR_SIZE == 4 ? 1 : -1) +
                             machine_value(PTR_SIZE == 4 ? 2 : 1,
                                           PTR_SIZE == 4 ? 3 : -1);
                else if (!strcmp(instruction->func_name, "test_va")) {
                    result = PTR_SIZE == 4 ? machine_value(2, 3) : registers[1];
                } else {
                    assert(!strcmp(instruction->func_name, "test_mix"));
                    result = 0;
                    for (int word = 0; word < 7; word++)
                        result += (int) argument_word(word);
                    int cursor = 7;
                    for (int index = 0; index < 1; index++) {
                        if (PTR_SIZE == 4 &&
                            (ELF_MACHINE != 0xf3 || cursor >= MAX_ARGS_IN_REG))
                            cursor = ALIGN_UP(cursor, 2);
                        unsigned long long value = argument_word(cursor++);
                        if (PTR_SIZE == 4)
                            value = (unsigned int) value |
                                    (argument_word(cursor++) << 32);
                        result += value;
                    }
                }
                memset(registers, 0xa5, sizeof(registers));
                machine_write(0, PTR_SIZE == 4 ? 1 : -1, result);
                continue;
            case OP_jump:
                block = instruction->next_bb;
                goto next_block;
            case OP_branch:
                block = left ? instruction->then_bb : instruction->else_bb;
                goto next_block;
            case OP_return:
                return left;
            default:
                assert(!"unsupported interpreter opcode");
            }
            machine_write(instruction->dest, instruction->dest_hi, result);
        }
        assert(!"unterminated machine block");
    next_block:;
    }
    assert(!"machine loop exceeded limit");
    return 0;
}

static vir_call_signature_t signature(vir_type_t result)
{
    vir_call_signature_t type;
    memset(&type, 0, sizeof(type));
    type.result.type = result;
    type.va_start_slot = (unsigned int) -1;
    return type;
}

static void test_strength_reduction_overflow_gate(void)
{
    vir_function_t function;
    char *error;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *pre = vir_block_create(&function);
    vir_block_t *header = vir_block_create(&function);
    vir_block_t *body = vir_block_create(&function);
    vir_block_t *done = vir_block_create(&function);
    vir_value_t *i = vir_block_add_param(&function, header, VIR_TYPE_I32);
    vir_value_t *zero = vir_const_i32(&function, pre, 0);
    vir_value_t *initial[] = {zero};
    assert(vir_edge_create(&function, pre, header, initial, 1));
    vir_value_t *bound = vir_const_i32(&function, header, INT_MAX);
    vir_edge_args_t yes = {body, NULL, 0}, no = {done, NULL, 0};
    assert(vir_block_set_branch(
        &function, header, vir_slt(&function, header, i, bound), &yes, &no));
    vir_value_t *scaled =
        vir_mul(&function, body, i, vir_const_i32(&function, body, 4));
    vir_type_t width = PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32;
    vir_value_t *wide = width == VIR_TYPE_I64
                            ? vir_sext(&function, body, scaled, width)
                            : scaled;
    vir_value_t *offset = vir_mul(&function, body, wide,
                                  vir_const_int(&function, body, width, 4));
    vir_value_t *root =
        vir_global_addr(&function, body, "overflow_array", 4, 4);
    assert(vir_store(&function, body, vir_ptradd(&function, body, root, offset),
                     i));
    vir_value_t *next =
        vir_add(&function, body, i, vir_const_i32(&function, body, 1));
    vir_value_t *updated[] = {next};
    assert(vir_edge_create(&function, body, header, updated, 1));
    assert(vir_block_set_return(&function, done, NULL));
    assert(vir_verify(&function, &error));

    /* LP64 must preserve the sign-extension discontinuity when i*4 wraps. ILP32
     * pointer addition wraps identically, so a modular recurrence is safe.
     */
    assert(vir_strength_reduce(&function, VIR_OPT_O1) ==
           (PTR_SIZE == 8 ? 0 : 1));
    assert(vir_verify(&function, &error));
    vir_function_release(&function);
}

static void test_inplace_edge(void)
{
    vir_function_t function;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *from = vir_block_create(&function);
    vir_block_t *to = vir_block_create(&function);
    vir_value_t *argument = vir_block_add_param(&function, from, VIR_TYPE_I64);
    vir_value_t *parameter = vir_block_add_param(&function, to, VIR_TYPE_I64);
    vir_edge_t *edge = vir_edge_create(&function, from, to, &argument, 1);
    assert(edge);
    vir_location_t locations[2] = {{0}};
    basic_block_t *blocks[2];
    vir_lower_t lower = {0};
    lower.function = &function;
    lower.locations = locations;
    lower.blocks = blocks;
    blocks[from->id] = vir_machine_block(&lower);
    blocks[to->id] = vir_machine_block(&lower);
    lower.block = blocks[from->id];
    int fixed = REG_CNT - CALLEE_SAVED_REGS;
    locations[parameter->id].fixed = fixed;
    locations[argument->id].reg = fixed;
    locations[argument->id].high = PTR_SIZE == 4 ? fixed + 1 : -1;
    basic_block_t *tail = lower.tail;
    assert(vir_machine_edge(&lower, edge) == blocks[to->id]);
    assert(lower.tail == tail && !tail->rpo_next);
    assert(lower.block == blocks[from->id]);
    locations[argument->id].reg = 0;
    locations[argument->id].fixed = -1;
    locations[argument->id].high = PTR_SIZE == 4 ? 1 : -1;
    basic_block_t *copy = vir_machine_edge(&lower, edge);
    assert(copy != blocks[to->id] && lower.tail == copy);
    assert(copy->ph2_ir_list.tail->op == OP_jump);
    assert(copy->ph2_ir_list.tail->next_bb == blocks[to->id]);
    assert(lower.block == blocks[from->id]);
    vir_function_release(&function);
}

static void test_affinity_parameter_entry(void)
{
    vir_function_t function;
    vir_function_init(&function, 4096);
    vir_block_t *entry = vir_block_create(&function);
    vir_block_t *next = vir_block_create(&function);
    vir_value_t *old = vir_block_add_param(&function, entry, VIR_TYPE_I32);
    vir_value_t *replacement =
        vir_block_add_param(&function, next, VIR_TYPE_I32);
    vir_value_t *first = vir_add(&function, next, old, replacement);
    assert(first && first->order == 0);
    vir_lower_t lower = {0};
    lower.function = &function;
    int seen[2];
    vir_block_t *work[2];
    assert(vir_machine_interferes(&lower, replacement, old, seen, work));
    vir_function_release(&function);
}

static void test_pair_multiply_aliases(void)
{
    for (int side = 0; side < 2; side++) {
        vir_function_t function;
        vir_machine_function_t machine;
        vir_call_abi_type_t params[] = {{VIR_TYPE_I64, false, false},
                                        {VIR_TYPE_I64, false, false}};
        vir_call_signature_t type = signature(VIR_TYPE_I64);
        type.params = params;
        type.param_count = 2;
        vir_function_init(&function, 4096);
        vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
        vir_block_t *block = vir_block_create(&function);
        vir_value_t *a = vir_block_add_param(&function, block, VIR_TYPE_I64);
        vir_value_t *b = vir_block_add_param(&function, block, VIR_TYPE_I64);
        assert(vir_block_set_return(&function, block,
                                    vir_mul(&function, block, a, b)));
        assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
        if (ELF_MACHINE == 0xf3 && side) {
            int low = -1, high = -1;
            for (ph2_ir_t *instruction = machine.entry->ph2_ir_list.head;
                 instruction; instruction = instruction->next) {
                if (instruction->op == OP_mul) {
                    low = instruction->dest = instruction->src1;
                    high = instruction->dest_hi = instruction->src1_hi;
                } else if (instruction->op == OP_return) {
                    instruction->src0 = low;
                    instruction->src0_hi = high;
                }
            }
            assert(low >= 0 && high >= 0);
        }
        install_function(side ? "vir_mul_right" : "vir_mul_left", &machine);
        vir_function_release(&function);
    }
}

static void test_wide_pointer_offsets(void)
{
    vir_function_t function;
    char *error;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, 64);
    vir_block_t *block = vir_block_create(&function);
    vir_value_t *base = vir_block_add_param(&function, block, VIR_TYPE_PTR);
    vir_value_t *narrow = vir_block_add_param(&function, block, VIR_TYPE_I32);
    vir_value_t *address = vir_ptradd(&function, block, base, narrow);
    assert(address && address->op1->opcode == VIR_OP_SEXT &&
           address->op1->type == VIR_TYPE_I64);
    assert(vir_block_set_return(&function, block, address));
    assert(vir_verify(&function, &error));
    vir_function_release(&function);
    if (PTR_SIZE != 8)
        return;
    vir_machine_function_t machine;
    vir_call_abi_type_t params[] = {{VIR_TYPE_PTR, false, false},
                                    {VIR_TYPE_I64, false, false}};
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    type.params = params;
    type.param_count = 2;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, 64);
    block = vir_block_create(&function);
    base = vir_block_add_param(&function, block, VIR_TYPE_PTR);
    vir_value_t *offset = vir_block_add_param(&function, block, VIR_TYPE_I64);
    address = vir_ptradd(&function, block, base, offset);
    assert(vir_block_set_return(
        &function, block,
        vir_ptrtoint(&function, block, address, VIR_TYPE_I64)));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    install_function("vir_pointer_add", &machine);
    vir_function_release(&function);
}

static void test_va_start_root(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_abi_type_t param = {VIR_TYPE_PTR, false, false};
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    type.params = &param;
    type.param_count = type.fixed_param_count = 1;
    type.is_variadic = true;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *block = vir_block_create(&function);
    assert(vir_block_add_param(&function, block, VIR_TYPE_PTR));
    vir_value_t *start =
        vir_stack_addr(&function, block, 1, PTR_SIZE, PTR_SIZE);
    type.va_start_slot = start->address_slot;
    /* RV32 aligns an unnamed register pair to an even ABI word. */
    vir_type_t width = PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32;
    if (PTR_SIZE == 4)
        start = vir_ptradd(&function, block, start,
                           vir_const_int(&function, block, width, PTR_SIZE));
    vir_value_t *loaded = vir_load(&function, block, start, VIR_TYPE_I64);
    assert(vir_block_set_return(&function, block, loaded));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    install_function("test_va_aggregate", &machine);
    vir_function_release(&function);
}

static void test_pointer_strength_reduction(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_signature_t type = signature(VIR_TYPE_I32);
    char *error;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *pre = vir_block_create(&function);
    vir_block_t *header = vir_block_create(&function);
    vir_block_t *body = vir_block_create(&function);
    vir_block_t *done = vir_block_create(&function);
    vir_value_t *i = vir_block_add_param(&function, header, VIR_TYPE_I32);
    vir_value_t *sum = vir_block_add_param(&function, header, VIR_TYPE_I32);
    vir_value_t *root = vir_stack_addr(&function, pre, 0, 32, 4);
    vir_type_t width = PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32;
    for (int n = 0; n < 8; n++) {
        vir_value_t *offset = vir_const_int(&function, pre, width, n * 4);
        assert(vir_store(&function, pre,
                         vir_ptradd(&function, pre, root, offset),
                         vir_const_i32(&function, pre, n)));
    }
    vir_value_t *zero = vir_const_i32(&function, pre, 0);
    vir_value_t *initial[] = {zero, zero};
    assert(vir_edge_create(&function, pre, header, initial, 2));
    vir_value_t *bound = vir_const_i32(&function, header, 8);
    vir_edge_args_t yes = {body, NULL, 0}, no = {done, NULL, 0};
    assert(vir_block_set_branch(
        &function, header, vir_slt(&function, header, i, bound), &yes, &no));
    vir_value_t *wide =
        width == VIR_TYPE_I64 ? vir_sext(&function, body, i, width) : i;
    vir_value_t *offset = vir_mul(&function, body, wide,
                                  vir_const_int(&function, body, width, 4));
    vir_value_t *address = vir_ptradd(&function, body, root, offset);
    vir_value_t *load = vir_load(&function, body, address, VIR_TYPE_I32);
    vir_value_t *next_i =
        vir_add(&function, body, i, vir_const_i32(&function, body, 1));
    vir_value_t *next_sum = vir_add(&function, body, sum, load);
    vir_value_t *updated[] = {next_i, next_sum};
    assert(vir_edge_create(&function, body, header, updated, 2));
    assert(vir_block_set_return(&function, done, sum));
    assert(vir_verify(&function, &error));
    assert(vir_strength_reduce(&function, VIR_OPT_O0) == 0);
    assert(vir_strength_reduce(&function, VIR_OPT_O1) == 1);
    assert(vir_local_cse(&function, VIR_OPT_O1) >= 0);
    assert(vir_dce(&function, VIR_OPT_O1) >= 0);
    assert(vir_verify(&function, &error));
    assert(header->param_count == 2 &&
           header->last_param->type == VIR_TYPE_PTR);
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    assert(run_machine(&machine) == 28);
    install_function("vir_sr", &machine);
    vir_function_release(&function);
}

static void test_loop_parallel_copy(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_signature_t type = signature(VIR_TYPE_I32);
    char *error;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&function);
    vir_block_t *loop = vir_block_create(&function);
    vir_block_t *done = vir_block_create(&function);
    vir_value_t *a = vir_block_add_param(&function, loop, VIR_TYPE_I32);
    vir_value_t *b = vir_block_add_param(&function, loop, VIR_TYPE_I32);
    vir_value_t *n = vir_block_add_param(&function, loop, VIR_TYPE_I32);
    vir_value_t *x = vir_const_i32(&function, test_entry, 7);
    vir_value_t *y = vir_const_i32(&function, test_entry, 9);
    vir_value_t *zero = vir_const_i32(&function, test_entry, 0);
    vir_value_t *initial[] = {x, y, zero};
    assert(vir_edge_create(&function, test_entry, loop, initial, 3));
    vir_value_t *one = vir_const_i32(&function, loop, 1);
    vir_value_t *count = vir_add(&function, loop, n, one);
    vir_value_t *limit = vir_const_i32(&function, loop, 4);
    vir_value_t *condition =
        vir_binary(&function, loop, VIR_OP_SLT, VIR_TYPE_I1, count, limit);
    vir_value_t *swapped[] = {b, a, count};
    vir_edge_args_t yes = {loop, swapped, 3};
    vir_edge_args_t no = {done, NULL, 0};
    assert(vir_block_set_branch(&function, loop, condition, &yes, &no));
    vir_value_t *scale = vir_const_i32(&function, done, 10);
    vir_value_t *scaled = vir_mul(&function, done, a, scale);
    assert(vir_block_set_return(&function, done,
                                vir_add(&function, done, scaled, b)));
    assert(vir_verify(&function, &error));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    assert(run_machine(&machine) == 97);
    install_function("vir_loop", &machine);
    vir_function_release(&function);
}

static void test_calls_memory_order(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    char *error;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&function);
    vir_value_t *address = vir_stack_addr(&function, test_entry, 0, 8, 8);
    vir_value_t *a = vir_const_int(&function, test_entry, VIR_TYPE_I64,
                                   0x12345678ffffffffull);
    assert(vir_store(&function, test_entry, address, a));
    vir_value_t *b = vir_load(&function, test_entry, address, VIR_TYPE_I64);
    vir_value_t *args[] = {a, b};
    vir_effect_t *call =
        vir_call(&function, test_entry, "test_add", args, 2, VIR_TYPE_I64);
    vir_call_abi_type_t params[2] = {{VIR_TYPE_I64, false, false},
                                     {VIR_TYPE_I64, false, false}};
    vir_call_signature_t called = type;
    called.params = params;
    called.param_count = 2;
    assert(call && vir_call_set_signature(&function, call, &called));
    vir_value_t *after = vir_load(&function, test_entry, address, VIR_TYPE_I64);
    vir_value_t *total = vir_add(&function, test_entry, call->result, after);
    assert(vir_block_set_return(&function, test_entry, total));
    assert(vir_verify(&function, &error));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    assert(run_machine(&machine) == 0x369d036afffffffdull);
    install_function("vir_wide", &machine);
    vir_function_release(&function);
}

static void test_signed_extensions(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&function);
    vir_value_t *a = vir_const_int(&function, test_entry, VIR_TYPE_I8, 255);
    vir_value_t *signed_value =
        vir_sext(&function, test_entry, a, VIR_TYPE_I64);
    assert(vir_block_set_return(&function, test_entry, signed_value));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    assert(run_machine(&machine) == ~0ull);
    install_function("vir_signed", &machine);
    vir_function_release(&function);
}

static void test_mixed_arguments(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&function);
    vir_value_t *a = vir_const_i32(&function, test_entry, -13);
    vir_value_t *b = vir_const_int(&function, test_entry, VIR_TYPE_I64,
                                   0x123456789abcdef0ull);
    vir_value_t *args[8];
    vir_call_abi_type_t params[8];
    args[0] = a;
    for (int index = 0; index < 7; index++) {
        if (index)
            args[index] = vir_const_i32(&function, test_entry, 0);
        params[index] = (vir_call_abi_type_t) {VIR_TYPE_I32, false, false};
    }
    args[7] = b;
    params[7] = (vir_call_abi_type_t) {VIR_TYPE_I64, false, false};
    vir_call_signature_t called = type;
    called.params = params;
    called.param_count = 8;
    vir_effect_t *call =
        vir_call(&function, test_entry, "test_mix", args, 8, VIR_TYPE_I64);
    assert(call && vir_call_set_signature(&function, call, &called));
    assert(vir_block_set_return(&function, test_entry, call->result));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    assert(run_machine(&machine) == 0x123456789abcdee3ull);
    install_function("vir_mixed", &machine);
    vir_function_release(&function);
}

static void test_variadic_call(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&function);
    vir_value_t *a = vir_const_i32(&function, test_entry, 0);
    vir_value_t *b = vir_const_int(&function, test_entry, VIR_TYPE_I64,
                                   0x123456789abcdef0ull);
    vir_value_t *args[] = {a, b};
    vir_call_abi_type_t params[] = {{VIR_TYPE_I32, false, false},
                                    {VIR_TYPE_I64, false, false}};
    vir_call_signature_t called = type;
    called.params = params;
    called.param_count = 2;
    called.is_variadic = true;
    called.fixed_param_count = 1;
    vir_effect_t *call =
        vir_call(&function, test_entry, "test_va", args, 2, VIR_TYPE_I64);
    assert(call && vir_call_set_signature(&function, call, &called));
    assert(vir_block_set_return(&function, test_entry, call->result));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    assert(run_machine(&machine) == 0x123456789abcdef0ull);
    install_function("vir_vararg", &machine);
    vir_function_release(&function);
}

static void test_core_extensions(void)
{
    vir_function_t function;
    char *error;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&function);
    vir_value_t *address = vir_stack_addr(&function, test_entry, 0, 4, 4);
    vir_value_t *value = vir_const_i32(&function, test_entry, 7);
    vir_effect_t *store = vir_store(&function, test_entry, address, value);
    vir_value_t *load = vir_load(&function, test_entry, address, VIR_TYPE_I32);
    vir_effect_t *effect = load->def_effect;
    assert(vir_block_set_return(&function, test_entry, load));
    assert(!vir_effect_remove(&function, effect));
    assert(vir_replace_all_uses(&function, load, value));
    assert(vir_effect_remove(&function, effect));
    assert(vir_effect_remove(&function, store));
    assert(vir_verify(&function, &error));
    vir_function_release(&function);

    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    test_entry = vir_block_create(&function);
    assert(vir_block_set_return(
        &function, test_entry,
        vir_function_addr(&function, test_entry, "test_add")));
    assert(vir_verify(&function, &error));
    vir_function_release(&function);

    vir_function_init(&function, 4096);
    test_entry = vir_block_create(&function);
    vir_ssa_t *ssa = vir_ssa_create(&function);
    assert(ssa);
    assert(vir_ssa_predeclare(ssa, test_entry, 42, VIR_TYPE_I32));
    assert(vir_ssa_discard_variable(ssa, 42));
    assert(!test_entry->param_count && !vir_ssa_read(ssa, test_entry, 42));
    vir_ssa_release(ssa);
    vir_function_release(&function);
}

static void test_reused_homes(void)
{
    vir_function_t function;
    vir_machine_function_t machine;
    vir_call_signature_t type = signature(VIR_TYPE_I32);
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&function);
    vir_value_t *one = vir_const_i32(&function, test_entry, 1);
    vir_value_t *value = vir_const_i32(&function, test_entry, 0);
    for (int index = 0; index < 1000; index++)
        value = vir_add(&function, test_entry, value, one);
    assert(vir_block_set_return(&function, test_entry, value));
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    assert(machine.stack_size == 0);
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    assert(run_machine(&machine) == 1000);
    int spills = 0;
    for (ph2_ir_t *instruction = machine.entry->ph2_ir_list.head; instruction;
         instruction = instruction->next)
        if (instruction->op == OP_store)
            spills++;
    assert(spills == 0);
    vir_function_release(&function);
}

#ifdef SHECC_VIR_LOWER_TEST
static void vir_lower_test_hook(void)
{
    vir_function_t addition;
    vir_machine_function_t machine;
    vir_call_abi_type_t params[2] = {{VIR_TYPE_I64, false, false},
                                     {VIR_TYPE_I64, false, false}};
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    type.params = params;
    type.param_count = 2;
    vir_function_init(&addition, 4096);
    vir_function_set_pointer_bits(&addition, PTR_SIZE * 8);
    vir_block_t *test_entry = vir_block_create(&addition);
    vir_value_t *a = vir_block_add_param(&addition, test_entry, VIR_TYPE_I64);
    vir_value_t *b = vir_block_add_param(&addition, test_entry, VIR_TYPE_I64);
    assert(vir_block_set_return(&addition, test_entry,
                                vir_add(&addition, test_entry, a, b)));
    assert(vir_lower_machine(&addition, &type, NULL, NULL, &machine));
    install_machine = true;
    install_function("test_add", &machine);
    vir_function_release(&addition);
    vir_function_init(&addition, 4096);
    vir_function_set_pointer_bits(&addition, PTR_SIZE * 8);
    test_entry = vir_block_create(&addition);
    vir_value_t *narrow[7];
    vir_call_abi_type_t mixed_params[8];
    for (int index = 0; index < 7; index++) {
        narrow[index] =
            vir_block_add_param(&addition, test_entry, VIR_TYPE_I32);
        mixed_params[index] =
            (vir_call_abi_type_t) {VIR_TYPE_I32, false, false};
    }
    a = vir_block_add_param(&addition, test_entry, VIR_TYPE_I64);
    vir_value_t *sum = a;
    for (int index = 0; index < 7; index++)
        sum = vir_add(
            &addition, test_entry, sum,
            vir_sext(&addition, test_entry, narrow[index], VIR_TYPE_I64));
    assert(vir_block_set_return(&addition, test_entry, sum));
    mixed_params[7] = (vir_call_abi_type_t) {VIR_TYPE_I64, false, false};
    type.params = mixed_params;
    type.param_count = 8;
    assert(vir_lower_machine(&addition, &type, NULL, NULL, &machine));
    install_function("test_mix", &machine);
    vir_function_release(&addition);
    vir_function_init(&addition, 4096);
    vir_function_set_pointer_bits(&addition, PTR_SIZE * 8);
    test_entry = vir_block_create(&addition);
    vir_value_t *named =
        vir_block_add_param(&addition, test_entry, VIR_TYPE_I32);
    (void) named;
    vir_value_t *root = vir_stack_addr(&addition, test_entry, 0, 4, 4);
    vir_value_t *offset = vir_const_int(
        &addition, test_entry, PTR_SIZE == 4 ? VIR_TYPE_I32 : VIR_TYPE_I64, 8);
    vir_value_t *address = vir_ptradd(&addition, test_entry, root, offset);
    assert(vir_block_set_return(
        &addition, test_entry,
        vir_load(&addition, test_entry, address, VIR_TYPE_I64)));
    vir_call_abi_type_t fixed_param = {VIR_TYPE_I32, false, false};
    unsigned int root_slot = 0;
    type.params = &fixed_param;
    type.param_count = 1;
    type.is_variadic = true;
    type.fixed_param_count = 1;
    type.param_slots = &root_slot;
    assert(vir_lower_machine(&addition, &type, NULL, NULL, &machine));
    install_function("test_va", &machine);
    vir_function_release(&addition);
    install_machine = true;
    test_inplace_edge();
    test_affinity_parameter_entry();
    test_pair_multiply_aliases();
    test_wide_pointer_offsets();
    test_va_start_root();
    test_strength_reduction_overflow_gate();
    test_pointer_strength_reduction();
    test_loop_parallel_copy();
    test_calls_memory_order();
    test_signed_extensions();
    test_mixed_arguments();
    test_variadic_call();
    MAIN_BB = find_func("main")->bbs;
}
#endif

/* Odd pairs can strand r0/r3 while r1/r2 and r6/r7 are locked. */
static void test_pair_claim_alignment(void)
{
    if (PTR_SIZE != 4)
        return;
    vir_function_t function;
    vir_function_init(&function, 4096);
    vir_location_t locations[4] = {{0}};
    vir_lower_t lower = {0};
    lower.function = &function;
    lower.locations = locations;
    lower.block = arena_alloc_bb();
    lower.pinned_mask = 3u << (REG_CNT - CALLEE_SAVED_REGS);
    for (int reg = 0; reg < REG_CNT; reg++)
        lower.owners[reg] = -1;
    for (int id = 0; id < 4; id++) {
        locations[id].reg = locations[id].high = locations[id].fixed = -1;
        locations[id].slot = id * 8;
    }
    vir_value_t scalar = {0}, first = {0}, second = {0}, result = {0};
    scalar.id = 0;
    scalar.type = VIR_TYPE_I32;
    first.id = 1;
    second.id = 2;
    result.id = 3;
    first.type = second.type = result.type = VIR_TYPE_I64;
    assert(vir_machine_claim(&lower, &scalar) == 0);
    assert(!(vir_machine_claim(&lower, &first) & 1));
    assert(!(vir_machine_claim(&lower, &second) & 1));
    vir_machine_read(&lower, &first);
    vir_machine_read(&lower, &second);
    assert(!(vir_machine_claim(&lower, &result) & 1));
    lower.locked = 15;
    lower.pinned_mask = 3u << 4;
    locations[0].reg = 0;
    locations[0].high = -1;
    scalar.type = VIR_TYPE_I1;
    basic_block_t *normalization = arena_alloc_bb();
    lower.block = normalization;
    vir_machine_normalize(&lower, &scalar);
    assert(normalization->ph2_ir_list.head->dest == 6);
    vir_function_release(&function);
}

static void test_machine_liveness(void)
{
    func_t *function = add_func("test_liveness", false);
    func_t *callee = add_func("test_liveness_callee", true);
    callee->has_prototype = true;
    basic_block_t *entry = arena_alloc_bb(), *loop = arena_alloc_bb();
    basic_block_t *exit = arena_alloc_bb();
    function->bbs = entry;
    entry->rpo_next = entry->then_ = loop;
    loop->rpo_next = entry->else_ = exit;
    loop->next = entry;
    int saved = REG_CNT - CALLEE_SAVED_REGS;
    ph2_ir_t *constant = bb_add_ph2_ir(entry, OP_load_constant);
    constant->dest = 0;
    ph2_ir_t *copy = bb_add_ph2_ir(entry, OP_assign);
    copy->dest = 2;
    copy->src0 = 0;
    ph2_ir_t *pair = bb_add_ph2_ir(loop, OP_assign);
    pair->dest = 2;
    pair->dest_hi = 3;
    pair->src0 = 0;
    pair->src0_hi = 1;
    ph2_ir_t *call = bb_add_ph2_ir(exit, OP_call);
    call->func_name = callee->return_def.var_name;
    ph2_ir_t *returned = bb_add_ph2_ir(exit, OP_return);
    returned->src0 = saved;
    ph2_compute_liveness();
    assert((entry->machine_liveout & 3u) == 3u);
    assert(entry->machine_liveout & (1u << saved));
    assert(reg_read_after(entry, copy, 0));
    assert(fold_loses_dest(entry, constant, copy));
    assert(!(ph2_ir_defs(call) & (1u << saved)));
    assert((ph2_ir_defs(call) & 3u) == 3u);
    assert(!(exit->machine_use & 1u));
    assert(exit->machine_use & (1u << saved));
    call->func_name = "unknown_liveness_call";
    ph2_compute_liveness();
    assert((exit->machine_use & ((1u << MAX_ARGS_IN_REG) - 1)) ==
           (1u << MAX_ARGS_IN_REG) - 1);
    function->bbs = NULL;
}

int main(int argc, char **argv)
{
#ifdef SHECC_VIR_LOWER_TEST
    return compiler_main(argc, argv);
#else
    (void) argc;
    (void) argv;
    global_init();
    test_inplace_edge();
    test_affinity_parameter_entry();
    test_pair_claim_alignment();
    test_machine_liveness();
    test_wide_pointer_offsets();
    test_strength_reduction_overflow_gate();
    test_pointer_strength_reduction();
    test_loop_parallel_copy();
    test_calls_memory_order();
    test_signed_extensions();
    test_mixed_arguments();
    test_variadic_call();
    test_reused_homes();
    test_core_extensions();
    puts("VIR machine lowering tests passed");
    return 0;
#endif
}
