static int inner(void)
{
    return 29;
}

static int outer(void)
{
    return inner();
}

int main(void)
{
    return outer();
}
