/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* minimal libc implementation */
#include "c.h"
#define INT_BUF_LEN 16

int strlen(char *str)
{
    /* process the string by checking 4 characters (a 32-bit word) at a time */
    int i = 0;
    for (;; i += 4) {
        if (!str[i])
            return i;
        if (!str[i + 1])
            return i + 1;
        if (!str[i + 2])
            return i + 2;
        if (!str[i + 3])
            return i + 3;
    }
}

int strcmp(char *s1, char *s2)
{
    int i = 0;
    while (s1[i] && s2[i]) {
        if (s1[i] < s2[i])
            return -1;
        if (s1[i] > s2[i])
            return 1;
        i++;
    }
    return s1[i] - s2[i];
}

int strncmp(char *s1, char *s2, int len)
{
    int i = 0;
    while (i < len) {
        if (s1[i] < s2[i])
            return -1;
        if (s1[i] > s2[i])
            return 1;
        if (!s1[i])
            return 0;
        i++;
    }
    return 0;
}

char *strcpy(char *dest, char *src)
{
    int i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
    return dest;
}

char *strncpy(char *dest, char *src, int len)
{
    int i = 0;
    int beyond = 0;
    while (i < len) {
        if (beyond == 0) {
            dest[i] = src[i];
            if (src[i] == 0)
                beyond = 1;
        } else {
            dest[i] = 0;
        }
        i++;
    }
    return dest;
}

char *memcpy(char *dest, char *src, int count)
{
    int i = 0;

    /* Continues as long as there are at least 4 bytes remaining to copy. */
    for (; i + 4 <= count; i += 4) {
        dest[i] = src[i];
        dest[i + 1] = src[i + 1];
        dest[i + 2] = src[i + 2];
        dest[i + 3] = src[i + 3];
    }

    /* Ensure all @count bytes are copied, even if @count is not a multiple of
     * 4, or if @count was less than 4 initially.
     */
    for (; i < count; i++)
        dest[i] = src[i];

    return dest;
}

int memcmp(void *s1, void *s2, int n)
{
    char *p1 = (char *) s1, *p2 = (char *) s2;

    for (int i = 0; i < n; i++) {
        if (p1[i] < p2[i])
            return -1;
        if (p1[i] > p2[i])
            return 1;
    }
    return 0;
}

void *memset(void *s, int c, int n)
{
    int i = 0;
    char *ptr = (char *) s;
    char byte_val = (char) c;
    for (; i + 4 <= n; i += 4) {
        ptr[i] = byte_val;
        ptr[i + 1] = byte_val;
        ptr[i + 2] = byte_val;
        ptr[i + 3] = byte_val;
    }

    for (; i < n; i++)
        ptr[i] = byte_val;

    return s;
}

/* set 10 digits (32bit) without div
 *
 * This function converts a given integer value to its string representation
 * in base-10 without using division operations. The method involves calculating
 * the approximate quotient and remainder using bitwise operations, which are
 * then used to derive each digit of the result.
 *
 * The logic is based on an efficient method of dividing by constants, as
 * detailed in the reference link:
 * http://web.archive.org/web/20180517023231/http://www.hackersdelight.org/divcMore.pdf.
 * This approach avoids expensive division instructions by using a series of
 * bitwise shifts and additions to calculate the quotient and remainder.
 */
void __str_base10(char *pb, int val)
{
    int neg = 0;
    int q, r, t;
    int i = INT_BUF_LEN - 1;

    if (val == -2147483648) {
        strncpy(pb + INT_BUF_LEN - 11, "-2147483648", 11);
        return;
    }
    if (val < 0) {
        neg = 1;
        val = -val;
    }

    while (val) {
        q = (val >> 1) + (val >> 2);
        q += (q >> 4);
        q += (q >> 8);
        q += (q >> 16);
        q >>= 3;
        r = val - (((q << 2) + q) << 1);
        t = ((r + 6) >> 4);
        q += t;
        r -= (((t << 2) + t) << 1);

        pb[i] += r;
        val = q;
        i--;
    }

    if (neg)
        pb[i] = '-';
}

