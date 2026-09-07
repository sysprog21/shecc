/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"

/* Forward declaration for string interning */
char *intern_string(char *str);

/* Lexer */
token_t *cur_token;
/* TOKEN_CACHE maps filename to the corresponding computed token stream */
hashmap_t *TOKEN_CACHE;
strbuf_t *LIBC_SRC;

/* Global objects */

hashmap_t *SRC_FILE_MAP;
hashmap_t *FUNC_MAP;
hashmap_t *CONSTANTS_MAP;

/* Types */

type_t *TYPES;
int types_idx = 0;

type_t *TY_void;
type_t *TY_char;
type_t *TY_bool;
type_t *TY_int;
type_t *TY_short;

/* Arenas */

arena_t *INSN_ARENA;

/* HASHMAP_ARENA is responsible for hashmap_node_t allocation */
arena_t *HASHMAP_ARENA;

/* BLOCK_ARENA is responsible for block_t / var_t allocation */
arena_t *BLOCK_ARENA;

/* BB_ARENA is responsible for basic_block_t / ph2_ir_t allocation */
arena_t *BB_ARENA;

/* TOKEN_ARENA is responsible for token_t (including literal) /
 * source_location_t allocation
 */
arena_t *TOKEN_ARENA;

/* GENERAL_ARENA is responsible for functions, symbols, constants, aliases,
 * macros, and traversal args
 */
arena_t *GENERAL_ARENA;

int bb_label_idx = 0;

ph2_ir_t **PH2_IR_FLATTEN;
int ph2_ir_idx = 0;

func_list_t FUNC_LIST;
func_t *GLOBAL_FUNC;
block_t *GLOBAL_BLOCK;
basic_block_t *MAIN_BB;
int elf_offset = 0;

regfile_t REGS[REG_CNT];

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

/* Command line compilation flags */
bool dynlink = false;
bool libc = true;
bool expand_only = false;
bool dump_ir = false;
bool dump_dot = false;
bool hard_mul_div = false;

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
    }

    void *ptr = arena->head->memory + arena->head->offset;
    arena->head->offset += size;
    return ptr;
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
    return arena_calloc(GENERAL_ARENA, 1, sizeof(func_t));
}

symbol_t *arena_alloc_symbol(void)
{
    return arena_calloc(GENERAL_ARENA, 1, sizeof(symbol_t));
}

constant_t *arena_alloc_constant(void)
{
    /* constant_t is simple, can avoid zeroing */
    constant_t *c = arena_alloc(GENERAL_ARENA, sizeof(constant_t));
    c->alias[0] = '\0';
    c->value = 0;
    return c;
}

bb_traversal_args_t *arena_alloc_traversal_args(void)
{
    /* Keep using calloc for safety */
    return arena_calloc(GENERAL_ARENA, 1, sizeof(bb_traversal_args_t));
}

void arena_free(arena_t *arena)
{
    arena_block_t *block = arena->head;
    arena_block_t *next;

    while (block) {
        next = block->next;
        arena_block_free(block);
        block = next;
    }

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
    map->table = calloc(map->cap, sizeof(hashmap_node_t));

    if (!map->table) {
        printf("Failed to allocate table in hashmap_t\n");
        free(map);
        return NULL;
    }

    return map;
}


void hashmap_rehash(hashmap_t *map)
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
            map->size++;
        }
    }
    free(old_table);
}

/* Put a key-value pair into given hashmap. If key already contains a value,
 * then replace it with new value, the old value will be freed.
 * @map: The hashmap to be put into. Must not be NULL.
 * @key: The key string. May be NULL.
 * @val: The value pointer. May be NULL. This value's lifetime is held by
 * hashmap.
 */
void hashmap_put(hashmap_t *map, char *key, void *val)
{
    if (!map)
        return;

    /* Check if size of map exceeds load factor 50% (or 1/2 of capacity) */
    if ((map->cap >> 1) <= map->size)
        hashmap_rehash(map);

    int index = hashmap_hash_index(map->cap, key);
    int start = index;

    while (map->table[index].occupied) {
        if (!strcmp(map->table[index].key, key)) {
            map->table[index].val = val;
            return;
        }

        index = (index + 1) & (map->cap - 1);
        if (index == start) {
            printf("Error: Hashmap is full\n");
            fflush(stdout); /* see fatal() */
            abort();
        }
    }

    map->table[index].key = arena_strdup(HASHMAP_ARENA, key);
    map->table[index].val = val;
    map->table[index].occupied = true;
    map->size++;
}

