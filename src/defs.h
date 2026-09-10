/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once
#include <stdbool.h>

/* definitions */

/* Common macro functions */
#define is_newline(c) (c == '\r' || c == '\n')

/* Whether the C library in the output buffers its own stream I/O, so that a
 * whole block can be handed to fread() or fwrite() in one call.
 *
 * A host compiler's runtime does, and so does the one a dynamically linked
 * shecc reaches through the PLT. The embedded lib/c.c does not: it has no
 * buffer behind fgets() or fputc(), so every byte would become its own read(2)
 * or write(2). Those builds call the kernel directly instead, which is
 * available on exactly the same condition, since '__syscall' is synthesized
 * only for static linking.
 */
#ifdef __SHECC__
#ifdef __SHECC_DYNLINK__
#define HOST_BUFFERED_STDIO
#endif
#else
#define HOST_BUFFERED_STDIO
#endif

/* Limitations */
#define MAX_TOKEN_LEN 256
#define MAX_ID_LEN 64
#define MAX_LINE_LEN 256
#define MAX_VAR_LEN 128
/* ".label." plus an int, for basic_block_t's dump name. */
#define MAX_LABEL_LEN 24

/* Staging buffer for one instruction's Graphviz label in bb_dump(). The widest
 * case is a binary operator: three MAX_VAR_LEN names, three subscripts, an
 * operator and 41 bytes of markup.
 */
#define DUMP_INSN_LEN 512
#define MAX_TYPE_LEN 32

/* Declaration limit, and with MAX_ARGS_IN_REG it also sizes the outgoing
 * stack-argument area every frame reserves (see add_func() in globals.c). A
 * target passing many arguments in registers must raise it or that area is
 * empty and a call with more arguments overwrites the caller's first locals.
 * Raising it for everyone would widen func_t.param_defs and the variadic spill
 * on targets that gain nothing, so each mk file states its own.
 */
#ifndef MAX_PARAMS
#define MAX_PARAMS 8
#endif
#define MAX_LOCALS 3200
#define MAX_FIELDS 64
#define MAX_TYPES 256
#define MAX_LABELS 256
/* Pending postfix ++/-- effects in one statement; each one appends 3. */
#define MAX_SIDE_EFFECT 64
/* Elements captured from an implicitly sized array initializer. */
#define MAX_IMPLICIT_ARRAY 256
/* A self-compile emits ~101k ph2_ir; one pointer per slot in PH2_IR_FLATTEN. */
#define MAX_IR_INSTR 262144
#define MAX_BB_PRED 128
#define MAX_BB_DOM_SUCC 64
#define MAX_CODE 262144
#define MAX_DATA 262144
#define MAX_SYMTAB 65536
#define MAX_STRTAB 65536
#define MAX_HEADER 1024
#define MAX_PROGRAM_HEADER 1024
#define MAX_SECTION_HEADER 1024
#define MAX_SHSTR 1024
#define MAX_INTERP 1024
#define MAX_DYNAMIC 1024
#define MAX_DYNSYM 1024
#define MAX_DYNSTR 1024
#define MAX_RELPLT 1024
#define MAX_RELAPLT 1024
#define MAX_PLT 1024
#define MAX_GOTPLT 1024
#define MAX_CONSTANTS 1024
#define MAX_NESTING 128

/* How many instructions an if may speculate when flattened into a select, and
 * how many blocks one of its arms may span. Beyond a handful, running the arm
 * that would have been skipped costs more than the misprediction it avoids.
 */
#define MAX_SPECULATED_INSNS 8
#define MAX_IF_ARM_BLOCKS 4

/* Recursion limits for nesting in the input. The parser descends recursively
 * for each of these, so a deeply nested program would otherwise exhaust the
 * machine stack before any diagnostic could be produced.
 */
#define MAX_EXPR_DEPTH 256
#define MAX_BLOCK_DEPTH 256
#define MAX_OPERAND_STACK_SIZE 32

/* Depth of the operator stack that read_expr() and the constant-expression
 * evaluator shunt through, and of the value stack the latter keeps beside it.
 */
#define MAX_OPERATOR_STACK_SIZE 10
#define MAX_ANALYSIS_STACK_SIZE 1600

/* Default capacities for common data structures, with the arena sizes taken
 * from typical usage patterns.
 */
