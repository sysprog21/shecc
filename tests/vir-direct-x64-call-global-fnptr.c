static int shared(int value)
{
    return value + 1;
}

int (*saved)(int) = shared;

int caller(int value)
{
    return saved(value);
}

int main(void)
{
    return shared(10) + caller(18);
}
