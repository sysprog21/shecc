int helper(_Bool value);

int main(int argc)
{
    return helper(argc == 1);
}
int helper(_Bool value)
{
    return value == 0 ? 7 : 42;
}
