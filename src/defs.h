/* definitions */

/* Limitations */
#define MAX_TOKEN_LEN 256
#define MAX_ID_LEN 64
#define MAX_LINE_LEN 256
#define MAX_VAR_LEN 64
#define MAX_TYPE_LEN 64
#define MAX_PARAMS 8
#define MAX_LOCALS 48
#define MAX_FIELDS 32
#define MAX_FUNCS 512
#define MAX_FUNC_TRIES 1536
#define MAX_BLOCKS 262144
#define MAX_TYPES 64
#define MAX_IR_INSTR 65536
#define MAX_SOURCE 262144
#define MAX_CODE 262144
#define MAX_DATA 262144
#define MAX_SYMTAB 65536
#define MAX_STRTAB 65536
#define MAX_HEADER 1024
#define MAX_SECTION 1024
#define MAX_ALIASES 1024
#define MAX_CONSTANTS 1024
#define MAX_CASES 128
#define MAX_NESTING 128

#define ELF_START 0x10000
#define PTR_SIZE 4

/* builtin types */
typedef enum { TYPE_void = 0, TYPE_int, TYPE_char, TYPE_struct } base_type_t;

/* IR opcode */
typedef enum {
    /* generic: intermediate use in front-end. No code generation */
    OP_generic,

    /* calling convention */
    OP_func_extry, /* function entry point */
    OP_exit,       /* program exit routine */
    OP_call,       /* function call */
    OP_indirect,   /* indirect call with function pointer */
    OP_func_exit,  /* function exit code */
    OP_return,     /* jump to function exit */

    OP_load_constant,     /* load constant */
    OP_load_data_address, /* lookup address of a constant in data section */

    /* stack operations */
    OP_push, /* push onto stack */
    OP_pop,  /* pop from stack */

    /* control flow */
    OP_jump,        /* unconditional jump */
    OP_label,       /* note label */
    OP_jz,          /* jump if false */
    OP_jnz,         /* jump if true */
    OP_block_start, /* code block start */
    OP_block_end,   /* code block end */

    /* memory address operations */
    OP_address_of, /* lookup variable's address */
    OP_read,       /* read from memory address */
    OP_write,      /* write to memory address */

    /* arithmetic operators */
    OP_add,
    OP_sub,
    OP_mul,
    OP_div,     /* signed division */
    OP_mod,     /* modulo */
    OP_ternary, /* ? : */
    OP_lshift,
    OP_rshift,
    OP_log_and,
    OP_log_or,
    OP_log_not,
    OP_eq,  /* equal */
    OP_neq, /* not equal */
    OP_lt,  /* less than */
    OP_leq, /* less than or equal */
    OP_gt,  /* greater than */
    OP_geq, /* greater than or equal */
    OP_bit_or,
    OP_bit_and,
    OP_bit_xor,
    OP_bit_not,
    OP_negate,

    /* platform-specific */
    OP_syscall,
    OP_start
} opcode_t;

/* IR instruction */
typedef struct {
    opcode_t op;     /* IR operation */
    int op_len;      /* binary length */
    int ir_index;    /* index in IR list */
    int code_offset; /* offset in code */
    int param_no;    /* destination */
    int int_param1;
    int int_param2;
    char *str_param1;
} ir_instr_t;

/* variable definition */
typedef struct {
    char type_name[MAX_TYPE_LEN];
    char var_name[MAX_VAR_LEN];
    int is_ptr;
    int is_func;
    int array_size;
    int offset;   /* offset from stack or frame */
    int init_val; /* for global initialization */
} var_t;

typedef struct {
    char name[MAX_VAR_LEN];
    int is_variadic;
    int start_source_idx;
    int prev_return_idx; /* the return index of the previous macro */
    var_t param_defs[MAX_PARAMS];
    int num_param_defs;
    int params[MAX_PARAMS];
    int num_params;
} macro_t;

/* function definition */
typedef struct {
    var_t return_def;
    var_t param_defs[MAX_PARAMS];
    int num_params;
    int entry_point; /* IR index */
    int exit_point;  /* IR index */
    int params_size;
} func_t;

/* block definition */
typedef struct block_t {
    var_t locals[MAX_LOCALS];
    int next_local;
    struct block_t *parent;
    func_t *func;
    int locals_size;
    int index;
} block_t;

/* type definition */
typedef struct {
    char type_name[MAX_TYPE_LEN];
    base_type_t base_type;
    int size;
    var_t fields[MAX_FIELDS];
    int num_fields;
} type_t;

/* lvalue details */
typedef struct {
    int size;
    int is_ptr;
    type_t *type;
} lvalue_t;

/* alias for #defines */
typedef struct {
    char alias[MAX_VAR_LEN];
    char value[MAX_VAR_LEN];
} alias_t;

/* constants for enums */
typedef struct {
    char alias[MAX_VAR_LEN];
    int value;
} constant_t;

typedef struct {
    int index;
    int next[128];
} trie_t;
