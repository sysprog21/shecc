/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* ELF file manipulation */

#include "../config"
#include "defs.h"
#include "globals.c"

#ifndef PAGESIZE
#define PAGESIZE 4096
#endif

static bool elf_dynamic_sections_ready(const strbuf_t *relplt)
{
    return !dynlink ||
           (dynamic_sections.elf_interp && relplt && dynamic_sections.elf_plt &&
            dynamic_sections.elf_got && dynamic_sections.elf_dynstr &&
            dynamic_sections.elf_dynsym && dynamic_sections.elf_dynamic);
}

int elf_symbol_index = 0;

void elf_write_str(strbuf_t *elf_array, const char *vals)
{
    /* Note that strbuf_puts() does not push the null character.
     *
     * If necessary, use elf_write_byte() to append the null character after
     * calling elf_write_str().
     */
    if (!elf_array || !vals)
        return;
    strbuf_puts(elf_array, vals);
}

void elf_write_byte(strbuf_t *elf_array, int val)
{
    if (!elf_array)
        return;
    strbuf_putc(elf_array, val);
}

char e_extract_byte(int v, int b)
{
    return (char) ((v >> (b << 3)) & 0xFF);
}

void elf_write_int(strbuf_t *elf_array, int val)
{
    if (!elf_array)
        return;
    for (int i = 0; i < 4; i++)
        strbuf_putc(elf_array, e_extract_byte(val, i));
}

/* Write a 16-bit little-endian value. */
void elf_write_short(strbuf_t *elf_array, int val)
{
    strbuf_putc(elf_array, val & 0xff);
    strbuf_putc(elf_array, (val >> 8) & 0xff);
}

/* Write a 64-bit little-endian value from a 32-bit quantity.
 *
 * shecc has no 64-bit integer type, and every address and size this compiler
 * emits fits in 32 bits (ELF_START is 0x10000), so the upper word is always
 * zero.
 */
void elf_write_quad(strbuf_t *elf_array, int val)
{
    elf_write_int(elf_array, val);
    elf_write_int(elf_array, 0);
}

void elf_write_blk(strbuf_t *elf_array, const void *blk, int sz)
{
    if (!elf_array || !blk || sz <= 0 || !strbuf_extend(elf_array, sz))
        return;
    memcpy(elf_array->elements + elf_array->size, blk, sz);
    elf_array->size += sz;
}

#if ELF_IS_64 == 0
static void elf_write_section_header(const elf32_shdr_t *shdr)
{
    elf_write_blk(elf_section_header, shdr, sizeof(*shdr));
}
#endif

/* The dynamic-linking tables differ only in width between the two ELF classes,
 * so the generator below writes them through these helpers rather than
 * memcpy-ing a struct: shecc has no 64-bit integer type, so an ELF64 entry
 * cannot be expressed as a C struct here at all.
 */

int elf_sym_size(void)
{
#if ELF_IS_64 == 1
    return 24;
#else
    return sizeof(elf32_sym_t);
#endif
}

int elf_jmprel_size(void)
{
#if ELF_IS_64 == 1
    return 24; /* Elf64_Rela */
#else
    if (dynamic_sections.use_relaplt)
        return sizeof(elf32_rela_t);
    return sizeof(elf32_rel_t);
#endif
}

/* One entry of the dynamic table. */
void elf_write_dyn(strbuf_t *buf, int tag, int val)
{
#if ELF_IS_64 == 1
    elf_write_quad(buf, tag);
    elf_write_quad(buf, val);
#else
    elf_write_int(buf, tag);
    elf_write_int(buf, val);
#endif
}

/* One .dynsym entry. The two classes order the fields differently. */
void elf_write_dynsym(strbuf_t *buf, int st_name, int st_info, int st_value)
{
#if ELF_IS_64 == 1
    elf_write_int(buf, st_name);
    elf_write_byte(buf, st_info);
    elf_write_byte(buf, 0);  /* st_other */
    elf_write_short(buf, 0); /* st_shndx */
    elf_write_quad(buf, st_value);
    elf_write_quad(buf, 0); /* st_size */
#else
    elf_write_int(buf, st_name);
    elf_write_int(buf, st_value);
    elf_write_int(buf, 0); /* st_size */
    elf_write_byte(buf, st_info);
    elf_write_byte(buf, 0);  /* st_other */
    elf_write_short(buf, 0); /* st_shndx */
#endif
}

/* One PLT relocation naming a dynamic symbol. */
void elf_write_jmprel(strbuf_t *buf, int offset, int sym_idx)
{
#if ELF_IS_64 == 1
    elf_write_quad(buf, offset);

    /* r_info is (symbol << 32) | type, so the two halves are written in
     * little-endian order as type followed by symbol index.
     */
    elf_write_int(buf, R_ARCH_JUMP_SLOT);
    elf_write_int(buf, sym_idx);
    elf_write_quad(buf, 0); /* r_addend */
#else
    elf_write_int(buf, offset);
    elf_write_int(buf, (sym_idx << 8) | R_ARCH_JUMP_SLOT);
    if (dynamic_sections.use_relaplt)
        elf_write_int(buf, 0); /* r_addend */
#endif
}

/* One pointer-sized value: eight bytes on an ELF64 target, four on ELF32. Used
 * for GOT slots and for any data-section object holding an address.
 */
void elf_write_ptr(strbuf_t *buf, int val)
{
#if ELF_IS_64 == 1
    elf_write_quad(buf, val);
#else
    elf_write_int(buf, val);
#endif
}

/* Place the dynamic sections. Every start is derived from the end of .rodata,
 * so a backend whose final code size is only known after emission can call this
 * again once it is.
 */
