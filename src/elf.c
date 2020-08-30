/* ELF file manipulation */

int elf_symbol_idx;
int elf_symtab_idx;
char elf_symtab[MAX_SYMTAB];
char elf_strtab[MAX_STRTAB];
int elf_strtab_idx;
char elf_section[MAX_SECTION];
int elf_section_idx;

void elf_write_section_str(char *vals, int len)
{
    int i;
    for (i = 0; i < len; i++)
        elf_section[elf_section_idx++] = vals[i];
}

void elf_write_data_str(char *vals, int len)
{
    int i;
    for (i = 0; i < len; i++)
        elf_data[elf_data_idx++] = vals[i];
}

void elf_write_header_byte(int val)
{
    elf_header[elf_header_idx++] = val;
}

void elf_write_section_byte(char val)
{
    elf_section[elf_section_idx++] = val;
}

char e_extract_byte(int v, int b)
{
    return (v >> (b * 8)) & 0xFF;
}

int elf_write_int(char *buf, int idx, int val)
{
    int i = 0;
    for (i = 0; i < 4; i++)
        buf[idx++] = e_extract_byte(val, i);
    return idx;
}

void elf_write_header_int(int val)
{
    elf_header_idx = elf_write_int(elf_header, elf_header_idx, val);
}

void elf_write_section_int(int val)
{
    elf_section_idx = elf_write_int(elf_section, elf_section_idx, val);
}

void elf_write_symbol_int(int val)
{
    elf_symtab_idx = elf_write_int(elf_symtab, elf_symtab_idx, val);
}

void elf_write_code_int(int val)
{
    elf_code_idx = elf_write_int(elf_code, elf_code_idx, val);
}

void elf_generate_header()
{
    /* ELF header */
    elf_write_header_int(0x464c457f); /* Magic: 0x7F followed by ELF */
    elf_write_header_byte(1);         /* 32-bit */
    elf_write_header_byte(1);         /* little-endian */
    elf_write_header_byte(1);         /* EI_VERSION */
    elf_write_header_byte(0);         /* System V */
    elf_write_header_int(0);          /* EI_ABIVERSION */
    elf_write_header_int(0);          /* EI_PAD: unused */
    elf_write_header_byte(2);         /* ET_EXEC */
    elf_write_header_byte(0);
    elf_write_header_byte(0x28); /* ARM (up to ARMv7/Aarch32) */
    elf_write_header_byte(0);
    elf_write_header_int(1);                          /* ELF version */
    elf_write_header_int(ELF_START + elf_header_len); /* entry point */
    elf_write_header_int(0x34); /* program header offset */
    elf_write_header_int(elf_header_len + elf_code_idx + elf_data_idx + 39 +
                         elf_symtab_idx +
                         elf_strtab_idx); /* section header offset */
    /* flags */
    elf_write_header_int(0x5000200); /* ARM */
    elf_write_header_byte(0x34);     /* header size */
    elf_write_header_byte(0);
    elf_write_header_byte(0x20); /* program header size */
    elf_write_header_byte(0);
    elf_write_header_byte(1); /* number of program headers */
    elf_write_header_byte(0);
    elf_write_header_byte(0x28); /* section header size */
    elf_write_header_byte(0);
    elf_write_header_byte(6); /* number of sections */
    elf_write_header_byte(0);
    elf_write_header_byte(5); /* section index with names */
    elf_write_header_byte(0);

    /* program header - code and data combined */
    elf_write_header_int(1);                           /* PT_LOAD */
    elf_write_header_int(elf_header_len);              /* offset of segment */
    elf_write_header_int(ELF_START + elf_header_len);  /* virtual address */
    elf_write_header_int(ELF_START + elf_header_len);  /* physical address */
    elf_write_header_int(elf_code_idx + elf_data_idx); /* size in file */
    elf_write_header_int(elf_code_idx + elf_data_idx); /* size in memory */
    elf_write_header_int(7);                           /* flags */
    elf_write_header_int(4);                           /* alignment */
}

