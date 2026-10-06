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
            case OP_neq:
                result = left != right;
                break;
            case OP_log_not:
                result = !left;
                break;
            case OP_lt:
                result = instruction->src0_is_unsigned ? left < right
                         : width == 8 ? (long long) left < (long long) right
                                      : (int) left < (int) right;
                break;
            case OP_cast:
                result = width == 8 ? left : left & ((1ull << (width * 8)) - 1);
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

static void test_resident_entry_parameters(void)
{
    for (int mode = 0; mode < 5; mode++) {
        vir_function_t function;
        vir_function_init(&function, 4096);
        assert(vir_function_set_pointer_bits(&function, PTR_SIZE * 8));
        vir_block_t *entry = vir_block_create(&function);
        vir_type_t kind = mode == 2   ? VIR_TYPE_I8
                          : mode == 4 ? VIR_TYPE_PTR
                                      : VIR_TYPE_I64;
        vir_call_abi_type_t params[2] = {{VIR_TYPE_I32, false, false},
                                         {kind, true, false}};
        vir_call_signature_t type = signature(kind);
        type.result.is_unsigned = true;
        type.params = mode == 1 ? params : params + 1;
        type.param_count = mode == 1 ? 2 : 1;
        if (mode == 1)
            assert(vir_block_add_param(&function, entry, VIR_TYPE_I32));
        vir_value_t *value = vir_block_add_param(&function, entry, kind);
        vir_block_t *exit = mode == 4 ? vir_block_create(&function) : entry;
        if (exit != entry)
            assert(vir_edge_create(&function, entry, exit, NULL, 0));
        assert(vir_block_set_return(&function, exit, value));
        vir_machine_function_t machine;
        assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
        int stores = 0;
        for (basic_block_t *block = machine.entry; block;
             block = block->rpo_next)
            for (ph2_ir_t *ir = block->ph2_ir_list.head; ir; ir = ir->next)
                stores += ir->op == OP_store;
        bool fallback =
            mode == 2 || (mode == 1 && PTR_SIZE == 4 && ELF_MACHINE == 0xf3);
        assert(stores == (fallback ? mode == 2 ? 1 : 2 : 0));
        if (!fallback)
            assert(machine.stack_size == 0);
        memset(registers, 0, sizeof(registers));
        memset(memory, 0, sizeof(memory));
        int source =
            mode == 1 ? PTR_SIZE == 4 && ELF_MACHINE != 0xf3 ? 2 : 1 : 0;
        unsigned long long bits = mode == 2   ? 0xab
                                  : mode == 4 ? 0x1234
                                              : 0x12345678ffffffffULL;
        machine_write(source,
                      PTR_SIZE == 4 && kind == VIR_TYPE_I64 ? source + 1 : -1,
                      bits);
        assert(run_machine(&machine) == bits);
        vir_function_release(&function);
    }

    /* A resident parameter surviving a call must still get a dirty spill. */
    vir_function_t function;
    vir_function_init(&function, 4096);
    assert(vir_function_set_pointer_bits(&function, PTR_SIZE * 8));
    vir_block_t *entry = vir_block_create(&function);
    vir_value_t *a = vir_block_add_param(&function, entry, VIR_TYPE_I64);
    vir_value_t *b = vir_block_add_param(&function, entry, VIR_TYPE_I64);
    vir_call_abi_type_t params[2] = {{VIR_TYPE_I64, false, false},
                                     {VIR_TYPE_I64, false, false}};
    vir_call_signature_t type = signature(VIR_TYPE_I64);
    type.params = params;
    type.param_count = 2;
    vir_value_t *args[] = {a, a};
    vir_effect_t *call =
        vir_call(&function, entry, "test_add", args, 2, VIR_TYPE_I64);
    assert(call && vir_call_set_signature(&function, call, &type));
    vir_value_t *sum = vir_add(&function, entry, call->result, b);
    assert(vir_block_set_return(&function, entry, sum));
    vir_machine_function_t machine;
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    memset(registers, 0, sizeof(registers));
    memset(memory, 0, sizeof(memory));
    machine_write(0, PTR_SIZE == 4 ? 1 : -1, 7);
    machine_write(PTR_SIZE == 4 ? 2 : 1, PTR_SIZE == 4 ? 3 : -1, 13);
    assert(run_machine(&machine) == 27);
    vir_function_release(&function);
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

/* Pin reuse must account for every family member and both pair lanes. */
static void test_parallel_transfers(void)
{
    for (int shape = 0; shape < 4; shape++) {
        vir_lower_t lower = {0};
        lower.block = arena_alloc_bb();
        lower.transfer_slot = 512;
        memset(registers, 0, sizeof(registers));
        registers[0] = 11;
        registers[1] = 22;
        registers[2] = 33;
        registers[6] = 77;
        vir_transfer_t copies[4] = {{0, 0, 1, 4, true, false},
                                    {1, 8, 0, 4, true, false},
                                    {0, 0, 3, 4, true, false},
                                    {2, 16, 2, 4, true, false}};
        int count = 2;
        if (shape >= 1) {
            copies[1].to = 2;
            copies[2].from = 2;
            copies[2].to = 0;
            count = 3;
        }
        if (shape == 2) {
            copies[3] = (vir_transfer_t) {0, 0, 3, 4, true, false};
            count = 4;
        }
        if (shape == 3) {
            registers[4] = 255;
            copies[3] = (vir_transfer_t) {4, 0, 4, 1, false, true};
            count = 4;
        }
        vir_machine_parallel(&lower, copies, count);
        ph2_ir_t *ret = bb_add_ph2_ir(lower.block, OP_return);
        ret->src0 = 0;
        vir_machine_function_t function = {0};
        function.entry = lower.block;
        assert(run_machine(&function) == (shape ? 33 : 22));
        assert(registers[1] == 11 && registers[6] == 77);
        if (shape)
            assert(registers[2] == 22);
        if (shape == 2)
            assert(registers[3] == 11);
        if (shape == 3)
            assert((unsigned int) registers[4] == 0xffffffffu);
    }
}

static void test_call_result_pin(void)
{
    vir_type_t types[] = {VIR_TYPE_I1, VIR_TYPE_I8, VIR_TYPE_I16, VIR_TYPE_I32,
                          VIR_TYPE_I64};
    for (int shape = 0; shape < 5; shape++) {
        vir_function_t function;
        vir_function_init(&function, 4096);
        vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
        vir_block_t *block = vir_block_create(&function);
        vir_value_t *argument =
            vir_block_add_param(&function, block, types[shape]);
        vir_value_t *args[] = {argument};
        vir_effect_t *first =
            vir_call(&function, block, "first", args, 1, types[shape]);
        vir_effect_t *second =
            vir_call(&function, block, "second", NULL, 0, types[shape]);
        assert(first && second);
        vir_call_abi_type_t parameter = {
            shape == 0 ? VIR_TYPE_I8 : types[shape], false, shape == 0};
        vir_call_signature_t called = signature(types[shape]);
        called.result = parameter;
        called.params = &parameter;
        called.param_count = called.fixed_param_count = 1;
        assert(vir_call_set_signature(&function, first, &called));
        assert(vir_block_set_return(&function, block, first->result));
        vir_lower_t lower = {0};
        lower.function = &function;
        lower.locations =
            calloc(function.next_value_id, sizeof(*lower.locations));
        assert(lower.locations);
        for (int i = 0; i < function.next_value_id; i++)
            lower.locations[i].reg = lower.locations[i].high =
                lower.locations[i].fixed = -1;
        for (int reg = 0; reg < REG_CNT; reg++)
            lower.owners[reg] = -1;
        assert(vir_machine_slots(&lower));
        vir_location_t *location = &lower.locations[first->result->id];
        assert(location->fixed >= REG_CNT - CALLEE_SAVED_REGS);
        int fixed = location->fixed;

        /* Force the dying argument into the result lane only after the normal
         * interference proof accepts it. This tests transfer ordering, not
         * whether score-based selection chooses that argument.
         */
        vir_value_t *values[4] = {0};
        values[first->result->id] = first->result;
        int seen[1];
        vir_block_t *work[1];
        assert(vir_machine_pin_safe(&lower, argument, fixed, values,
                                    function.next_value_id, seen, work));
        vir_location_t *input = &lower.locations[argument->id];
        input->fixed = input->reg = fixed;
        input->high = vir_machine_pair(&lower, argument) ? fixed + 1 : -1;
        lower.block = arena_alloc_bb();
        vir_machine_call(&lower, first);
        ph2_ir_t *transfer = lower.block->ph2_ir_list.head;
        assert(transfer->src0 == fixed && transfer->dest == 0);
        assert(location->reg == fixed && lower.owners[0] == -1);
        bool pair = vir_machine_pair(&lower, first->result);
        assert(location->high == (pair ? fixed + 1 : -1));
        if (pair)
            assert(lower.owners[1] == -1);
        assert(lower.block->ph2_ir_list.tail->dest ==
               (pair ? fixed + 1 : fixed));
        vir_machine_call(&lower, second);
        assert(location->reg == fixed);
        free(lower.locations);
        free(lower.block_positions);
        vir_function_release(&function);
    }
}

static void test_pin_family_reuse(void)
{
    for (int shape = 0; shape < 5; shape++) {
        vir_function_t function;
        vir_function_init(&function, 4096);
        vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
        vir_block_t *block = vir_block_create(&function);
        vir_type_t type = shape >= 2 ? VIR_TYPE_I64 : VIR_TYPE_I32;
        vir_value_t *old = vir_const_int(&function, block, type, 1);
        assert(vir_user_operation(&function, block, VIR_OP_ADD, old->type, old,
                                  old));
        vir_value_t *next = vir_const_int(&function, block,
                                          shape == 3 ? VIR_TYPE_I32 : type, 2);
        vir_value_t *member = NULL;
        if (shape == 4) {
            member = vir_const_int(&function, block, type, 3);
            assert(vir_user_operation(&function, block, VIR_OP_ADD,
                                      member->type, member, member));
            assert(vir_user_operation(&function, block, VIR_OP_ADD, next->type,
                                      next, next));
        } else if (shape == 3) {
            assert(vir_user_operation(&function, block, VIR_OP_ADD, old->type,
                                      old, old));
            assert(vir_user_operation(&function, block, VIR_OP_ADD, next->type,
                                      next, next));
        } else if (shape == 1)
            assert(vir_user_operation(&function, block, VIR_OP_ADD, old->type,
                                      old, next));
        else
            assert(vir_user_operation(&function, block, VIR_OP_ADD, next->type,
                                      next, next));
        int count = function.next_value_id;
        vir_value_t *values[8] = {0};
        vir_location_t locations[8] = {{0}};
        for (int id = 0; id < count; id++)
            locations[id].fixed = -1;
        values[old->id] = old;
        locations[old->id].fixed = 4;
        if (member) {
            values[member->id] = member;
            locations[member->id].fixed = 4;
        }
        vir_lower_t lower = {0};
        lower.function = &function;
        lower.locations = locations;
        int seen[1];
        vir_block_t *work[1];
        int fixed = shape == 3 && PTR_SIZE == 4 ? 5 : 4;
        bool safe = shape == 0 || shape == 2;
        assert(vir_machine_pin_safe(&lower, next, fixed, values, count, seen,
                                    work) == safe);
        assert(
            vir_machine_pin_safe(&lower, next, 8, values, count, seen, work));
        vir_function_release(&function);
    }
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

static void test_existing_pointer_counter(void)
{
    for (int mode = 0; mode < 8; mode++) {
        vir_function_t f;
        char *error;
        vir_function_init(&f, 4096);
        int bits = mode == 3 ? 32 : 64;
        vir_function_set_pointer_bits(&f, bits);
        vir_type_t width = bits == 64 ? VIR_TYPE_I64 : VIR_TYPE_I32;
        vir_block_t *pre = vir_block_create(&f);
        vir_block_t *header = vir_block_create(&f);
        vir_block_t *body = vir_block_create(&f);
        vir_block_t *done = vir_block_create(&f);
        vir_value_t *i = vir_block_add_param(&f, header, VIR_TYPE_I32);
        vir_value_t *p = vir_block_add_param(&f, header, VIR_TYPE_PTR);
        vir_value_t *root = vir_stack_addr(&f, pre, 0, 32, 4);
        vir_value_t *initial = vir_const_i32(&f, pre, mode == 1 ? 1 : 0);
        vir_value_t *args[] = {initial, root};
        assert(vir_edge_create(&f, pre, header, args, 2));
        vir_value_t *bound =
            mode == 6
                ? vir_block_add_param(&f, pre, VIR_TYPE_I32)
                : vir_const_i32(&f, header, mode == 2 || mode == 3 ? 3 : 8);
        vir_edge_args_t yes = {body, NULL, 0}, no = {done, NULL, 0};
        vir_value_t *condition = mode == 7 ? vir_slt(&f, header, i, bound)
                                           : vir_ult(&f, header, i, bound);
        assert(vir_block_set_branch(&f, header, condition, &yes, &no));
        vir_value_t *next_i = vir_add(&f, body, i, vir_const_i32(&f, body, 1));
        vir_value_t *step = vir_const_int(&f, body, width,
                                          mode == 2 || mode == 3 ? INT_MAX : 4);
        vir_value_t *next_p = vir_ptradd(&f, body, p, step);
        assert(vir_store(&f, body, p,
                         mode == 4   ? i
                         : mode == 5 ? next_i
                                     : vir_const_i32(&f, body, 1)));
        vir_value_t *updated[] = {next_i, next_p};
        assert(vir_edge_create(&f, body, header, updated, 2));
        assert(vir_block_set_return(&f, done, vir_const_i32(&f, done, 0)));
        assert(vir_verify(&f, &error));
        int changed = vir_strength_reduce(&f, VIR_OPT_O1);
        bool expected = mode < 3;
        assert(changed == (expected ? 1 : 0));
        assert(header->param_count == (expected ? 1 : 2));
        assert(vir_verify(&f, &error));
        vir_function_release(&f);
    }
}

static void test_descending_pointer_family(void)
{
    for (int mode = 0; mode < 13; mode++) {
        vir_function_t f;
        char *error;
        vir_function_init(&f, 4096);
        int bits = mode == 4 ? 32 : 64;
        vir_function_set_pointer_bits(&f, bits);
        vir_type_t width = bits == 64 ? VIR_TYPE_I64 : VIR_TYPE_I32;
        vir_block_t *pre = vir_block_create(&f);
        vir_block_t *header = vir_block_create(&f);
        vir_block_t *body = vir_block_create(&f);
        vir_block_t *done = vir_block_create(&f);
        vir_value_t *j = vir_block_add_param(&f, header, VIR_TYPE_I32);
        vir_value_t *root = vir_stack_addr(&f, pre, 0, 64, 4);
        unsigned initial_bits = (mode == 0 || mode == 12) ? 0
                                : mode == 1               ? 1
                                : mode == 3               ? UINT_MAX
                                                          : 4;
        vir_value_t *initial = vir_const_i32(&f, pre, initial_bits);
        vir_value_t *args[] = {initial};
        assert(vir_edge_create(&f, pre, header, args, 1));
        vir_value_t *zero = vir_const_i32(&f, header, 0);
        vir_value_t *condition = mode == 5 ? vir_slt(&f, header, zero, j)
                                           : vir_ult(&f, header, zero, j);
        vir_edge_args_t yes = {body, NULL, 0}, no = {done, NULL, 0};
        assert(vir_block_set_branch(&f, header, condition, &yes, &no));
        vir_value_t *next_j = vir_sub(&f, body, j, vir_const_i32(&f, body, 1));
        vir_value_t *wide =
            bits == 64 ? vir_zext(&f, body, next_j, width) : next_j;
        vir_value_t *stride =
            vir_const_int(&f, pre, width, mode == 10 ? INT_MAX : 4);
        vir_value_t *offset = vir_mul(&f, body, wide, stride);
        vir_value_t *address = vir_ptradd(&f, body, root, offset);
        vir_value_t *stored = mode == 6 ? j : vir_const_i32(&f, body, 7);
        vir_effect_t *effect =
            mode == 9 ? vir_volatile_store(&f, body, address, stored)
                      : vir_store(&f, body, address, stored);
        assert(effect);
        vir_value_t *updated[] = {next_j};
        assert(vir_edge_create(&f, body, header, updated, 1));
        wide = bits == 64 ? vir_zext(&f, done, j, width) : j;
        if (mode == 8)
            stride = vir_const_int(&f, pre, width, 8);
        offset = vir_mul(&f, done, wide, stride);
        vir_value_t *last = vir_ptradd(&f, done, root, offset);
        if (mode == 7)
            last = j;
        if (mode >= 11)
            assert(vir_edge_create(&f, done, body, NULL, 0));
        else
            assert(vir_block_set_return(&f, done, last));
        assert(vir_verify(&f, &error));
        int changed = vir_strength_reduce(&f, VIR_OPT_O1);
        bool expected = mode < 4 || mode == 9 || mode == 10;
        assert(changed == (expected ? 2 : 0));
        assert(header->param_count == 1);
        assert(header->params->type ==
               (expected ? VIR_TYPE_PTR : VIR_TYPE_I32));
        assert(effect->block == body && body->effects == effect);
        assert(vir_verify(&f, &error));
        vir_function_release(&f);
    }
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
    test_resident_entry_parameters();
    test_inplace_edge();
    test_affinity_parameter_entry();
    test_pin_family_reuse();
    test_call_result_pin();
    test_parallel_transfers();
    test_pair_multiply_aliases();
    test_wide_pointer_offsets();
    test_va_start_root();
    test_strength_reduction_overflow_gate();
    test_descending_pointer_family();
    test_existing_pointer_counter();
    test_pointer_strength_reduction();
    test_loop_parallel_copy();
    test_calls_memory_order();
    test_signed_extensions();
    test_mixed_arguments();
    test_variadic_call();
    MAIN_BB = find_func("main")->bbs;
}
#endif

static int rematerialize_global(const char *name, void *context)
{
    assert(!strcmp(name, "object") && context == NULL);
    return 24;
}

static void test_literal_rematerialization(void)
{
    vir_function_t function;
    vir_function_init(&function, 4096);
    assert(vir_function_set_pointer_bits(&function, PTR_SIZE * 8));
    vir_location_t locations[2] = {{0}};
    vir_lower_t lower = {0};
    lower.function = &function;
    lower.locations = locations;
    lower.global_offset = rematerialize_global;
    lower.pinned_mask = 3u << (REG_CNT - CALLEE_SAVED_REGS);
    for (int reg = 0; reg < REG_CNT; reg++)
        lower.owners[reg] = -1;
    for (int id = 0; id < 2; id++) {
        locations[id].reg = locations[id].high = locations[id].fixed = -1;
        locations[id].slot = id * 8;
        locations[id].end = 100;
    }
    vir_value_t kept = {0}, literal = {0};
    kept.type = VIR_TYPE_I32;
    literal.id = 1;
    literal.type = VIR_TYPE_I64;
    literal.opcode = VIR_OP_CONST;
    literal.constant = 0x12345678ffffffffULL;
    assert(vir_machine_claim(&lower, &kept) == 0);
    locations[1].rematerializable = vir_machine_rematerializable(&literal);
    lower.block = arena_alloc_bb();
    vir_machine_value(&lower, &literal);
    assert(!locations[1].dirty);
    ph2_ir_t *initial = lower.block->ph2_ir_list.tail;
    vir_machine_flush(&lower);
    assert(lower.block->ph2_ir_list.tail == initial);
    assert(vir_machine_claim(&lower, &kept) == 0);
    lower.locked = 1;
    lower.block = arena_alloc_bb();
    int reg = vir_machine_read(&lower, &literal);
    assert(reg != 0 && lower.owners[0] == 0 && (lower.locked & 1));
    ph2_ir_t *instruction = lower.block->ph2_ir_list.head;
    assert(instruction->op == OP_load_constant && instruction->src0 == -1 &&
           instruction->src1 == 0x12345678 && instruction->size_bytes == 8);
    assert(!(lower.pinned_mask & (1u << reg)));
    if (PTR_SIZE == 4)
        assert(instruction->dest_hi == reg + 1 && !(reg & 1));
    vir_machine_spill(&lower, reg);
    assert(lower.block->ph2_ir_list.tail == instruction);
    lower.locked = 1;
    literal.type = VIR_TYPE_PTR;
    literal.opcode = VIR_OP_GLOBAL_ADDR;
    literal.address_name = "object";
    reg = vir_machine_read(&lower, &literal);
    instruction = lower.block->ph2_ir_list.tail;
    assert(instruction->op == OP_global_address_of && instruction->src0 == 24 &&
           instruction->is_pointer && instruction->size_bytes == PTR_SIZE);
    vir_machine_spill(&lower, reg);
    literal.opcode = VIR_OP_RODATA_ADDR;
    literal.constant = 19;
    reg = vir_machine_read(&lower, &literal);
    instruction = lower.block->ph2_ir_list.tail;
    assert(instruction->op == OP_load_rodata_address &&
           instruction->src0 == 19);
    vir_machine_spill(&lower, reg);
    vir_effect_t call = {0};
    call.kind = VIR_EFFECT_CALL;
    vir_use_t use = {0};
    use.effect = &call;
    literal.uses = &use;
    locations[1].rematerializable = vir_machine_rematerializable(&literal);
    assert(!locations[1].rematerializable);
    reg = vir_machine_read(&lower, &literal);
    assert(lower.block->ph2_ir_list.tail->op == OP_load);
    lower.locations[1].dirty = true;
    vir_machine_spill(&lower, reg);
    assert(lower.block->ph2_ir_list.tail->op == OP_store);
    use.effect = NULL;
    vir_edge_t edge = {0};
    use.edge = &edge;
    locations[1].rematerializable = vir_machine_rematerializable(&literal);
    assert(!locations[1].rematerializable);
    /* Edge-affiliated literals use their fixed lane rather than a fake home. */
    int fixed = REG_CNT - CALLEE_SAVED_REGS;
    locations[1].fixed = locations[1].reg = fixed;
    literal.opcode = VIR_OP_CONST;
    literal.type = VIR_TYPE_I32;
    literal.constant = 9;
    lower.block = arena_alloc_bb();
    vir_machine_value(&lower, &literal);
    assert(locations[1].dirty && !locations[1].rematerializable);
    instruction = lower.block->ph2_ir_list.tail;
    assert(instruction->op == OP_load_constant && instruction->dest == fixed);
    assert(vir_machine_read(&lower, &literal) == fixed);
    assert(lower.block->ph2_ir_list.tail == instruction);
    vir_function_release(&function);
}

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

static void test_highest_allocated_register(void)
{
    func_t function = {0};
    basic_block_t block = {0};
    ph2_ir_t ir = {0};
    int first = REG_CNT - CALLEE_SAVED_REGS;
    function.bbs = &block;
    block.ph2_ir_list.head = &ir;
    for (int operand = 0; operand < 6; operand++) {
        ir.op = OP_add;
        ir.dest = ir.src0 = ir.src1 = ir.dest_hi = ir.src0_hi = ir.src1_hi = -1;
        int *fields[] = {&ir.dest,    &ir.src0,    &ir.src1,
                         &ir.dest_hi, &ir.src0_hi, &ir.src1_hi};
        int *field = fields[operand];
        *field = REG_CNT + operand;
        assert(func_highest_used_reg(&function, first) == first - 1);
        *field = REG_CNT - 1;
        assert(func_highest_used_reg(&function, first) == REG_CNT - 1);
    }
    ir.dest_hi = ir.src0_hi = ir.src1_hi = -1;
    ir.op = OP_load_constant;
    ir.dest = first;
    ir.src0 = ir.src1 = REG_CNT + 100;
    assert(func_highest_used_reg(&function, first) == first);
    ir.op = OP_store;
    ir.src0 = first;
    assert(func_highest_used_reg(&function, first) == first);
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

/* Exercise forced resident homes, independently of the allocator's scores. */
static void test_dead_instance_homes(void)
{
    for (int mode = 0; mode < 5; mode++) {
        vir_function_t function;
        vir_function_init(&function, 4096);
        vir_function_set_pointer_bits(&function, PTR_SIZE * 8);
        vir_block_t *entry = vir_block_create(&function), *block = entry;
        vir_type_t type = mode == 4 ? VIR_TYPE_I64 : VIR_TYPE_I32;
        vir_value_t *value = mode == 3
                                 ? vir_block_add_param(&function, entry, type)
                                 : vir_const_int(&function, entry, type, 7);
        if (mode == 1) {
            block = vir_block_create(&function);
            vir_edge_args_t edge = {block, NULL, 0};
            assert(vir_edge_create(&function, entry, edge.to, edge.args,
                                   edge.arg_count));
        }
        if (mode == 2 || mode == 4) {
            vir_block_t *target = vir_block_create(&function);
            vir_value_t *param = vir_block_add_param(&function, target, type);
            vir_value_t *args[] = {value};
            vir_edge_args_t edge = {target, args, 1};
            assert(vir_edge_create(&function, entry, edge.to, edge.args,
                                   edge.arg_count));
            assert(vir_block_set_return(&function, target, param));
        } else if (mode == 3)
            assert(vir_block_set_return(&function, entry, value));
        else {
            assert(vir_user_operation(&function, block, VIR_OP_ADD, type, value,
                                      value));
            vir_edge_args_t edge = {block, NULL, 0};
            assert(vir_edge_create(&function, block, edge.to, edge.args,
                                   edge.arg_count));
        }
        vir_location_t locations[4] = {{0}};
        vir_lower_t lower = {0};
        int positions[2] = {0}, seen[2];
        vir_block_t *work[2];
        lower.function = &function;
        lower.locations = locations;
        lower.block_positions = positions;
        lower.live_seen = seen;
        lower.live_work = work;
        lower.block = arena_alloc_bb();
        for (int reg = 0; reg < REG_CNT; reg++)
            lower.owners[reg] = -1;
        vir_location_t *location = &locations[value->id];
        location->value = value;
        location->reg = 0;
        location->high = mode == 4 && PTR_SIZE == 4 ? 1 : -1;
        location->end = INT_MAX;
        location->dirty = true;
        lower.owners[0] = value->id;
        if (location->high >= 0)
            lower.owners[1] = value->id;
        vir_machine_flush_successor(&lower, block, NULL);
        int stores = 0;
        for (ph2_ir_t *ir = lower.block->ph2_ir_list.head; ir; ir = ir->next)
            stores += ir->op == OP_store;
        assert(stores == (mode != 0));
        assert(lower.owners[0] < 0 &&
               (location->high < 0 || lower.owners[1] < 0));
        vir_function_release(&function);
    }
}

/* Natural pin pressure leaves the live comparison result in a dynamic lane. */
static void test_live_branch_condition(void)
{
    for (int mode = 0; mode < 4; mode++) {
        vir_function_t function;
        vir_function_init(&function, 4096);
        assert(vir_function_set_pointer_bits(&function, PTR_SIZE * 8));
        vir_block_t *entry = vir_block_create(&function);
        vir_block_t *yes = vir_block_create(&function);
        vir_block_t *no = vir_block_create(&function);
        vir_value_t *args[MAX_ARGS_IN_REG];
        vir_call_abi_type_t abi[MAX_ARGS_IN_REG];
        for (int i = 0; i < MAX_ARGS_IN_REG; i++) {
            vir_type_t type =
                i == MAX_ARGS_IN_REG - 1 ? VIR_TYPE_PTR : VIR_TYPE_I32;
            args[i] = vir_block_add_param(&function, entry, type);
            abi[i] = (vir_call_abi_type_t) {type, false, false};
        }
        vir_value_t *condition = vir_user_operation(
            &function, entry, mode & 1 ? VIR_OP_ULT : VIR_OP_EQ, VIR_TYPE_I1,
            args[0], args[1]);
        vir_edge_args_t taken = {yes, NULL, 0}, other = {no, NULL, 0};
        assert(
            vir_block_set_branch(&function, entry, condition, &taken, &other));
        vir_value_t *sum = vir_add(&function, yes, args[0], args[1]);
        for (int i = 2; i < MAX_ARGS_IN_REG - 1; i++)
            sum = vir_add(&function, yes, sum, args[i]);
        assert(vir_store(&function, yes, args[MAX_ARGS_IN_REG - 1], sum));
        vir_type_t result_type = mode < 2 ? VIR_TYPE_I1 : VIR_TYPE_I32;
        vir_value_t *result =
            mode < 2 ? condition
                     : vir_zext(&function, yes, condition, VIR_TYPE_I32);
        assert(vir_block_set_return(&function, yes, result));
        assert(vir_block_set_return(
            &function, no, vir_const_int(&function, no, result_type, 0)));
        vir_call_signature_t type = signature(result_type);
        type.params = abi;
        type.param_count = MAX_ARGS_IN_REG;
        vir_machine_function_t machine;
        assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
        int compare = -1;
        bool materialized = false;
        for (ph2_ir_t *ir = machine.entry->ph2_ir_list.head; ir;
             ir = ir->next) {
            if (ir->op == OP_eq || ir->op == OP_lt)
                compare = ir->dest;
            if (ir->op == OP_store && compare >= 0 && ir->src0 == compare)
                materialized = true;
        }
        assert(materialized);
        for (int take = 0; take < 2; take++) {
            memset(registers, 0, sizeof(registers));
            memset(memory, 0, sizeof(memory));
            registers[0] = take ? (mode & 1 ? 4 : 5) : 6;
            registers[1] = 5;
            for (int i = 2; i < MAX_ARGS_IN_REG - 1; i++)
                registers[i] = 7;
            registers[MAX_ARGS_IN_REG - 1] = 4096 + 256;
            assert(run_machine(&machine) == (unsigned int) take);
            unsigned int stored = 0;
            memcpy(&stored, memory + 256, sizeof(stored));
            assert(stored == (unsigned int) (take
                                                 ? (mode & 1 ? 9 : 10) +
                                                       7 * (MAX_ARGS_IN_REG - 3)
                                                 : 0));
        }
        vir_function_release(&function);
    }
}

static void test_boolean_branch_lowering(void)
{
    vir_function_t function;
    vir_function_init(&function, 4096);
    assert(vir_function_set_pointer_bits(&function, PTR_SIZE * 8));
    vir_block_t *entry = vir_block_create(&function);
    vir_block_t *yes = vir_block_create(&function);
    vir_block_t *no = vir_block_create(&function);
    vir_value_t *param = vir_block_add_param(&function, entry, VIR_TYPE_I1);
    vir_value_t *condition =
        vir_eq(&function, entry, param, vir_const_i1(&function, entry, 0));
    vir_edge_args_t when_zero = {yes, NULL, 0}, when_one = {no, NULL, 0};
    assert(vir_block_set_branch(&function, entry, condition, &when_zero,
                                &when_one));
    assert(
        vir_block_set_return(&function, yes, vir_const_i32(&function, yes, 7)));
    assert(
        vir_block_set_return(&function, no, vir_const_i32(&function, no, 9)));
    vir_call_abi_type_t argument = {VIR_TYPE_I1, false, false};
    vir_call_signature_t type = signature(VIR_TYPE_I32);
    type.params = &argument;
    type.param_count = 1;
    vir_machine_function_t machine;
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    int comparisons = 0;
    for (ph2_ir_t *ir = machine.entry->ph2_ir_list.head; ir; ir = ir->next) {
        comparisons += ir->op == OP_eq;
        assert(ir->op != OP_log_not);
    }
    assert(comparisons == 1);
    for (int value = 0; value < 2; value++) {
        memset(registers, 0, sizeof(registers));
        registers[0] = value;
        assert(run_machine(&machine) == (unsigned int) (value ? 9 : 7));
    }
    vir_function_release(&function);
}

static void test_late_constant_inversion(void)
{
    for (int mode = 0; mode < 3; mode++) {
        vir_function_t function;
        vir_function_init(&function, 4096);
        assert(vir_function_set_pointer_bits(&function, PTR_SIZE * 8));
        vir_block_t *entry = vir_block_create(&function);
        vir_value_t *replacement = vir_const_i1(&function, entry, mode != 2);
        vir_value_t *param = vir_block_add_param(&function, entry, VIR_TYPE_I1);
        vir_value_t *zero = vir_const_i1(&function, entry, 0);
        vir_value_t *equal = vir_user_operation(
            &function, entry, VIR_OP_EQ, VIR_TYPE_I1, mode == 1 ? zero : param,
            mode == 1 ? param : zero);
        assert(equal && equal->opcode == VIR_OP_EQ);
        assert(vir_block_set_return(&function, entry, equal));
        assert(vir_replace_all_uses(&function, param, replacement));
        vir_call_abi_type_t argument = {VIR_TYPE_I1, false, false};
        vir_call_signature_t type = signature(VIR_TYPE_I1);
        type.params = &argument;
        type.param_count = 1;
        vir_machine_function_t machine;
        assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
        memset(registers, 0, sizeof(registers));
        assert(run_machine(&machine) == (unsigned int) (mode == 2));
        vir_function_release(&function);
    }
}

static void test_dead_literal(void)
{
    for (int mode = 0; mode < 5; mode++) {
        basic_block_t *block = arena_alloc_bb();
        ph2_ir_t *first = bb_add_ph2_ir(block, OP_eq);
        first->src0 = 1;
        first->src1 = 2;
        first->dest = 0;
        first->size_bytes = 4;
        ph2_ir_t *literal = bb_add_ph2_ir(block, OP_load_constant);
        literal->dest = 3;
        literal->src0 = 0;
        literal->src1 = 0;
        literal->size_bytes = 4;
        if (mode == 1)
            block->machine_liveout = 1u << 3;
        else if (mode == 2) {
            ph2_ir_t *use = bb_add_ph2_ir(block, OP_return);
            use->src0 = 3;
        } else if (mode == 3)
            literal->dest_hi = 4;
        else if (mode == 4) {
            ph2_ir_t *call = bb_add_ph2_ir(block, OP_call);
            call->func_name = "unknown_literal_consumer";
        }
        assert(peephole_at(block, first) == (mode == 0));
        assert(first->next == (mode == 0 ? NULL : literal));
    }
}

static void test_comparison_inversion(void)
{
    for (int mode = 0; mode < 7; mode++) {
        basic_block_t *block = arena_alloc_bb();
        ph2_ir_t *compare = bb_add_ph2_ir(block, mode == 1 ? OP_neq : OP_eq);
        ph2_ir_t *invert = bb_add_ph2_ir(block, mode == 5 ? OP_eq : OP_log_not);
        compare->src0 = 1;
        compare->src1 = 2;
        compare->dest = 0;
        compare->size_bytes = 8;
        compare->src0_is_unsigned = mode == 2;
        invert->src0 = mode == 6 ? 3 : 0;
        invert->src1 = 4;
        invert->dest = mode == 3 ? 0 : 3;
        invert->size_bytes = 4;
        if (mode == 4)
            block->machine_liveout = 1u;
        bool expected = mode < 4;
        assert(insn_fusion(block, compare) == expected);
        if (expected) {
            assert(compare->op == (mode == 1 ? OP_eq : OP_neq));
            assert(compare->dest == invert->dest && !compare->next);
            assert(block->ph2_ir_list.tail == compare);
            assert(compare->size_bytes == 8);
            assert(compare->src0_is_unsigned == (mode == 2));
        }
    }
}

static void test_load_forwarding(void)
{
    for (int global = 0; global < 2; global++) {
        for (int mismatch = -2; mismatch < 5; mismatch++) {
            basic_block_t *block = arena_alloc_bb();
            opcode_t opcode = global ? OP_global_load : OP_load;
            ph2_ir_t *first = bb_add_ph2_ir(block, opcode);
            ph2_ir_t *second = bb_add_ph2_ir(block, opcode);
            first->dest = 0;
            second->dest = 1;
            first->src0 = second->src0 = 16;
            first->src1 = second->src1 = 0;
            first->size_bytes = second->size_bytes = PTR_SIZE;
            first->is_volatile = mismatch == -2;
            if (mismatch == 0)
                second->size_bytes = 1;
            else if (mismatch == 1)
                second->is_unsigned = true;
            else if (mismatch == 2)
                second->is_pointer = true;
            else if (mismatch == 3)
                second->ofs_based_on_stack_top = true;
            else if (mismatch == 4)
                second->is_volatile = true;
            assert(peephole_at(block, first) == (mismatch < 0));
            assert(second->op == (mismatch < 0 ? OP_assign : opcode));
            if (mismatch < 0)
                assert(second->src0 == first->dest && second->dest == 1);
        }
    }
}

#if ELF_MACHINE == 0x3e
static void test_call_result_forwarding(void)
{
    for (int mode = 0; mode < 7; mode++) {
        basic_block_t *block = arena_alloc_bb();
        basic_block_t *other = arena_alloc_bb();
        ph2_ir_t *call = bb_add_ph2_ir(block, OP_call);
        ph2_ir_t *copy = bb_add_ph2_ir(block, OP_assign);
        ph2_ir_t *last = bb_add_ph2_ir(block, OP_return);
        copy->src0 = 0;
        copy->dest = 7;
        copy->size_bytes = 4;
        copy->is_unsigned = true;
        last->src0 = 7;
        if (mode == 1)
            last->src0 = 0;
        else if (mode == 2) {
            block->machine_liveout = 1;
            last->op = OP_assign;
            last->src0 = 2;
            last->dest = 1;
        } else if (mode == 3) {
            last->op = OP_indirect;
            last->src0 = REG_CNT - 1;
        } else if (mode == 4)
            copy->op = OP_trunc;
        else if (mode == 5)
            copy->dest = 0;
        ph2_ir_t *instructions[] = {call, copy, last};
        basic_block_t *blocks[] = {block, block, mode == 6 ? other : block};
        PH2_IR_FLATTEN = instructions;
        ph2_ir_idx = 3;
        instruction_to_bb = blocks;
        instruction_to_bb_capacity = 3;
        emit_ir_index = 0;
        emit_next_ir = copy;
        src0_override = src0_override_at = -1;
        folds_off = false;
        int size = elf_code->size;
        emit_call_result();
        assert((src0_override == 0) == (mode == 0));
        assert(elf_code->size == size + (mode == 0 ? 0 : 3));
    }
    PH2_IR_FLATTEN = NULL;
    ph2_ir_idx = 0;
    instruction_to_bb = NULL;
    instruction_to_bb_capacity = 0;
    emit_next_ir = NULL;
    src0_override = src0_override_at = -1;
}
#endif

int main(int argc, char **argv)
{
#ifdef SHECC_VIR_LOWER_TEST
    return compiler_main(argc, argv);
#else
    (void) argc;
    (void) argv;
    global_init();
#if ELF_MACHINE == 0x3e
    test_call_result_forwarding();
#endif
    test_inplace_edge();
    test_affinity_parameter_entry();
    test_pin_family_reuse();
    test_call_result_pin();
    test_parallel_transfers();
    test_literal_rematerialization();
    test_pair_claim_alignment();
    test_highest_allocated_register();
    test_machine_liveness();
    test_load_forwarding();
    test_comparison_inversion();
    test_dead_literal();
    test_late_constant_inversion();
    test_boolean_branch_lowering();
    test_live_branch_condition();
    test_dead_instance_homes();
    test_wide_pointer_offsets();
    test_strength_reduction_overflow_gate();
    test_descending_pointer_family();
    test_existing_pointer_counter();
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
