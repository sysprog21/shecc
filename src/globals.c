/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"

/* Forward declaration for string interning */
char *intern_string(char *str);

/* Lexer */
char token_str[MAX_TOKEN_LEN];
token_t next_token;
char next_char;
bool skip_newline = true;

/* Token memory management */
token_pool_t *TOKEN_POOL;
token_buffer_t *TOKEN_BUFFER;
source_location_t current_location; /* Will be initialized at runtime */

bool preproc_match;

/* Point to the first character after where the macro has been called. It is
 * needed when returning from the macro body.
 */
int macro_return_idx;

/* Global objects */

/* FUNC_MAP is used to integrate function storing and boost lookup
 * performance, currently it uses FNV-1a hash function to hash function
 * name.
 */
hashmap_t *MACROS_MAP;
hashmap_t *FUNC_MAP;
hashmap_t *ALIASES_MAP;
hashmap_t *CONSTANTS_MAP;

/* Types */

type_t *TYPES;
int types_idx = 0;

type_t *TY_void;
type_t *TY_char;
type_t *TY_bool;
type_t *TY_int;

/* Arenas */

arena_t *INSN_ARENA;

/* HASHMAP_ARENA is responsible for hashmap_node_t allocation */
arena_t *HASHMAP_ARENA;

/* BLOCK_ARENA is responsible for block_t / var_t allocation */
arena_t *BLOCK_ARENA;

/* BB_ARENA is responsible for basic_block_t / ph2_ir_t allocation */
arena_t *BB_ARENA;

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

strbuf_t *SOURCE;

hashmap_t *INCLUSION_MAP;

/* ELF sections */
strbuf_t *elf_code;
strbuf_t *elf_data;
strbuf_t *elf_rodata;
strbuf_t *elf_header;
strbuf_t *elf_symtab;
strbuf_t *elf_strtab;
strbuf_t *elf_section;
int elf_header_len = 0x54; /* ELF fixed: 0x34 + 1 * 0x20 */
int elf_code_start;
int elf_data_start;
int elf_rodata_start;
int elf_bss_start;
int elf_bss_size;

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
        abort();
    }

    block->memory = malloc(capacity * sizeof(char));

    if (!block->memory) {
        printf("Failed to allocate memory for arena block buffer\n");
        free(block);
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
        abort();
    }
    arena->head = arena_block_create(initial_capacity);
    arena->total_bytes = initial_capacity;
    /* Use the initial capacity as the default block size for future growth. */
    arena->block_size = initial_capacity;
    return arena;
}

