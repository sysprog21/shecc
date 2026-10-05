typedef int *pointer_t;
typedef pointer_t(*grouped_pointer_t);
typedef pointer_t(*const const_grouped_pointer_t);
typedef pointer_t (*row_pointer_t)[2];
int main(void)
{
    int value = 7;
    pointer_t pointer = &value;
    grouped_pointer_t grouped = &pointer;
    const_grouped_pointer_t constant = &pointer;
    pointer_t row[2] = {&value, &value};
    row_pointer_t rows = &row;
    return sizeof(grouped_pointer_t) != sizeof(void *) || **grouped != 7 ||
           **constant != 7 || *(*rows)[1] != 7;
}