void __str_base8(char *pb, int val)
{
    int c = INT_BUF_LEN - 1, v;

    /* Because every 3 binary digits can be converted to 1 octal digit, here
     * performs the conversion 10 times, derived from 32 divided by 3.
     *
     * Finally, the remaining 2 bits are processed after the loop.
     */
    int times = (sizeof(int) << 3) / 3;
    for (int i = 0; i < times; i++) {
        v = val & 0x7;
        pb[c] = '0' + v;
        val >>= 3;
        c--;
    }
    v = val & 0x3;
    pb[c] = '0' + v;
}

void __str_base16(char *pb, int val)
{
    int c = INT_BUF_LEN - 1;
    int times = sizeof(int) << 1;
    for (int i = 0; i < times; i++) {
        int v = val & 0xf;
        if (v < 10)
            pb[c] = '0' + v;
        else if (v < 16)
            pb[c] = 'a' + v - 10;
        else {
            abort();
            break;
        }
        val >>= 4;
        c--;
    }
}

/* The specification of snprintf() is defined in C99 7.19.6.5, and its behavior
 * and return value should comply with the following description:
 * - If n is zero, nothing is written.
 * - Writes at most n bytes, including the null character.
 * - On success, the return value should be the length of the entire converted
 *   string even if n is insufficient to store it.
 *
 * Thus, a structure fmtbuf_t is defined for formatted output conversion for
 * the functions in the printf() family.
 * @buf: the current position of the buffer.
 * @n  : the remaining space of the buffer.
 * @len: the number of characters that would have been written (excluding the
 * null terminator) had n been sufficiently large.
 *
 * Once a write operation is performed, buf and n will be respectively
 * incremented and decremented by the actual written size if n is sufficient,
 * and len must be incremented to store the length of the entire converted
 * string.
 */
typedef struct {
    char *buf;
    int n;
    int len;
} fmtbuf_t;

void __fmtbuf_write_char(fmtbuf_t *fmtbuf, int val)
{
    fmtbuf->len += 1;

    /* Write the given character when n is greater than 1.
     * This means preserving one position for the null character.
     */
    if (fmtbuf->n <= 1)
        return;

    char ch = (char) (val & 0xFF);
    fmtbuf->buf[0] = ch;
    fmtbuf->buf += 1;
    fmtbuf->n -= 1;
}

void __fmtbuf_write_str(fmtbuf_t *fmtbuf, char *str, int l)
{
    fmtbuf->len += l;

    /* Write the given string when n is greater than 1.
     * This means preserving one position for the null character.
     */
    if (fmtbuf->n <= 1)
        return;

    /* If the remaining space is less than the length of the string, write only
     * n - 1 bytes.
     */
    int sz = fmtbuf->n - 1;
    l = l <= sz ? l : sz;
    strncpy(fmtbuf->buf, str, l);
    fmtbuf->buf += l;
    fmtbuf->n -= l;
}

