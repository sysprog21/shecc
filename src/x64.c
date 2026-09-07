/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* x86-64 instruction encoding */

/* Identifier naming conventions
 *   - prefix emit_ : writes encoded bytes into the code section.
 *   - prefix patch_ : rewrites bytes already emitted.
 *   - no prefix (modrm, reg_low3) : pure encoding arithmetic.
 *
 * An x86-64 instruction is a variable-length sequence -- prefix, opcode, ModRM,
 * SIB, displacement, immediate -- so unlike src/arm.c and src/riscv.c, which
 * return one fixed-width word, there is nothing to return. The helpers append
 * to elf_code and the caller composes them:
 *
 *     emit_rex(1, rd, base);        REX.W, extension bits from the registers
 *     emit_byte(0x8D);              LEA
 *     emit_mem_base(rd, base, ofs, true);   ModRM + SIB + displacement
 *
 * What belongs here is anything whose arguments are purely architectural --
 * register numbers, immediates, displacements, opcode bytes. Anything that
 * reads shecc's IR, the register allocator's state or a basic block belongs in
 * src/x64-codegen.c.
 */

#include "defs.h"

/* REX prefix bits */
#define REX_W 0x48 /* 64-bit operand size */
#define REX_R 0x44 /* ModR/M reg extension */
#define REX_X 0x42 /* SIB index extension */
#define REX_B 0x41 /* ModR/M r/m extension */
#define REX_BASE 0x40

/* ModR/M byte encoding */
#define MOD_INDIRECT 0x00 /* [reg] */
#define MOD_DISP8 0x40    /* [reg + disp8] */
#define MOD_DISP32 0x80   /* [reg + disp32] */
#define MOD_DIRECT 0xC0   /* reg */

/* ModR/M opcode extensions selecting which shift 0xC1 encodes */
#define SHIFT_EXT_SHL 4
#define SHIFT_EXT_SAR 7

/* ModR/M byte construction: mod (2 bits) | reg (3 bits) | r/m (3 bits)
 * Use a helper function instead of macro to avoid complex macro expansion
 * during stage0 parsing.
 */
int modrm(int mod, int reg, int rm)
{
    return ((mod & 0xC0) | ((reg & 0x07) << 3) | (rm & 0x07));
}

/* Extract low 3 bits of register for ModR/M encoding */
int reg_low3(int reg)
{
    return reg & 0x07;
}

/* Helper to emit bytes to the code buffer Takes an int because shecc has no
 * 'unsigned': every opcode above 0x7F would otherwise be a value the call site
 * cannot write and the conversion silently changes. The narrowing to the byte
 * actually emitted happens here, once and on purpose, rather than 134 times
 * implicitly.
 */
void emit_byte(int byte)
{
    strbuf_putc(elf_code, (char) (byte & 0xff));
}

/* Emit a REX prefix. 'w' selects a 64-bit operand size; the two register
 * numbers supply the extension bits for the ModRM reg and r/m fields, so a
 * caller naming a high register (r8-r15) gets REX.R or REX.B automatically.
 * Pass -1 for a field the instruction does not use. REX prefix for reg/rm, and
 * for the SIB index when there is one. @index of -1 means the operand names no
 * index.
 */
void emit_rex_sib(int w, int reg, int rm, int index)
{
    int rex = REX_BASE;
    if (w)
        rex = rex | REX_W;
    if (reg >= 8)
        rex = rex | REX_R;
    if (index >= 8)
        rex = rex | REX_X;
    if (rm >= 8)
        rex = rex | REX_B;
    emit_byte(rex);
}

void emit_rex(int w, int reg, int rm)
{
    emit_rex_sib(w, reg, rm, -1);
}

void emit_dword(int dword)
{
    elf_write_int(elf_code, dword);
}

/* PUSH/POP of any of the sixteen registers. */
void emit_push_reg(int reg)
{
    if (reg >= 8)
        emit_byte(REX_B);
    emit_byte(0x50 + reg_low3(reg));
}

