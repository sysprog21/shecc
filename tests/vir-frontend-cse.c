int local_cse(int a, int b)
{
    int first = a + b;
    int second = a + b;

    return first + second;
}

int main(void)
{
    return local_cse(6, 8);
}
