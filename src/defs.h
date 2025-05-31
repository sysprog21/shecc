/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once
#include <stdbool.h>

/* definitions */

/* Limitations */
#define MAX_TOKEN_LEN 256
#define MAX_ID_LEN 64
#define MAX_LINE_LEN 256
#define MAX_VAR_LEN 32
#define MAX_TYPE_LEN 32
#define MAX_PARAMS 8
#define MAX_LOCALS 1600
#define MAX_FIELDS 64
#define MAX_TYPES 64
#define MAX_IR_INSTR 60000
#define MAX_BB_PRED 128
#define MAX_BB_DOM_SUCC 64
#define MAX_BB_RDOM_SUCC 256
#define MAX_GLOBAL_IR 256
#define MAX_SOURCE 524288
#define MAX_CODE 262144
#define MAX_DATA 262144
#define MAX_SYMTAB 65536
#define MAX_STRTAB 65536
#define MAX_HEADER 1024
#define MAX_SECTION 1024
#define MAX_ALIASES 128
#define MAX_CONSTANTS 1024
#define MAX_CASES 128
#define MAX_NESTING 128
#define MAX_OPERAND_STACK_SIZE 32
#define MAX_ANALYSIS_STACK_SIZE 800

/* Default capacities for common data structures */
/* Default arena size is initialized with 256 KiB */
#define DEFAULT_ARENA_SIZE 262144
#define DEFAULT_FUNCS_SIZE 64
#define DEFAULT_INCLUSIONS_SIZE 16

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
/* configure host data model when using 'memcpy'. */
#define HOST_PTR_SIZE __SIZEOF_POINTER__
#endif

/* Common data structures */
typedef struct arena_block {
    char *memory;
    int capacity;
    int offset;
    struct arena_block *next;
} arena_block_t;

typedef struct {
    arena_block_t *head;
} arena_t;

/* string-based hash map definitions */

typedef struct hashmap_node {
    char *key;
    void *val;
    struct hashmap_node *next;
} hashmap_node_t;

typedef struct {
    int size;
    int cap;
    hashmap_node_t **buckets;
} hashmap_t;

/* lexer tokens */
typedef enum {
    T_start, /* FIXME: it was intended to start the state machine. */
    T_numeric,
    T_identifier,
    T_comma,  /* , */
    T_string, /* null-terminated string */
    T_char,
    T_open_bracket,  /* ( */
    T_close_bracket, /* ) */
    T_open_curly,    /* { */
    T_close_curly,   /* } */
    T_open_square,   /* [ */
    T_close_square,  /* ] */
    T_asterisk,      /* '*' */
    T_divide,        /* / */
    T_mod,           /* % */
    T_bit_or,        /* | */
    T_bit_xor,       /* ^ */
    T_bit_not,       /* ~ */
    T_log_and,       /* && */
    T_log_or,        /* || */
    T_log_not,       /* ! */
    T_lt,            /* < */
    T_gt,            /* > */
    T_le,            /* <= */
    T_ge,            /* >= */
    T_lshift,        /* << */
    T_rshift,        /* >> */
    T_dot,           /* . */
    T_arrow,         /* -> */
    T_plus,          /* + */
    T_minus,         /* - */
    T_minuseq,       /* -= */
    T_pluseq,        /* += */
    T_asteriskeq,    /* *= */
    T_divideeq,      /* /= */
    T_modeq,         /* %= */
    T_lshifteq,      /* <<= */
    T_rshifteq,      /* >>= */
    T_xoreq,         /* ^= */
    T_oreq,          /* |= */
    T_andeq,         /* &= */
    T_eq,            /* == */
    T_noteq,         /* != */
    T_assign,        /* = */
    T_increment,     /* ++ */
    T_decrement,     /* -- */
    T_question,      /* ? */
    T_colon,         /* : */
    T_semicolon,     /* ; */
    T_eof,           /* end-of-file (EOF) */
    T_ampersand,     /* & */
    T_return,
    T_if,
    T_else,
    T_while,
    T_for,
    T_do,
    T_typedef,
    T_enum,
    T_struct,
    T_sizeof,
    T_elipsis, /* ... */
    T_switch,
    T_case,
    T_break,
    T_default,
    T_continue,
    /* C pre-processor directives */
    T_cppd_include,
    T_cppd_define,
    T_cppd_undef,
    T_cppd_error,
    T_cppd_if,
    T_cppd_elif,
    T_cppd_else,
    T_cppd_endif,
    T_cppd_ifdef,
    T_cppd_ifndef,
    T_cppd_pragma
} token_t;

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
    OP_branch,   /* conditional jump */
    OP_jump,     /* unconditional jump */
    OP_func_ret, /* returned value */

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

    /* data type conversion */
    OP_trunc,
    OP_sign_ext,

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

typedef struct insn insn_t;

typedef struct use_chain_node {
    insn_t *insn;
    struct use_chain_node *next;
    struct use_chain_node *prev;
} use_chain_t;

typedef struct var var_t;
typedef struct type type_t;

typedef struct var_list {
    int capacity;
    int size;
    var_t **elements;
} var_list_t;

