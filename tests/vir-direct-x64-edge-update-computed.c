int main(void)
{
    int i = 3;
    int sum = 0;

    while (i > 0) {
        int scaled = i * 3;

        if (i & 1)
            sum += 1;
        else
            sum += 2;
        sum += scaled;
        i--;
    }
    return sum;
}
