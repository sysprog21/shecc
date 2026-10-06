/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "defs.h"

source_location_t *cur_token_loc(void);
__noreturn void error_at(char *msg, source_location_t *loc);
__noreturn void limit_error(char *msg);

/* Forward declaration for string interning */
char *intern_string(char *str);
func_t *find_func(char *func_name);
__noreturn void fatal(const char *msg);

/* Lexer */
token_t *cur_token;
/* TOKEN_CACHE maps filename to the corresponding computed token stream */
hashmap_t *TOKEN_CACHE;
strbuf_t *LIBC_SRC;

/* Global objects */

hashmap_t *SRC_FILE_MAP;
hashmap_t *FUNC_MAP;

/* Types */

type_t *TYPES;
int types_idx = 0;
int builtin_types_idx;
static unsigned int internal_linkage_name_idx;

static const char *file_scope_var_name(const var_t *var)
{
    return var->source_name ? var->source_name : var->var_name;
}

static void assign_internal_linkage_name(var_t *var, const char *source_name)
{
    char name[48];

    var->source_name = intern_string((char *) source_name);
    snprintf(name, sizeof(name), "__shecc_internal_%u",
             internal_linkage_name_idx++);
    var->var_name = intern_string(name);
}

static const char *function_source_name(const func_t *func)
{
    return file_scope_var_name(&func->return_def);
}

typedef enum {
    ORDINARY_NONE = 0,
    ORDINARY_CONSTANT = 1,
    ORDINARY_VARIABLE = 2,
    ORDINARY_TYPEDEF = 4,
    ORDINARY_PARAMETER = 8,
    ORDINARY_ANY = 15
} ordinary_kind_t;

typedef struct tu_ordinary_binding {
    ordinary_kind_t kind;
    void *value;
} tu_ordinary_binding_t;

static void register_tu_ordinary_name(const char *name,
                                      ordinary_kind_t kind,
                                      void *value);
static ordinary_kind_t lookup_tu_ordinary_name(const char *name,
                                               int kinds,
                                               void **value);

type_t *TY_void;
type_t *TY_char;
type_t *TY_schar;
type_t *TY_uchar;
type_t *TY_bool;
type_t *TY_int;
type_t *TY_uint;
type_t *TY_long;
type_t *TY_ulong;
type_t *TY_short;
type_t *TY_ushort;
type_t *TY_long_long;
type_t *TY_ulong_long;
type_t *TY_float;
type_t *TY_double;
type_t *TY_long_double;

/* Arenas */


/* BLOCK_ARENA is responsible for block_t / var_t allocation */
arena_t *BLOCK_ARENA;

/* BB_ARENA is responsible for basic_block_t / ph2_ir_t allocation */
arena_t *BB_ARENA;

/* TOKEN_ARENA is responsible for token_t (including literal) /
 * source_location_t allocation
 */
arena_t *TOKEN_ARENA;

/* GENERAL_ARENA is responsible for functions, symbols, constants, aliases, and
 * macros
 */
arena_t *GENERAL_ARENA;

ph2_ir_t **PH2_IR_FLATTEN;
int ph2_ir_idx = 0;
int ph2_ir_capacity;

func_list_t FUNC_LIST;
func_t *GLOBAL_FUNC;
block_t *GLOBAL_BLOCK;
block_t *CURRENT_TU_SCOPE;
basic_block_t *MAIN_BB;
int elf_offset = 0;
int translation_unit_count;


hashmap_t *INCLUSION_MAP;

/* ELF sections */
strbuf_t *elf_code;
strbuf_t *elf_data;
strbuf_t *elf_rodata;
strbuf_t *elf_header;
strbuf_t *elf_program_header;
strbuf_t *elf_symtab;
strbuf_t *elf_strtab;
strbuf_t *elf_section_header;
strbuf_t *elf_shstrtab;
int elf_header_len;
int elf_code_start;
int elf_data_start;
int elf_rodata_start;
int elf_bss_start;
int elf_bss_size;
/* Offset of the entry point within the code section (0 = start of code). */
int elf_entry_offset = 0;
dynamic_sections_t dynamic_sections;

/* C99 integer suffix order: [u][l|ll][u]. */
bool numeric_suffix_is_valid(const char *suffix)
{
    int pos = 0;

    if ((suffix[pos] | 32) == 'u')
        pos++;
    if ((suffix[pos] | 32) == 'l') {
        pos++;
        if ((suffix[pos] | 32) == 'l')
            pos++;
    }
    if ((suffix[pos] | 32) == 'u')
        pos++;
    return suffix[pos] == '\0';
}

/* The radix implied by an integer literal's leading characters. */
int numeric_literal_base(const char *literal)
{
    if (literal[0] != '0')
        return 10;
    if ((literal[1] | 32) == 'x')
        return 16;
    if ((literal[1] | 32) == 'b')
        return 2;
    return 8;
}

opcode_t operator_for_token(token_kind_t token)
{
    switch (token) {
    case T_plus:
        return OP_add;
    case T_minus:
        return OP_sub;
    case T_bit_not:
        return OP_bit_not;
    case T_log_not:
        return OP_log_not;
    case T_asterisk:
        return OP_mul;
    case T_divide:
        return OP_div;
    case T_mod:
        return OP_mod;
    case T_lshift:
        return OP_lshift;
    case T_rshift:
        return OP_rshift;
    case T_log_and:
        return OP_log_and;
    case T_log_or:
        return OP_log_or;
    case T_eq:
        return OP_eq;
    case T_noteq:
        return OP_neq;
    case T_lt:
        return OP_lt;
    case T_le:
        return OP_leq;
    case T_gt:
        return OP_gt;
    case T_ge:
        return OP_geq;
    case T_ampersand:
        return OP_bit_and;
    case T_bit_or:
        return OP_bit_or;
    case T_bit_xor:
        return OP_bit_xor;
    case T_question:
        return OP_ternary;
    default:
        return OP_generic;
    }
}

/* Shared by the parser and preprocessor's expression evaluator. */
int get_operator_prio(opcode_t op)
{
    switch (op) {
    case OP_ternary:
        return 3;
    case OP_log_or:
        return 4;
    case OP_log_and:
        return 5;
    case OP_bit_or:
        return 6;
    case OP_bit_xor:
        return 7;
    case OP_bit_and:
        return 8;
    case OP_eq:
    case OP_neq:
        return 9;
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
        return 10;
    case OP_lshift:
    case OP_rshift:
        return 11;
    case OP_add:
    case OP_sub:
        return 12;
    case OP_mul:
    case OP_div:
    case OP_mod:
        return 13;
    default:
        return 0;
    }
}

/* Command line compilation flags */
bool dynlink = false;
bool libc = true;
bool expand_only = false;
bool dump_ir = false;
bool dump_vir = false;
bool dump_dot = false;
bool hard_mul_div = false;
bool warn_string_literals = false;
bool strict_c99 = false;
bool dump_stats = false;
char *include_dirs[MAX_INCLUDE_DIRS];
int include_dirs_idx = 0;

/* Allocation counters are a stable pre-VIR baseline even when later passes
 * delete or rewrite the objects they counted.
 */
int stats_func_count = 0;
int stats_var_count = 0;
int stats_field_slot_count = 0;
int stats_bb_count = 0;
int stats_ph2_ir_count = 0;
static int stats_tu_index_lookups = 0;
static int stats_tu_index_registrations = 0;
static hashmap_t *TU_ORDINARY_MAP;

/* Payload capacity currently retained by all live arenas, and its true
 * allocation-time high water. This deliberately excludes arena headers, malloc
 * metadata, and non-arena allocations.
 */
int stats_live_arena_capacity = 0;
int stats_peak_arena_capacity = 0;
int stats_retired_token_arena_peak = 0;

/* Create a new arena block with given capacity.
 * @capacity: The capacity of the arena block. Must be positive.
 *
 * Return: The pointer of created arena block. NULL if failed to allocate.
 */
arena_block_t *arena_block_create(int capacity)
{
    arena_block_t *block = malloc(sizeof(arena_block_t));

    if (!block) {
        printf("Failed to allocate memory for arena block structure\n");
        fflush(stdout); /* see fatal() */
        abort();
    }

    block->memory = malloc(capacity * sizeof(char));

    if (!block->memory) {
        printf("Failed to allocate memory for arena block buffer\n");
        free(block);
        fflush(stdout); /* see fatal() */
        abort();
    }

    block->capacity = capacity;
    block->offset = 0;
    block->next = NULL;
    return block;
}

/* Free a single arena block and its memory buffer.
 * @block: Pointer to the arena_block_t to free. Must not be NULL.
 */
void arena_block_free(arena_block_t *block)
{
    free(block->memory);
    free(block);
}

/* Initialize the given arena with initial capacity.
 * @initial_capacity: The initial capacity of the arena. Must be positive.
 *
 * Return: The pointer of initialized arena.
 */
arena_t *arena_init(int initial_capacity)
{
    arena_t *arena = malloc(sizeof(arena_t));
    if (!arena) {
        printf("Failed to allocate memory for arena structure\n");
        fflush(stdout); /* see fatal() */
        abort();
    }
    arena->head = arena_block_create(initial_capacity);
    arena->total_bytes = initial_capacity;
    arena->peak_bytes = initial_capacity;
    stats_live_arena_capacity += initial_capacity;
    if (stats_live_arena_capacity > stats_peak_arena_capacity)
        stats_peak_arena_capacity = stats_live_arena_capacity;
    /* Use the initial capacity as the default block size for future growth. */
    arena->block_size = initial_capacity;
    return arena;
}

/* Allocate memory from the given arena with given size. The arena may create a
 * new arena block if no space is available.
 * @arena: The arena to allocate memory from. Must not be NULL.
 * @size: The size of memory to allocate. Must be positive.
 *
 * Return: The pointer of allocated memory. NULL if new arena block is failed to
 * allocate.
 */
void *arena_alloc(arena_t *arena, int size)
{
    if (size <= 0) {
        printf("arena_alloc: size must be positive\n");
        fflush(stdout); /* see fatal() */
        abort();
    }

    /* Align to sizeof(void*) bytes for host compatibility */
    const int alignment = sizeof(void *);
    size = (size + alignment - 1) & ~(alignment - 1);

    if (!arena->head || arena->head->offset + size > arena->head->capacity) {
        /* Need a new block: choose capacity = max(DEFAULT_ARENA_SIZE,
         * arena->block_size, size)
         */
        const int base =
            (arena->block_size > DEFAULT_ARENA_SIZE ? arena->block_size
                                                    : DEFAULT_ARENA_SIZE);
        const int new_capacity = (size > base ? size : base);
        arena_block_t *new_block = arena_block_create(new_capacity);
        new_block->next = arena->head;
        arena->head = new_block;
        arena->total_bytes += new_capacity;
        stats_live_arena_capacity += new_capacity;
        if (stats_live_arena_capacity > stats_peak_arena_capacity)
            stats_peak_arena_capacity = stats_live_arena_capacity;
        if (arena->total_bytes > arena->peak_bytes)
            arena->peak_bytes = arena->total_bytes;
    }

    void *ptr = arena->head->memory + arena->head->offset;
    arena->head->offset += size;
    return ptr;
}

/* Mark consumed arena payload without charging future allocations. The mark is
 * valid while its block remains in the arena's newest-to-oldest chain.
 */
