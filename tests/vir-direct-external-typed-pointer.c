extern int *__errno_location(void);

int main(void)
{
    int *error = __errno_location();

    *error = 41;
    return *error - 41;
}
