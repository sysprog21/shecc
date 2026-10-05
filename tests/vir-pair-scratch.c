/* A live scalar and two wide operands must leave a complete result pair. */
long long pair_source(int i)
{
    return i + 1;
}
long long pair_sum(int n, long long factor)
{
    long long sum = 0;
    for (int i = 0; i < n; i++) {
        long long x = pair_source(i);
        sum += x * factor + x;
    }
    return sum;
}
int main(void)
{
    return pair_sum(5, 4) != 75;
}