void __format(fmtbuf_t *fmtbuf,
              int val,
              int width,
              int zeropad,
              int base,
              int alternate_form)
{
    char pb[INT_BUF_LEN], ch;
    int pbi;

    /* set to zeroes */
    for (pbi = 0; pbi < INT_BUF_LEN; pbi++)
        pb[pbi] = '0';

    pbi = 0;

    switch (base) {
    case 8:
        __str_base8(pb, val);
        break;
    case 10:
        __str_base10(pb, val);
        break;
    case 16:
        __str_base16(pb, val);
        break;
    default:
        abort();
        break;
    }

    while (pb[pbi] == '0' && pbi < INT_BUF_LEN - 1)
        pbi++;

    switch (base) {
    case 8:
        if (alternate_form) {
            if (width && zeropad && pb[pbi] != '0') {
                __fmtbuf_write_char(fmtbuf, '0');
                width -= 1;
            } else if (pb[pbi] != '0')
                pb[--pbi] = '0';
        }
        break;
    case 10:
        if (width && zeropad && pb[pbi] == '-') {
            __fmtbuf_write_char(fmtbuf, '-');
            pbi++;
            width--;
        }
        break;
    case 16:
        if (alternate_form) {
            if (width && zeropad && pb[pbi] != '0') {
                __fmtbuf_write_char(fmtbuf, '0');
                __fmtbuf_write_char(fmtbuf, 'x');
                width -= 2;
            } else if (pb[pbi] != '0') {
                pb[--pbi] = 'x';
                pb[--pbi] = '0';
            }
        }
        break;
    }

    width -= (INT_BUF_LEN - pbi);
    if (width < 0)
        width = 0;

    ch = zeropad ? '0' : ' ';
    while (width) {
        __fmtbuf_write_char(fmtbuf, ch);
        width--;
    }

    __fmtbuf_write_str(fmtbuf, pb + pbi, INT_BUF_LEN - pbi);
}

void __format_to_buf(fmtbuf_t *fmtbuf, char *format, int *var_args)
{
    int si = 0, pi = 0;

    while (format[si]) {
        if (format[si] != '%') {
            __fmtbuf_write_char(fmtbuf, format[si]);
            si++;
        } else {
            int w = 0, zp = 0, pp = 0, v = var_args[pi], l;

            si++;
            if (format[si] == '#') {
                pp = 1;
                si++;
            }
            if (format[si] == '0') {
                zp = 1;
                si++;
            }
            if (format[si] >= '1' && format[si] <= '9') {
                w = format[si] - '0';
                si++;
                while (format[si] >= '0' && format[si] <= '9') {
                    w *= 10;
                    w += format[si] - '0';
                    si++;
                }
            }
            switch (format[si]) {
            case 's':
                /* append param pi as string */
                l = strlen((char *) v);
                __fmtbuf_write_str(fmtbuf, (char *) v, l);
                break;
            case 'c':
                /* append param pi as char */
                __fmtbuf_write_char(fmtbuf, (char) v);
                break;
            case 'o':
                /* append param as octal */
                __format(fmtbuf, v, w, zp, 8, pp);
                break;
            case 'd':
                /* append param as decimal */
                __format(fmtbuf, v, w, zp, 10, 0);
                break;
            case 'x':
                /* append param as hex */
                __format(fmtbuf, v, w, zp, 16, pp);
                break;
            case '%':
                /* append literal '%' character */
                __fmtbuf_write_char(fmtbuf, '%');
                si++;
                continue;
            }
            pi++;
            si++;
        }
    }

    /* If n is still greater than 0, set the null character. */
    if (fmtbuf->n)
        fmtbuf->buf[0] = 0;
}

int printf(char *str, ...)
{
    char buffer[200];
    fmtbuf_t fmtbuf;

    fmtbuf.buf = buffer;
    fmtbuf.n = INT_MAX;
    fmtbuf.len = 0;
    __format_to_buf(&fmtbuf, str, &str + 4);
    return __syscall(__syscall_write, 1, buffer, fmtbuf.len);
}

int sprintf(char *buffer, char *str, ...)
{
    fmtbuf_t fmtbuf;

    fmtbuf.buf = buffer;
    fmtbuf.n = INT_MAX;
    fmtbuf.len = 0;
    __format_to_buf(&fmtbuf, str, &str + 4);
    return fmtbuf.len;
}

int snprintf(char *buffer, int n, char *str, ...)
{
    fmtbuf_t fmtbuf;

    fmtbuf.buf = buffer;
    fmtbuf.n = n;
    fmtbuf.len = 0;
    __format_to_buf(&fmtbuf, str, &str + 4);
    return fmtbuf.len;
}

int __free_all(void);

