int helper(int *value);

int main(void)
{
    int value = 41;

    return helper(&value);
}
int helper(int *value)
{
    return *value + 1;
}