/* Get key-value pair node from hashmap from given key.
 * @map: The hashmap to be looked up. Must no be NULL.
 * @key: The key string. May be NULL.
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
 * @map: The hashmap to be looked up. Must no be NULL.
 * @key: The key string. May be NULL.
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
 * @map: The hashmap to be looked up. Must no be NULL.
 * @key: The key string. May be NULL.
 *
 * Return: The look up result, if the key-value pair entry exists, then returns
 * true, false otherwise.
 */
bool hashmap_contains(hashmap_t *map, char *key)
{
    return hashmap_get_node(map, key);
}

/* Free the hashmap, this also frees key-value pair entry's value.
 * @map: The hashmap to be looked up. Must no be NULL.
 */
void hashmap_free(hashmap_t *map)
{
    if (!map)
        return;

    free(map->table);
    free(map);
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
    char head = type_name[0];

    for (int i = 0; i < types_idx; i++) {
        if (TYPES[i].type_name[0] != head)
            continue;
        if (TYPES[i].base_type == TYPE_struct ||
            TYPES[i].base_type == TYPE_union) {
            if (flag == 1)
                continue;
            if (!strcmp(TYPES[i].type_name, type_name))
                return &TYPES[i];
        } else {
            if (flag == 2)
                continue;
            if (!strcmp(TYPES[i].type_name, type_name)) {
                /* If it is a forwardly declared alias of a structure, return
                 * the base structure type.
                 */
                if (TYPES[i].base_type == TYPE_typedef && TYPES[i].size == 0)
                    return TYPES[i].base_struct;
                return &TYPES[i];
            }
        }
    }
    return NULL;
}

ph2_ir_t *add_existed_ph2_ir(ph2_ir_t *ph2_ir)
{
    if (ph2_ir_idx >= MAX_IR_INSTR) {
        printf("Error: too many phase-2 IR instructions\n");
        fflush(stdout); /* see fatal() */
        abort();
    }
    PH2_IR_FLATTEN[ph2_ir_idx++] = ph2_ir;
    return ph2_ir;
}

ph2_ir_t *add_ph2_ir(opcode_t op)
{
    ph2_ir_t *ph2_ir = arena_alloc(BB_ARENA, sizeof(ph2_ir_t));
    ph2_ir->op = op;
    /* Initialize all fields explicitly */
    ph2_ir->next = NULL;
    ph2_ir->is_branch_detached = 0;
    ph2_ir->src0 = 0;
    ph2_ir->src1 = 0;

    /* Only a select names a third source, but the allocation is not zeroed and
     * every field is set here by hand.
     */
    ph2_ir->src2 = 0;
    ph2_ir->dest = 0;
    ph2_ir->func_name = NULL;
    ph2_ir->next_bb = NULL;
    ph2_ir->then_bb = NULL;
    ph2_ir->else_bb = NULL;
    ph2_ir->ofs_based_on_stack_top = false;

    /* Default to the full slot. Slots are PTR_SIZE wide, so a wide access is
     * always valid; only an address-taken narrow scalar may be written behind
     * the allocator's back, and reg-alloc narrows those explicitly.
     */
    ph2_ir->size_bytes = PTR_SIZE;
    ph2_ir->is_pointer = false;
    return add_existed_ph2_ir(ph2_ir);
}