#define DEFAULT_ARENA_SIZE 262144 /* 256 KiB - standard default */
#define SMALL_ARENA_SIZE 65536    /* 64 KiB - for small allocations */
#define LARGE_ARENA_SIZE 524288   /* 512 KiB - for instruction arena */
#define DEFAULT_FUNCS_SIZE 64
#define DEFAULT_SRC_FILE_COUNT 8

/* Arena compaction bitmask flags for selective memory reclamation */
#define COMPACT_ARENA_BLOCK 0x01   /* BLOCK_ARENA - variables/blocks */
#define COMPACT_ARENA_INSN 0x02    /* INSN_ARENA - instructions */
#define COMPACT_ARENA_BB 0x04      /* BB_ARENA - basic blocks */
#define COMPACT_ARENA_HASHMAP 0x08 /* HASHMAP_ARENA - hash nodes */
#define COMPACT_ARENA_GENERAL 0x10 /* GENERAL_ARENA - misc allocations */

#define ELF_START 0x10000
#ifndef PTR_SIZE
#define PTR_SIZE 4
#endif

/* Registers the allocator may hand out. A target with more spare registers can
 * raise this from its own configuration.
 */
#ifndef REG_CNT
#define REG_CNT 8
#endif

/* This macro will be automatically defined at shecc run-time. */
#ifdef __SHECC__
/* use do-while as a substitution for nop */
#define UNUSED(x) \
    do {          \
        ;         \
    } while (0)

/* shecc runs on the target it compiles for, so the host pointer width is the
 * target's. This must not be hardcoded to 4: on an LP64 target the var_list
 * allocations and memcpy sizes below would be half what they need.
 */
#define HOST_PTR_SIZE PTR_SIZE

/* shecc parses no attributes, and does not need the hint: it reports what it
 * cannot compile rather than warning about it.
 */
#define __noreturn
#else
/* suppress GCC/Clang warnings */
#define UNUSED(x) (void) (x)
/* configure host data model when using 'memcpy'. */
#define HOST_PTR_SIZE __SIZEOF_POINTER__

/* Marks the diagnostic paths that never come back. Without it the host compiler
 * cannot see that a variable set on every surviving path is initialized, and
 * reports each of those as a maybe-uninitialized use -- which is why the
 * warning used to be switched off for the whole tree, taking the real cases
 * with it.
 */
#define __noreturn __attribute__((noreturn))
#endif

#ifndef MIN_ALIGNMENT
#define MIN_ALIGNMENT 8
#endif

#ifndef ALIGN_UP
#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))
#endif

/* Targets whose PLT has no lazy-resolution path ask the loader to bind every
 * PLT entry at load time.
 */
#ifndef DYN_BIND_NOW
#define DYN_BIND_NOW 0
#endif

#define ELF_MACHINE_ARM32 0x28
#define ELF_MACHINE_RV32 0xf3
#define ELF_MACHINE_X86_64 0x3e
#define ELF_MACHINE_AARCH64 0xb7

/* ELF class of the active target: a 64-bit pointer means ELF64, and every
 * 32-bit target means ELF32. Used to select the header/segment writers in
 * elf.c. Deriving it from the pointer width rather than listing machines means
 * a new target gets the right class from the PTR_SIZE its mk file already
 * states.
 */
#if PTR_SIZE == 8
#define ELF_IS_64 1
#else
#define ELF_IS_64 0
#endif

/* Ceilings for strength_reduce(): how long a chain of instructions may compute
 * one address, and how many rounds the step analysis takes to settle.
 */
#define MAX_IV_CHAIN 12
#define MAX_IV_ROUNDS 4

/* A pointer the loop advances costs a register for the whole loop and an
 * addition at the bottom of it, so it only pays where it replaces more work
 * than that -- and only a couple of them fit before the loop's own variables
 * start going to the frame instead.
 */
#define MIN_IV_CHAIN 3
#define MAX_IV_PER_LOOP 2

/* How many blocks one natural loop's walk keeps in hand at once. Past this the
 * walk stops widening, which can only understate a depth.
 */
#define MAX_LOOP_WALK 512

/* Limits on copying a function into its callers: how big a body is worth
 * copying, how many distinct variables one such body may name, and how many
 * times the pass sweeps the program so that a function which becomes copyable
 * only after its own callee was copied into it still gets copied.
 */