void emit_pop_reg(int reg)
{
    if (reg >= 8)
        emit_byte(REX_B);
    emit_byte(0x58 + reg_low3(reg));
}

/* ModRM (plus SIB and displacement as needed) naming [base] or [base + disp].
 * R12 and RSP share the low three bits that mean "a SIB byte follows", and R13
 * and RBP share the ones that mean "RIP-relative"; both therefore need a longer
 * encoding than the other registers.
 */
void emit_mem_base(int reg_field, int base, int disp, bool have_disp)
{
    int b = reg_low3(base);
    int mod = MOD_INDIRECT;
    if (!have_disp && b == 5) {
        have_disp = true; /* [r13] and [rbp] have no zero-displacement form */
        disp = 0;
    }
    if (have_disp)
        mod = disp >= -128 && disp <= 127 ? MOD_DISP8 : MOD_DISP32;
    emit_byte(modrm(mod, reg_low3(reg_field), b));
    if (b == 4)
        emit_byte(0x24); /* SIB: no index, base is the register itself */
    if (mod == MOD_DISP8)
        emit_byte(disp);
    else if (mod == MOD_DISP32)
        emit_dword(disp);
}

/* ModRM and SIB naming [base + index * (1 << scale) + disp]. */
void emit_mem_sib(int reg_field, int base, int index, int scale, int disp)
{
    int b = reg_low3(base);
    int mod = MOD_INDIRECT;

    /* RBP and R13 have no zero-displacement form, so spell one out. */
    if (disp || b == 5)
        mod = (disp >= -128 && disp <= 127) ? MOD_DISP8 : MOD_DISP32;

    emit_byte(modrm(mod, reg_low3(reg_field), 4)); /* rm = 100: SIB follows */
    emit_byte((scale << 6) | (reg_low3(index) << 3) | b);
    if (mod == MOD_DISP8)
        emit_byte(disp);
    else if (mod == MOD_DISP32)
        emit_dword(disp);
}

/* Emit the ModRM/displacement bytes for a [R15 + ofs] memory operand.
 *
 * R15 holds the base of the global area. Its low three bits are 7, so no SIB
 * byte is needed and offset 0 can use the plain indirect form.
 */
void emit_r15_mem(int reg_low, int ofs)
{
    emit_mem_base(reg_low, 15, ofs, ofs != 0);
}

/* Emit the ModRM/SIB/displacement bytes for a [RSP + ofs] memory operand.
 *
 * The shared phase-2 IR addresses locals as "sp + offset", with offsets growing
 * upward, so the backend must too: an array at sp+16 has its second element at
 * sp+24. Using RBP with negated offsets would invert that and make b[i] walk
 * backwards over neighbouring slots.
 *
 * RSP as a base register always requires a SIB byte; 0x24 encodes "base = RSP,
 * no index".
 */
void emit_rsp_mem(int reg_field, int ofs)
{
    emit_mem_base(reg_field, 4, ofs, true);
}

/* Store a little-endian value into already-emitted code.
 *
 * A patch site sits at whatever offset the instruction that needs it happened
 * to land on, so a wider store there is misaligned; writing the four bytes
 * explicitly also stops the encoding depending on the host's byte order.
 */
void patch_dword(int at, int val)
{
    char *code_ptr = elf_code->elements + at;

    code_ptr[0] = val & 0xFF;
    code_ptr[1] = (val >> 8) & 0xFF;
    code_ptr[2] = (val >> 16) & 0xFF;
    code_ptr[3] = (val >> 24) & 0xFF;
}

void patch_qword(int at, int val)
{
    patch_dword(at, val);
    patch_dword(at + 4, 0);
}

