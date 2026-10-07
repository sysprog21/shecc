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

/* A string literal after its adjacent pieces are decoded and joined. Each piece
 * is still one token of at most MAX_TOKEN_LEN, but a long message is routinely
 * written as many of them.
 */
#define MAX_STRING_LEN 4096
#define MAX_INCLUDE_DIRS 16
#define MAX_VAR_LEN 128
/* ".label." plus an int, for basic_block_t's dump name. */
#define MAX_LABEL_LEN 24

/* Staging buffer for one instruction's Graphviz label in bb_dump(). The widest
 * case is a binary operator: three MAX_VAR_LEN names, three subscripts, an
 * operator and 41 bytes of markup.
 */
#define DUMP_INSN_LEN 512

/* A type name is a struct, union, enum or typedef identifier, which the lexer
 * already bounds by MAX_ID_LEN; a narrower buffer rejects valid tags.
 */
#define MAX_TYPE_LEN MAX_ID_LEN

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

/* var_t itself is parsed as a record by every bootstrap stage. Leave room for
 * compiler metadata as well as user records with many declarators; C99 5.2.4.1
 * asks for 127 members in one structure. Field tables are allocated lazily, so
 * this does not inflate scalar type storage.
 */
#define MAX_FIELDS 128
/* The self-hosting compiler has outgrown the original 256-entry table. */
#define MAX_TYPES 512
#define MAX_LABELS 256
/* Elements captured from an implicitly sized array initializer. */
#define MAX_IMPLICIT_ARRAY 256
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
#define MAX_NESTING 128

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
#define LARGE_ARENA_SIZE 524288   /* 512 KiB - for token storage */
#define DEFAULT_FUNCS_SIZE 64
#define DEFAULT_SRC_FILE_COUNT 8

/* Arena compaction bitmask flags for selective memory reclamation */
#define COMPACT_ARENA_BLOCK 0x01   /* BLOCK_ARENA - variables/blocks */
#define COMPACT_ARENA_BB 0x04      /* BB_ARENA - basic blocks */
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
#define MAX_WEIGHTED_LOOP_DEPTH 4

/* How many registers at the top of the allocator's file a call preserves. Only
 * such a register can hold a value across a call, so only these may be given to
 * a variable for the whole of a function that calls anything. A target states
 * its own count in mk/<arch>.mk, beside the REG_CNT that fixes the file it
 * counts from; a target that has not had its file checked this way keeps none.
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
    int peak_bytes;  /* High-water payload capacity before compaction. */
    int block_size;  /* Default block size for new blocks */
} arena_t;

typedef struct {
    arena_block_t *head;
    int offset;
} arena_mark_t;

arena_mark_t arena_mark(arena_t *arena);
int arena_bytes_since(arena_t *arena, arena_mark_t mark);
/* Discard allocations after a live mark; return false if the mark is stale. */
bool arena_rewind(arena_t *arena, arena_mark_t mark);

/* string-based hash map definitions */

typedef struct hashmap_node {
    char *key;
    void *val;
    bool occupied;
    bool owns_key;
} hashmap_node_t;

typedef struct {
    int size;
    int cap;
    hashmap_node_t *table;
    bool has_owned_keys;
} hashmap_t;

hashmap_t *hashmap_create(int cap);
void hashmap_put(hashmap_t *map, char *key, void *val);
void hashmap_put_borrowed(hashmap_t *map, char *key, void *val);
hashmap_node_t *hashmap_get_node(hashmap_t *map, char *key);
void *hashmap_get(hashmap_t *map, char *key);
bool hashmap_contains(hashmap_t *map, char *key);
void hashmap_free(hashmap_t *map);

/* lexer tokens */
typedef enum {
    T_start, /* FIXME: Unused, intended for lexer state machine init */
    T_translation_unit, /* internal boundary between source input files */
    T_eof,              /* end-of-file (EOF) */
    T_numeric,
    T_floating, /* C99 floating literal; lowering is staged separately */
    T_identifier,
    T_comma,   /* , */
    T_string,  /* null-terminated string */
    T_wstring, /* L"..." wide string literal */
    T_char,
    T_wchar,         /* L'...' wide character constant */
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
    T_volatile,
    T_static,
    T_extern,
    T_register,
    T_auto,
    T_restrict,
    T_inline,
    T_signed,
    T_unsigned,
    T_long,
    T_float,
    T_double,
    T_complex,
    T_imaginary,
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
    T_cppd_line,
    T_cppd_unknown, /* a directive name shecc does not support */

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

    /* Immutable physical path used for quoted-include lookup. #line changes
     * filename only, which remains the logical diagnostic/__FILE__ name.
     */
    char *physical_filename;
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

bool numeric_suffix_is_valid(const char *suffix);
int numeric_literal_base(const char *literal);

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
    TYPE_long,
    TYPE_long_long,
    TYPE_float,
    TYPE_double,
    TYPE_long_double,
    TYPE_struct,
    TYPE_union,
    TYPE_typedef
} base_type_t;

