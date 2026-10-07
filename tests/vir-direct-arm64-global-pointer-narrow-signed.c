signed char value = -3;
signed char *p = &value;

int main(int argc)
{
    int initial = *p;

    *p = argc;
    return initial + *p;
}