#define MAX_INLINE_INSNS 16
#define MAX_INLINE_VARS 32
#define MAX_INLINE_ROUNDS 3

/* What a naming inside one loop is worth against one in straight-line code, and
 * how many nesting levels still multiply it.
 */
#define LOOP_USE_WEIGHT 8
#define MAX_WEIGHTED_LOOP_DEPTH 3

/* How many registers at the top of the allocator's file a call preserves. Only
 * such a register can hold a value across a call, so only these may be given to
 * a variable for the whole of a function that calls anything. A target states
 * its own count in mk/<arch>.mk, beside the REG_CNT that fixes the file it
 * counts from; a target that has not had its file checked this way keeps none.
 *
 * HAVE_COND_MOVE comes from the same place and says whether the target can
 * select between two values without branching, which is what makes flattening
 * an if into a select worthwhile.
 */
#ifndef CALLEE_SAVED_REGS
#define CALLEE_SAVED_REGS 0
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
    int total_bytes; /* Track total allocation for profiling */
    int block_size;  /* Default block size for new blocks */
} arena_t;

/* string-based hash map definitions */

typedef struct hashmap_node {
    char *key;
    void *val;
    bool occupied;
} hashmap_node_t;

typedef struct {
    int size;
    int cap;
    hashmap_node_t *table;
} hashmap_t;

/* lexer tokens */
typedef enum {
    T_start, /* FIXME: Unused, intended for lexer state machine init */
    T_eof,   /* end-of-file (EOF) */
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
    T_union,
    T_sizeof,
    T_elipsis, /* ... */
    T_switch,
    T_case,
    T_break,
    T_default,
    T_continue,
    T_goto,
    T_const, /* const qualifier */
    T_static,
    T_signed,
    T_unsigned,
    T_long,
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
    T_cppd_pragma,

    /* C pre-processor specific, these kinds will be removed after
     * pre-processing is done.
     */
    T_newline,
    T_backslash,
    T_whitespace,
    T_tab,

    /* '#' and '##' inside a macro replacement list. Resolved while the macro is
     * expanded, so neither ever reaches the parser.
     */
    T_hash,
    T_hashhash
} token_kind_t;

/* Source location tracking for better error reporting */
typedef struct {
    int pos; /* raw source file position */
    int len; /* length of token */
    int line;
    int column;
    char *filename;
} source_location_t;

typedef struct token {
    token_kind_t kind;
    char *literal;
    source_location_t location;
    struct token *next;
} token_t;

typedef struct token_stream {
    token_t *head;
    token_t *tail;
} token_stream_t;

/* String pool for identifier deduplication */
typedef struct {
    hashmap_t *strings; /* Map string -> interned string */
} string_pool_t;

/* String literal pool for deduplicating string constants */
typedef struct {
    hashmap_t *literals; /* Map string literal -> ELF data offset */
} string_literal_pool_t;

/* builtin types */
typedef enum {
    TYPE_void = 0,
    TYPE_int,
    TYPE_char,
    TYPE_short,
    TYPE_long_long,
    TYPE_struct,
    TYPE_union,
    TYPE_typedef
} base_type_t;

/* IR opcode */
typedef enum {
    /* intermediate use in front-end. No code generation */
    OP_generic,

    OP_phi,
    OP_unwound_phi, /* work like address_of + store */
    /* rd = rs2 ? rs1 : rs3 -- select without a branch. */
    OP_cmov,

    /* calling convention */
    OP_define,   /* function entry point */
    OP_push,     /* prepare arguments */
    OP_call,     /* function call */
    OP_indirect, /* indirect call with function pointer */
    OP_return,   /* explicit return */

    OP_allocat, /* allocate space on stack */
    OP_assign,
    OP_load_constant,       /* load constant */
    OP_load_data_address,   /* lookup address of a constant in data section */
    OP_load_rodata_address, /* lookup address of a constant in rodata section */

    /* control flow */
    OP_branch,   /* conditional jump */
    OP_jump,     /* unconditional jump */
    OP_func_ret, /* returned value */
    OP_label,    /* for goto label */

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
    OP_cast,

    /* entry point of the state machine */
    OP_start
} opcode_t;

/* variable definition */

/* Depth of the SSA renaming stack: how many definitions of one variable can be
 * live along a single dominator path.
 */
