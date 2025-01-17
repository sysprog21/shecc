/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* minimal libc implementation */

#define NULL 0

#define bool _Bool
#define true 1
#define false 0

#if defined(__arm__)
#define __SIZEOF_POINTER__ 4
#define __syscall_exit 1
#define __syscall_read 3
#define __syscall_write 4
#define __syscall_close 6
#define __syscall_open 5
#define __syscall_mmap2 192
#define __syscall_munmap 91

#elif defined(__riscv)
#define __SIZEOF_POINTER__ 4
#define __syscall_exit 93
#define __syscall_read 63
#define __syscall_write 64
#define __syscall_close 57
#define __syscall_open 1024
#define __syscall_openat 56
#define __syscall_mmap2 222
#define __syscall_munmap 215

#else /* Only Arm32 and RV32 are supported */
#error "Unsupported architecture"
#endif

#define INT_BUF_LEN 16

typedef int FILE;

void abort();

int strlen(char *str)
{
    int i = 0;
    while (str[i])
        i++;
    return i;
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
    while (count > 0) {
        count--;
        dest[count] = src[count];
    }
    return dest;
}

/*
 * set 10 digits (32bit) without div
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

    if (neg == 1)
        pb[i] = '-';
}

void __str_base8(char *pb, int val)
{
    int c = INT_BUF_LEN - 1, v;
    /*
     * Because every 3 binary digits can be converted
     * to 1 octal digit, here performs the conversion
     * 10 times, derived from 32 divided by 3.
     *
     * Finally, the remaining 2 bits are processed after
     * the loop.
     * */
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

int __format(char *buffer,
             int val,
             int width,
             int zeropad,
             int base,
             int alternate_form)
{
    int bi = 0;
    char pb[INT_BUF_LEN];
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
                buffer[bi++] = '0';
                width -= 1;
            } else if (pb[pbi] != '0')
                pb[--pbi] = '0';
        }
        break;
    case 10:
        if (width && zeropad && pb[pbi] == '-') {
            buffer[bi++] = '-';
            pbi++;
            width--;
        }
        break;
    case 16:
        if (alternate_form) {
            if (width && zeropad && pb[pbi] != '0') {
                buffer[bi++] = '0';
                buffer[bi++] = 'x';
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

    while (width) {
        buffer[bi++] = zeropad ? '0' : ' ';
        width--;
    }

    for (; pbi < INT_BUF_LEN; pbi++)
        buffer[bi++] = pb[pbi];

    return bi;
}

void printf(char *str, ...)
{
    int *var_args = &str + 4;
    char buffer[200];
    int si = 0, bi = 0, pi = 0;

    while (str[si]) {
        if (str[si] != '%') {
            buffer[bi] = str[si];
            bi++;
            si++;
        } else {
            int w = 0, zp = 0, pp = 0;

            si++;
            if (str[si] == '#') {
                pp = 1;
                si++;
            }
            if (str[si] == '0') {
                zp = 1;
                si++;
            }
            if (str[si] >= '1' && str[si] <= '9') {
                w = str[si] - '0';
                si++;
                while (str[si] >= '0' && str[si] <= '9') {
                    w *= 10;
                    w += str[si] - '0';
                    si++;
                }
            }
            if (str[si] == 's') {
                /* append param pi as string */
                int l = strlen(var_args[pi]);
                strcpy(buffer + bi, var_args[pi]);
                bi += l;
            } else if (str[si] == 'c') {
                /* append param pi as char */
                buffer[bi] = var_args[pi];
                bi += 1;
            } else if (str[si] == 'o') {
                /* append param as octal */
                int v = var_args[pi];
                bi += __format(buffer + bi, v, w, zp, 8, pp);
            } else if (str[si] == 'd') {
                /* append param as decimal */
                int v = var_args[pi];
                bi += __format(buffer + bi, v, w, zp, 10, 0);
            } else if (str[si] == 'x') {
                /* append param as hex */
                int v = var_args[pi];
                bi += __format(buffer + bi, v, w, zp, 16, pp);
            } else if (str[si] == '%') {
                /* append literal '%' character */
                buffer[bi] = '%';
                bi++;
                si++;
                continue;
            }
            pi++;
            si++;
        }
    }
    buffer[bi] = 0;
    __syscall(__syscall_write, 1, buffer, bi);
}

void sprintf(char *buffer, char *str, ...)
{
    int *var_args = &str + 4;
    int si = 0, bi = 0, pi = 0;

    while (str[si]) {
        if (str[si] != '%') {
            buffer[bi] = str[si];
            bi++;
            si++;
        } else {
            int w = 0, zp = 0, pp = 0;

            si++;
            if (str[si] == '#') {
                pp = 1;
                si++;
            }
            if (str[si] == '0') {
                zp = 1;
                si++;
            }
            if (str[si] >= '1' && str[si] <= '9') {
                w = str[si] - '0';
                si++;
                if (str[si] >= '0' && str[si] <= '9') {
                    w *= 10;
                    w += str[si] - '0';
                    si++;
                }
            }
            switch (str[si]) {
            case 37: /* % */
                buffer[bi++] = '%';
                si++;
                continue;
            case 99: /* c */
                buffer[bi++] = var_args[pi];
                break;
            case 115: /* s */
                strcpy(buffer + bi, var_args[pi]);
                bi += strlen(var_args[pi]);
                break;
            case 111: /* o */
                bi += __format(buffer + bi, var_args[pi], w, zp, 8, pp);
                break;
            case 100: /* d */
                bi += __format(buffer + bi, var_args[pi], w, zp, 10, 0);
                break;
            case 120: /* x */
                bi += __format(buffer + bi, var_args[pi], w, zp, 16, pp);
                break;
            default:
                abort();
                break;
            }
            pi++;
            si++;
        }
    }
    buffer[bi] = 0;
}

int __free_all();

void exit(int exit_code)
{
    __free_all();
    __syscall(__syscall_exit, exit_code);
}

void abort()
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
 * of 0 to 127 for the character, or -1 on the end of file. */
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
        /* Not support casting yet. Simply assign it. */
        str[i] = c;

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
    char buf[1];
    buf[0] = c;
    __syscall(__syscall_write, stream, buf, 1);
    return 0;
}

