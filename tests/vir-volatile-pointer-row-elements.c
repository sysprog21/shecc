typedef int *volatile volatile_pointer_t;
typedef volatile_pointer_t pointer_row_t[2];
typedef pointer_row_t *row_slots_t[2];
int a = 7;
int b = 11;
pointer_row_t row = {&a, &b};
row_slots_t slots = {&row, &row};

int touch_row(void)
{
    int *value = (*slots[0])[0];
    return *value + *(*slots[1])[1];
}

int main(void)
{
    return touch_row() != 18;
}