/* IR opcode */
typedef enum {
    /* intermediate use in front-end. No code generation */
    OP_generic,

    /* calling convention */
    OP_define,   /* function entry point */
    OP_push,     /* prepare arguments */
    OP_call,     /* function call */
    OP_indirect, /* indirect call with function pointer */
    OP_return,   /* explicit return */

    OP_va_start, /* address of the first unnamed ABI argument */
    OP_allocat,  /* allocate space on stack */
    OP_assign,
    OP_load_constant,       /* load constant */
    OP_load_data_address,   /* lookup address of a constant in data section */
    OP_load_rodata_address, /* lookup address of a constant in rodata section */

    /* control flow */
    OP_branch,   /* conditional jump */
    OP_jump,     /* unconditional jump */
    OP_func_ret, /* returned value */

    /* function pointer */
    OP_address_of_func, /* resolve function entry */
    OP_load_func,       /* prepare indirective call */

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
    OP_cast
} opcode_t;

#define OP_USUAL_ARITHMETIC_CASES \
    case OP_add:                  \
    case OP_sub:                  \
    case OP_mul:                  \
    case OP_div:                  \
    case OP_mod:                  \
    case OP_bit_and:              \
    case OP_bit_or:               \
    case OP_bit_xor

/* variable definition */

typedef struct var var_t;
typedef struct type type_t;

typedef struct var_list {
    int capacity;
    int size;
    var_t **elements;
} var_list_t;

struct var {
    type_t *type;
    /* Lexical owner, used while parsing declarator constant expressions. */
    void *scope;

    /* Interned, not copied. A MAX_VAR_LEN array was 128 of this struct's 312
     * bytes on every one of the ~86k variables a self-compile creates, and
     * var_t is embedded by value MAX_FIELDS times in each type_t and MAX_PARAMS
     * times in each func_t, so the array cost another 8 KiB per type. Generated
     * temporary names come from gen_name(); source-level names come from
     * intern_string(). Never NULL -- an unnamed variable holds "".
     */
    char *var_name;

    /* Original C spelling when an internal-linkage name is mangled for the
     * merged object/function tables. NULL for names emitted as-is.
     */
    char *source_name;
    int ptr_level;
    bool is_func;
    bool is_function_designator;

    /* A pointer-to-callback object is not callable itself. Its compact callback
     * pointee signature is restored only after one dereference.
     */
    void *pointee_func_signature;

    /* Set only for the unparenthesized `result name(parameters)` spelling.
     * Block typedef support uses it to keep direct function aliases distinct
     * from the older parenthesized pointer-to-function declarator path.
     */
    bool is_direct_function_declarator;

    /* The parenthesized callback spelling `int (*name)(int)` is distinct from a
     * direct function declarator. Block typedef lowering uses this count to
     * admit exactly one pointer layer as a callback-pointer alias.
     */
    int parenthesized_function_pointer_level;
    bool parenthesized_function_pointer_const;
    bool parenthesized_function_pointer_restrict;

    /* For a depth-two callback slot, only the second (outer) star denotes the
     * pointer object retained after normalization. Keep qualifier placement so
     * its mask can be collapsed with that object rather than silently lost.
     */
    bool parenthesized_function_pointer_outer_const;
    bool parenthesized_function_pointer_outer_volatile;
    bool parenthesized_function_pointer_outer_restrict;
    bool parenthesized_function_pointer_inner_qualified;

    /* A block-scope `extern int f(void);` hides a local object named f but
     * resolves expressions through the translation unit's function table.
     */
    bool is_extern_function_alias;

    /* A function declaration originated in a block. Its linkage metadata
     * remains global, but its ordinary identifier is lexical until a real
     * file-scope declaration appears.
     */
    bool is_block_scope_function_declaration;
    bool is_global;

