extern void *malloc(unsigned long size);
extern void free(void *pointer);

int main(void)
{
    unsigned long size = 1;
    unsigned char *pointer = malloc(size);
    int value;

    *pointer = 41;
    value = *pointer;
    free(pointer);
    return value - 41;
}
