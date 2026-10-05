signed char add8(signed char value)
{
    return value + 1;
}

unsigned short add16(unsigned short value)
{
    return value + 1;
}

int main(void)
{
    return add8(-2) == -1 && add16(0xff00u) == 0xff01u ? 42 : 7;
}
