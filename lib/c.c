/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* minimal libc implementation */
#include "c.h"

/* Staging buffer for the printf family that writes straight to a descriptor.
 *
 * The longest single call in the tree is ssa.c's "insn_%p [label=%s]": a
 * DUMP_INSN_LEN staging buffer plus 26 bytes around it, so 537. Every byte here
 * is stack in every program shecc emits, so it stays close to that.
 */
#define FMT_BUF_LEN 576

#define __is_alpha(c) ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
#define __is_digit(c) ((c >= '0' && c <= '9'))
#define __is_hex(c) \
    (__is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))

int isdigit(int c)
{
    return __is_digit(c);
}

int isalpha(int c)
{
    return __is_alpha(c);
}

int isalnum(int c)
{
    return __is_alpha(c) || __is_digit(c);
}

int isxdigit(int c)
{
    return __is_hex(c);
}

int isblank(int c)
{
    return c == ' ' || c == '\t';
}

int strlen(const char *str)
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

int strcmp(const char *s1, const char *s2)
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

int strncmp(const char *s1, const char *s2, int len)
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

char *strcpy(char *dest, const char *src)
{
    int i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
    return dest;
}

char *strcat(char *dest, const char *src)
{
    strcpy(&dest[strlen(dest)], src);
    return dest;
}

char *strncat(char *dest, const char *src, int len)
{
    int i = strlen(dest), j = 0;
    while (j < len && src[j]) {
        dest[i] = src[j];
        i++;
        j++;
    }
    dest[i] = 0;
    return dest;
}

char *strchr(char *str, int ch)
{
    int i = 0;

    /* Compare both sides as bytes.
     *
     * A byte above 0x7F is the whole difficulty: comparing str[i] against the
     * int the caller passed fails wherever char is signed, since one side is
     * negative and the other is not. Converting the search value to a char is
     * not enough either -- the arm backend widens a char loaded from memory and
     * a char held in a variable differently, so the two disagree even though
     * each promotes to -61 on its own. Masking both to 0..255 leaves nothing to
     * disagree about, on any target.
     *
     * The terminator counts as part of the string, and a masked zero still
     * finds it.
     */
    int want = ch & 0xFF;
    while (str[i]) {
        if ((str[i] & 0xFF) == want)
            return str + i;
        i++;
    }
    if (!want)
        return str + i;
    return NULL;
}

char *strncpy(char *dest, const char *src, int len)
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

char *memcpy(char *dest, const char *src, int count)
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

