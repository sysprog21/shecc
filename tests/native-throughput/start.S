/* Minimal native startup shared by both GCC benchmark targets. */
.text
.global _start
_start:
#if defined(__aarch64__)
 bl main
 mov x8, #93
 svc #0
#elif defined(__x86_64__)
 call main
 mov %eax, %edi
 mov $60, %eax
 syscall
#else
#error Unsupported native benchmark architecture
#endif
.section .note.GNU-stack,"",@progbits
