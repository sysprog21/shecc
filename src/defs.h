/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* definitions */

/* Limitations */
#define MAX_TOKEN_LEN 256
#define MAX_ID_LEN 64
#define MAX_LINE_LEN 256
#define MAX_VAR_LEN 32
#define MAX_TYPE_LEN 32
#define MAX_PARAMS 8
#define MAX_LOCALS 1450
#define MAX_FIELDS 32
#define MAX_FUNCS 512
#define MAX_FUNC_TRIES 2160
#define MAX_BLOCKS 1150
#define MAX_TYPES 64
#define MAX_IR_INSTR 36864
#define MAX_BB_PRED 128
#define MAX_BB_DOM_SUCC 64
#define MAX_GLOBAL_IR 256
#define MAX_LABEL 4096
#define MAX_SOURCE 278528
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
#define MAX_OPERAND_STACK_SIZE 32
#define MAX_ANALYSIS_STACK_SIZE 750

#define ELF_START 0x10000
#define PTR_SIZE 4

/* Number of the available registers. Either 7 or 8 is accepted now. */
#define REG_CNT 8

/* This macro will be automatically defined at shecc run-time. */
#ifdef __SHECC__
/* use do-while as a substitution for nop */
#define UNUSED(x) \
    do {          \
        ;         \
    } while (0)
#define HOST_PTR_SIZE 4
#else
/* suppress GCC/Clang warnings */
#define UNUSED(x) (void) (x)
/* configure host data model when using `memcpy()` */
#define HOST_PTR_SIZE __SIZEOF_POINTER__
#endif

/* builtin types */
typedef enum {
    TYPE_void = 0,
    TYPE_int,
    TYPE_char,
    TYPE_struct,
    TYPE_typedef
} base_type_t;

/* IR opcode */
typedef enum {
    /* intermediate use in front-end. No code generation */
    OP_generic,

    OP_phi,
    OP_unwound_phi, /* work like address_of + store */

    /* calling convention */
    OP_define,   /* function entry point */
    OP_push,     /* prepare arguments */
    OP_call,     /* function call */
    OP_indirect, /* indirect call with function pointer */
    OP_return,   /* explicit return */

    OP_allocat, /* allocate space on stack */
    OP_assign,
    OP_load_constant,     /* load constant */
    OP_load_data_address, /* lookup address of a constant in data section */

    /* control flow */
    OP_label,
    OP_branch,      /* conditional jump */
    OP_jump,        /* unconditional jump */
    OP_func_ret,    /* returned value */
    OP_block_start, /* code block start */
    OP_block_end,   /* code block end */

    /* function pointer */
    OP_address_of_func, /* resolve function entry */
    OP_load_func,       /* prepare indirective call */
    OP_global_load_func,

    /* memory address operations */
    OP_address_of, /* lookup variable's address */
    OP_global_address_of,
    OP_load, /* load a word from stack */
    OP_global_load,
    OP_store, /* store a word to stack */
    OP_global_store,
    OP_read,  /* read from memory address */
    OP_write, /* write to memory address */

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

    /* entry point of the state machine */
    OP_start
} opcode_t;

/* variable definition */
typedef struct {
    int counter;
    int stack[64];
    int stack_idx;
} rename_t;

typedef struct ref_block ref_block_t;

struct ref_block_list {
    ref_block_t *head;
    ref_block_t *tail;
};

typedef struct ref_block_list ref_block_list_t;

struct var {
    char type_name[MAX_TYPE_LEN];
    char var_name[MAX_VAR_LEN];
    int is_ptr;
    int is_func;
    int is_global;
    int array_size;
    int offset;   /* offset from stack or frame, index 0 is reserved */
    int init_val; /* for global initialization */
    int liveness; /* live range */
    int in_loop;
    struct var *base;
    int subscript;
    struct var *subscripts[64];
    int subscripts_idx;
    rename_t rename;
    ref_block_list_t ref_block_list; /* blocks which kill variable */
    int consumed;
    int is_ternary_ret;
    int is_const; /* whether a constant representaion or not */
};

typedef struct var var_t;

typedef struct {
    char name[MAX_VAR_LEN];
    int is_variadic;
    int start_source_idx;
    var_t param_defs[MAX_PARAMS];
    int num_param_defs;
    int params[MAX_PARAMS];
    int num_params;
    int disabled;
} macro_t;

typedef struct fn fn_t;

/* function definition */
typedef struct {
    var_t return_def;
    var_t param_defs[MAX_PARAMS];
    int num_params;
    int va_args;
    int stack_size; /* stack always starts at offset 4 for convenience */
    fn_t *fn;
} func_t;