int memcmp(const void *s1, const void *s2, int n)
{
    /* C99 7.21.4 compares the bytes as unsigned char. */
    const unsigned char *p1 = s1, *p2 = s2;

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

/* Pointer width of the target, held in a variable rather than tested with the
 * preprocessor: shecc must be able to compile this file for either target, and
 * a constant condition would leave statically dead code behind.
 */
int __ptr_width = __SIZEOF_POINTER__;

/* The specification of snprintf() is defined in C99 7.19.6.5, and its behavior
 * and return value should comply with the following description:
 * - If n is zero, nothing is written.
 * - Writes at most n bytes, including the null character.
 * - On success, the return value should be the length of the entire converted
 *   string even if n is insufficient to store it.
 *
 * Thus, a structure fmtbuf_t is defined for formatted output conversion for the
 * functions in the printf() family.
 * @buf: the current position of the buffer.
 * @n : the remaining space of the buffer.
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

    /* Write the given character when n is greater than 1. This means preserving
     * one position for the null character.
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

    /* Write the given string when n is greater than 1. This means preserving
     * one position for the null character.
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

void __fmtbuf_pad(fmtbuf_t *fmtbuf, int ch, int count)
{
    for (; count > 0; count--)
        __fmtbuf_write_char(fmtbuf, ch);
}

/* Conversion flags (C99 7.19.6.1p6) */
#define __FMT_LEFT 1
#define __FMT_PLUS 2
#define __FMT_SPACE 4
#define __FMT_ALT 8
#define __FMT_ZERO 16

/* Convert the 64-bit integer @hi:@lo for conversion @conv, one of d, i, u, o,
 * x, X and p, with @flags, @width and @precision, where a negative precision is
 * none. This avoids long long arithmetic, so that the library needs no 64-bit
 * lowering on a 32-bit target: a digit is the remainder of a bitwise long
 * division of the two words, over the low word alone once the high one is zero.
 */
void __format_int(fmtbuf_t *fmtbuf,
                  unsigned lo,
                  unsigned hi,
                  int conv,
                  int flags,
                  int width,
                  int precision)
{
    char digits[24], prefix[2];
    char *symbols = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    int di = 24, prefix_len = 0, zeros = 0, count, base = 10;

    if (conv == 'o')
        base = 8;
    else if (conv == 'x' || conv == 'X' || conv == 'p')
        base = 16;

    if (conv == 'd' || conv == 'i') {
        if (hi & 0x80000000) {
            prefix[prefix_len++] = '-';
            lo = ~lo + 1;
            hi = ~hi + (lo == 0);
        } else if (flags & __FMT_PLUS)
            prefix[prefix_len++] = '+';
        else if (flags & __FMT_SPACE)
            prefix[prefix_len++] = ' ';
    } else if ((lo || hi) && (conv == 'p' || ((flags & __FMT_ALT) &&
                                              (conv == 'x' || conv == 'X')))) {
        prefix[prefix_len++] = '0';
        prefix[prefix_len++] = conv == 'X' ? 'X' : 'x';
    }

    /* A zero precision converts a zero value to no characters. */
    if (precision || lo || hi) {
        do {
            unsigned rem = 0;

            for (int bit = hi ? 63 : 31; bit >= 0; bit--) {
                unsigned *word = bit >= 32 ? &hi : &lo;
                unsigned mask = 1U << (bit % 32);

                rem = (rem << 1) | ((*word & mask) != 0);
                *word &= ~mask;
                if (rem >= (unsigned) base) {
                    rem -= base;
                    *word |= mask;
                }
            }
            digits[--di] = symbols[rem];
        } while (lo || hi);
    }
    count = 24 - di;

    if (precision > count)
        zeros = precision - count;

    /* The alternate form of o makes the first digit a zero. */
    if ((flags & __FMT_ALT) && conv == 'o' && !zeros &&
        (!count || digits[di] != '0'))
        zeros = 1;

    /* The 0 flag pads with zeros after the sign or prefix, unless a precision
     * is given or the field is left-justified.
     */
    if ((flags & __FMT_ZERO) && !(flags & __FMT_LEFT) && precision < 0 &&
        width > prefix_len + zeros + count)
        zeros = width - prefix_len - count;

    width -= prefix_len + zeros + count;
    if (!(flags & __FMT_LEFT))
        __fmtbuf_pad(fmtbuf, ' ', width);
    __fmtbuf_write_str(fmtbuf, prefix, prefix_len);
    __fmtbuf_pad(fmtbuf, '0', zeros);
    __fmtbuf_write_str(fmtbuf, digits + di, count);
    if (flags & __FMT_LEFT)
        __fmtbuf_pad(fmtbuf, ' ', width);
}

/* Write @length characters of @str in a field of @width, as %s and %c do. */
void __format_str(fmtbuf_t *fmtbuf, char *str, int length, int flags, int width)
{
    width -= length;
    if (!(flags & __FMT_LEFT))
        __fmtbuf_pad(fmtbuf, ' ', width);
    __fmtbuf_write_str(fmtbuf, str, length);
    if (flags & __FMT_LEFT)
        __fmtbuf_pad(fmtbuf, ' ', width);
}

/* @var_args follows @named named arguments. Each variadic argument takes
 * VA_INT_STEP int-sized slots, and a 64-bit one takes two. On a 32-bit target
 * the pair starts at an even slot counted from the first named argument, so one
 * after an odd count of slots is preceded by an unused one.
 *
 * long has the width of int in shecc, so l changes no argument's width; ll and
 * j read 64 bits everywhere, and z and t read a pointer's width.
 */
void __format_to_buf(fmtbuf_t *fmtbuf,
                     const char *format,
                     int *var_args,
                     int named)
{
    int si = 0, slot = 0;

    /* A pointer-width view of the same argument area, for %s, %p and %n.
     * Reading a pointer argument through an int would truncate it on LP64.
     */
    char **var_args_p = (char **) var_args;

    while (format[si]) {
        if (format[si] != '%') {
            __fmtbuf_write_char(fmtbuf, format[si]);
            si++;
            continue;
        }

        int flags = 0, width = 0, precision = -1, size = 0, conv;
        unsigned lo, hi;

        si++;
        for (;; si++) {
            if (format[si] == '-')
                flags |= __FMT_LEFT;
            else if (format[si] == '+')
                flags |= __FMT_PLUS;
            else if (format[si] == ' ')
                flags |= __FMT_SPACE;
            else if (format[si] == '#')
                flags |= __FMT_ALT;
            else if (format[si] == '0')
                flags |= __FMT_ZERO;
            else
                break;
        }

        /* A negative width from '*' is a '-' flag and a positive width. */
        if (format[si] == '*') {
            width = var_args[slot];
            slot += VA_INT_STEP;
            si++;
            if (width < 0) {
                flags |= __FMT_LEFT;
                width = -width;
            }
        } else {
            while (format[si] >= '0' && format[si] <= '9')
                width = width * 10 + format[si++] - '0';
        }

        /* A negative precision from '*' is taken as none. */
        if (format[si] == '.') {
            si++;
            precision = 0;
            if (format[si] == '*') {
                precision = var_args[slot];
                slot += VA_INT_STEP;
                si++;
                if (precision < 0)
                    precision = -1;
            } else {
                while (format[si] >= '0' && format[si] <= '9')
                    precision = precision * 10 + format[si++] - '0';
            }
        }

        /* @size: -2 hh, -1 h, 0 int or long, 8 a 64-bit argument. */
        if (format[si] == 'h') {
            size = -1;
            if (format[++si] == 'h') {
                size = -2;
                si++;
            }
        } else if (format[si] == 'l') {
            if (format[++si] == 'l') {
                size = 8;
                si++;
            }
        } else if (format[si] == 'j') {
            size = 8;
            si++;
        } else if (format[si] == 'z' || format[si] == 't') {
            if (__ptr_width == 8)
                size = 8;
            si++;
        }

        conv = format[si];
        if (!conv)
            break;
        si++;

        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (size == 8) {
                if (__ptr_width == 4 && ((named + slot) & 1))
                    slot++;
                lo = var_args[slot];
                hi = var_args[slot + 1];
                slot += 2;
            } else {
                int v = var_args[slot];

                slot += VA_INT_STEP;
                if (size == -2)
                    v = conv == 'd' || conv == 'i' ? (signed char) v : v & 0xff;
                else if (size == -1)
                    v = conv == 'd' || conv == 'i' ? (short) v : v & 0xffff;
                lo = v;
                hi = (conv == 'd' || conv == 'i') && v < 0 ? 0xffffffff : 0;
            }
            __format_int(fmtbuf, lo, hi, conv, flags, width, precision);
            break;
        case 'p':
            /* A pointer occupies VA_INT_STEP int-sized slots, so on an LP64
             * target the second one carries the high word. A null pointer is
             * spelled "(nil)", as glibc does.
             */
            lo = var_args[slot];
            hi = VA_INT_STEP > 1 ? var_args[slot + 1] : 0;
            slot += VA_INT_STEP;
            if (lo || hi)
                __format_int(fmtbuf, lo, hi, 'p', flags & __FMT_LEFT, width,
                             -1);
            else
                __format_str(fmtbuf, "(nil)", 5, flags, width);
            break;
        case 'c': {
            char ch = (char) var_args[slot];

            slot += VA_INT_STEP;
            __format_str(fmtbuf, &ch, 1, flags, width);
            break;
        }
        case 's': {
            /* A precision bounds the characters read, so the array need not be
             * null-terminated (C99 7.19.6.1p8).
             */
            char *str = var_args_p[slot / VA_INT_STEP];
            int length = 0;

            slot += VA_INT_STEP;
            if (!str)
                str = "(null)";
            while ((precision < 0 || length < precision) && str[length])
                length++;
            __format_str(fmtbuf, str, length, flags, width);
            break;
        }
        case 'n': {
            /* Store the count of characters written so far. */
            int *count = (int *) var_args_p[slot / VA_INT_STEP];

            slot += VA_INT_STEP;
            if (size == -2)
                *(char *) count = fmtbuf->len;
            else if (size == -1)
                *(short *) count = fmtbuf->len;
            else {
                count[0] = fmtbuf->len;
                if (size == 8)
                    count[1] = 0;
            }
            break;
        }
        case '%':
            __fmtbuf_write_char(fmtbuf, '%');
            break;
        default:
            /* An unknown conversion is written as it stands. */
            __fmtbuf_write_char(fmtbuf, '%');
            __fmtbuf_write_char(fmtbuf, conv);
            break;
        }
    }

    /* If n is still greater than 0, set the null character. */
    if (fmtbuf->n)
        fmtbuf->buf[0] = 0;
}

