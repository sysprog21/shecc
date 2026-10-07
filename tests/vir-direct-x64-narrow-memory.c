int main(void)
{
    signed char sc;
    unsigned char uc;
    short ss;
    unsigned short us;

    *(&sc) = -5;
    *(&uc) = 250;
    *(&ss) = -20;
    *(&us) = 50000;
    if (*(&sc) != -5 || *(&uc) != 250)
        return 0;
    return *(&ss) == -20 && *(&us) == 50000 ? 29 : 0;
}