void elf_layout_dynamic(void)
{
    /* Everything below is placed from the end of .rodata, and .got holds
     * pointers, so that end has to be pointer-aligned. Padding here rather than
     * in the caller keeps the two runs of this function in agreement -- the pad
     * is already present the second time, so it is idempotent -- which a
     * backend that bakes the PLT address into its calls, as AArch64's BL does,
     * depends on.
     */
    while ((elf_rodata_start + elf_rodata->size) % PTR_SIZE)
        elf_write_byte(elf_rodata, 0);

    int relplt_bytes = dynamic_sections.use_relaplt
                           ? dynamic_sections.relaplt_size
                           : dynamic_sections.relplt_size;
    int interp_bytes = strlen(DYN_LINKER) + 1;
    interp_bytes = ALIGN_UP(interp_bytes, PTR_SIZE);

    int ro_end = elf_rodata_start + elf_rodata->size;
    if (dynamic_sections.use_relaplt)
        dynamic_sections.elf_relaplt_start = ro_end;
    else
        dynamic_sections.elf_relplt_start = ro_end;
    dynamic_sections.elf_plt_start = ro_end + relplt_bytes;

    /* .interp opens the second load segment, so start it a page clear of the
     * first: the two must not share a page, and the offset must stay congruent
     * to the address modulo the page size.
     */
    dynamic_sections.elf_interp_start =
        dynamic_sections.elf_plt_start + dynamic_sections.plt_size + PAGESIZE;
    dynamic_sections.elf_got_start =
        dynamic_sections.elf_interp_start + interp_bytes;
}

/* Address of .dynamic, which follows .got, .dynstr and .dynsym. */
int elf_dynamic_start(void)
{
    return dynamic_sections.elf_got_start + dynamic_sections.elf_got->size +
           dynamic_sections.elf_dynstr->size +
           dynamic_sections.elf_dynsym->size;
}

/* The PLT relocation buffer, whichever form this target uses. */
strbuf_t *elf_relplt_buf(void)
{
    if (dynamic_sections.use_relaplt)
        return dynamic_sections.elf_relaplt;
    return dynamic_sections.elf_relplt;
}

static void elf_write_file_header(int is_64,
                                  int entry,
                                  int phnum,
                                  int shoff,
                                  int shnum,
                                  int shstrndx)
{
    int header_size = is_64 ? 64 : sizeof(elf32_hdr_t);
    int program_header_size = is_64 ? 56 : sizeof(elf32_phdr_t);

    elf_write_byte(elf_header, 0x7f);
    elf_write_str(elf_header, "ELF");
    elf_write_byte(elf_header, is_64 ? 2 : 1);
    elf_write_byte(elf_header, 1);
    elf_write_byte(elf_header, 1);
    for (int i = 0; i < 9; i++)
        elf_write_byte(elf_header, 0);

    elf_write_short(elf_header, 2);
    elf_write_short(elf_header, ELF_MACHINE);
    elf_write_int(elf_header, 1);
    if (is_64) {
        elf_write_quad(elf_header, entry);
        elf_write_quad(elf_header, header_size);
        elf_write_quad(elf_header, 0);
    } else {
        elf_write_int(elf_header, entry);
        elf_write_int(elf_header, header_size);
        elf_write_int(elf_header, shoff);
    }
    elf_write_int(elf_header, ELF_FLAGS);
    elf_write_short(elf_header, header_size);
    elf_write_short(elf_header, program_header_size);
    elf_write_short(elf_header, phnum);
    elf_write_short(elf_header, is_64 ? 0 : sizeof(elf32_shdr_t));
    elf_write_short(elf_header, shnum);
    elf_write_short(elf_header, shstrndx);
}

