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

#ifndef PAGESIZE
#define PAGESIZE 4096
#endif

int elf_symbol_index = 0;

void elf_write_str(strbuf_t *elf_array, const char *vals)
{
    /* Note that strbuf_puts() does not push the null character.
     *
     * If necessary, use elf_write_byte() to append the null character
     * after calling elf_write_str().
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

void elf_write_blk(strbuf_t *elf_array, void *blk, int sz)
{
    if (!elf_array || !blk || sz <= 0)
        return;
    char *ptr = blk;
    for (int i = 0; i < sz; i++)
        strbuf_putc(elf_array, ptr[i]);
}

void elf_generate_header(void)
{
    /* Check for null pointers to prevent crashes */
    if (!elf_code || !elf_data || !elf_symtab || !elf_strtab || !elf_header) {
        fatal("ELF buffers not initialized");
        return;
    }

    elf32_hdr_t hdr;
    int phnum, shnum, shstrndx, shoff;

    if (dynlink) {
        /* In dynamic linking mode:
         * - number of program headers = 4
         * - number of section headers = 15
         * - section header index of .shstrtab = 14
         */
        phnum = 4;
        shnum = 15;
        shstrndx = 14;
        shoff =
            elf_header_len + elf_code->size + elf_data->size +
            elf_rodata->size + elf_symtab->size + elf_strtab->size +
            elf_shstrtab->size + dynamic_sections.elf_interp->size +
            dynamic_sections.elf_relplt->size + dynamic_sections.elf_plt->size +
            dynamic_sections.elf_got->size + dynamic_sections.elf_dynstr->size +
            dynamic_sections.elf_dynsym->size +
            dynamic_sections.elf_dynamic->size;
    } else {
        /* In static linking mode:
         * - number of program headers = 2
         * - number of section headers = 8
         * - section header index of .shstrtab = 7
         */
        phnum = 2;
        shnum = 8;
        shstrndx = 7;
        shoff = elf_header_len + elf_code->size + elf_data->size +
                elf_rodata->size + elf_symtab->size + elf_strtab->size +
                elf_shstrtab->size;
    }
    /* The following table explains the meaning of each field in the
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
    hdr.e_ident[0] = (char) 0x7F; /* ELF magic number */
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
    hdr.e_version = 1;                     /* ELF version */
    hdr.e_entry = elf_code_start;          /* entry point */
    hdr.e_phoff = sizeof(elf32_hdr_t);     /* program header offset */
    hdr.e_shoff = shoff;                   /* section header offset */
    hdr.e_flags = ELF_FLAGS;               /* flags */
    hdr.e_ehsize[0] = sizeof(elf32_hdr_t); /* header size */
    hdr.e_ehsize[1] = 0;
    hdr.e_phentsize[0] = sizeof(elf32_phdr_t); /* program header size */
    hdr.e_phentsize[1] = 0;
    hdr.e_phnum[0] = phnum; /* number of program headers */
    hdr.e_phnum[1] = 0;
    hdr.e_shentsize[0] = sizeof(elf32_shdr_t); /* section header size */
    hdr.e_shentsize[1] = 0;
    hdr.e_shnum[0] = shnum; /* number of section headers */
    hdr.e_shnum[1] = 0;
    hdr.e_shstrndx[0] = shstrndx; /* section index with names */
    hdr.e_shstrndx[1] = 0;
    elf_write_blk(elf_header, &hdr, sizeof(elf32_hdr_t));
}

