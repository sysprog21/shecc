/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once
/* Declarations of C standard library functions */

#define NULL 0

#define bool _Bool
#define true 1
#define false 0

#define INT_MAX 0x7fffffff
#define INT_MIN 0x80000000

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

/* Non-portable: Assume page size is 4KiB */
#define PAGESIZE 4096

/* Minimum alignment for all memory allocations. */
#define MIN_ALIGNMENT 8
#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

/* va_list support for variadic functions */
typedef int *va_list;

/* File I/O */
typedef int FILE;
FILE *fopen(char *filename, char *mode);
int fclose(FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *str, int n, FILE *stream);
int fputc(int c, FILE *stream);

/* string-related functions */
int strlen(char *str);
int strcmp(char *s1, char *s2);
int strncmp(char *s1, char *s2, int len);
char *strcpy(char *dest, char *src);
char *strncpy(char *dest, char *src, int len);
char *memcpy(char *dest, char *src, int count);
int memcmp(void *s1, void *s2, int n);
void *memset(void *s, int c, int n);

/* formatted output string */
int printf(char *str, ...);
int sprintf(char *buffer, char *str, ...);
int snprintf(char *buffer, int n, char *str, ...);

/* Terminating program */
void exit(int exit_code);
void abort(void);

/* Dynamic memory allocation/deallocation functions */
void *malloc(int size);
void *calloc(int n, int size);
void free(void *ptr);