int __write_fmt(int fd, const char *str, int *var_args, int named)
{
    char buffer[FMT_BUF_LEN];
    fmtbuf_t fmtbuf;

    fmtbuf.buf = buffer;
    fmtbuf.n = FMT_BUF_LEN;
    fmtbuf.len = 0;
    __format_to_buf(&fmtbuf, str, var_args, named);

    /* len counts what the conversion would have produced, not what fit. */
    int len = fmtbuf.len;
    if (len < FMT_BUF_LEN)
        return __syscall(__syscall_write, fd, buffer, len);

    /* Longer than the staging buffer. Format it again into one that holds it,
     * rather than writing a silently truncated line. Re-reading var_args is
     * safe: __format_to_buf() only reads it.
     */
    char *wide = malloc(len + 1);

    if (!wide)
        return __syscall(__syscall_write, fd, buffer, FMT_BUF_LEN - 1);

    fmtbuf.buf = wide;
    fmtbuf.n = len + 1;
    fmtbuf.len = 0;
    __format_to_buf(&fmtbuf, str, var_args, named);

    int written = __syscall(__syscall_write, fd, wide, fmtbuf.len);

    free(wide);
    return written;
}

int printf(const char *str, ...)
{
    return __write_fmt(1, str, &str + 1, 1);
}

