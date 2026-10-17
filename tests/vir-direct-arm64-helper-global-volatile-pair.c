volatile int slot;
volatile int mirror;

static int helper(int value);

int main(int argc)
{
    return helper(argc);
}
static int helper(int value)
{
    slot = value;
    mirror = value;
    return slot;
}
