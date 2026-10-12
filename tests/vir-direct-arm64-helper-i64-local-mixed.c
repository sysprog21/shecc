static long long helper(long long value)
{
    int low = (int) value;

    return value + low;
}

int main(int argc)
{
    long long value = ((long long) argc << 32) + 1;

    return helper(value) == value + 1 ? argc : -1;
}