arena_mark_t arena_mark(arena_t *arena)
{
    arena_mark_t mark;

    mark.head = arena ? arena->head : NULL;
    mark.offset = mark.head ? mark.head->offset : 0;
    return mark;
}

/* Return payload allocated since a mark, including new blocks and growth of its
 * original head block.
 *
 * Return -1 if the marked block is no longer present.
 */
int arena_bytes_since(arena_t *arena, arena_mark_t mark)
{
    int bytes = 0;
    arena_block_t *block = arena ? arena->head : NULL;

    while (block && block != mark.head) {
        bytes += block->offset;
        block = block->next;
    }
    if (block != mark.head)
        return -1;
    return bytes + (mark.head ? mark.head->offset - mark.offset : 0);
}

bool arena_rewind(arena_t *arena, arena_mark_t mark)
{
    if (!arena || !mark.head)
        return false;

    arena_block_t *block = arena->head;
    while (block && block != mark.head)
        block = block->next;
    if (!block || mark.offset > mark.head->offset)
        return false;

    while (arena->head != mark.head) {
        block = arena->head;
        arena->head = block->next;
        arena->total_bytes -= block->capacity;
        stats_live_arena_capacity -= block->capacity;
        arena_block_free(block);
    }
    arena->head->offset = mark.offset;
    return true;
}

/* arena_alloc() plus explicit zero‑initialization.
 * @arena: The arena to allocate memory from. Must not be NULL.
 * @n: Number of elements.
 * @size: Size of each element in bytes.
 *
 * Internally calls arena_alloc(n * size) and then fills the entire region with
 * zero bytes.
 *
 * Return: Pointer to zero-initialized memory.
 */
void *arena_calloc(arena_t *arena, int n, int size)
{
    /* Reject a count and size whose product does not fit, rather than
     * allocating the wrapped-around amount and letting the caller write past
     * it.
     */
    if (n <= 0 || size <= 0 || n > 0x7fffffff / size) {
        printf("arena_calloc: invalid allocation size\n");
        fflush(stdout); /* see fatal() */
        abort();
    }

    int total = n * size;
    void *ptr = arena_alloc(arena, total);

    /* Use memset for better performance */
    memset(ptr, 0, total);

    return ptr;
}

/* Reallocate a previously allocated region within the arena to a different
 * size.
 *
 * Behaviors:
 * 1. If oldptr == NULL and oldsz == 0, act like malloc.
 * 2. If newsz <= oldsz, return oldptr immediately.
 * 3. Grow in place if oldptr is the last allocation in the current block.
 * 4. Otherwise, allocate a new region and copy old data.
 *
 * @arena: Pointer to the arena. Must not be NULL.
 * @oldptr: Pointer to the previously allocated memory in the arena.
 * @oldsz: Original size (in bytes) of that allocation.
 * @newsz: New desired size (in bytes).
 *
 * Return: Pointer to the reallocated (resized) memory region.
 */
void *arena_realloc(arena_t *arena, char *oldptr, int oldsz, int newsz)
{
    /* act like malloc */
    if (!oldptr) {
        if (oldsz != 0) {
            printf("arena_realloc: oldptr == NULL requires oldsz == 0\n");
            fflush(stdout); /* see fatal() */
            abort();
        }
        return arena_alloc(arena, newsz);
    }
    if (oldsz == 0) {
        printf("arena_realloc: oldptr != NULL requires oldsz > 0\n");
        fflush(stdout); /* see fatal() */
        abort();
    }

    /* return oldptr immediately */
    if (newsz <= oldsz) {
        return oldptr;
    }

    /* From here on, oldptr != NULL and newsz > oldsz and oldsz != 0 */
    int delta = newsz - oldsz;
    arena_block_t *blk = arena->head;
    const char *block_end = blk->memory + blk->offset;

    /* grow in place if oldptr is the last allocation in the current block */
    if (oldptr + oldsz == block_end && blk->offset + delta <= blk->capacity) {
        blk->offset += delta;
        return oldptr;
    }

    /* allocate a new region and copy old data */
    void *newptr = arena_alloc(arena, newsz);
    memcpy(newptr, oldptr, oldsz);
    return newptr;
}

/* Duplicate a NULL-terminated string into the arena.
 *
 * @arena: a Pointer to the arena. Must not be NULL.
 * @str: NULL-terminated input string to duplicate. Must not be NULL.
 *
 * Return: Pointer to the duplicated string stored in the arena.
 */
char *arena_strdup(arena_t *arena, const char *str)
{
    const int n = strlen(str);
    char *dup = arena_alloc(arena, n + 1);
    memcpy(dup, str, n);
    dup[n] = '\0';
    return dup;
}

/* Typed allocators for consistent memory management */
func_t *arena_alloc_func(void)
{
    func_t *func = arena_calloc(GENERAL_ARENA, 1, sizeof(func_t));

    stats_func_count++;
    return func;
}

basic_block_t *arena_alloc_bb(void)
{
    stats_bb_count++;
    return arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));
}

constant_t *arena_alloc_constant(void)
{
    /* constant_t is simple, can avoid zeroing */
    constant_t *c = arena_alloc(GENERAL_ARENA, sizeof(constant_t));
    c->alias[0] = '\0';
    c->value = 0;
    return c;
}

void arena_free(arena_t *arena)
{
    if (!arena)
        return;

    arena_block_t *block = arena->head;
    arena_block_t *next;

    while (block) {
        next = block->next;
        arena_block_free(block);
        block = next;
    }

    stats_live_arena_capacity -= arena->total_bytes;
    free(arena);
}

/* Hash a string with FNV-1a hash function and converts into usable hashmap
 * index. The range of returned hashmap index is ranged from "(0 ~
 * 2,147,483,647) mod size" due to lack of unsigned integer implementation.
 * @size: The size of map. Must not be negative or 0.
 * @key: The key string. May be NULL.
 *
 * Return: The usable hashmap index.
 */
int hashmap_hash_index(int size, char *key)
{
    if (!key)
        return 0;

    int hash = 0x811c9dc5;

    for (; *key; key++) {
        hash ^= *key;
        hash *= 0x01000193;
    }

    const int mask = hash >> 31;
    return ((hash ^ mask) - mask) & (size - 1);
}