block_t *add_block(block_t *parent, func_t *func)
{
    block_t *blk = arena_alloc(BLOCK_ARENA, sizeof(block_t));

    /* Initialize all fields explicitly */
    blk->locals.size = 0;
    blk->locals.capacity = 16;
    blk->locals.elements =
        arena_alloc(BLOCK_ARENA, blk->locals.capacity * sizeof(var_t *));
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

    hashmap_put(string_pool->strings, interned, interned);

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

int unescape_string(const char *input, char *output, int output_size)
{
    if (!input || !output || output_size == 0)
        return -1;

    int i = 0, j = 0;

    while (input[i] != '\0' && j < output_size - 1) {
        if (input[i] != '\\') {
            /* Regular characters */
            output[j++] = input[i++];
            continue;
        }

        i++;

        switch (input[i]) {
        case 'a':
            output[j++] = '\a';
            i++;
            break;
        case 'b':
            output[j++] = '\b';
            i++;
            break;
        case 'f':
            output[j++] = '\f';
            i++;
            break;
        case 'e':
            output[j++] = 27;
            i++;
            break;
        case 'n':
            output[j++] = '\n';
            i++;
            break;
        case 'r':
            output[j++] = '\r';
            i++;
            break;
        case 't':
            output[j++] = '\t';
            i++;
            break;
        case 'v':
            output[j++] = '\v';
            i++;
            break;
        case '\\':
            output[j++] = '\\';
            i++;
            break;
        case '\'':
            output[j++] = '\'';
            i++;
            break;
        case '"':
            output[j++] = '"';
            i++;
            break;
        case '?':
            output[j++] = '\?';
            i++;
            break;
        case 'x': {
            /* Hexadecimal escape sequence: \xhh */
            i++; /* Skips 'x' */

            if (!isxdigit(input[i])) {
                /* Terminate before bailing: callers read output[0], and an
                 * unterminated buffer left them reading stack garbage.
                 */
                output[j] = '\0';
                return -1;
            }

            int value = 0;
            int count = 0;

            while (isxdigit(input[i]) && count < 2) {
                value = (value << 4) + hex_digit_value(input[i]);
                i++;
                count++;
            }

            output[j++] = (char) value;
            break;
        }
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            /* Octal escape sequence: \ooo (up to 3 digits) */
            int value = 0;
            int digit_count = 0;

            while (input[i] >= '0' && input[i] <= '7' && digit_count < 3) {
                value = value * 8 + (input[i] - '0');
                i++;
                digit_count++;
            }

            output[j++] = (char) value;
            break;
        }
        default:
            /* Unknown escape sequence - treat as literal character */
            output[j++] = input[i++];
            break;
        }
    }

    output[j] = '\0';

    /* Check if we ran out of output space */
    if (input[i] != '\0')
        return -1;

    return j;
}

