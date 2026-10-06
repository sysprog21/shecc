typedef int *const pointer_row_t[2];
typedef pointer_row_t *row_slots_t[2];
int a;
int b;
pointer_row_t row = {&a, &a};
row_slots_t slots = {&row, &row};
int main(void)
{
    (*slots[0])[0] = &b;
    return 0;
}