    /* A compile-time address of static storage, kept distinct from reading a
     * global object's value while lowering aggregate initializers.
     */
    bool is_global_address;
    bool is_static;       /* declaration used the static storage class */
    bool is_extern;       /* file-scope declaration used extern */
    bool is_register;     /* declaration used the register storage class */
    bool is_inline;       /* declaration used the inline function specifier */
    bool has_initializer; /* a file-scope definition supplied an initializer */
    bool is_const_qualified; /* true if variable has const qualifier */
    bool is_volatile;        /* declaration used the volatile qualifier */
    bool is_volatile_access; /* computed address designates volatile storage */
    bool is_const_pointer;   /* true for the outermost `* const` qualifier */
    /* One bit per pointer level, counted from the base type. The legacy
     * is_const_pointer flag describes only the outermost level; this retains
     * qualifiers on intermediate pointers such as `int * const *`.
     */
    unsigned int pointer_const_mask;
    unsigned int pointer_volatile_mask;
    bool address_taken; /* true if variable address was taken (&var) */
    int array_size;
    bool has_direct_array_declarator;
    bool has_unsized_array; /* `T name[]`: bound is supplied by initializer */
    /* `T member[]` at the end of a struct has no initializer-supplied bound and
     * contributes no bytes to the record's fixed layout.
     */
    bool is_flexible_array_member;
    int array_dim2; /* second dimension size for multidimensional arrays */
    int array_dim3; /* third dimension size for multidimensional arrays */
    int array_dim4; /* fourth dimension size for multidimensional arrays */
    /* Bounds of the array addressed by a parenthesized pointer declarator, e.g.
     * `int (*row)[2]`. They describe the pointee, not this pointer-sized
     * object, so they must never participate in size_var().
     */
    int pointee_array_size;
    bool has_direct_pointee_array_declarator;
    int pointee_array_dim2;
    int pointee_array_dim3;
    int pointee_array_dim4;

    /* Pointer depth of one element in the array described above. This
     * distinguishes `int (*p)[2]` from `int *(*p)[2]`: both address rows, but
     * the latter's row elements are pointers.
     */
    int pointee_array_element_ptr_level;
    int offset; /* offset from stack or frame, index 0 is reserved */
    int vir_source_index;

    /* Record bit-field metadata. `offset` is the containing storage unit's byte
     * offset. `is_bitfield` distinguishes an ordinary member from the valid
     * unnamed zero-width field used as an allocation-unit barrier.
     */
    bool is_bitfield;
    int bit_width;
    int bit_offset;
    int bit_storage_size;
    int init_val;    /* for global initialization */
    int init_val_hi; /* upper word of an 8-byte integer constant */
    bool is_ternary_ret;
    bool is_logical_ret;
    bool is_const; /* whether a constant representaion or not */

    /* The value of an assignment expression, read back from the object the
     * assignment stored. C11 6.5.16p3 permits that read without requiring it,
     * even of a volatile object, so it is not an access that must survive when
     * nothing uses the value.
     */
    bool is_assignment_reload;
    bool space_is_allocated; /* whether space is allocated for this variable */
    bool has_backing_storage;

    /* True when this variable was synthesized to hold a compound literal (e.g.,
     * array or struct literal temporaries).
     */
    bool is_compound_literal;

    /* A value loaded from an object that has no declaration of its own: an
     * element or member of a compound literal, or an object reached through a
     * dereference, subscript or member selection. This points at the object's
     * address so a following update, store or member selection reaches the
     * object rather than the temporary.
     */
    bool is_compound_literal_reference;
    struct var *compound_literal_address;

    /* A record value with no storage of its own: its bytes are those of the
     * object at compound_literal_address. A member selection or address-of uses
     * that object, and a record copy reads the bytes from it.
     */
    bool defers_record_copy;

    /* A non-NULL field means the reference is a bit-field and must use the
     * mask-and-merge store path rather than a byte/word OP_write.
     */
    struct var *compound_literal_bitfield;

    /* String literals have immutable storage duration in C. Preserve that
     * provenance separately from the pointer type so the compatibility warning
     * can remain opt-in while legacy source still compiles.
     */
    bool is_string_literal;

    /* For the address of a static object in a constant initializer, the bytes
     * one integer step moves it by, so `1 + array` advances as `array + 1`
     * does; 0 where no such step is known.
     */
    int address_stride;

    /* The null pointer constant `(void *) 0` (C99 6.3.2.3p3), which unlike any
     * other void pointer converts to a function pointer.
     */
    bool is_void_null_pointer;

