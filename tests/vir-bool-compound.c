int main(void)
{
    if (((_Bool) {0} = 2) != 1)
        return 1;
    if (((_Bool) {1} += 2) != 1)
        return 2;
    return 0;
}