int round_up_pow2(int v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

/* Create a hashmap on heap. Notice that provided size will always be rounded up
 * to nearest power of 2.
 * @size: The initial bucket size of hashmap. Must not be 0 or negative.
 *
 * Return: The pointer of created hashmap.
 */
hashmap_t *hashmap_create(int cap)
{
    hashmap_t *map = malloc(sizeof(hashmap_t));

    if (!map) {
        printf("Failed to allocate hashmap_t with capacity %d\n", cap);
        return NULL;
    }

    map->size = 0;
    map->cap = round_up_pow2(cap);
    map->has_owned_keys = false;
    map->table = calloc(map->cap, sizeof(hashmap_node_t));

    if (!map->table) {
        printf("Failed to allocate table in hashmap_t\n");
        free(map);
        return NULL;
    }

    return map;
}


static void hashmap_rehash(hashmap_t *map)
{
    if (!map)
        return;

    int old_cap = map->cap;
    hashmap_node_t *old_table = map->table;

    map->cap <<= 1;
    map->table = calloc(map->cap, sizeof(hashmap_node_t));

    if (!map->table) {
        printf("Failed to allocate new table in hashmap_t\n");
        map->table = old_table;
        map->cap = old_cap;
        return;
    }

    map->size = 0;

    for (int i = 0; i < old_cap; i++) {
        if (old_table[i].occupied) {
            char *key = old_table[i].key;
            void *val = old_table[i].val;

            int index = hashmap_hash_index(map->cap, key);
            int start = index;

            while (map->table[index].occupied) {
                index = (index + 1) & (map->cap - 1);
                if (index == start) {
                    printf("Error: New table is full during rehash\n");
                    fflush(stdout); /* see fatal() */
                    abort();
                }
            }

            map->table[index].key = key;
            map->table[index].val = val;
            map->table[index].occupied = true;
            map->table[index].owns_key = old_table[i].owns_key;
            map->size++;
        }
    }
    free(old_table);
}

/* Find an existing key or the empty slot for a new one, growing the table
 * before its load factor exceeds 50%.
 */
static hashmap_node_t *hashmap_prepare_slot(hashmap_t *map, char *key)
{
    int index;
    int start;

    if (!map)
        return NULL;

    if ((map->cap >> 1) <= map->size)
        hashmap_rehash(map);

    index = hashmap_hash_index(map->cap, key);
    start = index;
    while (map->table[index].occupied) {
        if (!strcmp(map->table[index].key, key))
            return &map->table[index];

        index = (index + 1) & (map->cap - 1);
        if (index == start) {
            printf("Error: Hashmap is full\n");
            fflush(stdout); /* see fatal() */
            abort();
        }
    }

    return &map->table[index];
}

/* Put an owned copy of @key into the map. Replacing an existing value keeps its
 * original key and ownership.
 * @map: The hashmap to be put into. May be NULL.
 * @key: Non-NULL key string.
 * @val: The value pointer. May be NULL. The map does not own the value.
 */
static void hashmap_put_impl(hashmap_t *map,
                             char *key,
                             void *val,
                             bool copy_key)
{
    hashmap_node_t *node = hashmap_prepare_slot(map, key);

    if (!node)
        return;

    if (!node->occupied) {
        if (copy_key) {
            size_t key_size = strlen(key) + 1;
            char *stored_key = malloc(key_size);

            if (!stored_key) {
                printf("Failed to allocate hashmap key\n");
                fflush(stdout); /* see fatal() */
                abort();
            }
            memcpy(stored_key, key, key_size);
            key = stored_key;
        }
        node->key = key;
        node->occupied = true;
        node->owns_key = copy_key;
        map->has_owned_keys |= copy_key;
        map->size++;
    }

    node->val = val;
}

void hashmap_put(hashmap_t *map, char *key, void *val)
{
    hashmap_put_impl(map, key, val, true);
}

/* The caller must provide a non-NULL key and keep it alive for the map's
 * lifetime.
 */
void hashmap_put_borrowed(hashmap_t *map, char *key, void *val)
{
    hashmap_put_impl(map, key, val, false);
}

/* Get key-value pair node from hashmap from given key.
 * @map: The hashmap to be looked up. May be NULL.
 * @key: Non-NULL key string.
 *
 * Return: The look up result, if the key-value pair entry exists, then returns
 * address of itself, NULL otherwise.
 */
hashmap_node_t *hashmap_get_node(hashmap_t *map, char *key)
{
    if (!map)
        return NULL;

    int index = hashmap_hash_index(map->cap, key);
    int start = index;

    while (map->table[index].occupied) {
        if (!strcmp(map->table[index].key, key))
            return &map->table[index];

        index = (index + 1) & (map->cap - 1);
        if (index == start)
            return NULL;
    }

    return NULL;
}

/* Get value from hashmap from given key.
 * @map: The hashmap to be looked up. May be NULL.
 * @key: Non-NULL key string.
 *
 * Return: The look up result, if the key-value pair entry exists, then returns
 * its value's address, NULL otherwise.
 */
void *hashmap_get(hashmap_t *map, char *key)
{
    hashmap_node_t *node = hashmap_get_node(map, key);
    return node ? node->val : NULL;
}

/* Check if the key-value pair entry exists from given key.
 * @map: The hashmap to be looked up. May be NULL.
 * @key: Non-NULL key string.
 *
 * Return: The look up result, if the key-value pair entry exists, then returns
 * true, false otherwise.
 */
bool hashmap_contains(hashmap_t *map, char *key)
{
    return hashmap_get_node(map, key);
}

/* Free the map table, any owned key copies, and the header. Mapped values have
 * separate lifetimes and are not freed here.
 * @map: The hashmap to be freed. May be NULL.
 */
void hashmap_free(hashmap_t *map)
{
    if (!map)
        return;

    if (map->has_owned_keys)
        for (int i = 0; i < map->cap; i++)
            if (map->table[i].occupied && map->table[i].owns_key)
                free(map->table[i].key);
    free(map->table);
    free(map);
}

void reset_tu_ordinary_index(void)
{
    hashmap_free(TU_ORDINARY_MAP);
    TU_ORDINARY_MAP = hashmap_create(32);
    if (!TU_ORDINARY_MAP)
        fatal("Failed to allocate translation-unit ordinary-name index");
}

static void register_tu_ordinary_name(const char *name,
                                      ordinary_kind_t kind,
                                      void *value)
{
    tu_ordinary_binding_t *binding;
    char *stable_name;

    if (!TU_ORDINARY_MAP)
        fatal("Translation-unit ordinary-name index is not initialized");
    stable_name = intern_string((char *) name);
    binding = hashmap_get(TU_ORDINARY_MAP, stable_name);
    if (binding)
        return;
    binding = arena_alloc(BLOCK_ARENA, sizeof(*binding));
    binding->kind = kind;
    binding->value = value;
    hashmap_put_borrowed(TU_ORDINARY_MAP, stable_name, binding);
    if (dump_stats)
        stats_tu_index_registrations++;
}

static ordinary_kind_t lookup_tu_ordinary_name(const char *name,
                                               int kinds,
                                               void **value)
{
    tu_ordinary_binding_t *binding;

    if (dump_stats)
        stats_tu_index_lookups++;
    binding =
        TU_ORDINARY_MAP ? hashmap_get(TU_ORDINARY_MAP, (char *) name) : NULL;
    if (!binding || !(kinds & binding->kind))
        return ORDINARY_NONE;
    if (value)
        *value = binding->value;
    return binding->kind;
}

static type_t *resolve_type_alias(type_t *alias)
{
    type_t *base = alias->base_struct;

    if (alias->base_type != TYPE_typedef || alias->size || alias->ptr_level ||
        alias->is_direct_function_type)
        return alias;

    /* A zero-sized alias can be a forward declaration of a record, but a
     * function type with a void return is also zero-sized and is not an alias
     * to its return type.
     */
    if (!base || (!alias->is_const_qualified && !alias->is_volatile_qualified))
        return base;
    if (!base->size)
        return alias;

    /* Keep qualifiers on the alias while filling in the completed tag's layout.
     */
    alias->size = base->size;
    alias->alignment = base->alignment;
    alias->fields = base->fields;
    alias->num_fields = base->num_fields;
    alias->is_union = base->is_union;
    alias->has_flexible_array_member = base->has_flexible_array_member;
    return alias;
}

/* Reject mismatched prefixes before strcmp. */
static bool identifier_matches(const char *name, const char *query)
{
    return name[0] == query[0] && (!query[0] || name[1] == query[1]) &&
           !strcmp(name, query);
}

/* Find the type by the given name.
 * @type_name: The name to be searched.
 * @flag:
 *      0 - Search in all type names.
 *      1 - Search in all names, excluding the tags of structure.
 *      2 - Only search in tags.
 *
 * Return: The pointer to the type, or NULL if not found.
 */
type_t *find_type(const char *type_name, int flag)
{
    for (int i = 0; i < types_idx; i++) {
        type_t *type = &TYPES[i];

        if (!identifier_matches(type->type_name, type_name))
            continue;
        bool is_tag =
            type->base_type == TYPE_struct || type->base_type == TYPE_union;

        if ((is_tag && flag == 1) || (!is_tag && flag == 2))
            continue;
        return is_tag ? type : resolve_type_alias(type);
    }
    return NULL;
}

/* Builtin types are initialized once before translation units are parsed. User
 * typedefs share TYPES for their descriptors, but must be reached only through
 * their translation-unit/lexical bindings.
 */
static type_t *find_builtin_type(const char *name)
{
    for (int i = 0; i < builtin_types_idx; i++) {
        type_t *type = &TYPES[i];

        if (type->base_type != TYPE_struct && type->base_type != TYPE_union &&
            identifier_matches(type->type_name, name))
            return type;
    }
    return NULL;
}

bool ph2_ir_function_included(const func_t *func, bool include_declarations)
{
    return func && (func->bbs || include_declarations);
}

void ph2_ir_prepare(bool include_declarations)
{
    int count = 0;

    if (ph2_ir_idx != 0 || PH2_IR_FLATTEN)
        fatal("phase-2 IR prepared more than once");

    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        if (!ph2_ir_function_included(func, include_declarations))
            continue;
        if (count >= (INT_MAX - ((int) sizeof(void *) - 1)) /
                         (int) sizeof(*PH2_IR_FLATTEN))
            limit_error("too many phase-2 IR instructions");
        count++;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                if (count >= (INT_MAX - ((int) sizeof(void *) - 1)) /
                                 (int) sizeof(*PH2_IR_FLATTEN))
                    limit_error("too many phase-2 IR instructions");
                count++;
            }
        }
    }

    ph2_ir_capacity = count;
    if (count)
        PH2_IR_FLATTEN = arena_alloc(
            GENERAL_ARENA, (int) ((size_t) count * sizeof(*PH2_IR_FLATTEN)));
}

ph2_ir_t *add_existed_ph2_ir(ph2_ir_t *ph2_ir)
{
    if (ph2_ir_idx >= ph2_ir_capacity)
        fatal("phase-2 IR pre-count mismatch");
    PH2_IR_FLATTEN[ph2_ir_idx++] = ph2_ir;
    return ph2_ir;
}

ph2_ir_t *ph2_ir_flatten_insn(ph2_ir_t *insn,
                              int stack_top_ofs,
                              int stack_size,
                              int saved_regs,
                              bool restore_saved_regs)
{
    if (insn->ofs_based_on_stack_top) {
        if (insn->op == OP_load || insn->op == OP_address_of)
            insn->src0 += stack_top_ofs;
        else if (insn->op == OP_store)
            insn->src1 += stack_top_ofs;
    }
    ph2_ir_t *flat = add_existed_ph2_ir(insn);
    if (insn->op == OP_return) {
        flat->src1 = stack_size;
        if (restore_saved_regs)
            flat->src2 = saved_regs;
    }
    return flat;
}

int ph2_ir_flatten_function(func_t *func,
                            int stack_top_ofs,
                            int prologue_bytes,
                            int saved_regs,
                            bool restore_saved_regs,
                            void (*update_offset)(ph2_ir_t *))
{
    int count = 0;
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        bb->elf_offset = elf_offset;
        if (bb == func->bbs)
            elf_offset += prologue_bytes;
        for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn; insn = insn->next) {
            ph2_ir_t *flat =
                ph2_ir_flatten_insn(insn, stack_top_ofs, func->stack_size,
                                    saved_regs, restore_saved_regs);
            count++;
            if (update_offset)
                update_offset(flat);
        }
    }
    return count;
}

static ph2_ir_t *ph2_ir_create(opcode_t op)
{
    ph2_ir_t *instruction = arena_calloc(BB_ARENA, 1, sizeof(ph2_ir_t));
    stats_ph2_ir_count++;
    instruction->op = op;
    instruction->src0_hi = instruction->src1_hi = instruction->dest_hi = -1;
    instruction->size_bytes = PTR_SIZE;
    return instruction;
}

ph2_ir_t *bb_add_ph2_ir(basic_block_t *bb, opcode_t op)
{
    ph2_ir_t *instruction = ph2_ir_create(op);
    if (bb->ph2_ir_list.tail)
        bb->ph2_ir_list.tail->next = instruction;
    else
        bb->ph2_ir_list.head = instruction;
    bb->ph2_ir_list.tail = instruction;
    return instruction;
}

ph2_ir_t *add_ph2_ir(opcode_t op)
{
    return add_existed_ph2_ir(ph2_ir_create(op));
}

#if ELF_MACHINE == ELF_MACHINE_ARM32 || ELF_MACHINE == ELF_MACHINE_RV32 || \
    ELF_MACHINE == ELF_MACHINE_AARCH64
static void ph2_ir_visit_globals(void (*visit)(ph2_ir_t *))
{
    for (ph2_ir_t *ir = GLOBAL_FUNC->bbs->ph2_ir_list.head; ir; ir = ir->next)
        visit(ir);
}
#endif

#if ELF_MACHINE == ELF_MACHINE_ARM32 || ELF_MACHINE == ELF_MACHINE_RV32
typedef struct {
    int stack_top;
    int prologue_bytes;
    int saved_regs;
    bool restore_saved_regs;
} ph2_frame_layout_t;

static void ph2_ir_flatten_functions(
    ph2_frame_layout_t (*frame_layout)(func_t *, ph2_ir_t *),
    void (*update_offset)(ph2_ir_t *))
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        ph2_ir_t *define;
        ph2_frame_layout_t frame;

        if (!func->bbs)
            continue;
        define = add_ph2_ir(OP_define);
        define->src0 = func->stack_size;
        define->func_name = intern_string(func->return_def.var_name);
        frame = frame_layout(func, define);
        ph2_ir_flatten_function(func, frame.stack_top, frame.prologue_bytes,
                                frame.saved_regs, frame.restore_saved_regs,
                                update_offset);
    }
}
#endif

block_t *add_block(block_t *parent, func_t *func)
{
    block_t *blk = arena_alloc(BLOCK_ARENA, sizeof(block_t));

    /* Initialize all fields explicitly */
    blk->locals.size = 0;
    blk->locals.capacity = 16;
    blk->locals.elements =
        arena_alloc(BLOCK_ARENA, blk->locals.capacity * sizeof(var_t *));
    blk->type_tags = NULL;
    blk->constants = NULL;
    blk->typedefs = NULL;
    blk->parent = parent;
    blk->func = func;
    blk->next = NULL;
    return blk;
}

/* String pool global */
string_pool_t *string_pool;
string_literal_pool_t *string_literal_pool;

/* Safe string interning that works with self-hosting */
char *intern_string(char *str)
{
    char *existing;
    char *interned;
    int len;

    /* Safety: return original if NULL */
    if (!str)
        return NULL;

    /* Safety: can't intern before initialization */
    if (!GENERAL_ARENA || !string_pool)
        return str;

    /* Check if already interned */
    existing = hashmap_get(string_pool->strings, str);
    if (existing)
        return existing;

    /* Allocate and store new string */
    len = strlen(str) + 1;
    interned = arena_alloc(GENERAL_ARENA, len);
    strcpy(interned, str);

    hashmap_put_borrowed(string_pool->strings, interned, interned);

    return interned;
}

int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Encode a valid Unicode scalar value into this implementation's UTF-8
 * execution character set. Narrow literals retain bytes, so this also keeps a
 * UCN usable in strings, character constants, and #if character constants.
 */
