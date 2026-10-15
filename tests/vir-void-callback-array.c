int target;
void set_target(int value)
{
    target = value;
}
int main(void)
{
    typedef void (*setters_t[1][1][1][1])(int);
    setters_t setters = {{{{set_target}}}};
    setters[0][0][0][0](7);
    return target == 7 ? 0 : 1;
}
