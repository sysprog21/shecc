static int shift_count(int count)
{
    return count;
}
int main(void)
{
    int wide = shift_count(40);
    int negative = shift_count(-1);
    unsigned int one = shift_count(1);
    return !((1 << 40) == (1 << wide) && (8 >> 33) == (8 >> (wide - 7)) &&
             (1 << -1) == (1 << negative) && (-1 << 3) == -8 &&
             (one << 31) == 0x80000000u && (one << 3) == 8);
}