static int append_utf8(char *output, int out, int limit, unsigned int value)
{
    if (value <= 0x7f) {
        if (out + 1 >= limit)
            return -1;
        output[out++] = value;
    } else if (value <= 0x7ff) {
        if (out + 2 >= limit)
            return -1;
        output[out++] = 0xc0 | (value >> 6);
        output[out++] = 0x80 | (value & 0x3f);
    } else if (value <= 0xffff) {
        if (out + 3 >= limit)
            return -1;
        output[out++] = 0xe0 | (value >> 12);
        output[out++] = 0x80 | ((value >> 6) & 0x3f);
        output[out++] = 0x80 | (value & 0x3f);
    } else {
        if (out + 4 >= limit)
            return -1;
        output[out++] = 0xf0 | (value >> 18);
        output[out++] = 0x80 | ((value >> 12) & 0x3f);
        output[out++] = 0x80 | ((value >> 6) & 0x3f);
        output[out++] = 0x80 | (value & 0x3f);
    }
    return out;
}

/* C99 6.4.4.4 requires the value of a hexadecimal escape in a character
 * constant or narrow string literal to fit an unsigned char. Report whether a
 * literal's spelling holds one that does not. Wide literals accept larger
 * values, so only narrow callers ask.
 */
bool hex_escape_exceeds_byte(const char *text)
{
    for (int i = 0; text[i]; i++) {
        if (text[i] != '\\')
            continue;
        i++;
        if (!text[i])
            break;
        if (text[i] != 'x')
            continue;

        unsigned int value = 0;
        while (isxdigit((unsigned char) text[i + 1])) {
            i++;
            value = (value << 4) + hex_digit_value(text[i]);
            if (value > 0xff)
                return true;
        }
    }
    return false;
}

/* Decode one escape beginning at text[*input]. Narrow strings mask hex runs to
 * a byte; wide strings retain their value as one int unit.
 */
static bool decode_escape_value(const char *text,
                                int *input,
                                bool wide,
                                unsigned int *value,
                                bool *is_ucn)
{
    int i = *input;

    *is_ucn = false;
    switch (text[i]) {
    case 'a':
        *value = '\a';
        i++;
        break;
    case 'b':
        *value = '\b';
        i++;
        break;
    case 'f':
        *value = '\f';
        i++;
        break;
    case 'n':
        *value = '\n';
        i++;
        break;
    case 'r':
        *value = '\r';
        i++;
        break;
    case 't':
        *value = '\t';
        i++;
        break;
    case 'v':
        *value = '\v';
        i++;
        break;
    case '\\':
        *value = '\\';
        i++;
        break;
    case '\'':
        *value = '\'';
        i++;
        break;
    case '"':
        *value = '"';
        i++;
        break;
    case '?':
        *value = '?';
        i++;
        break;
    case 'e':
        if (strict_c99)
            return false;
        *value = 27;
        i++;
        break;
    case 'x':
        i++;
        if (!isxdigit((unsigned char) text[i]))
            return false;
        *value = 0;
        while (isxdigit((unsigned char) text[i])) {
            int digit = hex_digit_value(text[i]);

            if (wide) {
                if (*value > 0x07ffffffU)
                    return false;
                *value = (*value << 4) + digit;
            } else
                *value = ((*value << 4) + digit) & 0xff;
            i++;
        }
        if (wide && *value > 0x7fffffffU)
            return false;
        break;
    case 'u':
    case 'U': {
        int digits = text[i] == 'u' ? 4 : 8;

        *value = 0;
        i++;
        for (int n = 0; n < digits; n++) {
            if (!isxdigit((unsigned char) text[i]) ||
                (wide && *value > 0x07ffffffU))
                return false;
            *value = (*value << 4) + hex_digit_value(text[i++]);
        }

        /* C99 excludes surrogates, out-of-range values, and basic-source
         * characters other than $, @, and ` from universal escapes.
         */
        if ((wide && *value > 0x7fffffffU) || *value > 0x10ffff ||
            (*value >= 0xd800 && *value <= 0xdfff) ||
            (*value < 0xa0 && *value != '$' && *value != '@' && *value != '`'))
            return false;
        *is_ucn = true;
        break;
    }
    default:
        if (!text[i])
            return false;
        if (text[i] >= '0' && text[i] <= '7') {
            *value = 0;
            for (int n = 0; n < 3 && text[i] >= '0' && text[i] <= '7'; n++)
                *value = *value * 8 + (text[i++] - '0');
        } else
            *value = (unsigned char) text[i++];
        break;
    }
    *input = i;
    return true;
}

int unescape_string(const char *input, char *output, int output_size)
{
    if (!input || !output || output_size == 0)
        return -1;

    int i = 0, j = 0;

    while (input[i] != '\0' && j < output_size - 1) {
        if (input[i] != '\\') {
            output[j++] = input[i++];
            continue;
        }
        i++;

        unsigned int value;
        bool is_ucn;

        if (!decode_escape_value(input, &i, false, &value, &is_ucn)) {
            output[j] = '\0';
            return -1;
        }
        if (is_ucn) {
            j = append_utf8(output, j, output_size, value);
            if (j < 0) {
                output[0] = '\0';
                return -1;
            }
        } else
            output[j++] = (char) value;
    }

    output[j] = '\0';
    if (input[i] != '\0')
        return -1;
    return j;
}

/* C99 permits multi-character constants with an implementation-defined int
 * value. shecc packs their first four bytes left to right.
 */
int parse_character_constant(const char *literal)
{
    char unescaped[MAX_TOKEN_LEN];
    unsigned int value = 0;
    int length = unescape_string(literal, unescaped, sizeof(unescaped));

    if (length < 0)
        return 0;
    for (int i = 0; i < length && i < 4; i++)
        value = (value << 8) | (unsigned char) unescaped[i];
    return (int) value;
}

/* The narrow decoder translates UCNs to UTF-8, which is correct for char
 * strings but not for a wide literal: one UCN is one wchar_t element. This
 * execution-wide-character policy stores each source byte/escape value as an
 * int unit and preserves UCN scalar values directly.
 */
int decode_wstring_units(const char *text, int *units, int capacity)
{
    int in = 0;
    int out = 0;

    while (text[in]) {
        unsigned int value;

        if (out >= capacity)
            return -1;
        if (text[in] != '\\') {
            units[out++] = (unsigned char) text[in++];
            continue;
        }
        in++;

        bool is_ucn;

        if (!decode_escape_value(text, &in, true, &value, &is_ucn))
            return -1;
        units[out++] = (int) value;
    }
    return out;
}

/* A wide character constant that holds one execution-wide unit has the value of
 * that unit. Its hexadecimal and octal escapes are therefore not cut to the
 * byte a narrow constant keeps, and a UCN is not spread over UTF-8 bytes. A
 * constant of several units keeps the implementation-defined packing of
 * parse_character_constant().
 *
 * Returns false when an escape does not fit a unit.
 */
bool wide_character_constant(const char *literal, int *value)
{
    int units[MAX_TOKEN_LEN];
    int length = decode_wstring_units(literal, units, MAX_TOKEN_LEN);

    *value = 0;
    if (length < 0)
        return false;
    *value = length == 1 ? units[0] : parse_character_constant(literal);
    return true;
}

/* The value of an integer literal in one word. A literal past that word keeps
 * its low bits; the accumulator is unsigned so that is a wrap, never the host
 * compiler's signed overflow. Constant expressions that can hold such a literal
 * fold it through the two-word evaluator instead.
 */
int parse_numeric_constant(const char *buffer)
{
    int base = numeric_literal_base(buffer);
    int i = base == 16 || base == 2 ? 2 : 0;
    unsigned int value = 0;
    while (buffer[i]) {
        /* The lexer keeps C99 integer suffixes in the token. Their type is
         * selected by the parser; they are not digits of the value.
         */
        if ((buffer[i] | 32) == 'u' || (buffer[i] | 32) == 'l')
            break;
        char c = buffer[i++];
        value *= base;
        if (base == 2)
            value += c == '1';
        else if (base == 16) {
            c |= 32;
            if (isdigit(c))
                value += c - '0';
            else if (c >= 'a' && c <= 'f')
                value += c - 'a' + 10;
        } else
            value += c - '0';
    }
    return value;
}

/* Give @type its field table, on the first field it is asked for.
 *
 * The table is MAX_FIELDS var_t by value, and it has to stay put: a struct body
 * hands out a var_t * per declarator and reads it again after the next
 * declarator has been added, so a table that grew by reallocating would leave
 * those pointers behind. Allocating it once at full size keeps them valid.
 *
 * What it need not do is allocate for a type that never has a field. Most of
 * what add_type() creates -- every enum, every typedef of a scalar, every
 * builtin -- has none, and was paying for the whole table and for the loop that
 * walked it.
 */
void type_ensure_fields(type_t *type)
{
    if (type->fields)
        return;

    type->fields = arena_calloc(GENERAL_ARENA, MAX_FIELDS, sizeof(var_t));
    stats_field_slot_count += MAX_FIELDS;

    /* The field variables come out of a zeroed allocation, so give their
     * interned name pointers the empty string a reader can dereference.
     */
    for (int i = 0; i < MAX_FIELDS; i++)
        type->fields[i].var_name = "";
}

type_t *add_type(void)
{
    /* Every struct, union and enum specifier with a body, and every typedef,
     * takes an entry, so a large enough input reaches the end of the table.
     */
    if (types_idx >= MAX_TYPES)
        limit_error("Maximum number of types exceeded");
    type_t *t = &TYPES[types_idx++];
    t->fields = NULL;
    return t;
}

/* Record a struct, union or enum tag's name.
 *
 * type_name is a fixed array, and an identifier may be up to MAX_ID_LEN long,
 * so a longer tag would run past it into the fields that follow. Refuse it
 * rather than corrupting the type.
 */
__noreturn void fatal(const char *msg);
__noreturn void usage_error(const char *msg);

void set_type_name(type_t *type, char *name)
{
    if (strlen(name) >= MAX_TYPE_LEN)
        fatal("Type name too long");
    strcpy(type->type_name, intern_string(name));
}

type_t *add_named_type(char *name)
{
    type_t *type = add_type();
    /* Use interned string for type name */
    set_type_name(type, name);
    return type;
}

/* Enum names declared in a block shadow enclosing names, but must disappear
 * with that block. File-scope names use the active translation-unit root of the
 * same scope tree.
 */
void add_scoped_constant(block_t *block, char alias[], int value)
{
    constant_t *constant = arena_alloc_constant();

    if (!constant)
        fatal("Failed to allocate scoped enum constant");
    strcpy(constant->alias, intern_string(alias));
    constant->value = value;
    constant->next = block->constants;
    block->constants = constant;
    if (block == CURRENT_TU_SCOPE)
        register_tu_ordinary_name(alias, ORDINARY_CONSTANT, constant);
}

/* A file-scope enumeration constant has translation-unit scope, not process
 * scope. Keep the parser API but store it on the active input's root.
 */
void add_constant(char alias[], int value)
{
    add_scoped_constant(CURRENT_TU_SCOPE, alias, value);
}

/* Keep names declared at file scope in the active unit's ordinary namespace.
 * Global storage is merged for code generation, but this small per-unit index
 * preserves scope-sensitive collisions with no-linkage enumerators.
 */