#define MAX_RENAME_STACK 64

typedef struct {
    int counter;

    /* Grown on demand: only a base variable is ever renamed, so the SSA
     * versions copied from it -- the large majority of all variables -- would
     * otherwise each carry an unused MAX_RENAME_STACK array.
     */
    int *stack;
    int stack_idx;
    int stack_cap;
} rename_t;

typedef struct ref_block ref_block_t;

struct ref_block_list {
    ref_block_t *head, *tail;
};

typedef struct ref_block_list ref_block_list_t;

typedef struct insn insn_t;

typedef struct use_chain_node {
    insn_t *insn;
    struct use_chain_node *next, *prev;
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

    /* Interned, not copied. A MAX_VAR_LEN array was 128 of this struct's 312
     * bytes on every one of the ~86k variables a self-compile creates, and
     * var_t is embedded by value MAX_FIELDS times in each type_t and MAX_PARAMS
     * times in each func_t, so the array cost another 8 KiB per type. Generated
     * temporary names come from gen_name(); source-level names come from
     * intern_string(). Never NULL -- an unnamed variable holds "".
     */
    char *var_name;
    int ptr_level;
    bool is_func;
    bool is_global;
    bool is_static;       /* declaration used the static storage class */
    bool has_initializer; /* a file-scope definition supplied an initializer */
    bool is_const_qualified; /* true if variable has const qualifier */
    bool is_const_pointer;   /* true for the outermost `* const` qualifier */
    bool address_taken;      /* true if variable address was taken (&var) */
    /* Working state for strength_reduce(): how many instructions in the
     * function write the variable, whether it is written inside the loop being
     * examined, and how much its value moves per iteration when it does. All
     * three are recomputed per loop; nothing outside that pass reads them.
     */
    int def_cnt;
    int loop_stamp;
    int iv_gen;
    int iv_step;

    /* pin_registers()'s tally for the variable: what its namings are worth
     * weighted by loop depth, the reverse-post-order number of the last block
     * that named it, whether it was ever named in two, and whether a loop named
     * it. Stamped per function so that no array has to hold the candidates --
     * the file has a handful of registers and a function names hundreds of
     * variables, and the one worth a register is not reliably among the first
     * few met.
     */
    int pin_gen;
    int pin_weight;
    int pin_blk;
    bool pin_cross;
    bool pin_hot;

    /* Defined inside an arm that if_convert() flattened into a select. The
     * register its variable is pinned to still holds the value flowing into the
     * select, which the arms read and the select overwrites, so a value
     * computed on the way there must go somewhere else.
     */
    bool in_select_arm;
    int array_size;
    int array_dim2;  /* second dimension size for 2D arrays */
    int offset;      /* offset from stack or frame, index 0 is reserved */
    int init_val;    /* for global initialization */
    int init_val_hi; /* upper word of an 8-byte integer constant */
    /* Generation stamps used by compute_live_in() to test set membership in
     * constant time instead of rescanning live_kill and live_in per element.
     */
    int kill_gen;
    int in_gen;
    /* Stamp for the successor-union set merge_live_in() builds. */
    int merge_gen;
    struct var *base;
    int subscript;

    /* Every SSA version of this variable, grown on demand. A fixed 128-entry
     * array made every var_t 1 KiB heavier -- and var_t is embedded by value in
     * type_t's field table and in every function's parameter list -- while the
     * append was unchecked, so a variable assigned more than 128 times in one
     * function wrote past the end.
     */
    struct var **subscripts;
    int subscripts_idx;
    int subscripts_cap;

    /* SSA renaming state, allocated on first use by var_rename(). Only a base
     * variable is ever renamed, so the versions copied from it -- the large
     * majority of all variables -- each carried an unused 24-byte rename_t
     * inline. Field order here is load-bearing: reordering var_t breaks the
     * bootstrap, so the pointer stays where the struct sat.
     */
    rename_t *rename;
    ref_block_list_t ref_block_list; /* blocks which kill variable */
    use_chain_t *users_head, *users_tail;
    struct insn *last_assign;
    int consumed;
    bool is_ternary_ret;
    bool is_logical_ret;
    bool is_const; /* whether a constant representaion or not */
    int phys_reg;  /* Physical register assignment (-1 if unassigned) */
    int first_use; /* First instruction index where variable is used */
    int last_use;  /* Last instruction index where variable is used */
    int use_count; /* Number of times variable is used */
    bool space_is_allocated; /* whether space is allocated for this variable */
    bool has_backing_storage;

    /* This flag is used to indicate to the compiler that the offset of the
     * variable is based on the top of the local stack.
     */
    bool ofs_based_on_stack_top;

    /* True when this variable was synthesized to hold a compound literal (e.g.,
     * array or struct literal temporaries).
     */
    bool is_compound_literal;

    /* String literals have immutable storage duration in C. Preserve that
     * provenance separately from the pointer type so the compatibility warning
     * can remain opt-in while legacy source still compiles.
     */
    bool is_string_literal;

    /* A function-pointer declarator owns a prototype separately from its value
     * type. Keeping this syntax-only object lets an indirect call use the same
     * argument lowering as a direct call (notably record-by-value arguments)
     * without putting function ABI details into type_t. Kept void-typed so the
     * self-hosted parser does not need an incomplete `struct func` declaration
     * while reading var_t itself. parser.c owns the cast back to func_t.
     */
    void *func_signature;

    /* C ABI lowering passes record parameters as pointers to caller-owned
     * copies. The source-level declaration remains a record so field access and
     * record assignment keep their C semantics; OP_address_of materializes the
     * hidden incoming pointer instead of an address of a scalar slot.
     */
    bool is_aggregate_param;
};