void elf_generate_header(void)
{
    /* Check for null pointers to prevent crashes */
    if (!elf_code || !elf_data || !elf_symtab || !elf_strtab || !elf_header) {
        fatal("ELF buffers not initialized");
    }

    int phnum = dynlink ? 4 : 2;
    int shnum = 0, shstrndx = 0, shoff = 0;

#if ELF_IS_64 == 0
    if (dynlink) {
        /* In dynamic linking mode:
         * - number of program headers = 4
         * - number of section headers = 15
         * - section header index of .shstrtab = 14
         */
        int elf_relplt_size = dynamic_sections.use_relaplt
                                  ? dynamic_sections.elf_relaplt->size
                                  : dynamic_sections.elf_relplt->size;
        shnum = 15;
        shstrndx = 14;
        shoff = elf_header_len + elf_code->size + elf_data->size +
                elf_rodata->size + elf_symtab->size + elf_strtab->size +
                elf_shstrtab->size + dynamic_sections.elf_interp->size +
                elf_relplt_size + dynamic_sections.elf_plt->size +
                dynamic_sections.elf_got->size +
                dynamic_sections.elf_dynstr->size +
                dynamic_sections.elf_dynsym->size +
                dynamic_sections.elf_dynamic->size;
    } else {
        /* In static linking mode:
         * - number of program headers = 2
         * - number of section headers = 8
         * - section header index of .shstrtab = 7
         */
        shnum = 8;
        shstrndx = 7;

        /* .data starts the second load segment on a page boundary, and
         * elf_generate() pads the file up to it; count that padding too.
         */
        shoff = ALIGN_UP(elf_header_len + elf_code->size + elf_rodata->size,
                         PAGESIZE) +
                elf_data->size + elf_symtab->size + elf_strtab->size +
                elf_shstrtab->size;
    }

    /* The following table explains the meaning of each field in the ELF32 file
     * header.
     *
     * Notice that the following values are hexadecimal.
     *
     *    |  File          |                                                 |
     *  & |  Header bytes  | Explanation                                     |
     * ---+----------------+-------------------------------------------------+
     * 00 | 7F  45  4C  46 | e_ident[0] - e_ident[3]: ELF magic number.      |
     *    | 01             | e_ident[4]: 1 -> 32-bit, 2 -> 64-bit.           |
     *    |     01         | e_ident[5]: 1 -> little-endian. 2 -> big-endian.|
     *    |         01     | e_ident[6]: 1 -> ELF header version; must be 1. |
     *    |             00 | e_ident[7]: Target OS ABI; be 1 for Linux.      |
     *    | 00             | e_ident[8]: ABI version; should be 1 for Linux. |
     *    |     00  00  00 | e_ident[9] - e_ident[16]: Padding; Unused;      |
     *    | 00  00  00  00 |                           should be 0.          |
     * ---+----------------+-------------------------------------------------+
     *    | 02  00         | e_type: Object file type; 2 -> executable       |
     *    |         28  00 | e_machine: Instruction Set Architecture.        |
     *    |                |            0x28 -> ARMv7                        |
     *    |                |            0xF3 -> RISC-V                       |
     *    | 01  00  00  00 | e_version: ELF identification version;          |
     *    |                |            must be 1.                           |
     *    | 54  00  01  00 | e_entry: Memory address of entry point.         |
     *    |                |          (where process starts).                |
     *    | 34  00  00  00 | e_phoff: File offset of program headers.        |
     *    |                |          0x34 -> 32-bit, 0x40 -> 64-bit.        |
     *    | d7  8a  03  00 | e_shoff: File offset of section headers.        |
     * ---+----------------+-------------------------------------------------+
     *    | 00  02  00  50 | e_flags: 0x50000200 -> ARM Version5 EABI,       |
     *    |                |                        soft-float ABI           |
     *    |                |          0x00000000 -> RISC-V                   |
     *    | 34  00         | e_ehsize: Size of this header.                  |
     *    |                |           0x34 -> 32-bit, 0x40 -> 64-bit.       |
     *    |         20  00 | e_phentsize: Size of each program header.       |
     *    |                |              0x20 -> 32-bit, 0x38 -> 64-bit.    |
     *    | 01  00         | e_phnum: Number of program headers.             |
     *    |         28  00 | e_shentsize: Size of each section header.       |
     *    |                |              0x28 -> 32-bit, 0x40 -> 64-bit.    |
     *    | 06  00         | e_shnum: Number of section headers.             |
     *    |         05  00 | e_shstrndx: Index of section header containing  |
     *    |                |             section names.                      |
     * ---+----------------+-------------------------------------------------+
     * 34 |                |                                                 |
     */
#endif
    /* ELF64 carries no section table; ELF32 includes its generated table. */
    elf_write_file_header(
        ELF_IS_64,
        ELF_IS_64 ? elf_code_start + elf_entry_offset : elf_code_start, phnum,
        shoff, shnum, shstrndx);
}

static void elf_write_program_header(int type,
                                     int flags,
                                     int offset,
                                     int address,
                                     int file_size,
                                     int memory_size,
                                     int alignment)
{
#if ELF_IS_64 == 1
    elf_write_int(elf_program_header, type);
    elf_write_int(elf_program_header, flags);
    elf_write_quad(elf_program_header, offset);
    elf_write_quad(elf_program_header, address);
    elf_write_quad(elf_program_header, address);
    elf_write_quad(elf_program_header, file_size);
    elf_write_quad(elf_program_header, memory_size);
    elf_write_quad(elf_program_header, alignment);
#else
    elf32_phdr_t phdr = {type,      offset,      address, address,
                         file_size, memory_size, flags,   alignment};
    elf_write_blk(elf_program_header, &phdr, sizeof(phdr));
#endif
}

void elf_generate_program_headers(void)
{
    const strbuf_t *elf_relplt = elf_relplt_buf();
    int ro_size;
    int data_file_ofs;
    int rw_vaddr;
    int rw_filesz;
    int rw_memsz;
    int dyn_extra = 0;

    if (!elf_program_header || !elf_code || !elf_data || !elf_rodata ||
        !elf_dynamic_sections_ready(elf_relplt)) {
        fatal("ELF section buffers not initialized");
    }

    ro_size = elf_header_len + elf_code->size + elf_rodata->size;
    if (dynlink) {
        ro_size += elf_relplt->size + dynamic_sections.elf_plt->size;
        dyn_extra = dynamic_sections.elf_interp->size +
                    dynamic_sections.elf_got->size +
                    dynamic_sections.elf_dynstr->size +
                    dynamic_sections.elf_dynsym->size +
                    dynamic_sections.elf_dynamic->size;
    }
    data_file_ofs = dynlink ? ro_size : ALIGN_UP(ro_size, PAGESIZE);
    rw_vaddr = dynlink ? dynamic_sections.elf_interp_start : elf_data_start;
    rw_filesz = elf_data->size + dyn_extra;
    rw_memsz = elf_data->size + elf_bss_size + dyn_extra;

    elf_write_program_header(1, 5, 0, ELF_START, ro_size, ro_size, PAGESIZE);
    elf_write_program_header(1, 6, data_file_ofs, rw_vaddr, rw_filesz, rw_memsz,
                             PAGESIZE);

    if (dynlink) {
        int interp_size = strlen(DYN_LINKER) + 1;
        int dynamic_ofs = ro_size + dynamic_sections.elf_interp->size +
                          dynamic_sections.elf_got->size +
                          dynamic_sections.elf_dynstr->size +
                          dynamic_sections.elf_dynsym->size;

        elf_write_program_header(3, 4, ro_size,
                                 dynamic_sections.elf_interp_start, interp_size,
                                 interp_size, 1);
        elf_write_program_header(2, 6, dynamic_ofs, elf_dynamic_start(),
                                 dynamic_sections.elf_dynamic->size,
                                 dynamic_sections.elf_dynamic->size,
                                 ELF_IS_64 ? 8 : 4);
    }
}

