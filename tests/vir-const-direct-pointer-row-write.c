int a;
int b;
int *const row[2] = {&a, &a};
int *const (*slots[2])[2] = {&row, &row};
int main(void)
{
    (*slots[0])[0] = &b;
    return 0;
}
