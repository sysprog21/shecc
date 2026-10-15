typedef int *pointer_row_t[2];
typedef pointer_row_t *row_slots_t[3];
int a = 7;
int b = 11;
pointer_row_t global_row = {&a, &b};
row_slots_t global_slots = {&global_row, &global_row, &global_row};

typedef int **double_pointer_row_t[2];
typedef double_pointer_row_t *double_row_slots_t[3];
int *indirect = &a;
double_pointer_row_t double_row = {&indirect, &indirect};
double_row_slots_t double_slots = {&double_row, &double_row, &double_row};

typedef int (*callback_row_t[2])(int);
int add_one(int value)
{
    return value + 1;
}
int add_two(int value)
{
    return value + 2;
}
static int (*callbacks[2])(int) = {add_one, add_two};
callback_row_t *callback_rows(void)
{
    return &callbacks;
}

int main(void)
{
    pointer_row_t local_row = {&b, &a};
    row_slots_t local_slots = {&local_row, &global_row, &local_row};
    if (*(*global_slots[0])[1] != 11 || *(*local_slots[0])[1] != 7)
        return 1;
    if (sizeof(*global_slots[0]) != 2 * sizeof(void *))
        return 2;
    (*local_slots[0])[0] = &a;
    if (*local_row[0] != 7)
        return 3;
    if (**(*double_slots[0])[1] != 7)
        return 4;
    typedef int (*row_pointer_t)[2];
    int matrix[2][2] = {{1, 2}, {3, 4}};
    row_pointer_t row_pointer = matrix;
    row_pointer_t *deeper_slots[1];
    deeper_slots[0] = &row_pointer;
    if ((*deeper_slots[0])[1][1] != 4)
        return 5;
    row_pointer_t grouped_pointer = (deeper_slots[0])[0];
    if (grouped_pointer != row_pointer || (deeper_slots[0])[0][1][1] != 4)
        return 7;
    typedef int *element_pointer_t;
    element_pointer_t pointer_rows[2][2] = {{&a, &a}, {&b, &b}};
    element_pointer_t(*hidden_star_rows)[2] = pointer_rows;
    if (*(hidden_star_rows)[1][0] != 11 || *hidden_star_rows[1][0] != 11)
        return 8;
    typedef int *const const_pointer_t;
    const_pointer_t const_pointer = &a;
    const_pointer_t *decayed_slots[1];
    *decayed_slots = &const_pointer;
    if (**decayed_slots != &a)
        return 6;
    int (*plain_row)[2] = &matrix[0];
    int (**const middle)[2] = &plain_row;
    int (**const *outer)[2] = &middle;
    int (*loaded_row)[2] = outer[0][0];
    if (loaded_row != plain_row || callback_rows()[0][1](4) != 6)
        return 9;
    local_slots[1] = &local_row;
    return *(*local_slots[1])[1] != 7;
}
