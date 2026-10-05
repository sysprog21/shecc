/* The continue edge and the fallthrough edge form distinct loop backedges. */
static int multi_latch(int n, int a, int b)
{
    int i = 0;
    int sum = 0;

    while (i < n) {
        sum += a * b;
        i++;
        if (i & 1)
            continue;
    }
    return sum;
}

int main(void)
{
    return multi_latch(5, 2, 3) != 30;
}
