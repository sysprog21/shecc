static int liveout_no_folds;
static void liveout_test_hook(void);
#define main compiler_main
#include VIR_LIVEOUT_TEST_MAIN
#undef main
#include <assert.h>

static void install_machine(const char *name, vir_machine_function_t *machine)
{
    func_t *function = find_func((char *) name);
    assert(function);
    function->bbs = machine->entry;
    function->stack_size = machine->stack_size;
    for (basic_block_t *block = machine->entry; block; block = block->rpo_next)
        block->belong_to = function;
}

static void liveout_test_hook(void)
{
    vir_function_t function;
    vir_function_init(&function, 4096);
    vir_function_set_pointer_bits(&function, 64);
    vir_block_t *entry = vir_block_create(&function);
    vir_block_t *yes = vir_block_create(&function);
    vir_block_t *no = vir_block_create(&function);
    vir_value_t *a = vir_block_add_param(&function, entry, VIR_TYPE_I32);
    vir_value_t *b = vir_block_add_param(&function, entry, VIR_TYPE_I32);
    vir_value_t *condition = vir_eq(&function, entry, a, b);
    vir_edge_args_t taken = {yes, NULL, 0}, other = {no, NULL, 0};
    assert(vir_block_set_branch(&function, entry, condition, &taken, &other));
    assert(vir_block_set_return(
        &function, yes, vir_zext(&function, yes, condition, VIR_TYPE_I32)));
    assert(
        vir_block_set_return(&function, no, vir_const_i32(&function, no, 0)));
    vir_call_abi_type_t args[2] = {{VIR_TYPE_I32, false, false},
                                   {VIR_TYPE_I32, false, false}};
    vir_call_signature_t type = {0};
    type.result.type = VIR_TYPE_I32;
    type.params = args;
    type.param_count = 2;
    type.va_start_slot = (unsigned int) -1;
    vir_machine_function_t machine;
    assert(vir_lower_machine(&function, &type, NULL, NULL, &machine));
    assert(machine.entry->ph2_ir_list.head->op == OP_eq);
    /* This is an allocator-selected fixed lane, not a forced location. */
    assert(machine.entry->ph2_ir_list.head->dest == 7);
    install_machine("probe_cmp", &machine);
    vir_function_release(&function);

    basic_block_t *head = arena_alloc_bb(), *fall = arena_alloc_bb();
    basic_block_t *live = arena_alloc_bb();
    head->rpo_next = fall;
    fall->rpo_next = live;
    head->rpo = 0;
    fall->rpo = 1;
    live->rpo = 2;
    head->elf_offset = fall->elf_offset = live->elf_offset = -1;
    ph2_ir_t *ir = bb_add_ph2_ir(head, OP_bit_and);
    ir->src0 = 0;
    ir->src1 = 1;
    ir->dest = 7;
    ir->size_bytes = 4;
    ir = bb_add_ph2_ir(head, OP_branch);
    ir->src0 = 7;
    ir->then_bb = live;
    ir->else_bb = fall;
    vir_machine_connect(head, live, THEN);
    vir_machine_connect(head, fall, ELSE);
    ir = bb_add_ph2_ir(fall, OP_load_constant);
    ir->dest = 7;
    ir->src0 = 99;
    ir->size_bytes = 4;
    ir = bb_add_ph2_ir(fall, OP_return);
    ir->src0 = 7;
    ir = bb_add_ph2_ir(live, OP_return);
    ir->src0 = 7;
    machine.entry = head;
    machine.stack_size = 0;
    machine.block_count = 3;
    install_machine("probe_and", &machine);
    ph2_compute_liveness();
    assert(head->machine_liveout & (1u << 7));
    assert(find_func("probe_cmp")->bbs->machine_liveout & (1u << 7));
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--no-folds")) {
        liveout_no_folds = true;
        argc--;
        argv++;
    }
    return compiler_main(argc, argv);
}