    /* For a callback slot, whether the callback pointer it finally reaches is
     * itself const or volatile: `int (*const *slot)(int)`.
     */
    bool callback_is_const;
    bool callback_is_volatile;

    /* `&__func__` is a pointer to the compiler's static character array. Its
     * address has the same machine representation as the decayed char pointer,
     * but one unary dereference must restore that pointer without loading the
     * first bytes of the string as an address.
     */
    bool is_func_name_array_address;

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

typedef struct typedef_binding {
    char *name;
    type_t *type;
    struct typedef_binding *next;
} typedef_binding_t;

/* block definition */
struct block {
    var_list_t locals;

    /* Tags and enumerators follow C lexical scope. File-scope tag/enum bindings
     * live on the active translation-unit root; typedef bindings remain on the
     * migration path through TYPES. Variables remain in locals.
     */
    void *type_tags;
    void *constants;
    typedef_binding_t *typedefs;
    struct block *parent;
    func_t *func;
    struct block *next;
};

typedef struct block block_t;

int read_const_sizeof_type(block_t *scope);
int read_const_wstring_size(void);
int read_sizeof_constant(block_t *scope);
int read_const_expr_operand(block_t *scope);
int alignment_type(type_t *type);
void add_block_typedef(block_t *block, char name[], type_t *type);
bool find_block_typedef(block_t *block, const char *name);
type_t *find_visible_type(const char *name, block_t *block);
type_t *find_record_tag(char name[], block_t *block, base_type_t kind);
type_t *reference_record_tag(char name[], block_t *block, base_type_t kind);
type_t *local_record_tag(char name[], block_t *block, base_type_t kind);
void begin_record_definition(type_t *tag);
type_t *find_enum_tag(char name[], block_t *block);
type_t *reference_enum_tag(char name[], block_t *block);
type_t *local_enum_tag(char name[], block_t *block);
typedef struct basic_block basic_block_t;

/* Definition of a growable buffer for a mutable null-terminated string
 * @size: Current number of elements in the array
 * @capacity: Number of elements that can be stored without resizing
 * @elements: Pointer to the array of characters
 * @plain_source: set by the lexer once it has checked that a source buffer has
 *                no trigraph and no line splice, so phases 1 and 2 are the
 *                identity on it
 */
typedef struct {
    int size;
    int capacity;
    char *elements;
    bool plain_source;
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
    /* Backend frame metadata. */
    int src2;
    int dest;

    /* A 32-bit target represents a wide integer as low/high register pairs. -1
     * means this instruction uses the existing single-register form.
     */
    int src0_hi;
    int src1_hi;
    int dest_hi;
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

    /* The load, read or store accesses a volatile object, which is a side
     * effect whether or not its value is used (C99 6.7.3p6), so no rewrite may
     * drop it, replace a load with a copy of a value read earlier, or drop a
     * store for writing what the object already holds.
     */
    bool is_volatile;
};

typedef struct ph2_ir ph2_ir_t;

/* type definition */
struct type {
    char type_name[MAX_TYPE_LEN];
    base_type_t base_type;
    struct type *base_struct;
    int size;

    /* Natural ABI alignment of an object of this type. Record definitions
     * retain their maximum member alignment so nested records lay out correctly
     * too.
     */
    int alignment;

    /* Member table, allocated when the type is created rather than inlined. A
     * MAX_FIELDS array of var_t by value made type_t 12 KiB, and TYPES is a
     * flat MAX_TYPES array that global_init() zeroes up front -- 3 MiB of
     * resident memory for the 90 types a self-compile actually declares.
     */
    var_t *fields;
    int num_fields;
    int ptr_level; /* pointer level for typedef pointer types */
    /* A function-pointer typedef retains its parsed prototype here. Keep it
     * opaque because type_t is declared before func_t is complete.
     */
    void *func_signature;

    /* A pointer-to-callback typedef is itself non-callable, but its pointee
     * callback prototype must survive object declarations and conversions.
     */
    void *pointee_func_signature;

    /* Unlike a pointer-to-function typedef, this descriptor denotes the
     * function type itself. A single use-site star then forms a callable
     * function-pointer object.
     */
    bool is_direct_function_type;

    /* Qualifiers on a callback-pointer typedef apply to each pointer object,
     * not to its function return type.
     */
    bool is_volatile_qualified;

    /* Array bounds carried by an array typedef. Object declarators copy these
     * into var_t, where ordinary indexing and initialization already retain
     * their row-major representation.
     */
    int array_size;
    int array_dim2;
    int array_dim3;
    int array_dim4;