/* Allocate memory from the given arena with given size.
 * The arena may create a new arena block if no space is available.
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
        abort();
    }

    /* Align to sizeof(void*) bytes for host compatibility */
    const int alignment = sizeof(void *);
    size = (size + alignment - 1) & ~(alignment - 1);

    if (!arena->head || arena->head->offset + size > arena->head->capacity) {
        /* Need a new block: choose capacity = max(DEFAULT_ARENA_SIZE,
         * arena->block_size, size) */
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
 * @n:     Number of elements.
 * @size:  Size of each element in bytes.
 *
 * Internally calls arena_alloc(n * size) and then fills the entire region with
 * zero bytes.
 *
 * Return: Pointer to zero-initialized memory.
 */
void *arena_calloc(arena_t *arena, int n, int size)
{
    if (n * size == 0) {
        printf("arena_calloc: cannot allocate 0 bytes\n");
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
    if (oldptr == NULL) {
        if (oldsz != 0) {
            printf("arena_realloc: oldptr == NULL requires oldsz == 0\n");
            abort();
        }
        return arena_alloc(arena, newsz);
    }
    if (oldsz == 0) {
        printf("arena_realloc: oldptr != NULL requires oldsz > 0\n");
        abort();
    }

    /* return oldptr immediately */
    if (newsz <= oldsz) {
        return oldptr;
    }

    /* From here on, oldptr != NULL and newsz > oldsz and oldsz != 0 */
    int delta = newsz - oldsz;
    arena_block_t *blk = arena->head;
    char *block_end = blk->memory + blk->offset;

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
char *arena_strdup(arena_t *arena, char *str)
{
    const int n = strlen(str);
    char *dup = arena_alloc(arena, n + 1);
    memcpy(dup, str, n);
    dup[n] = '\0';
    return dup;
}

/* Duplicate a block of memory into the arena.
 * Allocates size bytes within the arena and copies data from the input pointer.
 *
 * @arena: a Pointer to the arena. Must not be NULL.
 * @data: data Pointer to the source memory. Must not be NULL.
 * @size: size Number of bytes to copy. Must be non-negative.
 *
 * Return: The pointer to the duplicated memory stored in the arena.
 */
void *arena_memdup(arena_t *arena, void *data, int size)
{
    return memcpy(arena_alloc(arena, size), data, size);
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

alias_t *arena_alloc_alias(void)
{
    /* alias_t is simple, can avoid zeroing */
    alias_t *a = arena_alloc(GENERAL_ARENA, sizeof(alias_t));
    a->alias[0] = '\0';
    a->value[0] = '\0';
    a->disabled = false;
    return a;
}

macro_t *arena_alloc_macro(void)
{
    return arena_calloc(GENERAL_ARENA, 1, sizeof(macro_t));
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

/* Hash a string with FNV-1a hash function
 * and converts into usable hashmap index. The range of returned
 * hashmap index is ranged from "(0 ~ 2,147,483,647) mod size" due to
 * lack of unsigned integer implementation.
 * @size: The size of map. Must not be negative or 0.
 * @key: The key string. May be NULL.
 *
 * Return: The usable hashmap index.
 */
int hashmap_hash_index(int size, char *key)
{
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

/* Create a hashmap on heap. Notice that provided size will always be rounded
 * up to nearest power of 2.
 * @size: The initial bucket size of hashmap. Must not be 0 or
 * negative.
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
    map->buckets = calloc(map->cap, sizeof(hashmap_node_t *));

    if (!map->buckets) {
        printf("Failed to allocate buckets in hashmap_t\n");
        free(map);
        return NULL;
    }

    return map;
}

/* Create a hashmap node on heap.
 * @key: The key of node. Must not be NULL.
 * @val: The value of node. Could be NULL.
 *
 * Return: The pointer of created node.
 */
hashmap_node_t *hashmap_node_new(char *key, void *val)
{
    if (!key)
        return NULL;

    const int len = strlen(key);
    hashmap_node_t *node = arena_alloc(HASHMAP_ARENA, sizeof(hashmap_node_t));


    if (!node) {
        printf("Failed to allocate hashmap_node_t\n");
        return NULL;
    }

    node->key = arena_alloc(HASHMAP_ARENA, len + 1);
    if (!node->key) {
        printf("Failed to allocate hashmap_node_t key with size %d\n", len + 1);
        return NULL;
    }

    strcpy(node->key, key);
    node->val = val;
    node->next = NULL;
    return node;
}

void hashmap_rehash(hashmap_t *map)
{
    if (!map)
        return;

    int old_cap = map->cap;
    hashmap_node_t **old_buckets = map->buckets;

    map->cap <<= 1;
    map->buckets = calloc(map->cap, sizeof(hashmap_node_t *));

    if (!map->buckets) {
        printf("Failed to allocate new buckets in hashmap_t\n");
        map->buckets = old_buckets;
        map->cap = old_cap;
        return;
    }

    for (int i = 0; i < old_cap; i++) {
        hashmap_node_t *cur = old_buckets[i];
        hashmap_node_t *next;
        hashmap_node_t *target_cur;

        while (cur) {
            next = cur->next;
            cur->next = NULL;
            int index = hashmap_hash_index(map->cap, cur->key);
            target_cur = map->buckets[index];

            if (!target_cur) {
                map->buckets[index] = cur;
            } else {
                cur->next = target_cur;
                map->buckets[index] = cur;
            }

            cur = next;
        }
    }

    free(old_buckets);
}

/* Put a key-value pair into given hashmap.
 * If key already contains a value, then replace it with new value, the old
 * value will be freed.
 * @map: The hashmap to be put into. Must not be NULL.
 * @key: The key string. May be NULL.
 * @val: The value pointer. May be NULL. This value's lifetime is held by
 * hashmap.
 */
void hashmap_put(hashmap_t *map, char *key, void *val)
{
    if (!map)
        return;

    int index = hashmap_hash_index(map->cap, key);
    hashmap_node_t *cur = map->buckets[index],
                   *new_node = hashmap_node_new(key, val);

    if (!cur) {
        map->buckets[index] = new_node;
    } else {
        while (cur->next)
            cur = cur->next;
        cur->next = new_node;
    }

    map->size++;
    /* Check if size of map exceeds load factor 75% (or 3/4 of capacity) */
    if ((map->cap >> 2) + (map->cap >> 1) <= map->size)
        hashmap_rehash(map);
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

    for (hashmap_node_t *cur = map->buckets[index]; cur; cur = cur->next)
        if (!strcmp(cur->key, key))
            return cur;

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

    free(map->buckets);
    free(map);
}

/* options */

bool dump_ir = false;
bool hard_mul_div = false;

/* Find the type by the given name.
 * @type_name: The name to be searched.
 * @flag:
 *      0 - Search in all type names.
 *      1 - Search in all names, excluding the tags of structure.
 *      2 - Only search in tags.
 *
 * Return: The pointer to the type, or NULL if not found.
 */
type_t *find_type(char *type_name, int flag)
{
    for (int i = 0; i < types_idx; i++) {
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
    ph2_ir->dest = 0;
    ph2_ir->func_name[0] = '\0';
    ph2_ir->next_bb = NULL;
    ph2_ir->then_bb = NULL;
    ph2_ir->else_bb = NULL;
    return add_existed_ph2_ir(ph2_ir);
}

void set_var_liveout(var_t *var, int end)
{
    if (var->liveness >= end)
        return;
    var->liveness = end;
}

block_t *add_block(block_t *parent, func_t *func, macro_t *macro)
{
    block_t *blk = arena_alloc(BLOCK_ARENA, sizeof(block_t));

    /* Initialize all fields explicitly */
    blk->locals.size = 0;
    blk->locals.capacity = 16;
    blk->locals.elements =
        arena_alloc(BLOCK_ARENA, blk->locals.capacity * sizeof(var_t *));
    blk->parent = parent;
    blk->func = func;
    blk->macro = macro;
    blk->next = NULL;
    return blk;
}

void add_alias(char *alias, char *value)
{
    alias_t *al = hashmap_get(ALIASES_MAP, alias);
    if (!al) {
        al = arena_alloc_alias();
        if (!al) {
            printf("Failed to allocate alias_t\n");
            return;
        }
        /* Use interned string for alias name */
        strcpy(al->alias, intern_string(alias));
        hashmap_put(ALIASES_MAP, alias, al);
    }
    strcpy(al->value, value);
    al->disabled = false;
}

char *find_alias(char alias[])
{
    alias_t *al = hashmap_get(ALIASES_MAP, alias);
    if (al && !al->disabled)
        return al->value;
    return NULL;
}

bool remove_alias(char *alias)
{
    alias_t *al = hashmap_get(ALIASES_MAP, alias);
    if (al && !al->disabled) {
        al->disabled = true;
        return true;
    }
    return false;
}

macro_t *add_macro(char *name)
{
    macro_t *ma = hashmap_get(MACROS_MAP, name);
    if (!ma) {
        ma = arena_alloc_macro();
        if (!ma) {
            printf("Failed to allocate macro_t\n");
            return NULL;
        }
        /* Use interned string for macro name */
        strcpy(ma->name, intern_string(name));
        hashmap_put(MACROS_MAP, name, ma);
    }
    ma->disabled = false;
    return ma;
}

macro_t *find_macro(char *name)
{
    macro_t *ma = hashmap_get(MACROS_MAP, name);
    if (ma && !ma->disabled)
        return ma;
    return NULL;
}

bool remove_macro(char *name)
{
    macro_t *ma = hashmap_get(MACROS_MAP, name);
    if (ma) {
        ma->disabled = true;
        return true;
    }
    return false;
}

void error(char *msg);

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

int find_macro_param_src_idx(char *name, block_t *parent)
{
    macro_t *macro = parent->macro;

    if (!parent)
        error("The macro expansion is not supported in the global scope");
    if (!parent->macro)
        return 0;

    for (int i = 0; i < macro->num_param_defs; i++) {
        if (!strcmp(macro->param_defs[i].var_name, name))
            return macro->params[i];
    }
    return 0;
}

type_t *add_type(void)
{
    if (types_idx >= MAX_TYPES) {
        printf("Error: Maximum number of types (%d) exceeded\n", MAX_TYPES);
        abort();
    }
    return &TYPES[types_idx++];
}

type_t *add_named_type(char *name)
{
    type_t *type = add_type();
    /* Use interned string for type name */
    strcpy(type->type_name, intern_string(name));
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

var_t *find_member(char token[], type_t *type)
{
    /* If it is a forwardly declared alias of a structure, switch to the base
     * structure type.
     */
    if (type->size == 0)
        type = type->base_struct;

    for (int i = 0; i < type->num_fields; i++) {
        if (!strcmp(type->fields[i].var_name, token))
            return &type->fields[i];
    }
    return NULL;
}

var_t *find_local_var(char *token, block_t *block)
{
    func_t *func = block->func;

    for (; block; block = block->parent) {
        var_list_t *var_list = &block->locals;
        for (int i = 0; i < var_list->size; i++) {
            if (!strcmp(var_list->elements[i]->var_name, token))
                return var_list->elements[i];
        }
    }

    if (func) {
        for (int i = 0; i < func->num_params; i++) {
            if (!strcmp(func->param_defs[i].var_name, token))
                return &func->param_defs[i];
        }
    }
    return NULL;
}

var_t *find_global_var(char *token)
{
    var_list_t *var_list = &GLOBAL_BLOCK->locals;

    for (int i = 0; i < var_list->size; i++) {
        if (!strcmp(var_list->elements[i]->var_name, token))
            return var_list->elements[i];
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
        size = 4;
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
    /* Use interned string for function name */
    strcpy(func->return_def.var_name, intern_string(func_name));
    func->stack_size = 4;

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

    /* Initialize prev array with NEXT type */
    for (int i = 0; i < MAX_BB_PRED; i++)
        bb->prev[i].type = NEXT;

    if (dump_ir)
        snprintf(bb->bb_label_name, MAX_VAR_LEN, ".label.%d", bb_label_idx++);

    return bb;
}

/* The pred-succ pair must have only one connection */
void bb_connect(basic_block_t *pred,
                basic_block_t *succ,
                bb_connection_type_t type)
{
    if (!pred)
        abort();
    if (!succ)
        abort();

    int i = 0;
    while (succ->prev[i].bb)
        i++;

    if (i > MAX_BB_PRED - 1) {
        printf("Error: too many predecessors\n");
        abort();
    }

    succ->prev[i].bb = pred;
    succ->prev[i].type = type;

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
    for (int i = 0; i < MAX_BB_PRED; i++) {
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
    n->sz = sz;
    n->useful = false;
    n->belong_to = bb;
    n->phi_ops = NULL;
    n->idx = 0;

    if (str)
        strcpy(n->str, intern_string(str));
    else
        n->str[0] = '\0';

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

    if (new_size > src->capacity << 1)
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
    if (!strbuf_extend(src, 1))
        return false;

    src->elements[src->size] = value;
    src->size++;

    return true;
}

bool strbuf_puts(strbuf_t *src, const char *value)
{
    int len = strlen(value);

    if (!strbuf_extend(src, len))
        return false;

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

/* This routine is required because the global variable initializations are
 * not supported now.
 */
void global_init(void)
{
    elf_code_start = ELF_START + elf_header_len;

    MACROS_MAP = hashmap_create(MAX_ALIASES);

    /* Initialize arenas first so we can use them for allocation */
    BLOCK_ARENA = arena_init(DEFAULT_ARENA_SIZE); /* Variables/blocks */
    INSN_ARENA = arena_init(LARGE_ARENA_SIZE); /* Instructions - high usage */
    BB_ARENA = arena_init(SMALL_ARENA_SIZE);   /* Basic blocks - low usage */
    HASHMAP_ARENA = arena_init(DEFAULT_ARENA_SIZE); /* Hash nodes */
    GENERAL_ARENA =
        arena_init(DEFAULT_ARENA_SIZE); /* For TYPES and PH2_IR_FLATTEN */

    /* Use arena allocation for better memory management */
    TYPES = arena_alloc(GENERAL_ARENA, MAX_TYPES * sizeof(type_t));
    PH2_IR_FLATTEN =
        arena_alloc(GENERAL_ARENA, MAX_IR_INSTR * sizeof(ph2_ir_t *));

    /* Initialize string pool for identifier deduplication */
    string_pool = arena_alloc(GENERAL_ARENA, sizeof(string_pool_t));
    string_pool->strings = hashmap_create(512);

    /* Initialize string literal pool for deduplicating string constants */
    string_literal_pool =
        arena_alloc(GENERAL_ARENA, sizeof(string_literal_pool_t));
    string_literal_pool->literals = hashmap_create(256);

    SOURCE = strbuf_create(MAX_SOURCE);
    FUNC_MAP = hashmap_create(DEFAULT_FUNCS_SIZE);
    INCLUSION_MAP = hashmap_create(DEFAULT_INCLUSIONS_SIZE);

    /* Initialize token management globals */
    current_location.line = 1;
    current_location.column = 1;
    current_location.filename = NULL;
    TOKEN_POOL = NULL;
    TOKEN_BUFFER = NULL;
    ALIASES_MAP = hashmap_create(MAX_ALIASES);
    CONSTANTS_MAP = hashmap_create(MAX_CONSTANTS);

    elf_code = strbuf_create(MAX_CODE);
    elf_data = strbuf_create(MAX_DATA);
    elf_rodata = strbuf_create(MAX_DATA);
    elf_header = strbuf_create(MAX_HEADER);
    elf_symtab = strbuf_create(MAX_SYMTAB);
    elf_strtab = strbuf_create(MAX_STRTAB);
    elf_section = strbuf_create(MAX_SECTION);
    elf_bss_size = 0;
}

/* Forward declaration for lexer cleanup */
void lexer_cleanup(void);

/* Free empty trailing blocks from an arena safely.
 * This only frees blocks that come after the last used block,
 * ensuring no pointers are invalidated.
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

/* Compact all arenas to reduce memory usage after compilation phases.
 * This safely frees only trailing empty blocks without invalidating pointers.
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

/* Compact specific arenas based on compilation phase.
 * Different phases have different memory usage patterns.
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

    hashmap_free(MACROS_MAP);

    /* Free string interning hashmaps */
    if (string_pool && string_pool->strings)
        hashmap_free(string_pool->strings);
    if (string_literal_pool && string_literal_pool->literals)
        hashmap_free(string_literal_pool->literals);

    arena_free(BLOCK_ARENA);
    arena_free(INSN_ARENA);
    arena_free(BB_ARENA);
    arena_free(HASHMAP_ARENA);
    arena_free(GENERAL_ARENA); /* free TYPES and PH2_IR_FLATTEN */

    strbuf_free(SOURCE);
    strbuf_free(elf_code);
    strbuf_free(elf_data);
    strbuf_free(elf_rodata);
    strbuf_free(elf_header);
    strbuf_free(elf_symtab);
    strbuf_free(elf_strtab);
    strbuf_free(elf_section);

    hashmap_free(FUNC_MAP);
    hashmap_free(INCLUSION_MAP);
    hashmap_free(ALIASES_MAP);
    hashmap_free(CONSTANTS_MAP);
}

/* Reports an error without specifying a position */
void fatal(char *msg)
{
    printf("[Error]: %s\n", msg);
    abort();
}

/* Reports an error and specifying a position */
void error(char *msg)
{
    /* Safety check for NULL message */
    if (!msg) {
        printf("[Error]: Unknown error occurred\n");
        abort();
    }

    /* Safety check for SOURCE buffer */
    if (!SOURCE || !SOURCE->elements || SOURCE->size < 0) {
        printf("[Error]: %s\nSource location unavailable (invalid source buffer)\n", msg);
        abort();
    }

    /* Handle empty source case */
    if (SOURCE->size == 0) {
        printf("[Error]: %s\nOccurs at start of file\n", msg);
        abort();
    }

    /* Construct error source diagnostics */
    int current_pos = SOURCE->size;
    int line_start, line_end;
    int i = 0;
    char diagnostic[512]; /* MAX_LINE_LEN * 2 */

    /* Ensure current_pos is within bounds */
    if (current_pos >= SOURCE->size) {
        current_pos = SOURCE->size - 1;
    }

    /* Find the start of the current line (scan backwards to find '\n') */
    line_start = current_pos;
    while (line_start > 0 && SOURCE->elements[line_start - 1] != '\n') {
        line_start--;
    }

    /* Find the end of the current line (scan forwards to find '\n' or end) */
    line_end = current_pos;
    while (line_end < SOURCE->size && SOURCE->elements[line_end] != '\n') {
        line_end++;
    }

    /* Copy the current line to diagnostic buffer with bounds checking */
    for (int pos = line_start; pos < line_end && i < (int)sizeof(diagnostic) - 50; pos++) {
        diagnostic[i++] = SOURCE->elements[pos];
    }
    
    /* Add newline after the source line */
    if (i < (int)sizeof(diagnostic) - 30) {
        diagnostic[i++] = '\n';
    }

    /* Add spaces to point to the error position */
    int error_column = current_pos - line_start;
    for (int spaces = 0; spaces < error_column && i < (int)sizeof(diagnostic) - 20; spaces++) {
        diagnostic[i++] = ' ';
    }

    /* Add the error pointer with bounds checking */
    const char *pointer_text = "^ Error occurs here";
    int pointer_len = strlen(pointer_text);
    if (i + pointer_len < (int)sizeof(diagnostic)) {
        strcpy(diagnostic + i, pointer_text);
        i += pointer_len;
    }
    
    /* Null terminate */
    if (i < (int)sizeof(diagnostic)) {
        diagnostic[i] = '\0';
    } else {
        diagnostic[sizeof(diagnostic) - 1] = '\0';
    }

    /* Calculate line number for better error reporting */
    int line_number = 1;
    for (int pos = 0; pos < current_pos && pos < SOURCE->size; pos++) {
        if (SOURCE->elements[pos] == '\n') {
            line_number++;
        }
    }

    /* Output the error with improved formatting */
    printf("[Error]: %s\n", msg);
    printf("At line %d, column %d (source position %d):\n", 
           line_number, error_column + 1, current_pos);
    printf("%s\n", diagnostic);
    abort();
}

void print_indent(int indent)
{
    for (int i = 0; i < indent; i++)
        printf("\t");
}

void dump_bb_insn(func_t *func, basic_block_t *bb, bool *at_func_start)
{
    var_t *rd, *rs1, *rs2;

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
    for (int i = 0; i < MAX_BB_DOM_SUCC; i++) {
        if (!bb->dom_next[i])
            break;
        dump_bb_insn_by_dom(func, bb->dom_next[i], at_func_start);
    }
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
        for (int i = 0; i < MAX_BB_PRED; i++) {
            basic_block_t *bb = func->exit->prev[i].bb;
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
