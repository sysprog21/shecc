typedef int *pointer_t;
typedef pointer_t(*const grouped_pointer_t);
static grouped_pointer_t pointer = 0;
int main(void)
{
    pointer = 0;
    return 0;
}
