extern _Bool dirty_false(void);
extern _Bool dirty_true(void);
extern signed char dirty_signed_byte(void);
extern unsigned char dirty_unsigned_byte(void);
extern short dirty_signed_short(void);
extern unsigned short dirty_unsigned_short(void);
int main(void)
{
    return dirty_false() || !dirty_true() || dirty_signed_byte() != -1 ||
           dirty_unsigned_byte() != 255 || dirty_signed_short() != -2 ||
           dirty_unsigned_short() != 65534;
}
