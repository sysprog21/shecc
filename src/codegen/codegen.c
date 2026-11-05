/*
 * Select the backend from the same target configuration used by the ELF writer
 * and the optimizer. Each target keeps encoding and lowering together.
 */
#if ELF_MACHINE == ELF_MACHINE_ARM32
#include "arm/codegen.c"
#elif ELF_MACHINE == ELF_MACHINE_AARCH64
#include "arm64/codegen.c"
#elif ELF_MACHINE == ELF_MACHINE_RV32
#include "riscv/codegen.c"
#elif ELF_MACHINE == ELF_MACHINE_X86_64
#include "x64/codegen.c"
#else
#error Unsupported code generation target
#endif
