static int first;
static int second;

static int helper(int value);

int main(int argc)
{
    return helper(argc);
}
static int helper(int value)
{
    first = value;
    second = value;
    return second;
}
