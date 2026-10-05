signed char sc = -5;
unsigned char uc = 250;
short ss = -20;
unsigned short us = 50000;

int main(void)
{
    if (sc != -5 || uc != 250 || ss != -20 || us != 50000)
        return 0;
    *(&sc) = -4;
    *(&uc) = 251;
    *(&ss) = -21;
    *(&us) = 50001;
    return *(&sc) == -4 && *(&uc) == 251 && *(&ss) == -21 && *(&us) == 50001
               ? 29
               : 0;
}
