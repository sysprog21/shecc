volatile unsigned long long expected = 0xffffffffffffffffULL;

int main(void)
{
    return expected == 0xffffffffffffffffULL ? 0 : 1;
}
