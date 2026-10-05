static int slot;

static int helper(int value)
{
    int i = 0;

    while (i < value) {
        slot = i;
        i++;
    }
    return slot;
}

int main(int argc, char **argv)
{
    (void) argv;
    return helper(argc) + 1;
}
