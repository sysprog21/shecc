int main(void)
{
    volatile unsigned char uc = 250;
    volatile short ss = -20;
    volatile unsigned char *byte = &uc;
    volatile short *word = &ss;

    return uc + ss;
}