void elf_generate_section_headers(void)
{
#if ELF_IS_64 == 0
    /* x86-64 output carries no section headers; the program headers alone are
     * sufficient to load and run the image. The body below is therefore
     * compiled out entirely for that target -- leaving it after an early return
     * would make it unreachable code, which shecc's own parser rejects when it
     * compiles this file.
     */

    const strbuf_t *elf_relplt = elf_relplt_buf();
    /* Check for null pointers to prevent crashes */
    if (!elf_section_header || !elf_code || !elf_data || !elf_rodata ||
        !elf_symtab || !elf_strtab || !elf_shstrtab ||
        !elf_dynamic_sections_ready(elf_relplt)) {
        fatal("ELF section buffers not initialized");
    }

    /* section header table */
    elf32_shdr_t shdr;
    int ofs = elf_header_len, sh_name = 0;

#define WRITE_FILE_SECTION(name, type, flags, addr, size, ...)        \
    do {                                                              \
        elf32_shdr_t file_section = {sh_name, type, flags,      addr, \
                                     ofs,     size, __VA_ARGS__};     \
        elf_write_section_header(&file_section);                      \
        ofs += file_section.sh_size;                                  \
        sh_name += sizeof(name);                                      \
    } while (0)

    /* The following table uses the text section header as an example to explain
     * the ELF32 section header.
     *
     *    |  Section       |                                                 |
     *  & |  Header bytes  | Explanation                                     |
     * ---+----------------+-------------------------------------------------+
     *    | 0b  00  00  00 | sh_name: Name of the section. Giving the        |
     *    |                |          location of a null-terminated string.  |
     *    | 01  00  00  00 | sh_type: Type of the section's contents         |
     *    |                |          and semantics.                         |
     *    |                |          1 -> holds the program-defined         |
     *    |                |               information                       |
     *    | 07  00  00  00 | sh_flags: Miscellaneous attributes.             |
     *    |                |           0x1 -> writable, 0x2 -> allocatable   |
     *    |                |           0x4 -> executable.                    |
     *    | 54  00  01  00 | sh_addr: Starting address of the section        |
     *    |                |          in the memory image of a process.      |
     *    | 54  00  00  00 | sh_offset: Offset of the section in the file.   |
     *    | 0b  30  03  00 | sh_size: Size of the section.                   |
     *    | 00  00  00  00 | sh_link: Section header table index link.       |
     *    | 00  00  00  00 | sh_info: Extra information.                     |
     *    | 04  00  00  00 | sh_addralign: Address alignment constraints.    |
     *    | 00  00  00  00 | sh_entsize: Size of each entry.                 |
     * ---+----------------+-------------------------------------------------+
     *    |                |                                                 |
     */
    /* NULL section */
    shdr = (elf32_shdr_t) {sh_name, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    elf_write_section_header(&shdr);
    sh_name += 1;

    /* .text */
    WRITE_FILE_SECTION(".text", 1, 7, elf_code_start, elf_code->size, 0, 0, 4,
                       0);

    /* .rodata */
    WRITE_FILE_SECTION(".rodata", 1, 2, elf_rodata_start, elf_rodata->size, 0,
                       0, 4, 0);

    if (dynlink) {
        /* .rel.plt or .rela.plt */
        int sh_type, sh_addr, sh_size, sh_entsize, __ofs, __sh_name;

        if (dynamic_sections.use_relaplt) {
            sh_type = 4; /* SHT_RELA */
            sh_addr = dynamic_sections.elf_relaplt_start;
            sh_size = dynamic_sections.elf_relaplt->size;
            sh_entsize = sizeof(elf32_rela_t);
            __ofs = dynamic_sections.elf_relaplt->size;
            __sh_name = strlen(".rela.plt") + 1;
        } else {
            sh_type = 9; /* SHT_REL */
            sh_addr = dynamic_sections.elf_relplt_start;
            sh_size = dynamic_sections.elf_relplt->size;
            sh_entsize = sizeof(elf32_rel_t);
            __ofs = dynamic_sections.elf_relplt->size;
            __sh_name = strlen(".rel.plt") + 1;
        }
        shdr = (elf32_shdr_t) {sh_name, sh_type, 0x42, sh_addr, ofs,
                               sh_size, 8,       6,    4,       sh_entsize};
        elf_write_section_header(&shdr);
        ofs += __ofs;
        sh_name += __sh_name;

        /* .plt */
        WRITE_FILE_SECTION(".plt", 1, 0x6, dynamic_sections.elf_plt_start,
                           dynamic_sections.elf_plt->size, 0, 0, 4, 4);

        /* .interp */
        shdr = (elf32_shdr_t) {sh_name, 1,
                               0x2,     dynamic_sections.elf_interp_start,
                               ofs,     strlen(DYN_LINKER) + 1,
                               0,       0,
                               1,       0};
        elf_write_section_header(&shdr);
        ofs += dynamic_sections.elf_interp->size;
        sh_name += strlen(".interp") + 1;

        /* .got */
        WRITE_FILE_SECTION(".got", 1, 0x3, dynamic_sections.elf_got_start,
                           dynamic_sections.elf_got->size, 0, 0, 4, PTR_SIZE);

        /* .dynstr */
        WRITE_FILE_SECTION(
            ".dynstr", 3, 0x2,
            dynamic_sections.elf_got_start + dynamic_sections.elf_got->size,
            dynamic_sections.elf_dynstr->size, 0, 0, 1, 0);

        /* .dynsym */
        WRITE_FILE_SECTION(
            ".dynsym", 11, 0x2,
            dynamic_sections.elf_got_start + dynamic_sections.elf_got->size +
                dynamic_sections.elf_dynstr->size,
            dynamic_sections.elf_dynsym->size, 7, 1, 4, sizeof(elf32_sym_t));

        /* .dynamic */
        WRITE_FILE_SECTION(".dynamic", 6, 0x3,
                           dynamic_sections.elf_got_start +
                               dynamic_sections.elf_got->size +
                               dynamic_sections.elf_dynstr->size +
                               dynamic_sections.elf_dynsym->size,
                           dynamic_sections.elf_dynamic->size, 7, 0, 4, 0);
    }

    if (!dynlink)
        ofs = ALIGN_UP(ofs, PAGESIZE);

    /* .data */
    WRITE_FILE_SECTION(".data", 1, 3, elf_data_start, elf_data->size, 0, 0, 4,
                       0);

    /* .bss */
    shdr = (elf32_shdr_t) {sh_name, 8, 3, elf_bss_start, ofs, elf_bss_size, 0,
                           0,       4, 0};
    elf_write_section_header(&shdr);
    sh_name += strlen(".bss") + 1;
    /* Note: .bss is not written to file (SHT_NOBITS) */

    /* .symtab */
    WRITE_FILE_SECTION(".symtab", 2, 0, 0, elf_symtab->size, dynlink ? 13 : 6,
                       elf_symbol_index, 4, 16);

    /* .strtab */
    WRITE_FILE_SECTION(".strtab", 3, 0, 0, elf_strtab->size, 0, 0, 1, 0);

    /* .shstr */
    shdr =
        (elf32_shdr_t) {sh_name, 3, 0, 0, ofs, elf_shstrtab->size, 0, 0, 1, 0};
    elf_write_section_header(&shdr);
    sh_name += strlen(".shstrtab") + 1;
#undef WRITE_FILE_SECTION
#endif
}

/* Pad @elf_array with zero bytes until its size is a multiple of @boundary,
 * which must be a power of two.
 */
void elf_align_to(strbuf_t *elf_array, int boundary)
{
    /* Check for null pointers to prevent crashes */
    if (!elf_array) {
        fatal("ELF buffers not initialized for alignment");
    }

    while (elf_array->size & (boundary - 1))
        elf_write_byte(elf_array, 0);
}

/* Pad to a four-byte boundary, which is what the sections holding words want. A
 * section whose contents are read as pointers wants elf_align_to(PTR_SIZE)
 * instead: on a 64-bit target four bytes is not enough.
 */
void elf_align(strbuf_t *elf_array)
{
    elf_align_to(elf_array, 4);
}

/* Lay out .interp, .dynsym, .dynstr, .rela.plt (.rel.plt), .got and .dynamic.
 *
 * Separated out because a backend whose instruction lengths are only known
 * after emission has to place these a second time, once the final code size
 * fixes their addresses.
 */
void elf_generate_dynamic_sections(void)
{
    strbuf_t *elf_relplt = elf_relplt_buf();

    /* In dynamic linking mode, elf_generate_sections() also generates .interp,
     * .dynsym, .dynstr, .rel.plt (.rela.plt), .got and dynamic sections.
     *
     * .plt section is generated at the code generation phase.
     */
    int dymsym_idx = 1, func_plt_ofs, st_name = 0;
    int libdl_name = 0;
    int rel_offset;

    /* .interp section */
    elf_write_str(dynamic_sections.elf_interp, DYN_LINKER);
    elf_write_byte(dynamic_sections.elf_interp, 0);

    /* .got follows .interp and the loader writes pointers into it, so pad to a
     * pointer boundary rather than the usual four bytes.
     */
    elf_align_to(dynamic_sections.elf_interp, PTR_SIZE);

    /* Add first symbol table entry (STN_UNDEF) to .dynsym section. */
    elf_write_dynsym(dynamic_sections.elf_dynsym, 0, 0, 0);

    /* Add first NULL byte to .dynstr section.  */
    elf_write_byte(dynamic_sections.elf_dynstr, 0);
    st_name += 1;

    /* Add "libc.so.6" to .dynstr section. */
    elf_write_str(dynamic_sections.elf_dynstr, LIBC_SO);
    elf_write_byte(dynamic_sections.elf_dynstr, 0);
    st_name += strlen(LIBC_SO) + 1;

    /* lib/c.h reaches the host's stream objects through dlsym(), which glibc
     * moved into libc.so.6 only in 2.34. Earlier releases keep it in
     * libdl.so.2, and later ones still ship that name as an empty stub, so a
     * program that calls it depends on both.
     */
    func_t *dlsym_func = find_func("dlsym");

    if (dlsym_func && dlsym_func->is_used && !dlsym_func->bbs) {
        libdl_name = st_name;
        elf_write_str(dynamic_sections.elf_dynstr, LIBDL_SO);
        elf_write_byte(dynamic_sections.elf_dynstr, 0);
        st_name += strlen(LIBDL_SO) + 1;
    }

    /* Perform the following steps for each external function.
     * - Add a new PLT relocation entry to .relplt section.
     * - Add a new dynamic symbol entry to .dynsym section.
     * - Append the external function name to .dynstr section.
     * - Set plt_offset for the external function.
     *
     * Since __libc_start_main is not added to the function list, it must be
     * handled additionally first.
     */
    rel_offset = dynamic_sections.elf_got_start + PTR_SIZE * RESERVED_GOT_NUM;
    elf_write_jmprel(elf_relplt, rel_offset, dymsym_idx);

    /* STB_GLOBAL = 1, STT_FUNC = 2 */
    elf_write_dynsym(dynamic_sections.elf_dynsym, st_name, ELF32_ST_INFO(1, 2),
                     0);
    dymsym_idx += 1;

    elf_write_str(dynamic_sections.elf_dynstr, "__libc_start_main");
    elf_write_byte(dynamic_sections.elf_dynstr, 0);
    st_name += strlen("__libc_start_main") + 1;

    /* Because PLT[1] is reserved for __libc_start_main, its plt_offset must be
     * PLT_FIXUP_SIZE. Therefore, no offset assignment is required for this
     * function.
     */

    func_plt_ofs = PLT_FIXUP_SIZE + PLT_ENT_SIZE;
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        if (!func->is_used || func->bbs)
            continue;

        /* If the function is used and has no basic block, consider it to be an
         * external function.
         */
        rel_offset += PTR_SIZE;
        elf_write_jmprel(elf_relplt, rel_offset, dymsym_idx);

        elf_write_dynsym(dynamic_sections.elf_dynsym, st_name,
                         ELF32_ST_INFO(1, 2), 0);
        dymsym_idx += 1;

        elf_write_str(dynamic_sections.elf_dynstr, func->return_def.var_name);
        elf_write_byte(dynamic_sections.elf_dynstr, 0);
        st_name += strlen(func->return_def.var_name) + 1;

        func->plt_offset = func_plt_ofs;

        func_plt_ofs += PLT_ENT_SIZE;
    }

    /* .dynsym begins where .dynstr ends, and its entries are read as aligned
     * words: 24 bytes each under ELF64, which wants 8. Four-byte alignment
     * would leave a string table ending 4 bytes off an 8-byte boundary, and
     * DT_SYMTAB misaligned with it.
     */
    elf_align_to(dynamic_sections.elf_dynstr, PTR_SIZE);

    /* .got section
     *
     * - Arm architecture:
     *   - GOT[0] holds the virtual address of .dynamic section.
     *   - GOT[1] and GOT[2] are reserved for link_map and resolver,
     *     and are initialized to 0.
     * - RISC-V architecture:
     *   - GOT[0] and GOT[1] are reserved for resolver and link_map,
     *     and are initialized to 0.
     * - x86-64:
     *   - GOT[0] holds the virtual address of .dynamic section, and
     *     GOT[1] and GOT[2] are filled in by the loader.
     * - The remaining entries are initialized to &PLT[0].
     */
    switch (ELF_MACHINE) {
    case ELF_MACHINE_ARM32:
    case ELF_MACHINE_X86_64:
    case ELF_MACHINE_AARCH64:
        /* GOT[0] holds the address of .dynamic. The GOT is still being built,
         * so its final size comes from got_size rather than the buffer.
         */
        elf_write_ptr(dynamic_sections.elf_got,
                      dynamic_sections.elf_got_start +
                          dynamic_sections.got_size +
                          dynamic_sections.elf_dynstr->size +
                          dynamic_sections.elf_dynsym->size);
        elf_write_ptr(dynamic_sections.elf_got, 0);
        elf_write_ptr(dynamic_sections.elf_got, 0);
        break;
    case ELF_MACHINE_RV32:
        elf_write_ptr(dynamic_sections.elf_got, 0);
        elf_write_ptr(dynamic_sections.elf_got, 0);
        break;
    }
    int got_idx = 0;
    for (int i = PTR_SIZE * RESERVED_GOT_NUM; i < dynamic_sections.got_size;
         i += PTR_SIZE) {
        int slot = dynamic_sections.elf_plt_start;
        if (ELF_MACHINE == ELF_MACHINE_X86_64)

            /* x86-64 reaches the resolver through the push in its own PLT
             * entry, which supplies the relocation index, rather than jumping
             * straight to PLT[0].
             */
            slot = dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE +
                   got_idx * PLT_ENT_SIZE + 6;
        elf_write_ptr(dynamic_sections.elf_got, slot);
        got_idx++;
    }

    /* .dynamic section */
    int dynstr_addr =
        dynamic_sections.elf_got_start + dynamic_sections.got_size;
    elf_write_dyn(dynamic_sections.elf_dynamic, 0x5, dynstr_addr);
    elf_write_dyn(dynamic_sections.elf_dynamic, 0xa,
                  dynamic_sections.elf_dynstr->size);
    elf_write_dyn(dynamic_sections.elf_dynamic, 0x6,
                  dynstr_addr + dynamic_sections.elf_dynstr->size);
    elf_write_dyn(dynamic_sections.elf_dynamic, 0xb, elf_sym_size());

    if (dynamic_sections.use_relaplt) {
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x7,
                      dynamic_sections.elf_relaplt_start);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x8,
                      dynamic_sections.relaplt_size);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x9, elf_jmprel_size());
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x2,
                      dynamic_sections.relaplt_size);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x14, 0x7);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x17,
                      dynamic_sections.elf_relaplt_start);
    } else {
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x11,
                      dynamic_sections.elf_relplt_start);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x12,
                      dynamic_sections.relplt_size);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x13, elf_jmprel_size());
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x2,
                      dynamic_sections.relplt_size);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x14, 0x11);
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x17,
                      dynamic_sections.elf_relplt_start);
    }

    elf_write_dyn(dynamic_sections.elf_dynamic, 0x3,
                  dynamic_sections.elf_got_start);
    elf_write_dyn(dynamic_sections.elf_dynamic, 0x1, 0x1);
    if (libdl_name)
        elf_write_dyn(dynamic_sections.elf_dynamic, 0x1, libdl_name);
