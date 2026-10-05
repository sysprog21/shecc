int first;
int second;

static int helper(int value)
{
    int old = first;

    second = value;
    return old;
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    first = 7;
    return helper(3) + first + second;
}