typedef struct func func_t;

/* block definition */
struct block {
    var_list_t locals;
    struct block *parent;
    func_t *func;
    struct block *next;
};

typedef struct block block_t;
typedef struct basic_block basic_block_t;

/* Definition of a growable buffer for a mutable null-terminated string
 * @size: Current number of elements in the array
 * @capacity: Number of elements that can be stored without resizing
 * @elements: Pointer to the array of characters
 */
typedef struct {
    int size;
    int capacity;
    char *elements;
} strbuf_t;

/* phase-2 IR definition */
struct ph2_ir {
    /* Grouped by width so the struct carries no interior padding: mixed in
     * declaration order it was 72 bytes for 63 bytes of fields, on all ~101k of
     * them a self-compile emits. Callee / definition name, interned in
     * GENERAL_ARENA rather than copied. A MAX_VAR_LEN array here was 128 of
     * this struct's 192 bytes while only OP_define, OP_call and
     * OP_address_of_func ever name anything. NULL when unused.
     */
    char *func_name;
    basic_block_t *next_bb;
    basic_block_t *then_bb;
    basic_block_t *else_bb;
    struct ph2_ir *next;

    opcode_t op;
    int src0;
    int src1;
    /* The register OP_cmov keeps when its condition does not hold. */
    int src2;
    int dest;
    /* Type information for LP64 support */
    int size_bytes; /* Size in bytes for load/store/read/write operations */

    bool is_branch_detached;

    /* When an instruction uses a variable that its offset is based on the top
     * of the stack, this instruction's flag is also set to indicate the
     * compiler to recalculate the offset after the function's stack size has
     * been determined.
     *
     * Currently, only OP_load, OP_store and OP_address_of need this flag to
     * recompute the offset.
     */
    bool ofs_based_on_stack_top;
    bool is_pointer; /* True if this operation involves a pointer type */
    /* Scalar signedness accompanies register values independently of their
     * storage width. Comparisons inspect their sources; arithmetic and loads
     * inspect the result.
     */
    bool is_unsigned;

    /* Operand provenance is required by LP64 backends: pointer arithmetic keeps
     * the address operand wide but sign-extends an int index.
     */
    bool src0_is_pointer;
    bool src1_is_pointer;
    bool src0_is_unsigned;
    bool src1_is_unsigned;
};

typedef struct ph2_ir ph2_ir_t;

/* type definition */
struct type {
    char type_name[MAX_TYPE_LEN];
    base_type_t base_type;
    struct type *base_struct;
    int size;

    /* Member table, allocated when the type is created rather than inlined. A
     * MAX_FIELDS array of var_t by value made type_t 12 KiB, and TYPES is a
     * flat MAX_TYPES array that global_init() zeroes up front -- 3 MiB of
     * resident memory for the 90 types a self-compile actually declares.
     */
    var_t *fields;
    int num_fields;
    int ptr_level; /* pointer level for typedef pointer types */
    bool is_union; /* preserves union semantics for anonymous typedef unions */
    bool is_const_qualified; /* qualifier carried by a scalar typedef */
    /* Integer representation is distinct from signedness: unsigned char and
     * unsigned int keep the ordinary scalar widths but require zero extension
     * and unsigned arithmetic lowering.
     */
    bool is_unsigned;
};