#if DYN_BIND_NOW == 1
    /* Resolve every PLT entry at load time. This target's PLT[0] does not
     * arrange the GOT[1]/GOT[2] hand-off the lazy resolver needs, so the loader
     * writes the final addresses straight into the GOT instead.
     */
    elf_write_dyn(dynamic_sections.elf_dynamic, 0x18, 0x0); /* DT_BIND_NOW */
    elf_write_dyn(dynamic_sections.elf_dynamic, 0x1e, 0x8); /* DF_BIND_NOW */
#endif
    elf_write_dyn(dynamic_sections.elf_dynamic, 0x0, 0x0);
}

/* Discard previously generated dynamic content so it can be rebuilt. */
void elf_reset_dynamic_sections(void)
{
    dynamic_sections.elf_interp->size = 0;
    dynamic_sections.elf_dynsym->size = 0;
    dynamic_sections.elf_dynstr->size = 0;
    dynamic_sections.elf_dynamic->size = 0;
    dynamic_sections.elf_got->size = 0;
    dynamic_sections.elf_plt->size = 0;
    strbuf_t *relplt_buf = elf_relplt_buf();
    relplt_buf->size = 0;
}

void elf_generate_sections(void)
{
    const strbuf_t *elf_relplt = elf_relplt_buf();
    if (!elf_shstrtab || !elf_dynamic_sections_ready(elf_relplt)) {
        fatal("ELF section buffers not initialized");
    }

    if (dynlink)
        elf_generate_dynamic_sections();

    /* shstr section; len = 53 If using dynamic linking, len = 105. */
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".text");
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".rodata");
    elf_write_byte(elf_shstrtab, 0);
    if (dynlink) {
        if (dynamic_sections.use_relaplt)
            elf_write_str(elf_shstrtab, ".rela.plt");
        else
            elf_write_str(elf_shstrtab, ".rel.plt");
        elf_write_byte(elf_shstrtab, 0);
        elf_write_str(elf_shstrtab, ".plt");
        elf_write_byte(elf_shstrtab, 0);
        elf_write_str(elf_shstrtab, ".interp");
        elf_write_byte(elf_shstrtab, 0);
        elf_write_str(elf_shstrtab, ".got");
        elf_write_byte(elf_shstrtab, 0);
        elf_write_str(elf_shstrtab, ".dynstr");
        elf_write_byte(elf_shstrtab, 0);
        elf_write_str(elf_shstrtab, ".dynsym");
        elf_write_byte(elf_shstrtab, 0);
        elf_write_str(elf_shstrtab, ".dynamic");
        elf_write_byte(elf_shstrtab, 0);
    }
    elf_write_str(elf_shstrtab, ".data");
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".bss");
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".symtab");
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".strtab");
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".shstrtab");
    elf_write_byte(elf_shstrtab, 0);
}

