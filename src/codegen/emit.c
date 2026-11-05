/*
 * Shared fixed-width instruction emission for ARM, AArch64 and RISC-V. Counting
 * saturates so an oversized operation cannot overflow the planner.
 */
void emit(int code)
{
    if (mop_count_only) {
        if (mop_counted_words < INT_MAX)
            mop_counted_words++;
    } else {
        elf_write_int(elf_code, code);
    }
}
