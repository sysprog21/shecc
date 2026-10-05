extern void *malloc(unsigned long size);
extern void free(void *pointer);

int main(void)
{
    int *pointer = malloc(sizeof(*pointer));
    int value;

    if (!pointer)
        return 1;
    *pointer = 41;
    value = *pointer;
    free(pointer);
    return value - 41;
}
