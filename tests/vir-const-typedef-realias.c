typedef const int constant_t;
typedef constant_t inherited_constant_t;
int main(void)
{
    inherited_constant_t value = 1;
    value = 2;
    return value;
}