void add_tu_global_var(var_t *var)
{
    if (lookup_tu_ordinary_name(file_scope_var_name(var), ORDINARY_VARIABLE,
                                NULL))
        return;
    register_tu_ordinary_name(file_scope_var_name(var), ORDINARY_VARIABLE, var);
}

static var_t *find_var_in_list(var_list_t *vars, const char *name, int *pos)
{
    for (int i = pos ? *pos : 0; i < vars->size; i++) {
        var_t *var = vars->elements[i];

        if (identifier_matches(var->var_name, name)) {
            if (pos)
                *pos = i + 1;
            return var;
        }
    }
    if (pos)
        *pos = vars->size;
    return NULL;
}

/* Resume at *@pos so callers can continue after a previous match. */
var_t *find_block_local(block_t *block, const char *name, int *pos)
{
    if (block == CURRENT_TU_SCOPE) {
        void *binding = NULL;

        if ((*pos)++)
            return NULL;
        if (lookup_tu_ordinary_name(name, ORDINARY_VARIABLE, &binding))
            return binding;
        return NULL;
    }
    return find_var_in_list(&block->locals, name, pos);
}

static var_t *find_tu_declaration(const char *name)
{
    void *binding = NULL;

    if (lookup_tu_ordinary_name(name, ORDINARY_VARIABLE, &binding))
        return binding;
    return NULL;
}

static var_t *find_func_param(func_t *func, const char *name)
{
    if (!func)
        return NULL;
    for (int i = 0; i < func->num_params; i++)
        if (identifier_matches(func->param_defs[i].var_name, name))
            return &func->param_defs[i];
    return NULL;
}

/* The binding @name has in @block's own scope among the @kinds asked for, or
 * ORDINARY_NONE. The outermost block of a function body shares the scope of the
 * function's parameters (C99 6.2.1p4); a nested block only hides them. When
 * @binding is not NULL it receives the constant_t, var_t or typedef_binding_t
 * found. A valid program binds a name once per scope, so the order in which the
 * kinds are tried only decides what an invalid redeclaration reports.
 */
ordinary_kind_t find_block_ordinary(block_t *block,
                                    const char *name,
                                    int kinds,
                                    void **binding)
{
    void *unused;

    if (!binding)
        binding = &unused;
    if (block == CURRENT_TU_SCOPE)
        return lookup_tu_ordinary_name(name, kinds, binding);
    if (kinds & ORDINARY_CONSTANT) {
        for (constant_t *constant = block->constants; constant;
             constant = constant->next) {
            if (identifier_matches(constant->alias, name)) {
                *binding = constant;
                return ORDINARY_CONSTANT;
            }
        }
    }
    if (kinds & ORDINARY_VARIABLE) {
        int pos = 0;

        *binding = find_block_local(block, name, &pos);
        if (*binding)
            return ORDINARY_VARIABLE;
    }
    if (kinds & ORDINARY_TYPEDEF) {
        for (typedef_binding_t *td = block->typedefs; td; td = td->next) {
            if (identifier_matches(td->name, name)) {
                *binding = td;
                return ORDINARY_TYPEDEF;
            }
        }
    }
    if ((kinds & ORDINARY_PARAMETER) && !block->parent && block->func) {
        var_t *param = find_func_param(block->func, name);

        if (param) {
            *binding = param;
            return ORDINARY_PARAMETER;
        }
    }
    return ORDINARY_NONE;
}

/* File-scope typedef aliases still use the legacy process-wide type table for
 * lookup, but their declaration names also belong in the active unit's
 * ordinary-identifier index for same-unit collision checks.
 */
void add_tu_typedef(type_t *type)
{
    const char *name = type->type_name;

    if (!name[0])
        return;
    ordinary_kind_t kind =
        find_block_ordinary(CURRENT_TU_SCOPE, name, ORDINARY_ANY, NULL);
    if (kind == ORDINARY_TYPEDEF)
        return;
    if (kind != ORDINARY_NONE)
        error_at("typedef name conflicts with an ordinary identifier",
                 cur_token_loc());

    typedef_binding_t *binding = arena_alloc(BLOCK_ARENA, sizeof(*binding));
    binding->name = intern_string(type->type_name);
    binding->type = type;
    binding->next = CURRENT_TU_SCOPE->typedefs;
    CURRENT_TU_SCOPE->typedefs = binding;
    register_tu_ordinary_name(name, ORDINARY_TYPEDEF, binding);
}

/* The enumeration constant @alias as seen from @block, or NULL when none is
 * visible. Constants share the ordinary identifier name space, so an object,
 * parameter or typedef name declared in a nearer scope hides an outer constant
 * of the same name (C99 6.2.1p4).
 *
 * Every identifier operand asks, and nearly all of them name no constant, so
 * find the constant first, which costs no scan of any block's locals. Only when
 * there is one are the scopes between the use and the constant's own searched
 * for a binding that hides it.
 */
constant_t *find_scoped_constant(char alias[], block_t *block)
{
    block_t *owner;
    void *constant = NULL;

    for (owner = block; owner && owner != GLOBAL_BLOCK; owner = owner->parent)
        if (find_block_ordinary(owner, alias, ORDINARY_CONSTANT, &constant))
            break;
    if (!owner || owner == GLOBAL_BLOCK) {
        /* Function scopes are not parented to the translation-unit scope. */
        owner = GLOBAL_BLOCK;
        if (!lookup_tu_ordinary_name(alias, ORDINARY_CONSTANT, &constant))
            return NULL;
    }
    for (; block && block != owner; block = block->parent) {
        if (find_block_ordinary(
                block, alias,
                ORDINARY_VARIABLE | ORDINARY_TYPEDEF | ORDINARY_PARAMETER,
                NULL))
            return NULL;
    }
    return constant;
}

void add_type_tag(block_t *block, char name[], type_t *type)
{
    type_tag_t *tag = arena_alloc(BLOCK_ARENA, sizeof(type_tag_t));

    if (strlen(name) >= MAX_TYPE_LEN)
        fatal("Type name too long");
    strcpy(tag->name, intern_string(name));
    tag->type = type;
    tag->next = block->type_tags;
    block->type_tags = tag;
}

type_t *find_local_type_tag(char name[], block_t *block)
{
    for (type_tag_t *tag = block->type_tags; tag; tag = tag->next)
        if (identifier_matches(tag->name, name))
            return tag->type;
    return NULL;
}

/* An enum type is int based (see initialize_enum_type()), which is also what
 * tells its tag apart from a struct or union tag.
 */
#define ENUM_TAG_KIND TYPE_int

/* File-scope tags live in the active translation unit, not the merged block
 * that owns global object declarations. Some parser paths still pass that
 * merged block to type lookup; normalize those file-scope arguments here.
 */
static block_t *tag_scope(block_t *block)
{
    return !block || block == GLOBAL_BLOCK ? CURRENT_TU_SCOPE : block;
}

/* C99 6.7.2.3p3: every declaration of a tag names the same kind of type, and
 * struct, union and enum tags share one name space, so a tag found under
 * another keyword is an error rather than a miss. @kind is TYPE_struct,
 * TYPE_union or ENUM_TAG_KIND.
 */
static type_t *check_tag_kind(type_t *type, base_type_t kind)
{
    if (type && type->base_type != kind)
        error_at("tag was previously declared as a different kind of tag",
                 cur_token_loc());
    return type;
}

/* The tag @name of @kind as seen from @block, or NULL when no tag of that name
 * is visible. Tags are registered with the block that declares them, file-scope
 * ones with CURRENT_TU_SCOPE, which a function body's chain does not reach on
 * its own, so a NULL @block sees the current file scope only. A tag declared in
 * another translation unit is never found. Looking in the tag table, not the
 * type table, also keeps a typedef name out of it.
 */
static type_t *find_visible_tag(char name[], block_t *block, base_type_t kind)
{
    type_t *type = NULL;

    block = tag_scope(block);
    for (; block && block != CURRENT_TU_SCOPE && !type; block = block->parent)
        type = find_local_type_tag(name, block);
    if (!type)
        type = find_local_type_tag(name, CURRENT_TU_SCOPE);
    return check_tag_kind(type, kind);
}

/* Create the incomplete struct or union tag @name of @kind in @block. */
static type_t *declare_record_tag(char name[], block_t *block, base_type_t kind)
{
    type_t *type = add_named_type(name);

    type->base_type = kind;
    add_type_tag(block, name, type);
    return type;
}

/* The struct or union tag @name as seen from @block, spelled with the keyword
 * for @kind, or NULL when no such tag is visible.
 */
type_t *find_record_tag(char name[], block_t *block, base_type_t kind)
{
    return find_visible_tag(name, block, kind);
}

/* A struct or union specifier with no member list: the visible tag, or else a
 * new incomplete one in the current scope (C99 6.7.2.3p8), file scope when
 * @block is NULL. An object of that type is rejected once its declarator is
 * read.
 */
type_t *reference_record_tag(char name[], block_t *block, base_type_t kind)
{
    block = tag_scope(block);
    type_t *type = find_record_tag(name, block, kind);

    return type ? type : declare_record_tag(name, block, kind);
}

/* The tag @name that @block itself declares, created incomplete when it has
 * none yet. A member list or a bare "struct tag;" declares the tag in the
 * current scope, shadowing any outer one (C99 6.7.2.3p5 and p7).
 */
type_t *local_record_tag(char name[], block_t *block, base_type_t kind)
{
    block = tag_scope(block);
    type_t *type = check_tag_kind(find_local_type_tag(name, block), kind);

    return type ? type : declare_record_tag(name, block, kind);
}

/* Open the member list of the struct or union tag @tag. C99 6.7.2.3p1 lets a
 * scope define a tag's content only once, and a definition of the same tag
 * inside that member list is a second one in the same scope, since a member
 * list opens no scope of its own. The tag is still incomplete there, so mark it
 * now rather than rely on its field count.
 */
void begin_record_definition(type_t *tag)
{
    if (tag->num_fields || tag->definition_started)
        error_at("redefinition of struct or union tag", cur_token_loc());
    tag->definition_started = true;
}

/* The enum tag @name as seen from @block, or NULL when none is visible. */
type_t *find_enum_tag(char name[], block_t *block)
{
    return find_visible_tag(name, block, ENUM_TAG_KIND);
}

/* The enum tag @name named by a specifier seen from @block. Unlike a record
 * tag, an enum tag is never incomplete (C99 6.7.2.3p2 requires an enumerator
 * list before the type is used), so a name with no visible definition is an
 * error.
 */
type_t *reference_enum_tag(char name[], block_t *block)
{
    type_t *type = find_enum_tag(name, block);

    if (!type)
        error_at("Unknown enum type: C99 forbids forward references to enums",
                 cur_token_loc());
    return type;
}

/* The enum tag @name that @block itself declares, or NULL. */
type_t *local_enum_tag(char name[], block_t *block)
{
    block = tag_scope(block);
    return check_tag_kind(find_local_type_tag(name, block), ENUM_TAG_KIND);
}

bool find_block_typedef(block_t *block, const char *name)
{
    return find_block_ordinary(block, name, ORDINARY_TYPEDEF, NULL) !=
           ORDINARY_NONE;
}