    /* Pointer depth of one element of an array typedef. This is distinct from
     * ptr_level when a later typedef adds a pointer to the whole array.
     */
    int array_element_ptr_level;

    /* Scalar base descriptor of an array typedef's element. Pointer-element
     * arrays need this after a subscript: the outer typedef descriptor still
     * carries the array's pointer depth and is not the loaded element type.
     */
    struct type *array_element_type;

    /* An array of callback slots is not itself a slot. Preserve the
     * non-callable callback prototype on each element for subscript loads.
     */
    void *array_element_pointee_func_signature;

    /* Qualifiers on `(**const slots[N])` and `(**volatile slots[N])` apply to
     * each selected outer slot pointer, not to the array object or the callback
     * pointer reached after one dereference.
     */
    bool array_element_is_const_pointer;
    bool array_element_is_volatile;

    /* Bounds carried by a pointer-to-array typedef, e.g. `int (*)[2]`. These
     * describe the pointed-to array rather than the pointer-sized alias itself
     * and are copied to var_t when the typedef names an object.
     */
    int pointee_array_size;
    int pointee_array_dim2;
    int pointee_array_dim3;
    int pointee_array_dim4;
    int pointee_array_element_ptr_level;

    /* The scalar element descriptor of a pointer-to-array typedef. Unlike an
     * ordinary pointer typedef, its outer descriptor is TYPE_typedef and
     * pointer-sized, so row indexing cannot recover this from `size`.
     */
    struct type *pointee_array_element_type;

    /* Qualifiers written after stars inside a typedef declarator. These bits
     * are relative to the typedef's own pointer depth; var_t keeps any stars
     * subsequently written at a use site.
     */
    unsigned int pointer_const_mask;
    unsigned int pointer_volatile_mask;
    bool is_union; /* preserves union semantics for anonymous typedef unions */
    bool
        has_flexible_array_member; /* cannot be embedded by value in a record */
    bool is_const_qualified;       /* qualifier carried by a scalar typedef */
    /* Set on a struct or union tag once its member list opens. num_fields is
     * written only when the list closes, so it cannot tell a definition nested
     * in the tag's own member list from the first one.
     */
    bool definition_started;

    /* Integer representation is distinct from signedness: unsigned char and
     * unsigned int keep the ordinary scalar widths but require zero extension
     * and unsigned arithmetic lowering.
     */
    bool is_unsigned;

    /* Floating scalars require a distinct IR/register class. This identity is
     * intentionally separate from width and signedness so they can never be
     * lowered as integer values by accident.
     */
    bool is_floating;

    /* Plain char and signed char share this target's representation but are
     * distinct C types. Scalar typedefs preserve this fact.
     */
    bool is_signed_char;

    /* `_Bool` otherwise shares the byte-sized TYPE_char representation. Keep
     * its C type identity through typedefs where pointer equality is lost.
     */
    bool is_bool;
};

/* lvalue details */
typedef struct {
    int size;
    int ptr_level;

    /* Pointer depth of the value designated by this lvalue. A subscript
     * computes an address (one level deeper than its selected value), so this
     * must not be inferred from the address provenance alone.
     */
    int value_ptr_level;
    bool is_func;
    bool is_reference;
    bool is_const_qualified;

    /* The lvalue designates an array, which C99 6.5.16 does not let an
     * assignment, ++ or -- modify.
     */
    bool is_array;

    /* Subscripts applied to the declaration in lvalue_t.decl. A designated
     * array keeps the bounds after the first this many.
     */
    int subscript_depth;
    unsigned int pointer_const_mask;
    type_t *type;

    /* A selected array element can be a non-callable callback slot even when
     * the array's scalar base type has no ordinary pointer descriptor.
     */
    void *pointee_func_signature;
    /* The declaration selected by the lvalue, including a struct member. */
    var_t *decl;
} lvalue_t;

/* constants for enums */
typedef struct constant {
    char alias[MAX_VAR_LEN];
    int value;
    struct constant *next;
} constant_t;

typedef struct type_tag {
    char name[MAX_TYPE_LEN];
    type_t *type;
    struct type_tag *next;
} type_tag_t;

typedef struct {
    ph2_ir_t *head, *tail;
} ph2_ir_list_t;

typedef enum { NEXT, ELSE, THEN } bb_connection_type_t;

typedef struct {
    basic_block_t *bb;
    bb_connection_type_t type;
} bb_connection_t;

struct basic_block {
    struct vir_block *vir;
    ph2_ir_list_t ph2_ir_list;
    unsigned int machine_use, machine_def, machine_liveout;