int parse_numeric_constant(const char *buffer)
{
    int i = 0;
    int value = 0;
    while (buffer[i]) {
        if (i == 1 && (buffer[i] | 32) == 'x') { /* hexadecimal */
            value = 0;
            i = 2;
            while (buffer[i]) {
                char c = buffer[i++];
                value <<= 4;
                if (isdigit(c))
                    value += c - '0';
                c |= 32; /* convert to lower case */
                if (c >= 'a' && c <= 'f')
                    value += (c - 'a') + 10;
            }
            return value;
        }
        if (i == 1 && (buffer[i] | 32) == 'b') { /* binary */
            value = 0;
            i = 2;
            while (buffer[i]) {
                char c = buffer[i++];
                value <<= 1;
                value += (c == '1');
            }
            return value;
        }
        if (buffer[0] == '0') /* octal */
            value = value * 8 + buffer[i++] - '0';
        else
            value = value * 10 + buffer[i++] - '0';
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

    /* The field variables come out of a zeroed allocation, so give their
     * interned name pointers the empty string a reader can dereference.
     */
    for (int i = 0; i < MAX_FIELDS; i++)
        type->fields[i].var_name = "";
}

type_t *add_type(void)
{
    if (types_idx >= MAX_TYPES) {
        printf("Error: Maximum number of types (%d) exceeded\n", MAX_TYPES);
        fflush(stdout); /* see fatal() */
        abort();
    }
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

void add_constant(char alias[], int value)
{
    constant_t *constant = arena_alloc_constant();
    if (!constant) {
        printf("Failed to allocate constant_t\n");
        return;
    }

    /* Use interned string for constant name */
    strcpy(constant->alias, intern_string(alias));
    constant->value = value;
    hashmap_put(CONSTANTS_MAP, alias, constant);
}

constant_t *find_constant(char alias[])
{
    return hashmap_get(CONSTANTS_MAP, alias);
}

var_t *find_member(const char token[], type_t *type)
{
    /* If it is a forwardly declared alias of a structure, switch to the base
     * structure type. A scalar -- or "void", whose size is also 0 -- has no
     * base to switch to, and following the NULL was a SIGSEGV.
     */
    if (type->size == 0)
        type = type->base_struct;
    if (!type)
        return NULL;

    char head = token[0];

    for (int i = 0; i < type->num_fields; i++) {
        if (type->fields[i].var_name[0] != head)
            continue;
        if (!strcmp(type->fields[i].var_name, token))
            return &type->fields[i];
    }
    return NULL;
}

/* Name lookup is the parser's inner loop: every identifier walks the enclosing
 * scopes, then the parameter list, then the globals, and all but the one match
 * is a strcmp against a name that differs immediately. A call into the C
 * library's vectorized strcmp costs far more than the comparison it performs on
 * such names, so each scan settles the common case -- a different first letter
 * -- before making the call. Names are never empty, so reading the first byte
 * of either side is always in bounds.
 */
var_t *find_local_var(const char *token, block_t *block)
{
    func_t *func = block->func;
    char head = token[0];

    for (; block; block = block->parent) {
        var_list_t *var_list = &block->locals;
        for (int i = 0; i < var_list->size; i++) {
            var_t *var = var_list->elements[i];
            if (var->var_name[0] != head)
                continue;
            if (!strcmp(var->var_name, token))
                return var;
        }
    }

    if (func) {
        for (int i = 0; i < func->num_params; i++) {
            var_t *param = &func->param_defs[i];
            if (param->var_name[0] != head)
                continue;
            if (!strcmp(param->var_name, token))
                return param;
        }
    }
    return NULL;
}

var_t *find_global_var(const char *token)
{
    var_list_t *var_list = &GLOBAL_BLOCK->locals;
    char head = token[0];

    for (int i = 0; i < var_list->size; i++) {
        var_t *var = var_list->elements[i];
        if (var->var_name[0] != head)
            continue;
        if (!strcmp(var->var_name, token))
            return var;
    }
    return NULL;
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
    func_t *func = hashmap_get(FUNC_MAP, func_name);

    if (func)
        return func;

    func = arena_alloc_func();
    hashmap_put(FUNC_MAP, func_name, func);
    for (int i = 0; i < MAX_PARAMS; i++)
        func->param_defs[i].var_name = "";
    /* Use interned string for function name */
    func->return_def.var_name = intern_string(func_name);

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

/* Create a basic block and set the scope of variables to 'parent' block */
basic_block_t *bb_create(block_t *parent)
{
    /* Use arena_calloc for basic_block_t as it has many arrays that need
     * zeroing (live_gen, live_kill, live_in, live_out, DF, RDF, dom_next, etc.)
     * This is simpler and safer than manually initializing everything.
     */
    basic_block_t *bb = arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));

    /* Initialize non-zero fields */
    bb->scope = parent;
    bb->belong_to = parent->func;

    /* -1 marks "no machine code emitted for this block yet". Backends assign a
     * real offset as they emit; 0 is a legitimate offset, so it cannot double
     * as the sentinel.
     */
    bb->elf_offset = -1;

    if (dump_ir || dump_dot) {
        /* MAX_VAR_LEN spent 128 bytes on a string that is always ".label." plus
         * an int. A self-compile calls bb_create() 52k times, so that was 6.4
         * MiB of arena where 1.2 MiB does.
         */
        bb->bb_label_name = arena_alloc(GENERAL_ARENA, MAX_LABEL_LEN);
        snprintf(bb->bb_label_name, MAX_LABEL_LEN, ".label.%d", bb_label_idx++);
    }

    return bb;
}

/* Bumped once per compute_live_in() call; a variable belongs to the set being
 * built when its stamp equals the current value.
 */
int liveness_gen;

/* Bumped once per recompute_live_out() call, stamping the union of successor
 * live_in sets as it is assembled.
 */
int live_merge_gen;

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
    int new_cap = *cap ? *cap << 1 : first;
    if (limit && new_cap > limit)
        fatal(what);
    void *grown = arena_realloc(arena, ptr, *cap * elem_sz, new_cap * elem_sz);
    *cap = new_cap;
    return grown;
}

/* Record another SSA version of @v, growing the version array as needed. */
void var_add_subscript(var_t *v, var_t *sub)
{
    if (v->subscripts_idx >= v->subscripts_cap)
        v->subscripts =
            arena_grow(BLOCK_ARENA, (char *) v->subscripts, &v->subscripts_cap,
                       sizeof(var_t *), 4, 0, NULL);
    v->subscripts[v->subscripts_idx++] = sub;
}

/* The first SSA version of @v, or NULL when it has none. Callers relied on the
 * old array being zero-filled to get NULL here.
 */
var_t *var_subscript0(var_t *v)
{
    if (!v->subscripts_idx)
        return NULL;
    return v->subscripts[0];
}

/* Detach @v from any version array it inherited from a by-value copy. */
void var_reset_subscripts(var_t *v)
{
    /* memcpy'd from a base, so the copy would otherwise alias the base's
     * renaming state as well as its version list.
     */
    v->rename = NULL;
    v->subscripts = NULL;
    v->subscripts_idx = 0;
    v->subscripts_cap = 0;
}