void elf_add_symbol(const char *symbol, int pc)
{
    /* Check for null pointers to prevent crashes */
    if (!symbol || !elf_symtab || !elf_strtab) {
        fatal("Invalid parameters for elf_add_symbol");
    }

    elf_write_int(elf_symtab, elf_strtab->size);
    elf_write_int(elf_symtab, pc);
    elf_write_int(elf_symtab, 0);
    elf_write_int(elf_symtab, pc == 0 ? 0 : 1 << 16);

    elf_write_str(elf_strtab, symbol);
    elf_write_byte(elf_strtab, 0);
    elf_symbol_index++;
}

void elf_preprocess(void)
{
#if ELF_IS_64 == 1
    /* ELF64 header (64) plus two program headers (56 each), and two more --
     * PT_INTERP and PT_DYNAMIC -- when linking dynamically.
     */
    elf_header_len = 64 + (56 << 1);
    if (dynlink)
        elf_header_len += (56 << 1);
#else
    elf_header_len = sizeof(elf32_hdr_t) + (sizeof(elf32_phdr_t) << 1);
    if (dynlink)
        elf_header_len += (sizeof(elf32_phdr_t) << 1);
#endif
    elf_align(elf_data);
    elf_align(elf_rodata);
    elf_code_start = ELF_START + elf_header_len;
    elf_rodata_start = elf_code_start + elf_offset;
    if (dynlink) {
        /* Dynamic startup clears the synthetic global frame with memset.
         * Reserve its PLT slot even when user code does not otherwise call it.
         */
        func_t *memset_func = find_func("memset");
        if (memset_func)
            memset_func->is_used = true;

        /* Precalculate the sizes of .rel.plt (.rela.plt), .plt and .got
         * sections.
         *
         * Suppose the compiled program has n external functions:
         * - .rel.plt (.rela.plt) contains n entries.
         * - .plt has n entries plus one fixup entry.
         * - .got includes n + RESERVED_GOT_NUM entries
         *   - Arm architecture:
         *     - GOT[0] holds the virtual address of .dynamic section.
         *     - GOT[1] and GOT[2] are reserved for link_map and resolver
         *       (both set to 0).
         *   - RISC-V architecture:
         *     - GOT[0] and GOT[1] are reserved for resolver and link_map.
         *   - Common:
         *     - The remaining entries correspond to all external functions.
         *
         * Next, consider the case of __libc_start_main before initializing the
         * sizes:
         * - .rel.plt (.rela.plt) has the one entry for __libc_start_main.
         * - .plt includes one fixup entry plus one entry for __libc_start_main.
         * - .got has RESERVED_GOT_NUM + 1 entries.
         *   - RESERVED_GOT_NUM entries for GOT[0] - GOT[RESERVED_GOT_NUM - 1].
         *   - 1 entry (GOT[RESERVED_GOT_NUM]) reserved for __libc_start_main.
         *
         * Therefore, the following code initialize the section sizes based on
         * the layout described above, and then traverse the function list in a
         * for loop to increment the sizes for each newly found external
         * function.
         */
        if (dynamic_sections.use_relaplt)
            dynamic_sections.relaplt_size = elf_jmprel_size();
        else
            dynamic_sections.relplt_size = elf_jmprel_size();
        dynamic_sections.plt_size = PLT_FIXUP_SIZE + PLT_ENT_SIZE;
        dynamic_sections.got_size = PTR_SIZE * RESERVED_GOT_NUM + PTR_SIZE;
        for (func_t *func = FUNC_LIST.head; func; func = func->next) {
            if (!func->is_used || func->bbs)
                continue;
            if (dynamic_sections.use_relaplt)
                dynamic_sections.relaplt_size += elf_jmprel_size();
            else
                dynamic_sections.relplt_size += elf_jmprel_size();
            dynamic_sections.plt_size += PLT_ENT_SIZE;
            dynamic_sections.got_size += PTR_SIZE;
        }

        elf_layout_dynamic();
    }
    elf_generate_sections();
    if (dynlink) {
        elf_data_start =
            elf_dynamic_start() + dynamic_sections.elf_dynamic->size;
    } else {
        /* To prevent two load segments from sharing a common page, add PAGESIZE
         * to elf_data_start, since the first section of the second load segment
         * is .data in static linking mode. ELF requires p_offset and p_vaddr to
         * agree modulo p_align. Derive its virtual address from the same
         * aligned file offset used before .data, rather than merely adding a
         * page to the preceding virtual end.
         */
        elf_data_start =
            ELF_START +
            ALIGN_UP(elf_header_len + elf_offset + elf_rodata->size, PAGESIZE);
    }
    elf_bss_start = elf_data_start + elf_data->size;
    elf_align(elf_symtab);
    elf_align(elf_strtab);
}

