/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* ELF file manipulation */

#include "../config"
#include "defs.h"
#include "globals.c"

int elf_symbol_index;

void elf_write_str(strbuf_t *elf_array, char *vals)
{
    /*
     * Note that strbuf_puts() does not push the null character.
     *
     * If necessary, use elf_write_byte() to append the null character
     * after calling elf_write_str().
     */
    strbuf_puts(elf_array, vals);
}

void elf_write_byte(strbuf_t *elf_array, int val)
{
    strbuf_putc(elf_array, val);
}

char e_extract_byte(int v, int b)
{
    return (v >> (b << 3)) & 0xFF;
}

void elf_write_int(strbuf_t *elf_array, int val)
{
    for (int i = 0; i < 4; i++)
        strbuf_putc(elf_array, e_extract_byte(val, i));
}

void elf_write_blk(strbuf_t *elf_array, void *blk, int sz)
{
    char *ptr = blk;
    for (int i = 0; i < sz; i++)
        strbuf_putc(elf_array, ptr[i]);
}

void elf_generate_header(void)
{
    elf32_hdr_t hdr;
    /*
     * The following table explains the meaning of each field in the
     * ELF32 file header.
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
    /* ELF file header */
    hdr.e_ident[0] = 0x7F; /* ELF magic number */
    hdr.e_ident[1] = 'E';
    hdr.e_ident[2] = 'L';
    hdr.e_ident[3] = 'F';
    hdr.e_ident[4] = 1; /* 32-bit */
    hdr.e_ident[5] = 1; /* little-endian */
    hdr.e_ident[6] = 1; /* ELF header version */
    hdr.e_ident[7] = 0; /* Target OS ABI */
    hdr.e_ident[8] = 0; /* ABI version */
    hdr.e_ident[9] = 0; /* Padding */
    hdr.e_ident[10] = 0;
    hdr.e_ident[11] = 0;
    hdr.e_ident[12] = 0;
    hdr.e_ident[13] = 0;
    hdr.e_ident[14] = 0;
    hdr.e_ident[15] = 0;
    hdr.e_type[0] = 2; /* Object file type */
    hdr.e_type[1] = 0;
    hdr.e_machine[0] = ELF_MACHINE; /* Instruction Set Architecture */
    hdr.e_machine[1] = 0;
    hdr.e_version = 1;                        /* ELF version */
    hdr.e_entry = ELF_START + elf_header_len; /* entry point */
    hdr.e_phoff = 0x34;                       /* program header offset */
    hdr.e_shoff = elf_header_len + elf_code->size + elf_data->size + 39 +
                  elf_symtab->size +
                  elf_strtab->size; /* section header offset */
    hdr.e_flags = ELF_FLAGS;        /* flags */
    hdr.e_ehsize[0] = 0x34;         /* header size */
    hdr.e_ehsize[1] = 0;
    hdr.e_phentsize[0] = 0x20; /* program header size */
    hdr.e_phentsize[1] = 0;
    hdr.e_phnum[0] = 1; /* number of program headers */
    hdr.e_phnum[1] = 0;
    hdr.e_shentsize[0] = 0x28; /* section header size */
    hdr.e_shentsize[1] = 0;
    hdr.e_shnum[0] = 6; /* number of section headers */
    hdr.e_shnum[1] = 0;
    hdr.e_shstrndx[0] = 5; /* section index with names */
    hdr.e_shstrndx[1] = 0;
    elf_write_blk(elf_header, &hdr, sizeof(elf32_hdr_t));

    /*
     * Explain the meaning of each field in the ELF32 program header.
     *
     *    |  Program       |                                                 |
     *  & |  Header bytes  | Explanation                                     |
     * ---+----------------+-------------------------------------------------+
     * 34 | 01  00  00  00 | p_type: Segment type; 1 -> loadable.            |
     *    | 54  00  00  00 | p_offset: Offset of segment in the file.        |
     *    | 54  00  01  00 | p_vaddr: Virtual address of loaded segment.     |
     *    | 54  00  01  00 | p_paddr: Only used on systems where physical    |
     *    |                |          address is relevant.                   |
     *    | 48  8a  03  00 | p_filesz: Size of the segment in the file image.|
     *    | 48  8a  03  00 | p_memsz: Size of the segment in memory.         |
     *    |                |          This value should be greater than or   |
     *    |                |          equal to p_filesz.                     |
     *    | 07  00  00  00 | p_flags: Segment-wise permissions;              |
     *    |                |          0x1 -> execute, 0x2 -> write,          |
     *    |                |          0x4 -> read                            |
     *    | 04  00  00  00 | p_align: Align segment to the specified value.  |
     * ---+----------------+-------------------------------------------------+
     * 54 |                |                                                 |
     */
    /* program header - code and data combined */
    elf32_phdr_t phdr;
    phdr.p_type = 1;                                 /* PT_LOAD */
    phdr.p_offset = elf_header_len;                  /* offset of segment */
    phdr.p_vaddr = ELF_START + elf_header_len;       /* virtual address */
    phdr.p_paddr = ELF_START + elf_header_len;       /* physical address */
    phdr.p_filesz = elf_code->size + elf_data->size; /* size in file */
    phdr.p_memsz = elf_code->size + elf_data->size;  /* size in memory */
    phdr.p_flags = 7;                                /* flags */
    phdr.p_align = 4;                                /* alignment */
    elf_write_blk(elf_header, &phdr, sizeof(elf32_phdr_t));
}

