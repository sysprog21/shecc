int slot;

static int helper(int value)
{
    if (value < 0) {
        slot = value;
        return slot;
    }
    slot = value;
    return slot;
}

int main(int argc, char **argv)
{
    int first;
    int second;

    (void) argc;
    (void) argv;
    first = helper(-3);
    second = helper(5);
    return first + second + slot;
}