void elf_generate_program_headers(void)
{
    if (!elf_program_header || !elf_code || !elf_data || !elf_rodata ||
        (dynlink &&
         (!dynamic_sections.elf_interp || !dynamic_sections.elf_relplt ||
          !dynamic_sections.elf_plt || !dynamic_sections.elf_got ||
          !dynamic_sections.elf_dynstr || !dynamic_sections.elf_dynsym ||
          !dynamic_sections.elf_dynamic))) {
        fatal("ELF section buffers not initialized");
        return;
    }

    elf32_phdr_t phdr;

    /* Explain the meaning of each field in the ELF32 program header.
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
    /* program header - read-only segment */
    phdr.p_type = 1;          /* PT_LOAD */
    phdr.p_offset = 0;        /* offset of segment */
    phdr.p_vaddr = ELF_START; /* virtual address */
    phdr.p_paddr = ELF_START; /* physical address */
    phdr.p_filesz =
        elf_header_len + elf_code->size + elf_rodata->size; /* size in file */
    phdr.p_memsz =
        elf_header_len + elf_code->size + elf_rodata->size; /* size in memory */
    phdr.p_flags = 5;                                       /* flags */
    phdr.p_align = PAGESIZE;                                /* alignment */
    if (dynlink) {
        phdr.p_filesz +=
            dynamic_sections.elf_relplt->size + dynamic_sections.elf_plt->size;
        phdr.p_memsz +=
            dynamic_sections.elf_relplt->size + dynamic_sections.elf_plt->size;
    }
    elf_write_blk(elf_program_header, &phdr, sizeof(elf32_phdr_t));

    /* program header - readable and writable segment */
    phdr.p_type = 1; /* PT_LOAD */
    phdr.p_offset = elf_header_len + elf_code->size +
                    elf_rodata->size;             /* offset of segment */
    phdr.p_vaddr = elf_data_start;                /* virtual address */
    phdr.p_paddr = elf_data_start;                /* physical address */
    phdr.p_filesz = elf_data->size;               /* size in file */
    phdr.p_memsz = elf_data->size + elf_bss_size; /* size in memory */
    phdr.p_flags = 6;                             /* flags */
    phdr.p_align = PAGESIZE;                      /* alignment */
    if (dynlink) {
        phdr.p_offset +=
            dynamic_sections.elf_relplt->size + dynamic_sections.elf_plt->size;
        phdr.p_vaddr = dynamic_sections.elf_interp_start;
        phdr.p_paddr = dynamic_sections.elf_interp_start;
        phdr.p_filesz += dynamic_sections.elf_interp->size +
                         dynamic_sections.elf_got->size +
                         dynamic_sections.elf_dynstr->size +
                         dynamic_sections.elf_dynsym->size +
                         dynamic_sections.elf_dynamic->size;
        phdr.p_memsz += dynamic_sections.elf_interp->size +
                        dynamic_sections.elf_got->size +
                        dynamic_sections.elf_dynstr->size +
                        dynamic_sections.elf_dynsym->size +
                        dynamic_sections.elf_dynamic->size;
    }
    elf_write_blk(elf_program_header, &phdr, sizeof(elf32_phdr_t));


    if (dynlink) {
        /* program header - program interpreter (.interp section) */
        phdr.p_type = 3; /* PT_INTERP */
        phdr.p_offset = elf_header_len + elf_code->size + elf_rodata->size +
                        dynamic_sections.elf_relplt->size +
                        dynamic_sections.elf_plt->size; /* offset of segment */
        phdr.p_vaddr = dynamic_sections.elf_interp_start; /* virtual address */
        phdr.p_paddr = dynamic_sections.elf_interp_start; /* physical address */
        phdr.p_filesz = strlen(DYN_LINKER) + 1;           /* size in file */
        phdr.p_memsz = strlen(DYN_LINKER) + 1;            /* size in memory */
        phdr.p_flags = 4;                                 /* flags */
        phdr.p_align = 1;                                 /* alignment */
        elf_write_blk(elf_program_header, &phdr, sizeof(elf32_phdr_t));

        /* program header - .dynamic section */
        phdr.p_type = 2; /* PT_DYNAMIC */
        phdr.p_offset =
            elf_header_len + elf_code->size + elf_rodata->size +
            dynamic_sections.elf_relplt->size + dynamic_sections.elf_plt->size +
            dynamic_sections.elf_interp->size + dynamic_sections.elf_got->size +
            dynamic_sections.elf_dynstr->size +
            dynamic_sections.elf_dynsym->size; /* offset of segment */
        phdr.p_vaddr = dynamic_sections.elf_got_start +
                       dynamic_sections.elf_got->size +
                       dynamic_sections.elf_dynstr->size +
                       dynamic_sections.elf_dynsym->size; /* virtual address */
        phdr.p_paddr = dynamic_sections.elf_got_start +
                       dynamic_sections.elf_got->size +
                       dynamic_sections.elf_dynstr->size +
                       dynamic_sections.elf_dynsym->size; /* physical address */
        phdr.p_filesz = dynamic_sections.elf_dynamic->size; /* size in file */
        phdr.p_memsz = dynamic_sections.elf_dynamic->size;  /* size in memory */
        phdr.p_flags = 6;                                   /* flags */
        phdr.p_align = 4;                                   /* alignment */
        elf_write_blk(elf_program_header, &phdr, sizeof(elf32_phdr_t));
    }
}