/* A nested block may legally shadow a parameter with a typedef, but the
 * function body's outermost block shares the parameters' scope.
 */
void add_block_typedef(block_t *block, char name[], type_t *type)
{
    if (find_block_ordinary(block, name, ORDINARY_ANY, NULL))
        error_at("typedef name conflicts with an ordinary identifier",
                 cur_token_loc());

    typedef_binding_t *binding;

    binding = arena_alloc(BLOCK_ARENA, sizeof(*binding));
    binding->name = intern_string(name);
    binding->type = type;
    binding->next = block->typedefs;
    block->typedefs = binding;
}

/* Ordinary identifiers and typedef names share C's ordinary identifier
 * namespace. Search each lexical scope inward-out so an object declaration
 * masks an outer typedef before the global type table is considered.
 */
type_t *find_visible_type(const char *name, block_t *block)
{
    bool searched_tu = false;

    if (!block || block == GLOBAL_BLOCK)
        block = CURRENT_TU_SCOPE;

    /* Parameters are tried with the function body's outermost block, so they
     * too hide a file-scope typedef unless a nearer binding has hidden them.
     */
    for (; block; block = block->parent) {
        void *binding;
        ordinary_kind_t kind =
            find_block_ordinary(block, name, ORDINARY_ANY, &binding);

        if (block == CURRENT_TU_SCOPE)
            searched_tu = true;
        if (kind == ORDINARY_TYPEDEF)
            return resolve_type_alias(((typedef_binding_t *) binding)->type);
        if (kind != ORDINARY_NONE)
            return NULL;
    }
    if (!searched_tu) {
        void *binding;
        ordinary_kind_t kind =
            find_block_ordinary(CURRENT_TU_SCOPE, name, ORDINARY_ANY, &binding);

        if (kind == ORDINARY_TYPEDEF)
            return resolve_type_alias(((typedef_binding_t *) binding)->type);
        if (kind != ORDINARY_NONE)
            return NULL;
    }
    return find_builtin_type(name);
}

var_t *find_member(const char token[], type_t *type)
{
    /* An alias that names a structure tag instead of defining the members,
     * whether forward declared or as a pointer as in "typedef struct S *SP",
     * finds them on the tag. A scalar -- or "void" -- has no base to switch to,
     * and following the NULL was a SIGSEGV.
     */
    if (!type->num_fields)
        type = type->base_struct;
    if (!type)
        return NULL;

    for (int i = 0; i < type->num_fields; i++) {
        if (identifier_matches(type->fields[i].var_name, token))
            return &type->fields[i];
    }
    return NULL;
}

/* Search locals from the innermost scope outward, then parameters. */
var_t *find_local_var(const char *token, block_t *block)
{
    func_t *func = block->func;
    var_t *var;

    for (; block; block = block->parent) {
        if ((var = find_var_in_list(&block->locals, token, NULL)))
            return var;
    }

    return find_func_param(func, token);
}

var_t *find_global_var(const char *token)
{
    var_t *unit_var = find_tu_declaration(token);

    if (unit_var && !unit_var->is_block_scope_function_declaration)
        return find_func(unit_var->var_name) ? NULL : unit_var;

    return find_var_in_list(&GLOBAL_BLOCK->locals, token, NULL);
}

var_t *find_var(char *token, block_t *parent)
{
    var_t *var = find_local_var(token, parent);
    if (!var)
        var = find_global_var(token);
    return var;
}

int size_var(var_t *var)
{
    int size;
    if (var->is_flexible_array_member)
        return 0;
    if (var->ptr_level > 0 || var->is_func) {
        /* Pointers and function pointers occupy a target pointer, which is 8
         * bytes on LP64 targets and 4 on the 32-bit ones.
         */
        size = PTR_SIZE;
    } else {
        type_t *type = var->type;
        if (type->size == 0)
            size = type->base_struct->size;
        else
            size = type->size;
    }
    if (var->array_size > 0)
        size = size * var->array_size;
    return size;
}

/* Create a new function and adds it to the function lookup table and function
 * list if it does not already exist, or returns the existing instance if the
 * function already exists.
 *
 * Synthesized functions (e.g., compiler-generated functions like '__syscall')
 * are excluded from SSA analysis.
 *
 * @func_name: The name of the function. May be NULL.
 * @synthesize: Indicates whether the function is synthesized by the compiler.
 * Synthesized functions will not be analyzed by the SSA unit.
 *
 * Return: A pointer to the function.
 */
func_t *add_func(char *func_name, bool synthesize)
{
    char *stable_name;
    func_t *func = hashmap_get(FUNC_MAP, func_name);

    if (func)
        return func;

    stable_name = intern_string(func_name);
    func = arena_alloc_func();
    hashmap_put_borrowed(FUNC_MAP, stable_name, func);
    for (int i = 0; i < MAX_PARAMS; i++)
        func->param_defs[i].var_name = "";
    func->return_def.var_name = stable_name;

    /* Prepare space for function arguments.
     *
     * For Arm architecture, the first four arguments (arg1 ~ arg4) are passed
     * to r0 ~ r3, and any additional arguments (arg5+) are passed to the stack.
     *
     * +-------------+
     * | local vars  |
     * +-------------+
     * |    ...      |
     * +-------------+ <-- sp + 16
     * |    arg 8    |
     * +-------------+ <-- sp + 12
     * |    arg 7    |
     * +-------------+ <-- sp + 8
     * |    arg 6    |
     * +-------------+ <-- sp + 4
     * |    arg 5    |
     * +-------------+ <-- sp
     *
     * If the target architecture is RISC-V, arg1 ~ arg8 are passed to registers
     * and arg9+ are passed to the stack.
     *
     * We reserve one slot per stack-passed argument at the bottom of every
     * frame so that each function can use the space to pass extra arguments.
     * The slot is pointer-sized, matching what abi_lower_call_args() stores:
     * sizing it at 4 on an LP64 target leaves the reservation short, and the
     * outgoing arguments then overwrite the first locals allocated above it.
     */
    func->stack_size = (MAX_PARAMS - MAX_ARGS_IN_REG) * PTR_SIZE;

    if (synthesize)
        return func;

    if (!FUNC_LIST.head) {
        FUNC_LIST.head = func;
        FUNC_LIST.tail = func;
    } else {
        FUNC_LIST.tail->next = func;
        FUNC_LIST.tail = func;
    }

    return func;
}

/* Find the function in function map.
 * @func_name: The name of the function. May be NULL.
 *
 * Return: A pointer to the function if exists, NULL otherwise.
 */
func_t *find_func(char *func_name)
{
    return hashmap_get(FUNC_MAP, func_name);
}

int function_entry_address(char *name)
{
    func_t *func = find_func(name);

    if (func->bbs)
        return elf_code_start + func->bbs->elf_offset;
    if (dynlink)
        return dynamic_sections.elf_plt_start + func->plt_offset;
    printf("The '%s' function is not implemented\n", name);
    fflush(stdout);
    abort();
}

/* Create a basic block and set the scope of variables to 'parent' block */
basic_block_t *bb_create(block_t *parent)
{
    /* Use arena_calloc for basic_block_t as it has many arrays that need
     * zeroing (live_gen, live_kill, live_in, live_out, DF, RDF, dom_next, etc.)
     * This is simpler and safer than manually initializing everything.
     */
    basic_block_t *bb = arena_alloc_bb();

    /* Initialize non-zero fields */
    bb->scope = parent;
    bb->belong_to = parent->func;

    /* -1 marks "no machine code emitted for this block yet". Backends assign a
     * real offset as they emit; 0 is a legitimate offset, so it cannot double
     * as the sentinel.
     */
    bb->elf_offset = -1;

    return bb;
}

/* log2 of @v when it is a power of two, otherwise -1. */
int exact_log2(int v)
{
    int k = 0;

    if (v <= 0 || (v & (v - 1)))
        return -1;
    while (v > 1) {
        v = v >> 1;
        k++;
    }
    return k;
}

/* Grow a doubling array held in @arena from @cap elements of @elem_sz to the
 * next capacity, starting at @first while it is still unallocated. A non-zero
 * @limit caps the growth, with @what naming the array in the diagnostic.
 * Returns the (possibly moved) array and writes the new capacity back to @cap.
 */
void *arena_grow(arena_t *arena,
                 char *ptr,
                 int *cap,
                 int elem_sz,
                 int first,
                 int limit,
                 char *what)
{
    int new_cap;

    if (*cap < 0 || elem_sz <= 0 || first <= 0 || *cap > INT_MAX / 2)
        return NULL;
    new_cap = *cap ? *cap << 1 : first;
    if (new_cap > INT_MAX / elem_sz)
        return NULL;
    if (limit && new_cap > limit)
        limit_error(what);
    void *grown = arena_realloc(arena, ptr, *cap * elem_sz, new_cap * elem_sz);
    if (grown)
        *cap = new_cap;
    return grown;
}

/* The pred-succ pair must have only one connection */
void bb_connect(basic_block_t *pred,
                basic_block_t *succ,
                bb_connection_type_t type)
{
    /* Statements after a return or goto are unreachable, and the parser walks
     * them with no current block. An edge out of nowhere is meaningless rather
     * than wrong, so drop it.
     */
    if (!pred || !succ)
        return;

    /* bb_disconnect() leaves holes, so reuse the first free slot before
     * extending. prev_idx is one past the highest slot ever filled.
     */
    int i = 0;
    while (i < succ->prev_idx && succ->prev[i].bb)
        i++;

    if (i >= succ->prev_cap)
        succ->prev = arena_grow(BB_ARENA, (char *) succ->prev, &succ->prev_cap,
                                sizeof(bb_connection_t), 4, MAX_BB_PRED,
                                "Too many predecessors");

    succ->prev[i].bb = pred;
    succ->prev[i].type = type;
    if (i >= succ->prev_idx)
        succ->prev_idx = i + 1;

    switch (type) {
    case NEXT:
        pred->next = succ;
        break;
    case THEN:
        pred->then_ = succ;
        break;
    case ELSE:
        pred->else_ = succ;
        break;
    default:
        abort();
    }
    vir_frontend_note_edge(pred, succ);
}

/* bb_disconnect() leaves holes in prev[], so prev_idx alone is not enough. */
bool bb_has_pred(const basic_block_t *bb)
{
    for (int i = 0; i < bb->prev_idx; i++)
        if (bb->prev[i].bb)
            return true;
    return false;
}

/* Return the sole live predecessor, or NULL when the block has zero or many. */
basic_block_t *bb_sole_pred(const basic_block_t *bb)
{
    basic_block_t *pred = NULL;

    for (int i = 0; i < bb->prev_idx; i++) {
        basic_block_t *candidate = bb->prev[i].bb;

        if (!candidate)
            continue;
        if (pred)
            return NULL;
        pred = candidate;
    }
    return pred;
}

/* Pointer-star qualifiers describe pointer storage independently of its base.
 */