int sprintf(char *buffer, const char *str, ...)
{
    fmtbuf_t fmtbuf;

    fmtbuf.buf = buffer;
    fmtbuf.n = INT_MAX;
    fmtbuf.len = 0;
    __format_to_buf(&fmtbuf, str, &str + 1, 2);
    return fmtbuf.len;
}

int snprintf(char *buffer, int n, const char *str, ...)
{
    fmtbuf_t fmtbuf;

    fmtbuf.buf = buffer;
    fmtbuf.n = n;
    fmtbuf.len = 0;
    __format_to_buf(&fmtbuf, str, &str + 1, 3);
    return fmtbuf.len;
}

int __free_all(void);

int fprintf(FILE *stream, const char *str, ...)
{
    return __write_fmt(stream, str, &str + 1, 2);
}

int fflush(FILE *stream)
{
    /* shecc's libc performs no user-space buffering. */
    return 0;
}

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

/* C99 7.2.1.1 has the message name the expression, the source file, the line
 * and the enclosing function.
 */
void __assert_fail(const char *expr,
                   const char *file,
                   unsigned int line,
                   const char *function)
{
    fprintf(stderr, "Assertion failed: %s, function %s, file %s, line %d\n",
            expr, function, file, line);
    abort();
}

FILE *fopen(const char *filename, const char *mode)
{
    int fd;

    if (!strcmp(mode, "w") || !strcmp(mode, "wb")) {
        /* Flags below are O_WRONLY | O_CREAT | O_TRUNC. Without O_TRUNC,
         * writing a shorter file over a longer one leaves the old tail in place
         * -- which turns a rebuilt executable into the new image followed by a
         * fragment of the previous one.
         *
         * "wb" writes an executable and opens 0775; "w" writes text, which has
         * no business being executable, and opens 0666 before the umask.
         */
        int perm = 0x1b6;

        if (!strcmp(mode, "wb"))
            perm = 0x1fd;
#if defined(__riscv) || defined(__aarch64__)
        /* FIXME: mode not work currently in RISC-V */
        fd = __syscall(__syscall_openat, -100, filename, 577, perm);
#else
        fd = __syscall(__syscall_open, filename, 577, perm);
#endif
    } else if (!strcmp(mode, "r") || !strcmp(mode, "rb")) {
#if defined(__riscv) || defined(__aarch64__)
        fd = __syscall(__syscall_openat, -100, filename, 0, 0);
#else
        fd = __syscall(__syscall_open, filename, 0, 0);
#endif
    } else
        return NULL;

    /* open(2) reports failure as a negative errno rather than as NULL, so a
     * caller's "if (!fp)" would sail straight past it. Fold it into NULL here
     * and every call site gets the standard test for free.
     */
    if (fd < 0)
        return NULL;
    return (FILE *) fd;
}

