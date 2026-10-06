unsigned int sink;
int main(void)
{
    unsigned int a = 1;
    for (unsigned int i = 1; i < 40000000u; i++)
        a = a * 1664525u + i + 1013904223u;
    sink = a;
    return sink != 375426474u;
}