void elf_generate_sections()
{
    /* symtab section */
    int b;
    for (b = 0; b < elf_symtab_idx; b++)
        elf_write_section_byte(elf_symtab[b]);

    /* strtab section */
    for (b = 0; b < elf_strtab_idx; b++)
        elf_write_section_byte(elf_strtab[b]);

    /* shstr section; len = 39 */
    elf_write_section_byte(0);
    elf_write_section_str(".shstrtab", 9);
    elf_write_section_byte(0);
    elf_write_section_str(".text", 5);
    elf_write_section_byte(0);
    elf_write_section_str(".data", 5);
    elf_write_section_byte(0);
    elf_write_section_str(".symtab", 7);
    elf_write_section_byte(0);
    elf_write_section_str(".strtab", 7);
    elf_write_section_byte(0);

    /* section header table */

    /* NULL section */
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(0);

    /* .text */
    elf_write_section_int(0xb);
    elf_write_section_int(1);
    elf_write_section_int(7);
    elf_write_section_int(ELF_START + elf_header_len);
    elf_write_section_int(elf_header_len);
    elf_write_section_int(elf_code_idx);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(4);
    elf_write_section_int(0);

    /* .data */
    elf_write_section_int(0x11);
    elf_write_section_int(1);
    elf_write_section_int(3);
    elf_write_section_int(elf_code_start + elf_code_idx);
    elf_write_section_int(elf_header_len + elf_code_idx);
    elf_write_section_int(elf_data_idx);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(4);
    elf_write_section_int(0);

    /* .symtab */
    elf_write_section_int(0x17);
    elf_write_section_int(2);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(elf_header_len + elf_code_idx + elf_data_idx);
    elf_write_section_int(elf_symtab_idx); /* size */
    elf_write_section_int(4);
    elf_write_section_int(elf_symbol_idx);
    elf_write_section_int(4);
    elf_write_section_int(16);

    /* .strtab */
    elf_write_section_int(0x1f);
    elf_write_section_int(3);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(elf_header_len + elf_code_idx + elf_data_idx +
                          elf_symtab_idx);
    elf_write_section_int(elf_strtab_idx); /* size */
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(1);
    elf_write_section_int(0);

    /* .shstr */
    elf_write_section_int(1);
    elf_write_section_int(3);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(elf_header_len + elf_code_idx + elf_data_idx +
                          elf_symtab_idx + elf_strtab_idx);
    elf_write_section_int(39);
    elf_write_section_int(0);
    elf_write_section_int(0);
    elf_write_section_int(1);
    elf_write_section_int(0);
}

void elf_align()
{
    int remainder = elf_data_idx & 3;
    if (remainder)
        elf_data_idx += (4 - remainder);

    remainder = elf_symtab_idx & 3;
    if (remainder)
        elf_symtab_idx += (4 - remainder);

    remainder = elf_strtab_idx & 3;
    if (remainder)
        elf_strtab_idx += (4 - remainder);
}

void elf_add_symbol(char *symbol, int len, int pc)
{
    elf_write_symbol_int(elf_strtab_idx);
    elf_write_symbol_int(pc);
    elf_write_symbol_int(0);
    if (pc == 0)
        elf_write_symbol_int(0);
    else
        elf_write_symbol_int(1 << 16);

    strncpy(elf_strtab + elf_strtab_idx, symbol, len);
    elf_strtab_idx += len;
    elf_strtab[elf_strtab_idx++] = 0;
    elf_symbol_idx++;
}

void elf_generate(char *outfile)
{
    FILE *fp;
    int i;

    elf_symbol_idx = 0;
    elf_symtab_idx = 0;
    elf_strtab_idx = 0;
    elf_section_idx = 0;

    elf_align();
    elf_generate_header();
    elf_generate_sections();

    if (outfile == NULL)
        outfile = "a.out";

    fp = fopen(outfile, "wb");
    for (i = 0; i < elf_header_idx; i++)
        fputc(elf_header[i], fp);
    for (i = 0; i < elf_code_idx; i++)
        fputc(elf_code[i], fp);
    for (i = 0; i < elf_data_idx; i++)
        fputc(elf_data[i], fp);
    for (i = 0; i < elf_section_idx; i++)
        fputc(elf_section[i], fp);
    fclose(fp);
}