static int var_volatile_pointer_depth(const var_t *var)
{
    return var->ptr_level + var->type->ptr_level + var->is_func +
           !!(var->array_size || var->has_unsized_array);
}
bool var_volatile_storage(const var_t *var)
{
    if (!var || !var->type)
        return false;
    int depth = var_volatile_pointer_depth(var);
    return depth ? depth <= 32 &&
                       (var->pointer_volatile_mask & (1U << (depth - 1)))
                 : var->is_volatile || var->type->is_volatile_qualified;
}
bool var_volatile_pointee(const var_t *var)
{
    if (!var || !var->type)
        return false;
    int depth = var_volatile_pointer_depth(var);
    const func_t *signature = var->pointee_func_signature;
    if (!signature && !var->is_func && var->type->func_signature &&
        !var->type->is_direct_function_type)
        signature = var->type->func_signature;
    if (signature) {
        const var_t *result = &signature->return_def;
        int return_depth =
            var->type->func_signature && !var->type->is_direct_function_type
                ? 0
                : result->ptr_level + result->type->ptr_level;
        if (depth == return_depth + 1)
            return var->callback_is_volatile;
    }
    return depth > 1 ? depth <= 33 &&
                           (var->pointer_volatile_mask & (1U << (depth - 2)))
                     : var->is_volatile || var->type->is_volatile_qualified;
}
bool var_is_volatile_object(const var_t *var)
{
    return var_volatile_storage(var) && var->var_name[0] != '.';
}

/* A volatile object a primary expression has named and no instruction has read
 * yet. See discard_operand().
 */
var_t *unread_volatile_object;

void add_insn(block_t *block,
              basic_block_t *bb,
              opcode_t op,
              var_t *rd,
              var_t *rs1,
              var_t *rs2,
              int sz,
              char *str)
{
    if (!bb)
        return;

    bb->scope = block;

    /* An instruction reading the object discard_operand() watches has given it
     * the read it is owed.
     */
    if (unread_volatile_object &&
        (rs1 == unread_volatile_object || rs2 == unread_volatile_object))
        unread_volatile_object = NULL;

    /* Mark variables as address-taken to prevent incorrect constant
     * optimization
     */
    if ((op == OP_address_of || op == OP_global_address_of) && rs1) {
        if (rd)
            rd->is_volatile_access = var_volatile_storage(rs1);
        rs1->address_taken = true;
        rs1->is_const = false; /* disable constant optimization */
    }

    /* A volatile object can be read or written behind the program's back, and
     * every access to it is a side effect (C99 6.7.3p6). Keep a local one in
     * its slot as though its address had escaped, so that each access by name
     * reaches memory rather than a register copy.
     */
    if (op == OP_allocat && var_volatile_storage(rd) && !rd->is_global)
        rd->address_taken = true;

    vir_frontend_note_insn(bb, op, rd, rs1, rs2, sz,
                           str ? intern_string(str) : NULL);
}

void dump_stats_phase(const char *phase)
{
    if (!dump_stats)
        return;

    /* Keep each call within the compiler's current fixed-argument ABI limit.
     * The line grouping is also intentionally stable for benchmark parsers.
     */
    fprintf(stderr, "stats phase=%s funcs=%d vars=%d bbs=%d\n", phase,
            stats_func_count, stats_var_count, stats_bb_count);
    if (!strcmp(phase, "parse")) {
        fprintf(stderr, "stats phase=parse translation_units=%d\n",
                translation_unit_count);
        fprintf(stderr,
                "stats phase=parse tu_index_lookups=%d "
                "tu_index_registrations=%d\n",
                stats_tu_index_lookups, stats_tu_index_registrations);
    }
    fprintf(stderr,
            "stats phase=%s field_slots=%d param_slots=%d var_slots=%d\n",
            phase, stats_field_slot_count, stats_func_count * MAX_PARAMS,
            stats_var_count + stats_field_slot_count +
                stats_func_count * MAX_PARAMS);
    vir_stats_t totals = {0};
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        const vir_function_t *graph = vir_frontend_function(func);
        if (!graph)
            continue;
        vir_stats_t stats;
        vir_collect_stats(graph, &stats);
        totals.blocks += stats.blocks;
        totals.values += stats.values;
        totals.params += stats.params;
        totals.effects += stats.effects;
        totals.arena_capacity += stats.arena_capacity;
    }
    fprintf(stderr,
            "stats phase=%s vir_blocks=%d vir_values=%d vir_params=%d\n", phase,
            totals.blocks, totals.values, totals.params);
    fprintf(stderr, "stats phase=%s vir_effects=%d vir_arena=%d ph2=%d\n",
            phase, totals.effects, totals.arena_capacity, stats_ph2_ir_count);
    fprintf(stderr, "stats phase=%s arena_capacity block=%d bb=%d\n", phase,
            BLOCK_ARENA ? BLOCK_ARENA->total_bytes : 0,
            BB_ARENA ? BB_ARENA->total_bytes : 0);
    fprintf(stderr, "stats phase=%s arena_capacity token=%d general=%d\n",
            phase, TOKEN_ARENA ? TOKEN_ARENA->total_bytes : 0,
            GENERAL_ARENA ? GENERAL_ARENA->total_bytes : 0);
    fprintf(stderr, "stats phase=%s arena_capacity_peak block=%d bb=%d\n",
            phase, BLOCK_ARENA ? BLOCK_ARENA->peak_bytes : 0,
            BB_ARENA ? BB_ARENA->peak_bytes : 0);
    fprintf(
        stderr, "stats phase=%s arena_capacity_peak token=%d general=%d\n",
        phase,
        TOKEN_ARENA ? TOKEN_ARENA->peak_bytes : stats_retired_token_arena_peak,
        GENERAL_ARENA ? GENERAL_ARENA->peak_bytes : 0);
    fprintf(stderr, "stats phase=%s arena_total_capacity=%d\n", phase,
            stats_live_arena_capacity);
    fprintf(stderr, "stats phase=%s arena_total_capacity_peak=%d\n", phase,
            stats_peak_arena_capacity);
}

void dump_stats_layout(void)
{
    if (!dump_stats)
        return;

    fprintf(stderr, "stats layout var=%d bb=%d ph2=%d\n", (int) sizeof(var_t),
            (int) sizeof(basic_block_t), (int) sizeof(ph2_ir_t));
}

strbuf_t *strbuf_create(int init_capacity)
{
    strbuf_t *array = malloc(sizeof(strbuf_t));
    if (!array)
        return NULL;

    array->size = 0;
    array->capacity = init_capacity;
    array->plain_source = false;
    array->elements = malloc(array->capacity * sizeof(char));
    if (!array->elements) {
        free(array);
        return NULL;
    }

    return array;
}

bool strbuf_extend(strbuf_t *src, int len)
{
    int new_size = src->size + len;

    if (new_size < src->capacity)
        return true;

    if (new_size > (src->capacity << 1))
        src->capacity = new_size;
    else
        src->capacity <<= 1;

    char *new_arr = malloc(src->capacity * sizeof(char));

    if (!new_arr)
        return false;

    memcpy(new_arr, src->elements, src->size * sizeof(char));

    free(src->elements);
    src->elements = new_arr;

    return true;
}

bool strbuf_putc(strbuf_t *src, char value)
{
    /* Appending one byte is how the whole of the generated machine code and
     * every ELF header reaches memory, several hundred thousand times per
     * compile, and all but a handful of those have room already. Testing for
     * the room here keeps the call to the growth path off that route.
     */
    if (src->size + 1 >= src->capacity) {
        if (!strbuf_extend(src, 1))
            return false;
    }

    src->elements[src->size] = value;
    src->size++;

    return true;
}

bool strbuf_puts(strbuf_t *src, const char *value)
{
    int len = strlen(value);

    if (src->size + len >= src->capacity) {
        if (!strbuf_extend(src, len))
            return false;
    }

    strncpy(src->elements + src->size, value, len);
    src->size += len;

    return true;
}

void strbuf_free(strbuf_t *src)
{
    if (!src)
        return;

    free(src->elements);
    free(src);
}

/* This routine is required because the global variable initializations are not
 * supported now.
 */
void global_init(void)
{
    internal_linkage_name_idx = 0;
    FUNC_LIST.head = NULL;
    FUNC_LIST.tail = NULL;

    /* Initialize arenas first so we can use them for allocation */
    BLOCK_ARENA = arena_init(DEFAULT_ARENA_SIZE); /* Variables/blocks */
    BB_ARENA = arena_init(SMALL_ARENA_SIZE);      /* Basic blocks - low usage */
    TOKEN_ARENA = arena_init(LARGE_ARENA_SIZE);
    GENERAL_ARENA =
        arena_init(DEFAULT_ARENA_SIZE); /* For TYPES and flattened IR */

    /* Use arena allocation for better memory management */
    TYPES = arena_calloc(GENERAL_ARENA, MAX_TYPES, sizeof(type_t));
    PH2_IR_FLATTEN = NULL;
    ph2_ir_capacity = 0;
    ph2_ir_idx = 0;

    /* Initialize string pool for identifier deduplication */
    string_pool = arena_alloc(GENERAL_ARENA, sizeof(string_pool_t));
    string_pool->strings = hashmap_create(512);

    /* Initialize string literal pool for deduplicating string constants */
    string_literal_pool =
        arena_alloc(GENERAL_ARENA, sizeof(string_literal_pool_t));
    string_literal_pool->literals = hashmap_create(256);

    TOKEN_CACHE = hashmap_create(DEFAULT_SRC_FILE_COUNT);
    SRC_FILE_MAP = hashmap_create(DEFAULT_SRC_FILE_COUNT);
    FUNC_MAP = hashmap_create(DEFAULT_FUNCS_SIZE);

    LIBC_SRC = strbuf_create(4096);
    elf_code = strbuf_create(MAX_CODE);
    elf_data = strbuf_create(MAX_DATA);
    elf_rodata = strbuf_create(MAX_DATA);
    elf_header = strbuf_create(MAX_HEADER);
    elf_program_header = strbuf_create(MAX_PROGRAM_HEADER);
    elf_symtab = strbuf_create(MAX_SYMTAB);
    elf_strtab = strbuf_create(MAX_STRTAB);
    elf_bss_size = 0;
    elf_shstrtab = strbuf_create(MAX_SHSTR);
    elf_section_header = strbuf_create(MAX_SECTION_HEADER);

    switch (ELF_MACHINE) {
    case ELF_MACHINE_ARM32:
        dynamic_sections.use_relaplt = false;
        break;
    case ELF_MACHINE_RV32:
    case ELF_MACHINE_X86_64:
    case ELF_MACHINE_AARCH64:
        /* Every target but Arm32 uses RELA throughout. */
        dynamic_sections.use_relaplt = true;
        break;
    }
    dynamic_sections.elf_interp = strbuf_create(MAX_INTERP);
    dynamic_sections.elf_dynamic = strbuf_create(MAX_DYNAMIC);
    dynamic_sections.elf_dynsym = strbuf_create(MAX_DYNSYM);
    dynamic_sections.elf_dynstr = strbuf_create(MAX_DYNSTR);
    if (dynamic_sections.use_relaplt)
        dynamic_sections.elf_relaplt = strbuf_create(MAX_RELAPLT);
    else
        dynamic_sections.elf_relplt = strbuf_create(MAX_RELPLT);
    dynamic_sections.elf_plt = strbuf_create(MAX_PLT);
    dynamic_sections.elf_got = strbuf_create(MAX_GOTPLT);
}

/* Forward declaration for lexer cleanup */
void lexer_cleanup(void);

