volatile int observed;
int reads;
int read_value(void)
{
    reads++;
    return observed;
}
int absorb_and(int x, int y)
{
    return x & (x | y);
}
int absorb_or(int x, int y)
{
    return x | (x & y);
}
int repeat_and(int x, int y)
{
    return x & (x & y);
}
int repeat_or(int x, int y)
{
    return x | (x | y);
}
int complement(int x)
{
    return (x ^ ~x) + (x & ~x) + (x | ~x);
}
int main(void)
{
    for (int x = -17; x < 18; x++) {
        for (int y = -17; y < 18; y++) {
            if (absorb_and(x, y) != x || absorb_or(x, y) != x ||
                repeat_and(x, y) != (x & y) || repeat_or(x, y) != (x | y) ||
                complement(x) != -2)
                return 1;
        }
    }
    observed = 37;
    int value = read_value();
    if ((value ^ value) != 0 || reads != 1)
        return 2;
    if ((observed ^ observed) != 0)
        return 3;
    return 0;
}
