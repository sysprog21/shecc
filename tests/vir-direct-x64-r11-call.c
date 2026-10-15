static int id(int x)
{
    return x;
}

static int clobber(int x)
{
    int value = x * 7;
    int called = id(value);

    return value + called;
}

int main(int argc, char **argv)
{
    int value = argc * 17;
    int called = clobber(value);

    return id(value) + called;
}
