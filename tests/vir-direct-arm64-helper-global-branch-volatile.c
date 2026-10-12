volatile int slot;
volatile int mirror;

static int helper(int value)
{
    if (value < 2) {
        slot = value;
        return slot;
    }
    mirror = value;
    return mirror;
}

int main(int argc, char **argv)
{
    (void) argv;
    return helper(argc);
}