struct var {
    type_t *type;
    char var_name[MAX_VAR_LEN];
    int is_ptr;
    bool is_func;
    bool is_global;
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
    use_chain_t *users_head;
    use_chain_t *users_tail;
    struct insn *last_assign;
    int consumed;
    bool is_ternary_ret;
    bool is_logical_ret;
    bool is_const; /* whether a constant representaion or not */
};

typedef struct {
    char name[MAX_VAR_LEN];
    bool is_variadic;
    int start_source_idx;
    var_t param_defs[MAX_PARAMS];
    int num_param_defs;
    int params[MAX_PARAMS];
    int num_params;
    bool disabled;
} macro_t;

typedef struct func func_t;

/* block definition */
struct block {
    var_list_t locals;
    struct block *parent;
    func_t *func;
    macro_t *macro;
    struct block *next;
};

typedef struct block block_t;
typedef struct basic_block basic_block_t;

/* Definition of a growable buffer for a mutable null-terminated string
 * size:     Current number of elements in the array
 * capacity: Number of elements that can be stored without resizing
 * elements: Pointer to the array of characters
 */
typedef struct {
    int size;
    int capacity;
    char *elements;
} strbuf_t;

/* Definition of a generic dynamic array.
 *
 * size:      Number of elements currently in use.
 * capacity:  Number of elements the buffer can hold before reallocation.
 * elem_size: Size of each element in bytes.
 * elements:  Pointer to the data buffer.
 * arena:     Arena allocator used for allocations.
 */
typedef struct {
    int size;
    int capacity;
    int elem_size;
    void *elements;

    arena_t *arena;
} dynarr_t;

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
    bool is_branch_detached;
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

/* lvalue details */
typedef struct {
    int size;
    int is_ptr;
    bool is_func;
    bool is_reference;
    type_t *type;
} lvalue_t;

/* alias for #defines */
typedef struct {
    char alias[MAX_VAR_LEN];
    char value[MAX_VAR_LEN];
    bool disabled;
} alias_t;

/* constants for enums */
typedef struct {
    char alias[MAX_VAR_LEN];
    int value;
} constant_t;

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
    bool useful; /* Used in DCE process. Set true if instruction is useful. */
    basic_block_t *belong_to;
    phi_operand_t *phi_ops;
    char str[64];
};

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
    char bb_label_name[MAX_VAR_LEN]; /* Used in instruction dumping when ir_dump
                                        is enabled. */
    struct basic_block *next;        /* normal BB */
    struct basic_block *then_;       /* conditional BB */
    struct basic_block *else_;
    struct basic_block *idom;
    struct basic_block *r_idom;
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
    struct basic_block *RDF[64];
    int df_idx;
    int rdf_idx;
    int visited;
    bool useful; /* indicate whether this BB contains useful instructions */
    struct basic_block *dom_next[64];
    struct basic_block *dom_prev;
    struct basic_block *rdom_next[256];
    struct basic_block *rdom_prev;
    func_t *belong_to;
    block_t *scope;
    symbol_list_t symbol_list; /* variable declaration */
    int elf_offset;
};

struct ref_block {
    basic_block_t *bb;
    struct ref_block *next;
};

/**
 * Syntatic representation of func, combines syntactic details
 * (e.g., return type, parameters) with SSA-related information
 * (e.g., basic blocks, control flow) to support parsing,
 * analysis, optimization, and code generation.
 */
struct func {
    /* Syntatic info */
    var_t return_def;
    var_t param_defs[MAX_PARAMS];
    int num_params;
    int va_args;
    int stack_size; /* stack always starts at offset 4 for convenience */

    /* SSA info */
    basic_block_t *bbs;
    basic_block_t *exit;
    symbol_list_t global_sym_list;
    int bb_cnt;
    int visited;

    struct func *next;
};

typedef struct {
    func_t *head;
    func_t *tail;
} func_list_t;

typedef struct {
    func_t *func;
    basic_block_t *bb;
    void (*preorder_cb)(func_t *, basic_block_t *);
    void (*postorder_cb)(func_t *, basic_block_t *);
} bb_traversal_args_t;

typedef struct {
    var_t *var;
    int polluted;
} regfile_t;

/* FIXME: replace char[2] with a short data type in ELF header structures */
/* ELF header */
typedef struct {
    char e_ident[16];
    char e_type[2];
    char e_machine[2];
    int e_version;
    int e_entry;
    int e_phoff;
    int e_shoff;
    int e_flags;
    char e_ehsize[2];
    char e_phentsize[2];
    char e_phnum[2];
    char e_shentsize[2];
    char e_shnum[2];
    char e_shstrndx[2];
} elf32_hdr_t;

/* ELF program header */
typedef struct {
    int p_type;
    int p_offset;
    int p_vaddr;
    int p_paddr;
    int p_filesz;
    int p_memsz;
    int p_flags;
    int p_align;
} elf32_phdr_t;

/* ELF section header */
typedef struct {
    int sh_name;
    int sh_type;
    int sh_flags;
    int sh_addr;
    int sh_offset;
    int sh_size;
    int sh_link;
    int sh_info;
    int sh_addralign;
    int sh_entsize;
} elf32_shdr_t;