/* Move @src into @dst keeping only its low @width bytes, sign-extended, when
 * @narrow says the upper ones do not belong to the value; otherwise copy it
 * whole. @width of 8 is always a plain copy.
 *
 * This is both what a narrowing conversion emits and how a register that
 * mirrors a slot narrower than itself is read back: a store narrower than the
 * register wrote only its low bytes, and OP_load reads those sign-extended, so
 * sign-extending the register does the same thing without going near memory.
 */
void emit_narrow_move(int dst, int src, int width, bool narrow)
{
    if (!narrow || width >= 8) {
        if (dst == src)
            return;
        emit_rex(1, src, dst); /* MOV dst, src */
        emit_byte(0x89);
        emit_byte(modrm(MOD_DIRECT, reg_low3(src), reg_low3(dst)));
        return;
    }
    if (width == 4) {
        emit_rex(1, dst, src); /* MOVSXD dst, src32 */
        emit_byte(0x63);
        emit_byte(modrm(MOD_DIRECT, reg_low3(dst), reg_low3(src)));
        return;
    }
    /* MOVSX dst, src8 / src16 */
    emit_rex(1, dst, src);
    emit_byte(0x0F);
    emit_byte(width == 1 ? 0xBE : 0xBF);
    emit_byte(modrm(MOD_DIRECT, reg_low3(dst), reg_low3(src)));
}

/* MOV dst, src, or nothing when the value is already in the destination. */
void emit_mov_reg(int dst, int src)
{
    emit_narrow_move(dst, src, 8, false);
}

/* Shift @reg by an immediate count. @ext picks the shift: SHIFT_EXT_SHL or
 * SHIFT_EXT_SAR.
 */
void emit_shift_imm(int reg, int ext, int imm)
{
    emit_rex(1, -1, reg);
    emit_byte(0xC1);
    emit_byte(modrm(MOD_DIRECT, ext, reg_low3(reg)));
    emit_byte(imm);
}

/* LEA rd, [base + index * scale] -- computes a sum into a third register in one
 * instruction, where the two-operand forms need MOV then ADD/SHL.
 *
 * Returns false when the encoding cannot express the operands, so callers fall
 * back: RSP cannot be an index at all and R12 shares its low three bits, while
 * RBP and R13 need a displacement this zero-displacement form does not carry.
 */
bool emit_lea_sum(int rd, int base, int index)
{
    if (reg_low3(base) == 5 || reg_low3(index) == 4)
        return false;
    emit_rex_sib(1, rd, base, index);
    emit_byte(0x8D);
    emit_mem_sib(rd, base, index, 0, 0);
    return true;
}

/* Pad with @n bytes that do nothing.
 *
 * x86 has a multi-byte NOP, so a run of padding costs one instruction instead
 * of one per byte -- which matters because alignment padding in front of a loop
 * header is executed on every entry to the loop. The encodings are spelled out
 * rather than tabulated because shecc cannot compile the initialiser such a
 * table needs, and this file builds under shecc itself.
 */
void emit_nop_bytes(int n)
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

/* LEA rd, [base + disp] -- adds a literal into a different register in one
 * instruction, where MOV plus ADD needs two. This is the two-operand LEA form,
 * which is full speed, unlike the scaled-index form.
 */
bool emit_lea_disp(int rd, int base, int disp)
{
    /* RSP and R12 need a SIB byte to be addressed at all, so they are left to
     * the two-instruction form.
     */
    if (reg_low3(base) == 4)
        return false;

    emit_rex(1, rd, base);
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

/* op rd, imm -- @ext is the opcode extension selecting the operation. Uses the
 * sign-extended imm8 form when it fits.
 */
void emit_alu_imm(int rd, int ext, int imm)
{
    emit_rex(1, -1, rd);
    if (imm >= -128 && imm <= 127) {
        emit_byte(0x83);
        emit_byte(modrm(MOD_DIRECT, ext, reg_low3(rd)));
        emit_byte(imm);
        return;
    }
    emit_byte(0x81);
    emit_byte(modrm(MOD_DIRECT, ext, reg_low3(rd)));
    emit_dword(imm);
}