void elf_postprocess(void)
{
    elf_generate_header();
    elf_generate_program_headers();
    elf_generate_section_headers();
}

/* Hand a whole section to the operating system at once.
 *
 * The image was written a byte at a time through fputc(), which costs a call
 * into the C library for every one of the several hundred thousand bytes of a
 * self-compile -- and, once shecc is compiled by itself, a write(2) for each of
 * them, because its own libc has no buffer behind fputc().
 *
 * That reasoning holds only where lib/c.c is the libc in the output. A host
 * compiler's runtime already buffers fwrite(), and so does the one a
 * dynamically linked shecc resolves through the PLT; neither build has
 * '__syscall' to call, since it is synthesized only for static linking.
 */
#ifdef HOST_BUFFERED_STDIO
void elf_write_all(FILE *fp, const char *buf, int len)
{
    if (len > 0)
        fwrite(buf, 1, len, fp);
}
#else
void elf_write_all(FILE *fp, const char *buf, int len)
{
    int off = 0;

    while (off < len) {
        int n = __syscall(__syscall_write, fp, buf + off, len - off);
        if (n <= 0)
            return;
        off += n;
    }
}
#endif

void elf_generate(const char *outfile)
{
    if (!outfile)
        outfile = "a.out";

    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        usage_error("Unable to open output file for writing");
        return;
    }

    elf_write_all(fp, elf_header->elements, elf_header->size);
    elf_write_all(fp, elf_program_header->elements, elf_program_header->size);
    /* Read-only sections */
    elf_write_all(fp, elf_code->elements, elf_code->size);
    elf_write_all(fp, elf_rodata->elements, elf_rodata->size);

    if (dynlink) {
        /* Read-only sections */
        if (dynamic_sections.use_relaplt)
            elf_write_all(fp, dynamic_sections.elf_relaplt->elements,
                          dynamic_sections.elf_relaplt->size);
        else {
            elf_write_all(fp, dynamic_sections.elf_relplt->elements,
                          dynamic_sections.elf_relplt->size);
        }
        elf_write_all(fp, dynamic_sections.elf_plt->elements,
                      dynamic_sections.elf_plt->size);
        /* Readable and writable sections */
        elf_write_all(fp, dynamic_sections.elf_interp->elements,
                      dynamic_sections.elf_interp->size);
        elf_write_all(fp, dynamic_sections.elf_got->elements,
                      dynamic_sections.elf_got->size);
        elf_write_all(fp, dynamic_sections.elf_dynstr->elements,
                      dynamic_sections.elf_dynstr->size);
        elf_write_all(fp, dynamic_sections.elf_dynsym->elements,
                      dynamic_sections.elf_dynsym->size);
        elf_write_all(fp, dynamic_sections.elf_dynamic->elements,
                      dynamic_sections.elf_dynamic->size);
    }

    /* Statically linked, .data begins the second load segment and has to start
     * on a page boundary so that p_vaddr === p_offset (mod p_align). Linked
     * dynamically that segment starts back at .interp, and everything from
     * .interp to .data has already been written contiguously, so padding here
     * would push .data past the offset the program header advertises.
     */
    if (!dynlink) {
        int ro_written = elf_header_len + elf_code->size + elf_rodata->size;
        int data_ofs = ALIGN_UP(ro_written, PAGESIZE);

        /* Written in chunks rather than from one PAGESIZE-sized buffer: a
         * target with a 64 KiB granule would otherwise put 64 KiB on the stack,
         * and zero all of it to emit the few bytes actually needed.
         */
        char pad[256];
        int left = data_ofs - ro_written;

        for (int i = 0; i < 256; i++)
            pad[i] = 0;
        while (left > 0) {
            int n = left < 256 ? left : 256;
            elf_write_all(fp, pad, n);
            left -= n;
        }
    }
    /* Readable and writable sections */
    elf_write_all(fp, elf_data->elements, elf_data->size);

    /* Note: .bss is not written to file (SHT_NOBITS) */

    /* Other sections and section headers.
     *
     * ELF64 output emits no section headers, so the symbol and string tables
     * have nothing to reference and are left out of the image.
     */
#if ELF_IS_64 == 0
    elf_write_all(fp, elf_symtab->elements, elf_symtab->size);
    elf_write_all(fp, elf_strtab->elements, elf_strtab->size);
    elf_write_all(fp, elf_shstrtab->elements, elf_shstrtab->size);
    elf_write_all(fp, elf_section_header->elements, elf_section_header->size);
#endif
    fclose(fp);

    /* A generated ELF is meant to be runnable directly, but the mode it gets
     * depends on which libc opened it, not on how the output is linked: the
     * embedded lib/c.c passes 0775 to openat(2) while glibc's fopen("wb")
     * yields 0666. Every compiler that reaches here -- host-built, static
     * self-hosted, or dynamic -- therefore sets the bits explicitly.
     */
    if (chmod(outfile, 0x1ed) < 0) /* 0755 */
        usage_error("Unable to mark output executable");
}
