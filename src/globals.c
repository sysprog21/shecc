/* Global objects */

block_t *BLOCKS;
int blocks_idx = 0;

func_t *FUNCS;
int funcs_idx = 0;

type_t *TYPES;
int types_idx = 0;

ir_instr_t *IR;
int ir_idx = 0;

alias_t *ALIASES;
int aliases_idx = 0;

constant_t *CONSTANTS;
int constants_idx = 0;

char *SOURCE;
int source_idx = 0;

/* ELF sections */

char *elf_code;
int elf_code_idx = 0;
char *elf_data;
int elf_data_idx = 0;
char *elf_header;
int elf_header_idx = 0;
int elf_header_len = 0x54; /* ELF fixed: 0x34 + 1 * 0x20 */
int elf_code_start;
char *elf_symtab;
char *elf_strtab;
char *elf_section;

/* options */

int dump_ir;

type_t *find_type(char *type_name)
{
    int i;
    for (i = 0; i < types_idx; i++)
        if (strcmp(TYPES[i].type_name, type_name) == 0)
            return &TYPES[i];
    return NULL;
}

ir_instr_t *add_instr(opcode_t op)
{
    ir_instr_t *ii = &IR[ir_idx];
    ii->op = op;
    ii->op_len = 0;
    ii->str_param1 = 0;
    ii->ir_index = ir_idx++;
    return ii;
}

block_t *add_block(block_t *parent, func_t *func)
{
    block_t *blk = &BLOCKS[blocks_idx];
    blk->index = blocks_idx++;
    blk->parent = parent;
    blk->func = func;
    blk->next_local = 0;
    return blk;
}

void add_alias(char *alias, char *value)
{
    alias_t *al = &ALIASES[aliases_idx++];
    strcpy(al->alias, alias);
    strcpy(al->value, value);
}

char *find_alias(char alias[])
{
    int i;
    for (i = 0; i < aliases_idx; i++)
        if (strcmp(alias, ALIASES[i].alias) == 0)
            return ALIASES[i].value;
    return NULL;
}

func_t *add_func(char *name)
{
    func_t *fn;
    int i;

    /* return existing if found */
    for (i = 0; i < funcs_idx; i++)
        if (strcmp(FUNCS[i].return_def.var_name, name) == 0)
            return &FUNCS[i];

    fn = &FUNCS[funcs_idx++];
    strcpy(fn->return_def.var_name, name);
    return fn;
}

type_t *add_type()
{
    return &TYPES[types_idx++];
}

type_t *add_named_type(char *name)
{
    type_t *type = add_type();
    strcpy(type->type_name, name);
    return type;
}

void add_constant(char alias[], int value)
{
    constant_t *constant = &CONSTANTS[constants_idx++];
    strcpy(constant->alias, alias);
    constant->value = value;
}

constant_t *find_constant(char alias[])
{
    int i;
    for (i = 0; i < constants_idx; i++)
        if (strcmp(CONSTANTS[i].alias, alias) == 0)
            return &CONSTANTS[i];
    return NULL;
}

func_t *find_func(char func_name[])
{
    int i;
    for (i = 0; i < funcs_idx; i++)
        if (strcmp(FUNCS[i].return_def.var_name, func_name) == 0)
            return &FUNCS[i];
    return NULL;
}

var_t *find_member(char token[], type_t *type)
{
    int i;
    for (i = 0; i < type->num_fields; i++)
        if (strcmp(type->fields[i].var_name, token) == 0)
            return &type->fields[i];
    return NULL;
}

var_t *find_local_var(char *token, block_t *block)
{
    int i;
    func_t *fn = block->func;

    for (; block; block = block->parent) {
        for (i = 0; i < block->next_local; i++)
            if (strcmp(block->locals[i].var_name, token) == 0)
                return &block->locals[i];
    }

    if (fn) {
        for (i = 0; i < fn->num_params; i++)
            if (strcmp(fn->param_defs[i].var_name, token) == 0)
                return &fn->param_defs[i];
    }
    return NULL;
}

var_t *find_global_var(char *token)
{
    int i;
    block_t *block = &BLOCKS[0];

    for (i = 0; i < block->next_local; i++)
        if (strcmp(block->locals[i].var_name, token) == 0)
            return &block->locals[i];
    return NULL;
}

var_t *find_var(char *token, block_t *parent)
{
    var_t *var = find_local_var(token, parent);
    if (var == NULL)
        var = find_global_var(token);
    return var;
}

int size_var(var_t *var)
{
    int s = 0;

    if (var->is_ptr > 0) {
        s += 4;
    } else {
        type_t *td = find_type(var->type_name);
        int bs = td->size;
        if (var->array_size > 0) {
            int j = 0;
            for (; j < var->array_size; j++)
                s += bs;
        } else
            s += bs;
    }
    return s;
}

/* This routine is required because the global variable initializations are
 * not supported now.
 */
void global_init()
{
    elf_code_start = ELF_START + elf_header_len;

    BLOCKS = malloc(MAX_BLOCKS * sizeof(block_t));
    FUNCS = malloc(MAX_FUNCS * sizeof(func_t));
    TYPES = malloc(MAX_TYPES * sizeof(type_t));
    IR = malloc(MAX_IR_INSTR * sizeof(ir_instr_t));
    SOURCE = malloc(MAX_SOURCE);
    ALIASES = malloc(MAX_ALIASES * sizeof(alias_t));
    CONSTANTS = malloc(MAX_CONSTANTS * sizeof(constant_t));

    elf_code = malloc(MAX_CODE);
    elf_data = malloc(MAX_DATA);
    elf_header = malloc(MAX_HEADER);
    elf_symtab = malloc(MAX_SYMTAB);
    elf_strtab = malloc(MAX_STRTAB);
    elf_section = malloc(MAX_SECTION);
}

void error(char *msg)
{
    /* TODO: figure out the corresponding C source and report line number */
    printf("Error %s at source location %d, IR index %d\n", msg, source_idx,
           ir_idx);
    abort();
}
