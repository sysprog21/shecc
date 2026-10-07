int main(int argc)
{
    volatile signed char byte = 0;
    volatile unsigned short word = 0;
    volatile int value = 0;
    volatile signed char *bp = &byte;
    volatile unsigned short *wp = &word;
    volatile int *p = &value;

    *bp = argc - 129;
    *wp = argc + 65533u;
    *p = (*bp < 0) + 2 * (*wp > 100000u);
    return *p + 7;
}
