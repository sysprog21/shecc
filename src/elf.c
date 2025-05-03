/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* ELF file manipulation */

int elf_symbol_index;

void elf_write_str(strbuf_t *elf_array, char *vals)
{
    /*
     * Note that strbuf_puts() does not push the null character.
     *
     * If necessary, use elf_write_byte() to append the null character
     * after calling strbuf_puts().
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

void elf_generate_header()
{
    /* ELF header */
    elf_write_int(elf_header, 0x464c457f); /* Magic: 0x7F followed by ELF */
    elf_write_byte(elf_header, 1);         /* 32-bit */
    elf_write_byte(elf_header, 1);         /* little-endian */
    elf_write_byte(elf_header, 1);         /* EI_VERSION */
    elf_write_byte(elf_header, 0);         /* System V */
    elf_write_int(elf_header, 0);          /* EI_ABIVERSION */
    elf_write_int(elf_header, 0);          /* EI_PAD: unused */
    elf_write_byte(elf_header, 2);         /* ET_EXEC */
    elf_write_byte(elf_header, 0);
    elf_write_byte(elf_header, ELF_MACHINE);
    elf_write_byte(elf_header, 0);
    elf_write_int(elf_header, 1);                          /* ELF version */
    elf_write_int(elf_header, ELF_START + elf_header_len); /* entry point */
    elf_write_int(elf_header, 0x34); /* program header offset */
    elf_write_int(elf_header, elf_header_len + elf_code->size + elf_data->size +
                                  39 + elf_symtab->size +
                                  elf_strtab->size); /* section header offset */
    /* flags */
    elf_write_int(elf_header, ELF_FLAGS);
    elf_write_byte(elf_header, 0x34); /* header size */
    elf_write_byte(elf_header, 0);
    elf_write_byte(elf_header, 0x20); /* program header size */
    elf_write_byte(elf_header, 0);
    elf_write_byte(elf_header, 1); /* number of program headers */
    elf_write_byte(elf_header, 0);
    elf_write_byte(elf_header, 0x28); /* section header size */
    elf_write_byte(elf_header, 0);
    elf_write_byte(elf_header, 6); /* number of sections */
    elf_write_byte(elf_header, 0);
    elf_write_byte(elf_header, 5); /* section index with names */
    elf_write_byte(elf_header, 0);

    /* program header - code and data combined */
    elf_write_int(elf_header, 1);              /* PT_LOAD */
    elf_write_int(elf_header, elf_header_len); /* offset of segment */
    elf_write_int(elf_header, ELF_START + elf_header_len); /* virtual address */
    elf_write_int(elf_header,
                  ELF_START + elf_header_len); /* physical address */
    elf_write_int(elf_header,
                  elf_code->size + elf_data->size); /* size in file */
    elf_write_int(elf_header,
                  elf_code->size + elf_data->size); /* size in memory */
    elf_write_int(elf_header, 7);                   /* flags */
    elf_write_int(elf_header, 4);                   /* alignment */
}

void elf_generate_sections()
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

    /* NULL section */
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);

    /* .text */
    elf_write_int(elf_section, 0xb);
    elf_write_int(elf_section, 1);
    elf_write_int(elf_section, 7);
    elf_write_int(elf_section, ELF_START + elf_header_len);
    elf_write_int(elf_section, elf_header_len);
    elf_write_int(elf_section, elf_code->size);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 4);
    elf_write_int(elf_section, 0);

    /* .data */
    elf_write_int(elf_section, 0x11);
    elf_write_int(elf_section, 1);
    elf_write_int(elf_section, 3);
    elf_write_int(elf_section, elf_code_start + elf_code->size);
    elf_write_int(elf_section, elf_header_len + elf_code->size);
    elf_write_int(elf_section, elf_data->size);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 4);
    elf_write_int(elf_section, 0);

    /* .symtab */
    elf_write_int(elf_section, 0x17);
    elf_write_int(elf_section, 2);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section,
                  elf_header_len + elf_code->size + elf_data->size);
    elf_write_int(elf_section, elf_symtab->size); /* size */
    elf_write_int(elf_section, 4);
    elf_write_int(elf_section, elf_symbol_index);
    elf_write_int(elf_section, 4);
    elf_write_int(elf_section, 16);

    /* .strtab */
    elf_write_int(elf_section, 0x1f);
    elf_write_int(elf_section, 3);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, elf_header_len + elf_code->size +
                                   elf_data->size + elf_symtab->size);
    elf_write_int(elf_section, elf_strtab->size); /* size */
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 1);
    elf_write_int(elf_section, 0);

    /* .shstr */
    elf_write_int(elf_section, 1);
    elf_write_int(elf_section, 3);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, elf_header_len + elf_code->size +
                                   elf_data->size + elf_symtab->size +
                                   elf_strtab->size);
    elf_write_int(elf_section, 39);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 0);
    elf_write_int(elf_section, 1);
    elf_write_int(elf_section, 0);
}

void elf_align()
{
    int remainder = elf_data->size & 3;
    if (remainder)
        elf_data->size += (4 - remainder);

    remainder = elf_symtab->size & 3;
    if (remainder)
        elf_symtab->size += (4 - remainder);

    remainder = elf_strtab->size & 3;
    if (remainder)
        elf_strtab->size += (4 - remainder);
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
