int main(void)
{
    int total = 0;

    for (int i = 0; i < 32; i++) {
        if (i & 1)
            total += i;
        else if (i & 2)
            total -= i;
        else
            total += i * 2;
    }

    return total;
}