/* block definition */
struct block {
    var_t locals[MAX_LOCALS];
    int next_local;
    struct block *parent;
    func_t *func;
    macro_t *macro;
    int locals_size;
    int index;
};

typedef struct block block_t;

/* phase-1 IR definition */
typedef struct {
    opcode_t op;
    char func_name[MAX_VAR_LEN];
    int param_num;
    int size;
    var_t *dest;
    var_t *src0;
    var_t *src1;
} ph1_ir_t;

/* label lookup table*/
typedef struct {
    char name[MAX_VAR_LEN];
    int offset;
} label_lut_t;

typedef struct basic_block basic_block_t;

/* phase-2 IR definition */
struct ph2_ir {
    opcode_t op;
    int src0;
    int src1;
    int dest;
    char func_name[MAX_VAR_LEN];
    basic_block_t *next_bb;
    basic_block_t *then_bb;
    basic_block_t *else_bb;
    struct ph2_ir *next;
    int is_branch_detached;
};

typedef struct ph2_ir ph2_ir_t;

/* type definition */
struct type {
    char type_name[MAX_TYPE_LEN];
    base_type_t base_type;
    struct type *base_struct;
    int size;
    var_t fields[MAX_FIELDS];
    int num_fields;
};

typedef struct type type_t;

/* lvalue details */
typedef struct {
    int size;
    int is_ptr;
    int is_func;
    int is_reference;
    type_t *type;
} lvalue_t;

/* alias for #defines */
typedef struct {
    char alias[MAX_VAR_LEN];
    char value[MAX_VAR_LEN];
    int disabled;
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

struct phi_operand {
    var_t *var;
    basic_block_t *from;
    struct phi_operand *next;
};

typedef struct phi_operand phi_operand_t;

struct insn {
    struct insn *next;
    struct insn *prev;
    int idx;
    opcode_t opcode;
    var_t *rd;
    var_t *rs1;
    var_t *rs2;
    int sz;
    phi_operand_t *phi_ops;
    char str[64];
};

typedef struct insn insn_t;

typedef struct {
    insn_t *head;
    insn_t *tail;
} insn_list_t;

typedef struct {
    ph2_ir_t *head;
    ph2_ir_t *tail;
} ph2_ir_list_t;

typedef enum { NEXT, ELSE, THEN } bb_connection_type_t;

typedef struct {
    basic_block_t *bb;
    bb_connection_type_t type;
} bb_connection_t;

struct symbol {
    var_t *var;
    int index;
    struct symbol *next;
};

typedef struct symbol symbol_t;

typedef struct {
    symbol_t *head;
    symbol_t *tail;
} symbol_list_t;

struct basic_block {
    insn_list_t insn_list;
    ph2_ir_list_t ph2_ir_list;
    bb_connection_t prev[MAX_BB_PRED];
    struct basic_block *next;  /* normal BB */
    struct basic_block *then_; /* conditional BB */
    struct basic_block *else_;
    struct basic_block *idom;
    struct basic_block *rpo_next;
    struct basic_block *rpo_r_next;
    var_t *live_gen[MAX_ANALYSIS_STACK_SIZE];
    int live_gen_idx;
    var_t *live_kill[MAX_ANALYSIS_STACK_SIZE];
    int live_kill_idx;
    var_t *live_in[MAX_ANALYSIS_STACK_SIZE];
    int live_in_idx;
    var_t *live_out[MAX_ANALYSIS_STACK_SIZE];
    int live_out_idx;
    int rpo;
    int rpo_r;
    struct basic_block *DF[64];
    int df_idx;
    int visited;
    struct basic_block *dom_next[64];
    struct basic_block *dom_prev;
    fn_t *belong_to;
    block_t *scope;
    symbol_list_t symbol_list; /* variable declaration */
    int elf_offset;
};

struct ref_block {
    basic_block_t *bb;
    struct ref_block *next;
};

/* TODO: integrate func_t into fn_t */
struct fn {
    basic_block_t *bbs;
    basic_block_t *exit;
    symbol_list_t global_sym_list;
    int bb_cnt;
    int visited;
    func_t *func;
    struct fn *next;
};

typedef struct {
    fn_t *head;
    fn_t *tail;
} func_list_t;

typedef struct {
    fn_t *fn;
    basic_block_t *bb;
    void (*preorder_cb)(fn_t *, basic_block_t *);
    void (*postorder_cb)(fn_t *, basic_block_t *);
} bb_traversal_args_t;

typedef struct {
    var_t *var;
    int polluted;
} regfile_t;
