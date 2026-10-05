int add(int left, int right)
{
    return left + right;
}

int call_add(void)
{
    return add(4, 9);
}

int main(void)
{
    return call_add();
}
