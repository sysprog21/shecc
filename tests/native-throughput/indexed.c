unsigned int a[1024];
unsigned int sink;
int main(void)
{
    for (unsigned int i = 0; i < 1024; i++)
        a[i] = i * 17u + 3u;
    unsigned int s = 1;
    for (unsigned int k = 0; k < 100000; k++)
        for (unsigned int i = 0; i < 128; i++)
            s += a[(s + i) & 1023u];
    sink = s;
    return sink != 3908711356u;
}
