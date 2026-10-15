unsigned int sink;
int main(void)
{
    unsigned int a = 1, s = 0;
    for (unsigned int i = 0; i < 30000000u; i++) {
        a = a * 1664525u + 1013904223u;
        if (a & 8)
            s += a ^ i;
        else
            s -= a + i;
    }
    sink = s;
    return sink != 2594414144u;
}