void elf_generate_sections(void)
{
    /* symtab section */
    for (int b = 0; b < elf_symtab->size; b++)
        elf_write_byte(elf_section, elf_symtab->elements[b]);

    /* strtab section */
    for (int b = 0; b < elf_strtab->size; b++)
        elf_write_byte(elf_section, elf_strtab->elements[b]);

    /* shstr section; len = 39 */
    elf_write_byte(elf_section, 0);
    elf_write_str(elf_section, ".shstrtab");
    elf_write_byte(elf_section, 0);
    elf_write_str(elf_section, ".text");
    elf_write_byte(elf_section, 0);
    elf_write_str(elf_section, ".data");
    elf_write_byte(elf_section, 0);
    elf_write_str(elf_section, ".symtab");
    elf_write_byte(elf_section, 0);
    elf_write_str(elf_section, ".strtab");
    elf_write_byte(elf_section, 0);

    /* section header table */
    elf32_shdr_t shdr;
    int ofs = elf_header_len;

    /*
     * The following table uses the text section header as an example
     * to explain the ELF32 section header.
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
    shdr.sh_name = 0;
    shdr.sh_type = 0;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = 0;
    shdr.sh_size = 0;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 0;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section, &shdr, sizeof(elf32_shdr_t));

    /* .text */
    shdr.sh_name = 0xb;
    shdr.sh_type = 1;
    shdr.sh_flags = 7;
    shdr.sh_addr = ELF_START + elf_header_len;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_code->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_code->size;

    /* .data */
    shdr.sh_name = 0x11;
    shdr.sh_type = 1;
    shdr.sh_flags = 3;
    shdr.sh_addr = elf_code_start + elf_code->size;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_data->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_data->size;

    /* .symtab */
    shdr.sh_name = 0x17;
    shdr.sh_type = 2;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_symtab->size;
    shdr.sh_link = 4;
    shdr.sh_info = elf_symbol_index;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 16;
    elf_write_blk(elf_section, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_symtab->size;

    /* .strtab */
    shdr.sh_name = 0x1f;
    shdr.sh_type = 3;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_strtab->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 1;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_strtab->size;

    /* .shstr */
    shdr.sh_name = 1;
    shdr.sh_type = 3;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = ofs;
    shdr.sh_size = 39;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 1;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section, &shdr, sizeof(elf32_shdr_t));
}

void elf_align(void)
{
    while (elf_data->size & 3)
        elf_write_byte(elf_data, 0);

    while (elf_symtab->size & 3)
        elf_write_byte(elf_symtab, 0);

    while (elf_strtab->size & 3)
        elf_write_byte(elf_strtab, 0);
}

void elf_add_symbol(char *symbol, int pc)
{
    elf_write_int(elf_symtab, elf_strtab->size);
    elf_write_int(elf_symtab, pc);
    elf_write_int(elf_symtab, 0);
    elf_write_int(elf_symtab, pc == 0 ? 0 : 1 << 16);

    elf_write_str(elf_strtab, symbol);
    elf_write_byte(elf_strtab, 0);
    elf_symbol_index++;
}

void elf_generate(char *outfile)
{
    elf_align();
    elf_generate_header();
    elf_generate_sections();

    if (!outfile)
        outfile = "a.out";

    FILE *fp = fopen(outfile, "wb");
    for (int i = 0; i < elf_header->size; i++)
        fputc(elf_header->elements[i], fp);
    for (int i = 0; i < elf_code->size; i++)
        fputc(elf_code->elements[i], fp);
    for (int i = 0; i < elf_data->size; i++)
        fputc(elf_data->elements[i], fp);
    for (int i = 0; i < elf_section->size; i++)
        fputc(elf_section->elements[i], fp);
    fclose(fp);
}
