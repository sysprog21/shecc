int add(int left, int right);
int call_add(void);


int main(void)
{
    return call_add();
}
int add(int left, int right)
{
    return left + right;
}
int call_add(void)
{
    return add(4, 9);
}