void elf_generate_section_headers(void)
{
    /* Check for null pointers to prevent crashes */
    if (!elf_section_header || !elf_code || !elf_data || !elf_rodata ||
        !elf_symtab || !elf_strtab || !elf_shstrtab ||
        (dynlink &&
         (!dynamic_sections.elf_interp || !dynamic_sections.elf_relplt ||
          !dynamic_sections.elf_plt || !dynamic_sections.elf_got ||
          !dynamic_sections.elf_dynstr || !dynamic_sections.elf_dynsym ||
          !dynamic_sections.elf_dynamic))) {
        fatal("ELF section buffers not initialized");
        return;
    }

    /* section header table */
    elf32_shdr_t shdr;
    int ofs = elf_header_len, sh_name = 0;

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
    shdr.sh_name = sh_name;
    shdr.sh_type = 0;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = 0;
    shdr.sh_size = 0;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 0;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    sh_name += 1;

    /* .text */
    shdr.sh_name = sh_name;
    shdr.sh_type = 1;
    shdr.sh_flags = 7;
    shdr.sh_addr = elf_code_start;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_code->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_code->size;
    sh_name += strlen(".text") + 1;

    /* .rodata */
    shdr.sh_name = sh_name; /* Offset in shstrtab for ".rodata" */
    shdr.sh_type = 1;       /* SHT_PROGBITS */
    shdr.sh_flags = 2;      /* SHF_ALLOC only (read-only) */
    shdr.sh_addr = elf_rodata_start;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_rodata->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_rodata->size;
    sh_name += strlen(".rodata") + 1;

    if (dynlink) {
        /* .rel.plt */
        shdr.sh_name = sh_name;
        shdr.sh_type = 9;     /* SHT_REL */
        shdr.sh_flags = 0x42; /* 0x40 | SHF_ALLOC */
        shdr.sh_addr = dynamic_sections.elf_relplt_start;
        shdr.sh_offset = ofs;
        shdr.sh_size = dynamic_sections.elf_relplt->size;
        shdr.sh_link = 8; /* The section header index of .dynsym. */
        shdr.sh_info = 6; /* The section header index of .got. */
        shdr.sh_addralign = 4;
        shdr.sh_entsize = sizeof(elf32_rel_t);
        elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
        ofs += dynamic_sections.elf_relplt->size;
        sh_name += strlen(".rel.plt") + 1;

        /* .plt */
        shdr.sh_name = sh_name;
        shdr.sh_type = 1;
        shdr.sh_flags = 0x6;
        shdr.sh_addr = dynamic_sections.elf_plt_start;
        shdr.sh_offset = ofs;
        shdr.sh_size = dynamic_sections.elf_plt->size;
        shdr.sh_link = 0;
        shdr.sh_info = 0;
        shdr.sh_addralign = 4;
        shdr.sh_entsize = 4;
        elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
        ofs += dynamic_sections.elf_plt->size;
        sh_name += strlen(".plt") + 1;

        /* .interp */
        shdr.sh_name = sh_name;
        shdr.sh_type = 1;
        shdr.sh_flags = 0x2;
        shdr.sh_addr = dynamic_sections.elf_interp_start;
        shdr.sh_offset = ofs;
        shdr.sh_size = strlen(DYN_LINKER) + 1;
        shdr.sh_link = 0;
        shdr.sh_info = 0;
        shdr.sh_addralign = 1;
        shdr.sh_entsize = 0;
        elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
        ofs += dynamic_sections.elf_interp->size;
        sh_name += strlen(".interp") + 1;

        /* .got */
        shdr.sh_name = sh_name;
        shdr.sh_type = 1;
        shdr.sh_flags = 0x3;
        shdr.sh_addr = dynamic_sections.elf_got_start;
        shdr.sh_offset = ofs;
        shdr.sh_size = dynamic_sections.elf_got->size;
        shdr.sh_link = 0;
        shdr.sh_info = 0;
        shdr.sh_addralign = 4;
        shdr.sh_entsize = PTR_SIZE;
        elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
        ofs += dynamic_sections.elf_got->size;
        sh_name += strlen(".got") + 1;

        /* .dynstr */
        shdr.sh_name = sh_name;
        shdr.sh_type = 3;
        shdr.sh_flags = 0x2;
        shdr.sh_addr =
            dynamic_sections.elf_got_start + dynamic_sections.elf_got->size;
        shdr.sh_offset = ofs;
        shdr.sh_size = dynamic_sections.elf_dynstr->size;
        shdr.sh_link = 0;
        shdr.sh_info = 0;
        shdr.sh_addralign = 1;
        shdr.sh_entsize = 0;
        elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
        ofs += dynamic_sections.elf_dynstr->size;
        sh_name += strlen(".dynstr") + 1;

        /* .dynsym */
        shdr.sh_name = sh_name;
        shdr.sh_type = 11;
        shdr.sh_flags = 0x2;
        shdr.sh_addr = dynamic_sections.elf_got_start +
                       dynamic_sections.elf_got->size +
                       dynamic_sections.elf_dynstr->size;
        shdr.sh_offset = ofs;
        shdr.sh_size = dynamic_sections.elf_dynsym->size;
        shdr.sh_link = 7; /* The section header index of .dynstr. */
        shdr.sh_info = 1; /* The index of the first non-local symbol. */
        shdr.sh_addralign = 4;
        shdr.sh_entsize = sizeof(elf32_sym_t);
        elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
        ofs += dynamic_sections.elf_dynsym->size;
        sh_name += strlen(".dynsym") + 1;

        /* .dynamic */
        shdr.sh_name = sh_name;
        shdr.sh_type = 6;
        shdr.sh_flags = 0x3;
        shdr.sh_addr = dynamic_sections.elf_got_start +
                       dynamic_sections.elf_got->size +
                       dynamic_sections.elf_dynstr->size +
                       dynamic_sections.elf_dynsym->size;
        shdr.sh_offset = ofs;
        shdr.sh_size = dynamic_sections.elf_dynamic->size;
        shdr.sh_link = 7; /* The section header index of .dynstr. */
        shdr.sh_info = 0;
        shdr.sh_addralign = 4;
        shdr.sh_entsize = 0;
        elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
        ofs += dynamic_sections.elf_dynamic->size;
        sh_name += strlen(".dynamic") + 1;
    }

    /* .data */
    shdr.sh_name = sh_name;
    shdr.sh_type = 1;
    shdr.sh_flags = 3;
    shdr.sh_addr = elf_data_start;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_data->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_data->size;
    sh_name += strlen(".data") + 1;

    /* .bss */
    shdr.sh_name = sh_name; /* Offset in shstrtab for ".bss" */
    shdr.sh_type = 8;       /* SHT_NOBITS */
    shdr.sh_flags = 3;      /* SHF_ALLOC | SHF_WRITE */
    shdr.sh_addr = elf_bss_start;
    shdr.sh_offset = ofs; /* File offset (not actually used for NOBITS) */
    shdr.sh_size = elf_bss_size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    sh_name += strlen(".bss") + 1;
    /* Note: .bss is not written to file (SHT_NOBITS) */

    /* .symtab */
    shdr.sh_name = sh_name;
    shdr.sh_type = 2;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_symtab->size;
    shdr.sh_link = dynlink ? 13 : 6; /* Link to .strtab */
    shdr.sh_info = elf_symbol_index;
    shdr.sh_addralign = 4;
    shdr.sh_entsize = 16;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_symtab->size;
    sh_name += strlen(".symtab") + 1;

    /* .strtab */
    shdr.sh_name = sh_name;
    shdr.sh_type = 3;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_strtab->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 1;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    ofs += elf_strtab->size;
    sh_name += strlen(".strtab") + 1;

    /* .shstr */
    shdr.sh_name = sh_name;
    shdr.sh_type = 3;
    shdr.sh_flags = 0;
    shdr.sh_addr = 0;
    shdr.sh_offset = ofs;
    shdr.sh_size = elf_shstrtab->size;
    shdr.sh_link = 0;
    shdr.sh_info = 0;
    shdr.sh_addralign = 1;
    shdr.sh_entsize = 0;
    elf_write_blk(elf_section_header, &shdr, sizeof(elf32_shdr_t));
    sh_name += strlen(".shstrtab") + 1;
}