/* Append to a basic block's dominance frontier, growing the array as needed.
 * The array starts unallocated, so a block that never contributes to a frontier
 * costs nothing beyond the pointer.
 */
void bb_add_df(basic_block_t *bb, basic_block_t *df)
{
    if (bb->df_idx >= bb->df_cap)
        bb->DF = arena_grow(BB_ARENA, (char *) bb->DF, &bb->df_cap,
                            sizeof(basic_block_t *), 8, 0, NULL);
    bb->DF[bb->df_idx++] = df;
}

/* The reverse-dominance-frontier counterpart of bb_add_df(). */
void bb_add_rdf(basic_block_t *bb, basic_block_t *rdf)
{
    if (bb->rdf_idx >= bb->rdf_cap)
        bb->RDF = arena_grow(BB_ARENA, (char *) bb->RDF, &bb->rdf_cap,
                             sizeof(basic_block_t *), 8, 0, NULL);
    bb->RDF[bb->rdf_idx++] = rdf;
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
}

/* The pred-succ pair must have only one connection */
void bb_disconnect(basic_block_t *pred, basic_block_t *succ)
{
    for (int i = 0; i < succ->prev_idx; i++) {
        if (succ->prev[i].bb == pred) {
            switch (succ->prev[i].type) {
            case NEXT:
                pred->next = NULL;
                break;
            case THEN:
                pred->then_ = NULL;
                break;
            case ELSE:
                pred->else_ = NULL;
                break;
            default:
                abort();
            }

            succ->prev[i].bb = NULL;
            break;
        }
    }
}

/* Count the predecessors still wired to 'bb'. bb_disconnect() leaves holes in
 * prev[], so prev_idx is only a high-water mark and the entries must be counted
 * rather than trusted.
 */
int bb_pred_count(const basic_block_t *bb)
{
    int n = 0;

    for (int i = 0; i < bb->prev_idx; i++) {
        if (bb->prev[i].bb)
            n++;
    }
    return n;
}

/* The symbol is an argument of function or the variable in declaration */
void add_symbol(basic_block_t *bb, var_t *var)
{
    if (!bb)
        return;
    symbol_t *sym;
    for (sym = bb->symbol_list.head; sym; sym = sym->next) {
        if (sym->var == var)
            return;
    }

    sym = arena_alloc_symbol();
    sym->var = var;

    if (!bb->symbol_list.head) {
        sym->index = 0;
        bb->symbol_list.head = sym;
        bb->symbol_list.tail = sym;
    } else {
        sym->index = bb->symbol_list.tail->index + 1;
        bb->symbol_list.tail->next = sym;
        bb->symbol_list.tail = sym;
    }
}

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

    insn_t *n = arena_alloc(INSN_ARENA, sizeof(insn_t));
    n->next = NULL;
    n->prev = NULL;
    n->opcode = op;
    n->rd = rd;
    n->rs1 = rs1;
    n->rs2 = rs2;

    /* Only a select names a third source. The allocation is not zeroed and
     * every field is set here by hand, so this one has to be too.
     */
    n->rs3 = NULL;
    n->sz = sz;
    n->useful = false;
    n->belong_to = bb;
    n->phi_ops = NULL;
    n->idx = 0;

    n->str = str ? intern_string(str) : NULL;

    /* Mark variables as address-taken to prevent incorrect constant
     * optimization
     */
    if ((op == OP_address_of || op == OP_global_address_of) && rs1) {
        rs1->address_taken = true;
        rs1->is_const = false; /* disable constant optimization */
    }

    if (!bb->insn_list.head)
        bb->insn_list.head = n;
    else
        bb->insn_list.tail->next = n;

    n->prev = bb->insn_list.tail;
    bb->insn_list.tail = n;
}

