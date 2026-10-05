static int shared(int value)
{
    return value + 1;
}

int (*saved)(int) = shared;

int legacy_user(int value)
{
    return saved(value);
}

int main(void)
{
    return shared(10) + legacy_user(18);
}
