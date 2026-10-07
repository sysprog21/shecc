typedef int (*callback_t)(int);
typedef callback_t const callback_row_t[2];
typedef callback_row_t *callback_slots_t[2];
int increment(int value)
{
    return value + 1;
}
callback_row_t row = {increment, increment};
callback_slots_t slots = {&row, &row};
int main(void)
{
    (*slots[0])[0] = increment;
    return 0;
}