strbuf_t *strbuf_create(int init_capacity)
{
    strbuf_t *array = malloc(sizeof(strbuf_t));
    if (!array)
        return NULL;

    array->size = 0;
    array->capacity = init_capacity;
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
    FUNC_LIST.head = NULL;
    FUNC_LIST.tail = NULL;
    memset(REGS, 0, sizeof(regfile_t) * REG_CNT);

    /* Initialize arenas first so we can use them for allocation */
    BLOCK_ARENA = arena_init(DEFAULT_ARENA_SIZE); /* Variables/blocks */
    INSN_ARENA = arena_init(LARGE_ARENA_SIZE); /* Instructions - high usage */
    BB_ARENA = arena_init(SMALL_ARENA_SIZE);   /* Basic blocks - low usage */
    HASHMAP_ARENA = arena_init(DEFAULT_ARENA_SIZE); /* Hash nodes */
    TOKEN_ARENA = arena_init(LARGE_ARENA_SIZE);
    GENERAL_ARENA =
        arena_init(DEFAULT_ARENA_SIZE); /* For TYPES and PH2_IR_FLATTEN */

    /* Use arena allocation for better memory management */
    TYPES = arena_calloc(GENERAL_ARENA, MAX_TYPES, sizeof(type_t));
    PH2_IR_FLATTEN =
        arena_alloc(GENERAL_ARENA, MAX_IR_INSTR * sizeof(ph2_ir_t *));

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
    CONSTANTS_MAP = hashmap_create(MAX_CONSTANTS);

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
        dynamic_sections.use_relaplt = true;
        break;
    case ELF_MACHINE_X86_64:
        /* x86-64 uses RELA throughout. */
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
 * otherwise stay resident through the memory peak in reg_alloc().
 *
 * The source buffers in SRC_FILE_MAP exist only to quote a line in a parse
 * error, so they go at the same time.
 */
void release_token_arena(void)
{
    if (TOKEN_ARENA) {
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

/* Compact all arenas to reduce memory usage after compilation phases. This
 * safely frees only trailing empty blocks without invalidating pointers.
 *
 * Return: Total bytes freed across all arenas.
 */
int compact_all_arenas(void)
{
    int total_saved = 0;

    /* Free trailing blocks from each arena */
    total_saved += arena_free_trailing_blocks(BLOCK_ARENA);
    total_saved += arena_free_trailing_blocks(INSN_ARENA);
    total_saved += arena_free_trailing_blocks(BB_ARENA);
    total_saved += arena_free_trailing_blocks(HASHMAP_ARENA);
    total_saved += arena_free_trailing_blocks(GENERAL_ARENA);

    return total_saved;
}

/* Compact specific arenas based on compilation phase. Different phases have
 * different memory usage patterns.
 *
 * @phase_mask: Bitmask using COMPACT_ARENA_* defines
 *              to indicate which arenas to compact.
 *
 * Return: Total bytes freed.
 */
int compact_arenas_selective(int phase_mask)
{
    int total_saved = 0;

    if (phase_mask & COMPACT_ARENA_BLOCK)
        total_saved += arena_free_trailing_blocks(BLOCK_ARENA);

    if (phase_mask & COMPACT_ARENA_INSN)
        total_saved += arena_free_trailing_blocks(INSN_ARENA);

    if (phase_mask & COMPACT_ARENA_BB)
        total_saved += arena_free_trailing_blocks(BB_ARENA);

    if (phase_mask & COMPACT_ARENA_HASHMAP)
        total_saved += arena_free_trailing_blocks(HASHMAP_ARENA);

    if (phase_mask & COMPACT_ARENA_GENERAL)
        total_saved += arena_free_trailing_blocks(GENERAL_ARENA);

    return total_saved;
}

void global_release(void)
{
    /* Cleanup lexer hashmaps */
    lexer_cleanup();

    /* Free string interning hashmaps */
    if (string_pool && string_pool->strings)
        hashmap_free(string_pool->strings);
    if (string_literal_pool && string_literal_pool->literals)
        hashmap_free(string_literal_pool->literals);

    arena_free(BLOCK_ARENA);
    arena_free(INSN_ARENA);
    arena_free(BB_ARENA);
    arena_free(HASHMAP_ARENA);
    if (TOKEN_ARENA)
        arena_free(TOKEN_ARENA);
    arena_free(GENERAL_ARENA); /* free TYPES and PH2_IR_FLATTEN */
    hashmap_free(TOKEN_CACHE);
    hashmap_free(SRC_FILE_MAP);
    hashmap_free(FUNC_MAP);
    hashmap_free(INCLUSION_MAP);
    hashmap_free(CONSTANTS_MAP);

    strbuf_free(LIBC_SRC);
    strbuf_free(elf_code);
    strbuf_free(elf_data);
    strbuf_free(elf_rodata);
    strbuf_free(elf_header);
    strbuf_free(elf_program_header);
    strbuf_free(elf_symtab);
    strbuf_free(elf_strtab);
    strbuf_free(elf_shstrtab);
    strbuf_free(elf_section_header);
    strbuf_free(dynamic_sections.elf_interp);
    strbuf_free(dynamic_sections.elf_dynamic);
    strbuf_free(dynamic_sections.elf_dynsym);
    strbuf_free(dynamic_sections.elf_dynstr);
    if (dynamic_sections.use_relaplt)
        strbuf_free(dynamic_sections.elf_relaplt);
    else
        strbuf_free(dynamic_sections.elf_relplt);
    strbuf_free(dynamic_sections.elf_plt);
    strbuf_free(dynamic_sections.elf_got);
}

/* Reports a broken invariant, which has no position in the source to point at
 * because nothing in the source is necessarily wrong. This one abort()s: a core
 * dump is what makes an internal failure debuggable. A mistake in the input
 * belongs in error_at(), and a mistake on the command line in usage_error().
 */
__noreturn void fatal(const char *msg)
{
    printf("[Error]: %s\n", msg);

    /* abort() does not flush, so a diagnostic written to a pipe -- a build log,
     * or any invocation whose output is captured -- is discarded and the
     * compiler appears to die silently.
     */
    fflush(stdout);
    abort();
}

/* Reports a mistake in how the compiler was invoked. A bad command line is not
 * a broken invariant, so this exits rather than abort()ing: no core dump, and
 * no "Aborted" line, for an ordinary typo.
 */
__noreturn void usage_error(const char *msg)
{
    printf("[Error]: %s\n", msg);
    fflush(stdout);
    exit(1);
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
        fflush(stdout);
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
        fflush(stdout);
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
    fflush(stdout); /* exit() flushes, but say so once rather than rely on it */
    exit(1);
}

void print_indent(int indent)
{
    for (int i = 0; i < indent; i++)
        printf("\t");
}

void dump_bb_insn(const func_t *func,
                  const basic_block_t *bb,
                  bool *at_func_start)
{
    if (!bb)
        return;
    const var_t *rd, *rs1, *rs2;

    if (bb != func->bbs && bb->insn_list.head) {
        if (!at_func_start[0])
            printf("%s:\n", bb->bb_label_name);
        else
            at_func_start[0] = false;
    }

    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        rd = insn->rd;
        rs1 = insn->rs1;
        rs2 = insn->rs2;

        switch (insn->opcode) {
        case OP_unwound_phi:
            /* Ignored */
            continue;
        case OP_allocat:
            print_indent(1);
            printf("allocat %s", rd->type->type_name);

            for (int i = 0; i < rd->ptr_level; i++)
                printf("*");

            printf(" %%%s", rd->var_name);

            if (rd->array_size > 0)
                printf("[%d]", rd->array_size);

            break;
        case OP_load_constant:
            print_indent(1);
            printf("const %%%s, %d", rd->var_name, rd->init_val);
            break;
        case OP_load_data_address:
            print_indent(1);
            /* offset from .data section */
            printf("%%%s = .data (%d)", rd->var_name, rd->init_val);
            break;
        case OP_load_rodata_address:
            print_indent(1);
            /* offset from .rodata section */
            printf("%%%s = .rodata (%d)", rd->var_name, rd->init_val);
            break;
        case OP_address_of:
            print_indent(1);
            printf("%%%s = &(%%%s)", rd->var_name, rs1->var_name);
            break;
        case OP_assign:
            print_indent(1);
            printf("%%%s = %%%s", rd->var_name, rs1->var_name);
            break;
        case OP_branch:
            print_indent(1);
            printf("br %%%s, %s, %s", rs1->var_name, bb->then_->bb_label_name,
                   bb->else_->bb_label_name);
            break;
        case OP_jump:
            print_indent(1);
            printf("jmp %s", bb->next->bb_label_name);
            break;
        case OP_label:
            print_indent(0);
            printf("%s:", insn->str);
            break;
        case OP_push:
            print_indent(1);
            printf("push %%%s", rs1->var_name);
            break;
        case OP_call:
            print_indent(1);
            printf("call @%s", insn->str);
            break;
        case OP_func_ret:
            print_indent(1);
            printf("retval %%%s", rd->var_name);
            break;
        case OP_return:
            print_indent(1);
            if (rs1)
                printf("ret %%%s", rs1->var_name);
            else
                printf("ret");
            break;
        case OP_read:
            print_indent(1);
            printf("%%%s = (%%%s), %d", rd->var_name, rs1->var_name, insn->sz);
            break;
        case OP_write:
            print_indent(1);
            if (rs1->is_func)
                printf("(%%%s) = @%s", rs1->var_name, rs2->var_name);
            else
                printf("(%%%s) = %%%s, %d", rs1->var_name, rs2->var_name,
                       insn->sz);
            break;
        case OP_indirect:
            print_indent(1);
            printf("indirect call @(%%%s)", rs1->var_name);
            break;
        case OP_negate:
            print_indent(1);
            printf("neg %%%s, %%%s", rd->var_name, rs1->var_name);
            break;
        case OP_add:
            print_indent(1);
            printf("%%%s = add %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_sub:
            print_indent(1);
            printf("%%%s = sub %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_mul:
            print_indent(1);
            printf("%%%s = mul %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_div:
            print_indent(1);
            printf("%%%s = div %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_mod:
            print_indent(1);
            printf("%%%s = mod %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_eq:
            print_indent(1);
            printf("%%%s = eq %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_neq:
            print_indent(1);
            printf("%%%s = neq %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_gt:
            print_indent(1);
            printf("%%%s = gt %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_lt:
            print_indent(1);
            printf("%%%s = lt %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_geq:
            print_indent(1);
            printf("%%%s = geq %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_leq:
            print_indent(1);
            printf("%%%s = leq %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_bit_and:
            print_indent(1);
            printf("%%%s = and %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_bit_or:
            print_indent(1);
            printf("%%%s = or %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_bit_not:
            print_indent(1);
            printf("%%%s = not %%%s", rd->var_name, rs1->var_name);
            break;
        case OP_bit_xor:
            print_indent(1);
            printf("%%%s = xor %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_log_and:
            print_indent(1);
            printf("%%%s = and %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_log_or:
            print_indent(1);
            printf("%%%s = or %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_log_not:
            print_indent(1);
            printf("%%%s = not %%%s", rd->var_name, rs1->var_name);
            break;
        case OP_rshift:
            print_indent(1);
            printf("%%%s = rshift %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_lshift:
            print_indent(1);
            printf("%%%s = lshift %%%s, %%%s", rd->var_name, rs1->var_name,
                   rs2->var_name);
            break;
        case OP_trunc:
            print_indent(1);
            printf("%%%s = trunc %%%s, %d", rd->var_name, rs1->var_name,
                   insn->sz);
            break;
        case OP_sign_ext:
            print_indent(1);
            printf("%%%s = sign_ext %%%s, %d", rd->var_name, rs1->var_name,
                   insn->sz);
            break;
        case OP_cast:
            print_indent(1);
            printf("%%%s = cast %%%s", rd->var_name, rs1->var_name);
            break;
        default:
            printf("<Unsupported opcode: %d>", insn->opcode);
            break;
        }

        printf("\n");
    }
}

void dump_bb_insn_by_dom(func_t *func, basic_block_t *bb, bool *at_func_start)
{
    dump_bb_insn(func, bb, at_func_start);
    for (int i = 0; bb && i < bb->dom_next_idx; i++)
        dump_bb_insn_by_dom(func, bb->dom_next[i], at_func_start);
}

void dump_insn(void)
{
    printf("==<START OF INSN DUMP>==\n");

    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        bool at_func_start = true;

        printf("def %s", func->return_def.type->type_name);

        for (int i = 0; i < func->return_def.ptr_level; i++)
            printf("*");
        printf(" @%s(", func->return_def.var_name);

        for (int i = 0; i < func->num_params; i++) {
            if (i != 0)
                printf(", ");
            printf("%s", func->param_defs[i].type->type_name);

            for (int k = 0; k < func->param_defs[i].ptr_level; k++)
                printf("*");
            printf(" %%%s", func->param_defs[i].var_name);
        }
        printf(") {\n");

        dump_bb_insn_by_dom(func, func->bbs, &at_func_start);

        /* Handle implicit return */
        for (int i = 0; func->exit && i < func->exit->prev_idx; i++) {
            const basic_block_t *bb = func->exit->prev[i].bb;
            if (!bb)
                continue;

            if (func->return_def.type != TY_void)
                continue;

            if (bb->insn_list.tail)
                if (bb->insn_list.tail->opcode == OP_return)
                    continue;

            print_indent(1);
            printf("ret\n");
        }

        printf("}\n");
    }

    printf("==<END OF INSN DUMP>==\n");
}
