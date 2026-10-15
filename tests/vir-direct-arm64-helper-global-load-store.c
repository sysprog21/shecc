static int first;
static int second;

static int helper(int value)
{
    int old = first;

    second = value;
    return old;
}

int main(int argc, char **argv)
{
    int result;

    (void) argc;
    (void) argv;
    first = 7;
    result = helper(3);
    return result + first + second;
}