/* Non-portable: Assume page size is 4KiB */
#define PAGESIZE 4096

#define CHUNK_SIZE_FREED_MASK 1
#define CHUNK_SIZE_SZ_MASK 0xFFFFFFFE
#define CHUNK_GET_SIZE(size) (size & CHUNK_SIZE_SZ_MASK)
#define IS_CHUNK_GET_FREED(size) (size & CHUNK_SIZE_FREED_MASK)

typedef struct chunk {
    struct chunk *next;
    struct chunk *prev;
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
    int mask = PAGESIZE - 1;
    return ((size - 1) | mask) + 1;
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

    /* to search the best chunk */
    chunk_t *best_fit_chunk = NULL;
    chunk_t *allocated;

    if (!__freelist_head->next) {
        /* If no more chunks in the free chunk list, allocate best_fit_chunk
         * as NULL.
         */
        allocated = best_fit_chunk;
    } else {
        /* record the size of the chunk */
        int bsize = 0;

        for (chunk_t *fh = __freelist_head; fh->next; fh = fh->next) {
            int fh_size = CHUNK_GET_SIZE(fh->size);
            if (fh_size >= size && !best_fit_chunk) {
                /* first time setting fh as best_fit_chunk */
                best_fit_chunk = fh;
                bsize = fh_size;
            } else if ((fh_size >= size) && best_fit_chunk &&
                       (fh_size < bsize)) {
                /* If there is a smaller chunk available, replace it. */
                best_fit_chunk = fh;
                bsize = fh_size;
            }
        }

        /* a suitable chunk has been found */
        if (best_fit_chunk) {
            /* remove the chunk from the freelist */
            if (best_fit_chunk->prev) {
                chunk_t *tmp = best_fit_chunk->prev;
                tmp->next = best_fit_chunk->next;
            } else
                __freelist_head = best_fit_chunk->next;

            if (best_fit_chunk->next) {
                chunk_t *tmp = best_fit_chunk->next;
                tmp->prev = best_fit_chunk->prev;
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

    __alloc_tail->next = allocated;
    allocated->prev = __alloc_tail;

    __alloc_tail = allocated;
    __alloc_tail->next = NULL;
    __alloc_tail->size = allocated->size;
    chunk_clear_freed(__alloc_tail);
    void *ptr = __alloc_tail + 1;
    return ptr;
}

void *calloc(int n, int size)
{
    char *p = malloc(n * size);

    if (!p)
        return NULL;
    for (int i = 0; i < n * size; i++)
        p[i] = 0;
    return p;
}

void __rfree(void *ptr, int size)
{
    if (!ptr)
        return;
    __syscall(__syscall_munmap, ptr, size);
}

int __free_all()
{
    if (!__freelist_head && !__alloc_head)
        return 0;

    chunk_t *cur = __freelist_head;
    chunk_t *rel;
    int size;

    /* release freelist */
    while (cur->next) {
        rel = cur;
        cur = cur->next;
        rel->next = NULL;
        rel->prev = NULL;
        size = CHUNK_GET_SIZE(rel->size);
        __rfree(rel, size);
    }

    if (__alloc_head->next) {
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

    char *__ptr = ptr;
    chunk_t *cur = __ptr - sizeof(chunk_t);
    if (IS_CHUNK_GET_FREED(cur->size)) {
        printf("free(): double free detected\n");
        abort();
    }

    chunk_t *prev;
    if (cur->prev) {
        prev = cur->prev;
        prev->next = cur->next;
    } else
        __alloc_head = cur->next;

    if (cur->next) {
        chunk_t *next = cur->next;
        next->prev = cur->prev;
    } else {
        prev->next = NULL;
        __alloc_tail = prev;
    }

    /* Insert head in __freelist_head */
    cur->next = __freelist_head;
    cur->prev = NULL;
    chunk_set_freed(cur);
    __freelist_head->prev = cur;
    __freelist_head = cur;
}
