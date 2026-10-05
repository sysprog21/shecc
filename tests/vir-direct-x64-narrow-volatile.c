volatile unsigned char uc = 250;
volatile short ss = -20;

int main(void)
{
    volatile unsigned char *byte = &uc;
    volatile short *word = &ss;

    *byte = 251;
    *word = -21;
    return uc + ss;
}
