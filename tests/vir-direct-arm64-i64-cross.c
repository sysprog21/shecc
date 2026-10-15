volatile unsigned long long high = 0x100000001ULL;

int main(void)
{
    unsigned long long value = high;
    int count = 0;

    while (count < 1) {
        value = value + 1;
        count++;
    }
    return value == high + 1 ? 0 : 1;
}