    /* Predecessor edges, grown on demand. A fixed MAX_BB_PRED array cost two
     * kilobytes in every basic block -- by far the largest thing in one --
     * while almost every block has one or two predecessors.
     */
    bb_connection_t *prev;

    struct basic_block *next;  /* normal BB */
    struct basic_block *then_; /* conditional BB */
    struct basic_block *else_;
    struct basic_block *rpo_next;
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
    int elf_offset;

    /* VIR block-map index plus one; zero means this parser block is unmapped.
     */
    int vir_map_id;

    /* Whether any emitted branch or jump names this block as its target. A
     * block no edge jumps to is reached only by falling out of the block
     * emitted before it, which is what lets the backend carry what the
     * registers hold across the boundary.
     */
    bool is_branch_target;
};

typedef struct {
    char label_name[MAX_ID_LEN];
    basic_block_t *bb;
    bool used;
} label_t;

struct func {
    /* Syntatic info */
    var_t return_def;

    /* By-value record returns use a private caller-provided destination pointer
     * as ABI argument zero. It is deliberately outside param_defs so C
     * prototype arity and compatibility remain source-level facts.
     */
    bool returns_aggregate;
    var_t sret_def;
    var_t param_defs[MAX_PARAMS];
    int num_params;
    int va_args;

    /* `f()` has no prototype in C99, while `f(void)` and every typed parameter
     * list constrain call arity.
     */
    bool has_prototype;

    /* A declaration introduced only within a block still has linkage, but its
     * ordinary identifier is visible only through that block's lexical alias
     * until a file-scope declaration or definition appears.
     */
    bool is_block_scope_only_declaration;
    bool is_static; /* internal-linkage declaration */
    /* The definition used the inline function specifier. C99 applies extra
     * linkage constraints to external-linkage inline definitions.
     */
    bool is_inline;

    int stack_size;
    void *vir_context;

    /* Parser cursors become the allocated machine CFG after lowering. */
    basic_block_t *bbs;
    basic_block_t *exit;
    int visited;

    /* Callee-saved registers the backend must preserve. */
    int saved_regs;

    /* Information used for dynamic linking */
    bool is_used;
    /* A direct aggregate-return call requires shecc's private sret ABI. */
    bool aggregate_call_used;
    int plt_offset;

    struct func *next;
};

typedef struct {
    func_t *head, *tail;
} func_list_t;

/* Iterate over functions with bodies, skipping declarations. */
#define FOR_EACH_FUNCTION_BODY(func)                             \
    for (func_t *func = FUNC_LIST.head; func; func = func->next) \
        if ((func)->bbs)

/* Load a two-word value without clobbering its address register. */
#define EMIT_PAIR_LOAD(load, rd, rd_hi, base) \
    do {                                      \
        if ((rd) == (base)) {                 \
            load(rd_hi, base, 4);             \
            load(rd, base, 0);                \
        } else {                              \
            load(rd, base, 0);                \
            load(rd_hi, base, 4);             \
        }                                     \
    } while (0)

#define PH2_PAIR_DEST_SRC0(ir) ((ir)->dest_hi >= 0 && (ir)->src0_hi >= 0)
#define PH2_PAIR_BINARY(ir) (PH2_PAIR_DEST_SRC0(ir) && (ir)->src1_hi >= 0)

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

int ph2_ir_flatten_function(func_t *func,
                            int stack_top_ofs,
                            int prologue_bytes,
                            int saved_regs,
                            bool restore_saved_regs,
                            void (*update_offset)(ph2_ir_t *));
ph2_ir_t *ph2_ir_flatten_insn(ph2_ir_t *insn,
                              int stack_top_ofs,
                              int stack_size,
                              int saved_regs,
                              bool restore_saved_regs);
int func_highest_used_reg(func_t *func, int first_callee_saved);
bool op_is_binary_alu(opcode_t op);
bool op_writes_dest(opcode_t op);
unsigned int ph2_ir_defs(ph2_ir_t *ir);
void ph2_compute_liveness(void);
bool op_is_integer_binary(opcode_t op);
bool op_is_comparison(opcode_t op);
bool op_is_scalar_unary(opcode_t op);
bool op_is_scalar_cast(opcode_t op);
const char *opcode_mnemonic(opcode_t op);
basic_block_t *bb_sole_pred(const basic_block_t *bb);

#define ELF32_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))
