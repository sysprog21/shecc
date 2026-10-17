int bits(int a, int b);
int shifts(int a, int b);
unsigned shifts_unsigned(unsigned a);



int main(void)
{
    return bits(6, 3) + shifts(6, 8) +
           (int) (shifts_unsigned(0xfffffffeu) & 255u);
}
int bits(int a, int b)
{
    return (~a & 15) | (a ^ b);
}
int shifts(int a, int b)
{
    return (a << 1) + (b >> 2);
}
unsigned shifts_unsigned(unsigned a)
{
    return a >> 1;
}
