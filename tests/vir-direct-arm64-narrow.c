int main(int argc)
{
    signed char s = 0;
    unsigned short u = 0;
    signed char *sp = &s;
    unsigned short *up = &u;

    *sp = argc - 2;
    *up = argc + 65530;
    return *sp + *up + (*up >> 9);
}