void exit(int exit_code)
{
    __free_all();
    __syscall(__syscall_exit, exit_code);
}

void abort(void)
{
    printf("Abnormal program termination\n");
    exit(-1);
}

FILE *fopen(char *filename, char *mode)
{
    if (!strcmp(mode, "wb")) {
#if defined(__arm__)
        return __syscall(__syscall_open, filename, 65, 0x1fd);
#elif defined(__riscv)
        /* FIXME: mode not work currently in RISC-V */
        return __syscall(__syscall_openat, -100, filename, 65, 0x1fd);
#endif
    }
    if (!strcmp(mode, "rb")) {
#if defined(__arm__)
        return __syscall(__syscall_open, filename, 0, 0);
#elif defined(__riscv)
        return __syscall(__syscall_openat, -100, filename, 0, 0);
#endif
    }
    return NULL;
}

int fclose(FILE *stream)
{
    __syscall(__syscall_close, stream);
    return 0;
}

/* Read a byte from file descriptor. So the return value is either in the range
 * of 0 to 127 for the character, or -1 on the end of file.
 */
int fgetc(FILE *stream)
{
    int buf = 0, r = __syscall(__syscall_read, stream, &buf, 1);
    if (r < 1)
        return -1;
    return buf;
}

char *fgets(char *str, int n, FILE *stream)
{
    int i;
    for (i = 0; i < n - 1; i++) {
        int c = fgetc(stream);
        if (c == -1) {
            if (i == 0)
                /* EOF on first char */
                return NULL;
            /* EOF in the middle */
            str[i] = 0;
            return str;
        }
        /* Use explicit cast for clarity */
        str[i] = (char) c;

        if (c == '\n') {
            str[i + 1] = 0;
            return str;
        }
    }
    str[i] = 0;
    return str;
}

int fputc(int c, FILE *stream)
{
    if (__syscall(__syscall_write, stream, &c, 1) < 0)
        return -1;
    return c;
}

#define CHUNK_SIZE_FREED_MASK 1
#define CHUNK_SIZE_SZ_MASK 0xFFFFFFFE
#define CHUNK_GET_SIZE(size) (size & CHUNK_SIZE_SZ_MASK)
#define IS_CHUNK_GET_FREED(size) (size & CHUNK_SIZE_FREED_MASK)

typedef struct chunk {
    struct chunk *next, *prev;
    int size;
} chunk_t;

void chunk_set_freed(chunk_t *chunk)
{
    chunk->size |= CHUNK_SIZE_FREED_MASK;
}

void chunk_clear_freed(chunk_t *chunk)
{
    chunk->size &= CHUNK_SIZE_SZ_MASK;
}

int __align_up(int size)
{
    return ALIGN_UP(size, PAGESIZE);
}

chunk_t *__alloc_head;
chunk_t *__alloc_tail;
chunk_t *__freelist_head;