void elf_align(strbuf_t *elf_array)
{
    /* Check for null pointers to prevent crashes */
    if (!elf_array) {
        fatal("ELF buffers not initialized for alignment");
        return;
    }

    while (elf_array->size & 3)
        elf_write_byte(elf_array, 0);
}

void elf_generate_sections(void)
{
    if (!elf_shstrtab ||
        (dynlink &&
         (!dynamic_sections.elf_interp || !dynamic_sections.elf_relplt ||
          !dynamic_sections.elf_plt || !dynamic_sections.elf_got ||
          !dynamic_sections.elf_dynstr || !dynamic_sections.elf_dynsym ||
          !dynamic_sections.elf_dynamic))) {
        fatal("ELF section buffers not initialized");
        return;
    }

    if (dynlink) {
        /* In dynamic linking mode, elf_generate_sections() also generates
         * .interp, .dynsym, .dynstr, .relplt, .got and dynamic sections.
         *
         * .plt section is generated at the code generation phase.
         *
         * TODO:
         * Define a new structure named 'elf32_rela_t' and use it to generate
         * relocation entries for RISC-V architecture.
         */
        elf32_sym_t sym;
        elf32_dyn_t dyn;
        elf32_rel_t rel;
        int dymsym_idx = 1, func_plt_ofs, func_got_ofs, st_name = 0;
        memset(&sym, 0, sizeof(elf32_sym_t));
        memset(&dyn, 0, sizeof(elf32_dyn_t));
        memset(&rel, 0, sizeof(elf32_rel_t));

        /* .interp section */
        elf_write_str(dynamic_sections.elf_interp, DYN_LINKER);
        elf_write_byte(dynamic_sections.elf_interp, 0);
        elf_align(dynamic_sections.elf_interp);

        /* Add first symbol table entry (STN_UNDEF) to .dynsym section. */
        elf_write_blk(dynamic_sections.elf_dynsym, &sym, sizeof(elf32_sym_t));

        /* Add first NULL byte to .dynstr section.  */
        elf_write_byte(dynamic_sections.elf_dynstr, 0);
        st_name += 1;

        /* Add "libc.so.6" to .dynstr section. */
        elf_write_str(dynamic_sections.elf_dynstr, LIBC_SO);
        elf_write_byte(dynamic_sections.elf_dynstr, 0);
        st_name += strlen(LIBC_SO) + 1;

        /* Perform the following steps for each external function.
         * - Add a new PLT relocation entry to .relplt section.
         * - Add a new dynamic symbol entry to .dynsym section.
         * - Append the external function name to .dynstr section.
         * - Set plt_offset and got_offset for the external function.
         *
         * Since __libc_start_main is not added to the function list,
         * it must be handled additionally first.
         */
        rel.r_offset = dynamic_sections.elf_got_start + PTR_SIZE * 3;
        rel.r_info = (dymsym_idx << 8) | R_ARCH_JUMP_SLOT;
        elf_write_blk(dynamic_sections.elf_relplt, &rel, sizeof(elf32_rel_t));

        sym.st_name = st_name;
        sym.st_info = ELF32_ST_INFO(1, 2); /* STB_GLOBAL = 1, STT_FUNC = 2 */
        elf_write_blk(dynamic_sections.elf_dynsym, &sym, sizeof(elf32_sym_t));
        dymsym_idx += 1;

        elf_write_str(dynamic_sections.elf_dynstr, "__libc_start_main");
        elf_write_byte(dynamic_sections.elf_dynstr, 0);
        st_name += strlen("__libc_start_main") + 1;

        /* Because PLT[1] and GOT[3] are reserved for __libc_start_main,
         * its plt_offset and got_offset must be PLT_FIXUP_SIZE and
         * PTR_SIZE * 3, respectively. Therefore, no offset assignment is
         * required for this function.
         */

        func_plt_ofs = PLT_FIXUP_SIZE + PLT_ENT_SIZE;
        func_got_ofs = PTR_SIZE << 2;
        for (func_t *func = FUNC_LIST.head; func; func = func->next) {
            if (func->is_used && !func->bbs) {
                rel.r_offset += PTR_SIZE;
                rel.r_info = (dymsym_idx << 8) | R_ARCH_JUMP_SLOT;
                elf_write_blk(dynamic_sections.elf_relplt, &rel,
                              sizeof(elf32_rel_t));

                sym.st_name = st_name;
                sym.st_info =
                    ELF32_ST_INFO(1, 2); /* STB_GLOBAL = 1, STT_FUNC = 2 */
                elf_write_blk(dynamic_sections.elf_dynsym, &sym,
                              sizeof(elf32_sym_t));
                dymsym_idx += 1;

                elf_write_str(dynamic_sections.elf_dynstr,
                              func->return_def.var_name);
                elf_write_byte(dynamic_sections.elf_dynstr, 0);
                st_name += strlen(func->return_def.var_name) + 1;

                func->plt_offset = func_plt_ofs;
                func->got_offset = func_got_ofs;

                func_plt_ofs += PLT_ENT_SIZE;
                func_got_ofs += PTR_SIZE;
            }
        }
        /* Ensure proper alignment for .dynstr section. */
        elf_align(dynamic_sections.elf_dynstr);

        /* .got section
         *
         * - GOT[0] holds the virtual address of .dynamic section.
         * - GOT[1] and GOT[2] are reserved for link_map and resolver,
         *   and are initialized to 0.
         * - The remaining entries are initialized to &PLT[0].
         */
        elf_write_int(dynamic_sections.elf_got,
                      dynamic_sections.elf_got_start +
                          dynamic_sections.got_size +
                          dynamic_sections.elf_dynstr->size +
                          dynamic_sections.elf_dynsym->size);
        elf_write_int(dynamic_sections.elf_got, 0);
        elf_write_int(dynamic_sections.elf_got, 0);
        for (int i = PTR_SIZE * 3; i < dynamic_sections.got_size; i += PTR_SIZE)
            elf_write_int(dynamic_sections.elf_got,
                          dynamic_sections.elf_plt_start);

        /* .dynamic section */
        dyn.d_tag = 0x5; /* STRTAB */
        dyn.d_un =
            dynamic_sections.elf_got_start +
            dynamic_sections.got_size; /* The virtual address of .dynstr. */
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0xa; /* STRSZ */
        dyn.d_un = dynamic_sections.elf_dynstr->size;
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x6; /* SYMTAB */
        dyn.d_un = dynamic_sections.elf_got_start + dynamic_sections.got_size +
                   dynamic_sections.elf_dynstr
                       ->size; /* The virtual address of .dynsym. */
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0xb;                /* SYMENT */
        dyn.d_un = sizeof(elf32_sym_t); /* Size of an entry. */
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x11; /* REL */
        dyn.d_un = dynamic_sections
                       .elf_relplt_start; /* The virtual address of .rel.plt. */
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x12; /* RELSZ */
        dyn.d_un = dynamic_sections.relplt_size;
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x13; /* RELENT */
        dyn.d_un = sizeof(elf32_rel_t);
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x3; /* PLTGOT */
        dyn.d_un =
            dynamic_sections.elf_got_start; /* The virtual address of .got.*/
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x2; /* PLTRELSZ */
        dyn.d_un = dynamic_sections.relplt_size;
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x14; /* PLTREL */
        dyn.d_un = 0x11;
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x17; /* JMPREL */
        dyn.d_un = dynamic_sections
                       .elf_relplt_start; /* The virtual address of .rel.plt. */
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x1; /* NEEDED */
        dyn.d_un = 0x1;  /* The index of "libc.so.6" in .dynstr. */
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));

        dyn.d_tag = 0x0; /* NULL */
        dyn.d_un = 0x0;
        elf_write_blk(dynamic_sections.elf_dynamic, &dyn, sizeof(elf32_dyn_t));
    }

    /* shstr section; len = 53
     * If using dynamic linking, len = 105.
     */
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".text");
    elf_write_byte(elf_shstrtab, 0);
    elf_write_str(elf_shstrtab, ".rodata");
    elf_write_byte(elf_shstrtab, 0);
    if (dynlink) {
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
        return;
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
    elf_header_len = sizeof(elf32_hdr_t) + (sizeof(elf32_phdr_t) << 1);
    if (dynlink)
        elf_header_len += (sizeof(elf32_phdr_t) << 1);
    elf_align(elf_data);
    elf_align(elf_rodata);
    elf_code_start = ELF_START + elf_header_len;
    elf_rodata_start = elf_code_start + elf_offset;
    if (dynlink) {
        /* Precalculate the sizes of .rel.plt, .plt and .got sections.
         *
         * Suppose the compiled program has n external functions:
         * - .rel.plt contains n entries.
         * - .plt has n entries plus one fixup entry.
         * - .got includes n + 3 entries
         *   - GOT[0] holds the virtual address of .dynamic section.
         *   - GOT[1] and GOT[2] are reserved for link_map and resolver
         *     (both set to 0).
         *   - The remaining entries correspond to all external functions.
         *
         * Next, consider the case of __libc_start_main before initializing
         * the sizes:
         * - .rel.plt has the one entry for __libc_start_main.
         * - .plt includes one fixup entry plus one entry for __libc_start_main.
         * - .got has 3 + 1 entries.
         *   - 3 entries for GOT[0] - GOT[2].
         *   - 1 entry (GOT[3]) reserved for __libc_start_main.
         *
         * Therefore, the following code initialize the section sizes based on
         * the layout described above, and then traverse the function list in a
         * for loop to increment the sizes for each newly found external
         * function.
         */
        dynamic_sections.relplt_size = sizeof(elf32_rel_t);
        dynamic_sections.plt_size = PLT_FIXUP_SIZE + PLT_ENT_SIZE;
        dynamic_sections.got_size = PTR_SIZE * 3 + PTR_SIZE;
        for (func_t *func = FUNC_LIST.head; func; func = func->next) {
            if (func->is_used && !func->bbs) {
                dynamic_sections.relplt_size += sizeof(elf32_rel_t);
                dynamic_sections.plt_size += PLT_ENT_SIZE;
                dynamic_sections.got_size += PTR_SIZE;
            }
        }

        /* Set the starting addresses of the three sections. */
        int elf_interp_size = strlen(DYN_LINKER) + 1;
        elf_interp_size = ALIGN_UP(elf_interp_size, 4);
        dynamic_sections.elf_relplt_start = elf_rodata_start + elf_rodata->size;
        dynamic_sections.elf_plt_start =
            dynamic_sections.elf_relplt_start + dynamic_sections.relplt_size;
        /* Since the first section of the second load segment is .interp
         * when using dynamic linking mode, adding PAGESIZE to elf_interp_start
         * is to ensure that two load segments don't share a common page.
         */
        dynamic_sections.elf_interp_start = dynamic_sections.elf_plt_start +
                                            dynamic_sections.plt_size +
                                            PAGESIZE;
        dynamic_sections.elf_got_start =
            dynamic_sections.elf_interp_start + elf_interp_size;
    }
    elf_generate_sections();
    if (dynlink) {
        elf_data_start = dynamic_sections.elf_got_start +
                         dynamic_sections.elf_got->size +
                         dynamic_sections.elf_dynstr->size +
                         dynamic_sections.elf_dynsym->size +
                         dynamic_sections.elf_dynamic->size;
    } else {
        /* To prevent two load segments from sharing a common page, add
         * PAGESIZE to elf_data_start, since the first section of the second
         * load segment is .data in static linking mode.
         */
        elf_data_start = elf_rodata_start + elf_rodata->size + PAGESIZE;
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

void elf_generate(const char *outfile)
{
    if (!outfile)
        outfile = "a.out";

    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        fatal("Unable to open output file for writing");
        return;
    }

    for (int i = 0; i < elf_header->size; i++)
        fputc(elf_header->elements[i], fp);
    for (int i = 0; i < elf_program_header->size; i++)
        fputc(elf_program_header->elements[i], fp);
    /* Read-only sections */
    for (int i = 0; i < elf_code->size; i++)
        fputc(elf_code->elements[i], fp);
    for (int i = 0; i < elf_rodata->size; i++)
        fputc(elf_rodata->elements[i], fp);

    if (dynlink) {
        /* Read-only sections */
        for (int i = 0; i < dynamic_sections.elf_relplt->size; i++)
            fputc(dynamic_sections.elf_relplt->elements[i], fp);
        for (int i = 0; i < dynamic_sections.elf_plt->size; i++)
            fputc(dynamic_sections.elf_plt->elements[i], fp);
        /* Readable and writable sections */
        for (int i = 0; i < dynamic_sections.elf_interp->size; i++)
            fputc(dynamic_sections.elf_interp->elements[i], fp);
        for (int i = 0; i < dynamic_sections.elf_got->size; i++)
            fputc(dynamic_sections.elf_got->elements[i], fp);
        for (int i = 0; i < dynamic_sections.elf_dynstr->size; i++)
            fputc(dynamic_sections.elf_dynstr->elements[i], fp);
        for (int i = 0; i < dynamic_sections.elf_dynsym->size; i++)
            fputc(dynamic_sections.elf_dynsym->elements[i], fp);
        for (int i = 0; i < dynamic_sections.elf_dynamic->size; i++)
            fputc(dynamic_sections.elf_dynamic->elements[i], fp);
    }
    /* Readable and writable sections */
    for (int i = 0; i < elf_data->size; i++)
        fputc(elf_data->elements[i], fp);
    /* Note: .bss is not written to file (SHT_NOBITS) */

    /* Other sections and section headers */
    for (int i = 0; i < elf_symtab->size; i++)
        fputc(elf_symtab->elements[i], fp);
    for (int i = 0; i < elf_strtab->size; i++)
        fputc(elf_strtab->elements[i], fp);
    for (int i = 0; i < elf_shstrtab->size; i++)
        fputc(elf_shstrtab->elements[i], fp);
    for (int i = 0; i < elf_section_header->size; i++)
        fputc(elf_section_header->elements[i], fp);
    fclose(fp);
}