int fclose(FILE *stream)
{
    __syscall(__syscall_close, stream);
    return 0;
}

int chmod(const char *filename, int mode)
{
#if defined(__riscv) || defined(__aarch64__)
    /* sys_fchmodat takes (dirfd, filename, mode); AT_FDCWD is -100. */
    return __syscall(__syscall_fchmodat, -100, filename, mode);
#else
    return __syscall(__syscall_chmod, filename, mode);
#endif
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

int fseek(FILE *stream, int offset, int whence)
{
    int result;

    /* RV32 has only _llseek, which splits the offset and returns the result
     * through a pointer. Every other target takes (fd, offset, whence)
     * directly. lib/c.h rejects an architecture that is neither.
     */
#if defined(__riscv)
    result = __syscall(__syscall_lseek, stream, 0, offset, NULL, whence);
#else
    result = __syscall(__syscall_lseek, stream, offset, whence);
#endif
    return result == -1;
}

int ftell(FILE *stream)
{
    /* See fseek(): only RV32 needs the split-offset _llseek form. */
#if defined(__riscv)
    int result;
    __syscall(__syscall_lseek, stream, 0, 0, &result, SEEK_CUR);
    return result;
#else
    return __syscall(__syscall_lseek, stream, 0, SEEK_CUR);
#endif
}

#define CHUNK_SIZE_FREED_MASK 1
#define CHUNK_SIZE_SZ_MASK 0xFFFFFFFE
#define CHUNK_GET_SIZE(size) (size & CHUNK_SIZE_SZ_MASK)
#define IS_CHUNK_GET_FREED(size) (size & CHUNK_SIZE_FREED_MASK)

typedef struct __chunk {
    struct __chunk *next, *prev;
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
        chunk_t *tmp = (chunk_t *) __syscall(__syscall_mmap2, NULL,
                                             __align_up(sizeof(chunk_t)), prot,
                                             flags, -1, 0);
        if (tmp == (void *) -1)
            return NULL;
        __alloc_head = tmp;
        __alloc_tail = tmp;
        __alloc_head->next = NULL;
        __alloc_head->prev = NULL;
        __alloc_head->size = 0;
    }

    if (!__freelist_head) {
        chunk_t *tmp = (chunk_t *) __syscall(__syscall_mmap2, NULL,
                                             __align_up(sizeof(chunk_t)), prot,
                                             flags, -1, 0);
        if (tmp == (void *) -1)
            return NULL;
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
        allocated = (chunk_t *) __syscall(__syscall_mmap2, NULL,
                                          __align_up(sizeof(chunk_t) + size),
                                          prot, flags, -1, 0);
        if (allocated == (void *) -1)
            return NULL;
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
