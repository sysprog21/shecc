volatile int slot;

static int helper(int value);

int main(int argc)
{
    return helper(argc);
}
static int helper(int value)
{
    slot = value;
    return slot;
}