/* lvalue details */
typedef struct {
    int size;
    int ptr_level;
    bool is_func;
    bool is_reference;
    bool is_const_qualified;
    type_t *type;
    /* The declaration selected by the lvalue, including a struct member. */
    var_t *decl;
} lvalue_t;

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
    struct insn *next, *prev;
    int idx;
    opcode_t opcode;
    var_t *rd;
    var_t *rs1;
    var_t *rs2;

    /* The value OP_cmov keeps when its condition does not hold. A select needs
     * three inputs and the two source fields are taken by the chosen value and
     * the condition; every other opcode leaves this NULL.
     */
    var_t *rs3;
    int sz;
    bool useful; /* Used in DCE process. Set true if instruction is useful. */
    basic_block_t *belong_to;
    phi_operand_t *phi_ops;

    /* Callee or goto-label name, interned rather than copied. add_insn()
     * already interned the text before copying it in, so the array was 64 of
     * this struct's 136 bytes for a string the pool owns anyway, on all ~73k
     * instructions a self-compile builds. NULL when the opcode names nothing.
     */
    char *str;
};

typedef struct {
    insn_t *head, *tail;
} insn_list_t;

typedef struct {
    ph2_ir_t *head, *tail;
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
    symbol_t *head, *tail;
} symbol_list_t;

struct basic_block {
    /* Members are grouped by width -- 16-byte lists, then pointers, then ints,
     * then the flags -- so the struct carries no interior padding. Mixed in
     * declaration order it was 320 bytes for 302 bytes of fields, on every one
     * of the ~51k blocks a self-compile creates.
     */
    insn_list_t insn_list;
    ph2_ir_list_t ph2_ir_list;
    var_list_t live_gen;
    var_list_t live_kill;
    var_list_t live_in;
    var_list_t live_out;
    symbol_list_t symbol_list; /* variable declaration */

    /* Predecessor edges, grown on demand. A fixed MAX_BB_PRED array cost two
     * kilobytes in every basic block -- by far the largest thing in one --
     * while almost every block has one or two predecessors.
     */
    bb_connection_t *prev;

    /* Register file on entry to this block, captured by reg_alloc() at the end
     * of the predecessor it falls out of. Non-NULL only when a file was handed
     * over, and bb_export_regs() does that only for an edge that is all three
     * of: the predecessor's single successor, that predecessor's rpo_next, and
     * this block's single predecessor. A sole predecessor alone is NOT enough
     * -- a branch target is emitted wherever the backend's linear walk puts it,
     * so the registers reaching it are not the ones the branch left.
     *
     * Allocated only for a block that actually inherits a file: fewer than one
     * block in thirteen does, so a REG_CNT array here cost 64 bytes on all ~51k
     * blocks to serve 7% of them.
     */
    struct var **entry_regs;

    /* Used in instruction dumping when ir_dump is enabled, and allocated only
     * then: a fixed array here is 128 bytes on every one of the tens of
     * thousands of blocks a self-compile creates, all of it zeroed for nothing
     * in the default path.
     */
    char *bb_label_name;
    struct basic_block *next;  /* normal BB */
    struct basic_block *then_; /* conditional BB */
    struct basic_block *else_;
    struct basic_block *idom;
    struct basic_block *r_idom;
    struct basic_block *rpo_next;
    struct basic_block *rpo_r_next;

    /* Dominance and reverse-dominance frontiers. These were fixed worst-case
     * arrays sized MAX_BB_DOM_SUCC / MAX_BB_RDOM_SUCC, which cost 2560 bytes in
     * every basic block while a typical block uses a handful of entries. Worse,
     * the appends were unchecked and a self-compile really does push df_idx to
     * 72, overrunning a 64-entry DF into the RDF that followed it. Growing them
     * on demand removes both the waste and the fixed ceiling.
     */
    struct basic_block **DF;
    struct basic_block **RDF;

