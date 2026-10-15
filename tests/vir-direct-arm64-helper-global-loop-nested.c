static int slot;

static int helper(int value)
{
    int i = 0;

    while (i < value) {
        int j = 0;

        while (j < value) {
            slot = 1;
            j++;
        }
        i++;
    }
    return slot;
}

int main(int argc, char **argv)
{
    (void) argv;
    return helper(argc);
}
