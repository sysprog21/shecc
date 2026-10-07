#define main compiler_main
#include "../src/main.c"
#undef main
#include <assert.h>
void reference_emit_nop_bytes(int n)
{
    while (n > 0) {
        int chunk = n > 9 ? 9 : n;
        n -= chunk;

        /* The 6- and 9-byte forms are the 5- and 8-byte ones behind an
         * operand-size prefix.
         */
        if (chunk == 6 || chunk == 9) {
            emit_byte(0x66);
            chunk--;
        }
        if (chunk == 1) {
            emit_byte(0x90);
            continue;
        }
        if (chunk == 2) {
            emit_byte(0x66);
            emit_byte(0x90);
            continue;
        }
        emit_byte(0x0F);
        emit_byte(0x1F);
        if (chunk == 3)
            emit_byte(0x00);
        else if (chunk == 4) {
            emit_byte(0x40);
            emit_byte(0x00);
        } else if (chunk == 5) {
            emit_byte(0x44);
            emit_byte(0x00);
            emit_byte(0x00);
        } else if (chunk == 7) {
            emit_byte(0x80);
            emit_dword(0);
        } else { /* 8 */
            emit_byte(0x84);
            emit_byte(0x00);
            emit_dword(0);
        }
    }
}
bool reference_emit_lea_disp_width(int rd, int base, int disp, bool wide)
{
    /* RSP and R12 need a SIB byte to be addressed at all, so they are left to
     * the two-instruction form.
     */
    if (reg_low3(base) == 4)
        return false;

    if (wide || rd >= 8 || base >= 8)
        emit_rex(wide, rd, base);
    emit_byte(0x8D);
    if (disp >= -128 && disp <= 127) {
        emit_byte(modrm(MOD_DISP8, reg_low3(rd), reg_low3(base)));
        emit_byte(disp);
        return true;
    }

    /* A wider displacement is still one instruction, and still shorter than
     * copying the source and then adding to it.
     */
    emit_byte(modrm(MOD_DISP32, reg_low3(rd), reg_low3(base)));
    emit_dword(disp);
    return true;
}
static void compare(strbuf_t *expected)
{
    assert(expected->size == elf_code->size);
    assert(!memcmp(expected->elements, elf_code->elements, expected->size));
    strbuf_free(expected);
    strbuf_free(elf_code);
}
int main(void)
{
    for (int n = -2; n <= 1024; n++) {
        elf_code = strbuf_create(128);
        reference_emit_nop_bytes(n);
        strbuf_t *expected = elf_code;
        elf_code = strbuf_create(128);
        emit_nop_bytes(n);
        compare(expected);
    }
    int disps[] = {-2147483647 - 1, -129, -128, -1, 0, 1, 127, 128, 2147483647};
    for (int rd = 0; rd < 16; rd++)
        for (int base = 0; base < 16; base++)
            for (int wide = 0; wide < 2; wide++)
                for (unsigned d = 0; d < sizeof(disps) / sizeof(disps[0]);
                     d++) {
                    elf_code = strbuf_create(128);
                    bool before =
                        reference_emit_lea_disp_width(rd, base, disps[d], wide);
                    strbuf_t *expected = elf_code;
                    elf_code = strbuf_create(128);
                    assert(before ==
                           emit_lea_disp_width(rd, base, disps[d], wide));
                    compare(expected);
                }
    return 0;
}
