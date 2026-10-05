static int shared(int value)
{
    return value + 1;
}

int legacy_user(int value)
{
    return shared(value);
}

int main(void)
{
    return shared(10) + legacy_user(18);
}
