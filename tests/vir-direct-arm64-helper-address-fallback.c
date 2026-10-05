static int helper(int x)
{
    return x + 1;
}

static int (*helper_pointer)(int) = helper;

static int legacy_observer(void)
{
    return helper_pointer != 0;
}

int main(int argc)
{
    return helper(argc);
}
