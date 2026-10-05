int value = 43;
int *pointers[16] = {&value, &value, &value, &value, &value, &value,
                     &value, &value, &value, &value, &value, &value,
                     &value, &value, &value, &value};
char tail;
int main(void)
{
    for (int i = 0; i < 16; i++)
        if (*pointers[i] != 43)
            return 1;
    return tail;
}