    /* Dominator-tree children, grown on demand for the same reason as prev[]
     * and the frontiers: a fixed array cost half a kilobyte in every block.
     */
    struct basic_block **dom_next;
    struct basic_block *dom_prev;
    struct basic_block *rdom_prev;
    func_t *belong_to;
    block_t *scope;

    int prev_cap;

    /* One past the highest slot bb_connect() has ever filled. Scans of prev[]
     * stop here instead of walking all MAX_BB_PRED slots; a block typically has
     * one or two predecessors, so the difference is two orders of magnitude.
     * Disconnecting clears a slot without lowering this, so it stays an upper
     * bound and the NULL checks in each loop still skip the holes.
     */
    int prev_idx;

    /* Index of this block's first instruction in PH2_IR_FLATTEN, or -1 when it
     * emitted none. Recorded while that mapping is built so the backend need
     * not search for it.
     */
    int ph2_base;
    int rpo;
    int rpo_r;

    /* How many loops enclose the block. pin_registers() weights a variable's
     * uses by it: a name inside a loop stands for as many reads as the loop has
     * iterations, and ranking by the plain count gave a register to a variable
     * named four times in straight-line code over one named once in the
     * innermost loop.
     */
    int loop_depth;

    /* What one naming of a variable in this block is worth to pin_registers(),
     * derived from loop_depth once rather than on every operand of every
     * instruction the tally walks.
     */
    int loop_weight;

    /* Stamp marking the block as already counted for the loop being walked, so
     * that one loop raises its depth once however many ways in there are.
     */
    int loop_mark;
    int df_idx;
    int rdf_idx;
    int df_cap;
    int rdf_cap;
    int visited;
    int dom_next_idx;
    int dom_next_cap;
    int elf_offset;

    /* Whether any emitted branch or jump names this block as its target. A
     * block no edge jumps to is reached only by falling out of the block
     * emitted before it, which is what lets the backend carry what the
     * registers hold across the boundary.
     */
    bool is_branch_target;
    bool useful; /* indicate whether this BB contains useful instructions */
};

struct ref_block {
    basic_block_t *bb;
    struct ref_block *next;
};

/* Syntactic representation of func, combines syntactic details (e.g., return
 * type, parameters) with SSA-related information (e.g., basic blocks, control
 * flow) to support parsing, analysis, optimization, and code generation.
 */

typedef struct {
    char label_name[MAX_ID_LEN];
    basic_block_t *bb;
    bool used;
} label_t;

struct func {
    /* Syntatic info */
    var_t return_def;
    var_t param_defs[MAX_PARAMS];
    int num_params;
    int va_args;
    bool is_static; /* internal-linkage declaration */

    /* inline_calls()'s verdict on this body and the return that ends it,
     * stamped with the round that reached them: a body is examined once per
     * round rather than once per call site that names it.
     */
    int inline_gen;
    bool inline_ok;
    struct insn *inline_ret;
    int stack_size;

    /* SSA info */
    basic_block_t *bbs;
    basic_block_t *exit;
    symbol_list_t global_sym_list;
    int bb_cnt;
    int visited;

    /* How many callee-saved registers this function's prologue must preserve,
     * counted from RBX upward. Functions that never need them pay nothing.
     */
    int saved_regs;

    /* Registers holding a variable for the whole function, one bit each. The
     * backend needs these: its notion of what is live out of a block comes from
     * the successor's entry registers, which say nothing about a variable that
     * is resident everywhere, and it would otherwise drop the code that puts a
     * value into one as dead.
     */
    int pinned_regs;

    /* Information used for dynamic linking */
    bool is_used;
    int plt_offset;

    struct func *next;
};