void *malloc(int size)
{
    if (size <= 0)
        return NULL;

    int flags = 34; /* MAP_PRIVATE (0x02) | MAP_ANONYMOUS (0x20) */
    int prot = 3;   /* PROT_READ (0x01) | PROT_WRITE (0x02) */

    /* Align size to MIN_ALIGNMENT */
    size = ALIGN_UP(size, MIN_ALIGNMENT);

    if (!__alloc_head) {
        chunk_t *tmp =
            __syscall(__syscall_mmap2, NULL, __align_up(sizeof(chunk_t)), prot,
                      flags, -1, 0);
        __alloc_head = tmp;
        __alloc_tail = tmp;
        __alloc_head->next = NULL;
        __alloc_head->prev = NULL;
        __alloc_head->size = 0;
    }

    if (!__freelist_head) {
        chunk_t *tmp =
            __syscall(__syscall_mmap2, NULL, __align_up(sizeof(chunk_t)), prot,
                      flags, -1, 0);
        __freelist_head = tmp;
        __freelist_head->next = NULL;
        __freelist_head->prev = NULL;
        __freelist_head->size = -1;
    }

    /* Search for the best fit chunk in the free list */
    chunk_t *best_fit_chunk = NULL;
    chunk_t *allocated;
    int best_size = 0;

    if (!__freelist_head->next) {
        allocated = NULL;
    } else {
        for (chunk_t *fh = __freelist_head; fh->next; fh = fh->next) {
            int fh_size = CHUNK_GET_SIZE(fh->size);
            if (fh_size >= size && (!best_fit_chunk || fh_size < best_size)) {
                best_fit_chunk = fh;
                best_size = fh_size;
            }
        }

        if (best_fit_chunk) {
            /* Remove from freelist */
            if (best_fit_chunk->prev) {
                best_fit_chunk->prev->next = best_fit_chunk->next;
            } else {
                __freelist_head = best_fit_chunk->next;
            }
            if (best_fit_chunk->next) {
                best_fit_chunk->next->prev = best_fit_chunk->prev;
            }
        }
        allocated = best_fit_chunk;
    }

    if (!allocated) {
        allocated =
            __syscall(__syscall_mmap2, NULL, __align_up(sizeof(chunk_t) + size),
                      prot, flags, -1, 0);
        allocated->size = __align_up(sizeof(chunk_t) + size);
    }

    /* Add to allocation list */
    __alloc_tail->next = allocated;
    allocated->prev = __alloc_tail;
    __alloc_tail = allocated;
    __alloc_tail->next = NULL;
    __alloc_tail->size = allocated->size;
    chunk_clear_freed(__alloc_tail);

    void *ptr = (void *) (__alloc_tail + 1);
    return ptr;
}

void *calloc(int n, int size)
{
    /* Check for overflow before multiplication */
    if (!n || !size)
        return NULL;
    if (n > INT_MAX / size)
        return NULL; /* Overflow protection */

    int total = n * size;
    char *p = malloc(total);

    if (!p)
        return NULL;

    return memset(p, 0, total);
}

void __rfree(void *ptr, int size)
{
    if (!ptr)
        return;
    __syscall(__syscall_munmap, ptr, size);
}

int __free_all(void)
{
    if (!__freelist_head && !__alloc_head)
        return 0;

    chunk_t *cur = __freelist_head;
    chunk_t *rel;
    int size;

    /* release freelist */
    while (cur && cur->next) {
        rel = cur;
        cur = cur->next;
        rel->next = NULL;
        rel->prev = NULL;
        size = CHUNK_GET_SIZE(rel->size);
        __rfree(rel, size);
    }

    if (__alloc_head && __alloc_head->next) {
        cur = __alloc_head->next;
        /* release chunks which not be free */
        while (cur) {
            rel = cur;
            cur = cur->next;
            rel->next = NULL;
            rel->prev = NULL;
            size = CHUNK_GET_SIZE(rel->size);
            __rfree(rel, size);
        }
    }
    return 0;
}

void free(void *ptr)
{
    if (!ptr)
        return;

    char *__ptr = (char *) ptr;
    chunk_t *cur = (chunk_t *) (__ptr - sizeof(chunk_t));
    if (IS_CHUNK_GET_FREED(cur->size)) {
        printf("free(): double free detected\n");
        abort();
    }

    chunk_t *prev = NULL;
    if (cur->prev) {
        prev = cur->prev;
        prev->next = cur->next;
    } else {
        __alloc_head = cur->next;
    }

    if (cur->next) {
        chunk_t *next = cur->next;
        next->prev = cur->prev;
    } else if (prev) {
        prev->next = NULL;
        __alloc_tail = prev;
    }

    /* Insert head in __freelist_head */
    cur->next = __freelist_head;
    cur->prev = NULL;
    chunk_set_freed(cur);
    if (__freelist_head)
        __freelist_head->prev = cur;
    __freelist_head = cur;
}
