int slot;

static int helper(int value)
{
    slot = value;
    return slot;
}

int main(int argc)
{
    return helper(argc);
}