typedef struct {
    func_t *head, *tail;
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

/* In the ELF specification, the following data types are defined for data
 * representation:
 *
 * +-------------+------+-----------+--------------------------+
 * | Name        | Size | Alignment | Purpose                  |
 * +-------------+------+-----------+--------------------------+
 * | Elf32_Addr  | 4    | 4         | Unsigned program address |
 * +-------------+------+-----------+--------------------------+
 * | Elf32_Half  | 2    | 2         | Unsigned medium integer  |
 * +-------------+------+-----------+--------------------------+
 * | Elf32_Off   | 4    | 4         | Unsigned file offset     |
 * +-------------+------+-----------+--------------------------+
 * | Elf32_Sword | 4    | 4         | Signed large integer     |
 * +-------------+------+-----------+--------------------------+
 * | Elf32_Word  | 4    | 4         | Unsigned large integer   |
 * +-------------+------+-----------+--------------------------+
 * | unsigned    | 1    | 1         | Unsigned small integer   |
 * | char        |      |           |                          |
 * +-------------+------+-----------+--------------------------+
 *
 * However, since the current implementation doesn't support unsigned data type
 * definitions, such as 'unsigned int', 'unsigned short' and so on, the ELF
 * structures now are implemented using the 'signed' data type primarily.
 * - Elf32_Addr -> int
 * - Elf32_Half -> short
 * - Elf32_Off -> int
 * - Elf32_Word -> int
 * - unsigned char -> char
 *
 * TODO: Use correct unsigned types for these ELF structures after the
 * 'unsigned' specifier is supported.
 */

/* ELF header */
typedef struct {
    char e_ident[16];  /* unsigned char [16] */
    short e_type;      /* Elf32_Half */
    short e_machine;   /* Elf32_Half */
    int e_version;     /* Elf32_Word */
    int e_entry;       /* Elf32_Addr */
    int e_phoff;       /* Elf32_Off */
    int e_shoff;       /* Elf32_Off */
    int e_flags;       /* Elf32_Word */
    short e_ehsize;    /* Elf32_Half */
    short e_phentsize; /* Elf32_Half */
    short e_phnum;     /* Elf32_Half */
    short e_shentsize; /* Elf32_Half */
    short e_shnum;     /* Elf32_Half */
    short e_shstrndx;  /* Elf32_Half */
} elf32_hdr_t;

/* ELF program header */
typedef struct {
    int p_type;   /* Elf32_Word */
    int p_offset; /* Elf32_Off */
    int p_vaddr;  /* Elf32_Addr */
    int p_paddr;  /* Elf32_Addr */
    int p_filesz; /* Elf32_Word */
    int p_memsz;  /* Elf32_Word */
    int p_flags;  /* Elf32_Word */
    int p_align;  /* Elf32_Word */
} elf32_phdr_t;

/* ELF section header */
typedef struct {
    int sh_name;      /* Elf32_Word */
    int sh_type;      /* Elf32_Word */
    int sh_flags;     /* Elf32_Word */
    int sh_addr;      /* Elf32_Addr */
    int sh_offset;    /* Elf32_Off */
    int sh_size;      /* Elf32_Word */
    int sh_link;      /* Elf32_Word */
    int sh_info;      /* Elf32_Word*/
    int sh_addralign; /* Elf32_Word */
    int sh_entsize;   /* Elf32_Word */
} elf32_shdr_t;

/* Structures for dynamic linked program ELF buffers for dynamic sections */
typedef struct {
    strbuf_t *elf_interp;
    strbuf_t *elf_dynamic;
    strbuf_t *elf_dynsym;
    strbuf_t *elf_dynstr;
    strbuf_t *elf_relplt;
    strbuf_t *elf_relaplt;
    strbuf_t *elf_plt;
    strbuf_t *elf_got;
    int elf_interp_start;
    int elf_relplt_start;
    int elf_relaplt_start;
    int elf_plt_start;
    int elf_got_start;
    int relplt_size;
    int relaplt_size;
    int plt_size;
    int got_size;

    /* Currently, we don't consider the scenarios involving a mixture of REL and
     * RELA relocation entries.
     *
     * Therefore, use a flag to determine the type of relocation entries to be
     * processed:
     * - true: use RELA relocation entries
     * - false: use REL relocation entries
     */
    bool use_relaplt;
} dynamic_sections_t;

/* For .dynsym section. */
typedef struct {
    int st_name;    /* Elf32_Word */
    int st_value;   /* Elf32_Addr */
    int st_size;    /* Elf32_Word */
    char st_info;   /* unsigned char */
    char st_other;  /* unsigned char */
    short st_shndx; /* Elf32_Half */
} elf32_sym_t;

/* For .rel.plt section */
typedef struct {
    int r_offset; /* Elf32_Addr */
    int r_info;   /* Elf32_Word */
} elf32_rel_t;

typedef struct {
    int r_offset; /* Elf32_Addr */
    int r_info;   /* Elf32_Word */
    int r_addend; /* Elf32_Sword */
} elf32_rela_t;

#define ELF32_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))
