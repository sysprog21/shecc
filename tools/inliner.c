/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* inliner - inline libc source into C file.
 *
 * The inliner is used at build-time, and developers can use the
 * "inline C" feature to implement target-specific parts such as
 * C runtime and essential libraries.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE_LEN 200
#define DEFAULT_SOURCE_SIZE 65536

#define write_char(c) strbuf_putc(SOURCE, c)
#define write_str(s) strbuf_puts(SOURCE, s)

typedef struct {
    int size;
    int capacity;
    char *elements;
} strbuf_t;

strbuf_t *SOURCE;

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

bool strbuf_puts(strbuf_t *src, char *value)
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

void write_line(char *src)
{
    write_str("  __c(\"");
    for (int i = 0; src[i]; i++) {
        if (src[i] == '\"') {
            write_char('\\');
            write_char('\"');
        } else if (src[i] != '\n') {
            write_char(src[i]);
        }
    }

    write_char('\\');
    write_char('n');
    write_str("\");\n");
}

void load_from(char *file)
{
    char buffer[MAX_LINE_LEN];
    FILE *f = fopen(file, "rb");
    for (;;) {
        if (!fgets(buffer, MAX_LINE_LEN, f)) {
            fclose(f);
            return;
        }
        write_line(buffer);
    }
    fclose(f);
}

void save_to(char *file)
{
    FILE *f = fopen(file, "wb");
    for (int i = 0; i < SOURCE->size; i++)
        fputc(SOURCE->elements[i], f);
    fclose(f);
}

int main(int argc, char *argv[])
{
    if (argc <= 2) {
        printf("Usage: inliner <input.c> <output.inc>\n");
        return -1;
    }

    SOURCE = strbuf_create(DEFAULT_SOURCE_SIZE);

    write_str("/* Created by tools/inliner - DO NOT EDIT. */\n");

    /* The __c construct is inspired by the __asm keyword, which is used to
     * invoke the inline assembler. In a similar vein, __c aims to "inline C
     * code," allowing for the emission of specified C code as needed. e.g.,
     *   __c("int strlen(char *str) {\n");
     *   __c("    int i = 0;\n");
     *   __c("    while (str[i])\n");
     *   __c("        i++;\n");
     *   __c("    return i;\n");
     *   __c("}\n");
     */
    write_str("void __c(char *src) {\n");
    write_str("    dynarr_extend(SOURCE, src, strlen(src));\n");
    write_str("}\n");

    write_str("void libc_generate() {\n");
    load_from(argv[1]);
    write_str("}\n");
    save_to(argv[2]);
    strbuf_free(SOURCE);

    return 0;
}
