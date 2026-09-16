/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once
/* Declarations of C standard library functions */

#define NULL 0

#define bool _Bool
#define true 1
#define false 0

#define INT_MAX 0x7fffffff
#define INT_MIN 0x80000000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Pointer width and syscall table are independent axes: RV32 and AArch64 share
 * the asm-generic table but not the width, while Arm32 and x86-64 each carry a
 * legacy table of their own. Selecting on each separately keeps one copy of the
 * numbers.
 */
#if defined(__arm__) || defined(__riscv)
#define __SIZEOF_POINTER__ 4
#elif defined(__aarch64__) || defined(__x86_64__)
#define __SIZEOF_POINTER__ 8
#else
#error "Unsupported architecture"
#endif

#if defined(__arm__)
#define __syscall_exit 1
#define __syscall_read 3
#define __syscall_write 4
#define __syscall_close 6
#define __syscall_open 5
#define __syscall_lseek 19
#define __syscall_chmod 15
#define __syscall_mmap2 192
#define __syscall_munmap 91

/* RV32 and AArch64 both use the asm-generic table. */
#elif defined(__riscv) || defined(__aarch64__)
#define __syscall_exit 93
#define __syscall_read 63
#define __syscall_write 64
#define __syscall_close 57
#define __syscall_open 1024
#define __syscall_openat 56
#define __syscall_lseek 62

/* That table has no chmod at all -- 90 is capget there -- so path-based mode
 * changes go through fchmodat(2).
 */
#define __syscall_fchmodat 53
#define __syscall_mmap2 222
#define __syscall_munmap 215

#elif defined(__x86_64__)
#define __syscall_exit 60
#define __syscall_read 0
#define __syscall_write 1
#define __syscall_close 3
#define __syscall_open 2
#define __syscall_openat 257
#define __syscall_lseek 8
#define __syscall_chmod 90
#define __syscall_mmap 9
#define __syscall_munmap 11

/* x86-64 provides no mmap2. Every call site passes offset 0, so mmap2's
 * page-granular offset is indistinguishable from mmap's byte offset here.
 */
#define __syscall_mmap2 9

#else
#error "Unsupported architecture"
#endif

/* Non-portable: Assume page size is 4KiB */
#define PAGESIZE 4096

/* Minimum alignment for all memory allocations. */
#define MIN_ALIGNMENT 8
#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

/* va_list support for variadic functions */
typedef int *va_list;

/* Every variadic argument occupies one pointer-sized stack slot, so an
 * int-based va_list must advance this many elements per argument: one on the
 * 32-bit targets, two on LP64.
 */
#define VA_INT_STEP (__SIZEOF_POINTER__ / 4)

/* Character predicate functions */
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isxdigit(int c);
int isblank(int c);

/* File I/O */
typedef int FILE;

#ifdef __SHECC_DYNLINK__
/* The host libc's own stream objects. Its stdio functions dereference the
 * stream pointer they are given, so a descriptor number would fault there. The
 * output cannot import a data symbol, only functions through the PLT, so each
 * use asks the dynamic linker for the address of the host's variable. A null
 * handle is RTLD_DEFAULT. Before glibc 2.34 dlsym() lived in libdl.so.2, so a
 * program that calls it names that library as well.
 */
void *dlsym(void *handle, const char *name);
#define stdin (*(FILE **) dlsym((void *) 0, "stdin"))
#define stdout (*(FILE **) dlsym((void *) 0, "stdout"))
#define stderr (*(FILE **) dlsym((void *) 0, "stderr"))
#else
/* Standard streams, as raw file descriptors */
#define stdin ((FILE *) 0)
#define stdout ((FILE *) 1)
#define stderr ((FILE *) 2)
#endif

FILE *fopen(const char *filename, const char *mode);
int fclose(FILE *stream);
int chmod(const char *filename, int mode);
int fgetc(FILE *stream);
char *fgets(char *str, int n, FILE *stream);
int fputc(int c, FILE *stream);

/* Only under dynamic linking, where the host libc supplies them and buffers
 * behind them. A statically linked program has neither, and moves whole blocks
 * through '__syscall' instead.
 */
int fread(char *ptr, int size, int nmemb, FILE *stream);
int fwrite(const void *ptr, int size, int nmemb, FILE *stream);
int fseek(FILE *stream, int offset, int whence);
int ftell(FILE *stream);

/* string-related functions */
int strlen(const char *str);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, int len);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, int len);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, int len);
char *strchr(char *str, int ch);
void *memcpy(void *dest, const void *src, int count);
int memcmp(const void *s1, const void *s2, int n);
void *memset(void *s, int c, int n);

/* formatted output string */
int printf(const char *str, ...);
int sprintf(char *buffer, const char *str, ...);
int snprintf(char *buffer, int n, const char *str, ...);
int fprintf(FILE *stream, const char *str, ...);
int fflush(FILE *stream);

/* Terminating program */
void exit(int exit_code);
void abort(void);

/* glibc's signature, so a dynamically linked program reaches the host's handler
 * with every argument it reads.
 */
void __assert_fail(const char *expr,
                   const char *file,
                   unsigned int line,
                   const char *function);

/* Dynamic memory allocation/deallocation functions */
void *malloc(int size);
void *calloc(int n, int size);
void free(void *ptr);