/* Free empty trailing blocks from an arena safely. This only frees blocks that
 * come after the last used block, ensuring no pointers are invalidated.
 *
 * NOTE: measured over a self-compile, this reclaims nothing. arena_alloc()
 * prepends each new block at the head, so the list runs newest-to-oldest and
 * every block behind the head is full by construction: last_used is always the
 * tail and there is never anything after it to free. The only case that ever
 * fires is an arena whose very first allocation was larger than its initial
 * block, leaving that block at offset 0 behind a newer one. Reclaiming a bump
 * allocator's memory needs a phase boundary that can drop a whole arena -- see
 * release_token_arena() -- not a scan for empty blocks.
 *
 * @arena: The arena to compact.
 * Return: Bytes freed.
 */
int arena_free_trailing_blocks(arena_t *arena)
{
    if (!arena || !arena->head)
        return 0;

    /* Find the last block with actual allocations */
    arena_block_t *last_used = NULL;
    arena_block_t *block;

    for (block = arena->head; block; block = block->next) {
        if (block->offset > 0)
            last_used = block;
    }

    /* If no blocks are used, keep just the head */
    if (!last_used)
        last_used = arena->head;

    /* Free all blocks after last_used */
    int freed = 0;
    if (last_used->next) {
        block = last_used->next;
        last_used->next = NULL;

        while (block) {
            arena_block_t *next = block->next;
            freed += block->capacity;
            arena->total_bytes -= block->capacity;
            stats_live_arena_capacity -= block->capacity;
            arena_block_free(block);
            block = next;
        }
    }

    return freed;
}

/* Release the whole token arena and the source buffers behind it.
 *
 * Every token, macro, hide set and conditional-inclusion record lives in
 * TOKEN_ARENA, and nothing survives parsing: identifiers and string literals
 * reach the parser through intern_string(), which copies into GENERAL_ARENA,
 * and every parser entry point copies a token's text into a local buffer before
 * storing it. So once parse() returns, all 17 MiB of it is garbage that would
 * otherwise stay resident through the memory peak in machine lowering.
 *
 * The source buffers in SRC_FILE_MAP exist only to quote a line in a parse
 * error, so they go at the same time.
 */
void release_token_arena(void)
{
    /* The lexer maps borrow pointers into TOKEN_ARENA, so destroy them before
     * releasing their values. Parsing is complete and they have no later
     * readers. global_release() may call this cleanup again safely.
     */
    lexer_cleanup();

    if (TOKEN_ARENA) {
        if (TOKEN_ARENA->peak_bytes > stats_retired_token_arena_peak)
            stats_retired_token_arena_peak = TOKEN_ARENA->peak_bytes;
        arena_free(TOKEN_ARENA);
        TOKEN_ARENA = NULL;
    }

    /* Every error_at() site is in the preprocessor or the parser, and both are
     * done by the time this runs, so no line will be quoted again. LIBC_SRC is
     * in the map as well, but the global owns it and global_release() frees it
     * there; freeing it here too would free it twice.
     */
    if (SRC_FILE_MAP) {
        for (int i = 0; i < SRC_FILE_MAP->cap; i++) {
            if (!SRC_FILE_MAP->table[i].occupied)
                continue;

            strbuf_t *src = SRC_FILE_MAP->table[i].val;
            if (src && src != LIBC_SRC)
                strbuf_free(src);
        }
        hashmap_free(SRC_FILE_MAP);
        SRC_FILE_MAP = NULL;
    }
}

static int compact_arenas(int phase_mask)
{
    int total_saved = 0;

    if (phase_mask & COMPACT_ARENA_BLOCK)
        total_saved += arena_free_trailing_blocks(BLOCK_ARENA);
    if (phase_mask & COMPACT_ARENA_BB)
        total_saved += arena_free_trailing_blocks(BB_ARENA);
    if (phase_mask & COMPACT_ARENA_GENERAL)
        total_saved += arena_free_trailing_blocks(GENERAL_ARENA);

    return total_saved;
}

/* Compact selected arenas for their compilation phase. */
int compact_arenas_selective(int phase_mask)
{
    return compact_arenas(phase_mask);
}

void global_release(void)
{
    /* Release token and source storage before the remaining compiler state. */
    release_token_arena();

    /* Free string interning hashmaps */
    if (string_pool && string_pool->strings)
        hashmap_free(string_pool->strings);
    if (string_literal_pool && string_literal_pool->literals)
        hashmap_free(string_literal_pool->literals);
    hashmap_free(TU_ORDINARY_MAP);
    TU_ORDINARY_MAP = NULL;

    arena_free(BLOCK_ARENA);
    arena_free(BB_ARENA);
    arena_free(GENERAL_ARENA); /* free TYPES */

    /* Every value in TOKEN_CACHE is one heap-allocated token_stream_t: the
     * tokens it spans come from TOKEN_ARENA, which is already gone, but the
     * header itself is malloc'd by the two gen_*_token_stream functions and
     * hashmap_free() releases only the table, never the values.
     */
    if (TOKEN_CACHE) {
        for (int i = 0; i < TOKEN_CACHE->cap; i++) {
            if (TOKEN_CACHE->table[i].occupied)
                free(TOKEN_CACHE->table[i].val);
        }
    }
    hashmap_free(TOKEN_CACHE);
    hashmap_free(FUNC_MAP);
    hashmap_free(INCLUSION_MAP);

    strbuf_t *buffers[] = {
        LIBC_SRC,
        elf_code,
        elf_data,
        elf_rodata,
        elf_header,
        elf_program_header,
        elf_symtab,
        elf_strtab,
        elf_shstrtab,
        elf_section_header,
        dynamic_sections.elf_interp,
        dynamic_sections.elf_dynamic,
        dynamic_sections.elf_dynsym,
        dynamic_sections.elf_dynstr,
        dynamic_sections.use_relaplt ? dynamic_sections.elf_relaplt
                                     : dynamic_sections.elf_relplt,
        dynamic_sections.elf_plt,
        dynamic_sections.elf_got,
    };
    for (size_t i = 0; i < sizeof(buffers) / sizeof(*buffers); i++)
        strbuf_free(buffers[i]);
    PH2_IR_FLATTEN = NULL;
    ph2_ir_idx = 0;
    ph2_ir_capacity = 0;
}

/* The function whose body the back half of the pipeline is working on, or NULL
 * before register allocation. An internal failure there carries no source
 * position, so naming the function is what points back at the input.
 */
char *fatal_function_context = NULL;

/* Reports a broken invariant, which has no position in the source to point at
 * because nothing in the source is necessarily wrong. This one abort()s: a core
 * dump is what makes an internal failure debuggable. A mistake in the input
 * belongs in error_at(), and a mistake on the command line in usage_error().
 */
__noreturn void fatal(const char *msg)
{
    if (fatal_function_context)
        printf("[Error]: %s (in function '%s')\n", msg, fatal_function_context);
    else
        printf("[Error]: %s\n", msg);

    /* abort() does not flush, so a diagnostic written to a pipe -- a build log,
     * or any invocation whose output is captured -- is discarded and the
     * compiler appears to die silently.
     *
     * The stream is NULL rather than stdout because a dynamically linked build
     * resolves fflush through the PLT to the host libc, where error output may
     * sit in any of its buffered streams. NULL means "every stream" there and
     * is ignored by the unbuffered embedded libc, so it is right for both. That
     * build needs the flush most, being the only one whose stdio actually
     * buffers.
     */
    fflush(NULL);
    abort();
}

/* Reports a mistake in how the compiler was invoked. A bad command line is not
 * a broken invariant, so this exits rather than abort()ing: no core dump, and
 * no "Aborted" line, for an ordinary typo.
 */
__noreturn void usage_error(const char *msg)
{
    printf("[Error]: %s\n", msg);
    fflush(NULL);
    exit(1);
}

/* Reports an input that exceeds one of the compiler's fixed limits, such as the
 * size of the type table or the predecessors of one basic block. The program
 * may be valid C, but the limit is not a broken invariant either, so it exits
 * through error_at() rather than taking fatal()'s core dump. The current token
 * is quoted while the parser still holds one; a limit reached after the token
 * arena is released has no line to point at.
 */
__noreturn void limit_error(char *msg)
{
    error_at(msg, cur_token && TOKEN_ARENA ? cur_token_loc() : NULL);
}

/* Reports a mistake in the input, quoting the line it sits on. A program the
 * compiler refuses is not a broken invariant, so this exits the way
 * usage_error() does rather than abort()ing: an ordinary syntax error should
 * not raise SIGABRT, wake the system crash handler, or leave a core behind.
 *
 * Falls back to the same message without context when the location is NULL or
 * the source file is no longer on hand.
 */
__noreturn void error_at(char *msg, source_location_t *loc)
{
    int offset, start_idx, i = 0, len, pos;
    char diagnostic[MAX_LINE_LEN];

    if (!loc) {
        printf("[Error]: %s\n", msg);
        fflush(NULL);
        exit(1);
    }

    len = loc->len;
    pos = loc->pos;

    strbuf_t *src = hashmap_get(SRC_FILE_MAP, loc->filename);

    /* The source text is no longer on hand, which changes what can be shown and
     * not what went wrong: still a mistake in the input, so still an exit
     * rather than the core dump fatal() would take.
     */
    if (!src) {
        printf("[Error]: %s\n", msg);
        fflush(NULL);
        exit(1);
    }

    if (len < 1)
        len = 1;

    printf("%s:%d:%d: [Error]: %s\n", loc->filename, loc->line, loc->column,
           msg);
    printf("%6d |  ", loc->line);

    /* Finds line's start position */
    for (offset = pos; offset >= 0 && src->elements[offset] != '\n'; offset--)
        ;

    start_idx = offset + 1;

    /* Copies whole line to diagnostic buffer. The source line, the caret column
     * and the underline length all come from the input, so every write here has
     * to stop at the end of the buffer.
     */
    for (offset = start_idx;
         offset < src->capacity && src->elements[offset] != '\n' &&
         src->elements[offset] != '\0' && i < MAX_LINE_LEN - 1;
         offset++) {
        diagnostic[i++] = src->elements[offset];
    }
    diagnostic[i] = '\0';

    printf("%s\n", diagnostic);
    printf("%6c |  ", ' ');

    /* Keep room for the note appended after the underline. */
    const char *note = " Error occurs here";
    int limit = MAX_LINE_LEN - strlen(note) - 1;

    i = 0;
    for (offset = start_idx; offset < pos && i < limit; offset++)
        diagnostic[i++] = ' ';
    if (i < limit)
        diagnostic[i++] = '^';
    for (; len > 1 && i < limit; len--)
        diagnostic[i++] = '~';

    strcpy(diagnostic + i, note);
    printf("%s\n", diagnostic);
    fflush(NULL); /* exit() flushes, but say so once rather than rely on it */
    exit(1);
}

int abi_arg_next(int cursor, const var_t *var, bool variadic)
{
    bool pair = PTR_SIZE == 4 && var && var->type && !var->ptr_level &&
                !var->array_size && !var->is_func &&
                var->type->base_type != TYPE_struct &&
                var->type->base_type != TYPE_union && var->type->size == 8;
    if (pair && (ELF_MACHINE != ELF_MACHINE_RV32 || variadic ||
                 cursor >= MAX_ARGS_IN_REG))
        cursor = ALIGN_UP(cursor, 2);
    return cursor + (pair ? 2 : 1);
}
