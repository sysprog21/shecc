int same_pointer(int *left, int *right)
{
    return left == right;
}

int main(void)
{
    int value = 29;

    return same_pointer(&value, &value) ? 29 : 0;
}
