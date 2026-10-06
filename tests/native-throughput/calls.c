unsigned int sink;
unsigned int mix(unsigned int x)
{
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    return x ^ (x >> 16);
}
int main(void)
{
    unsigned int s = 1;
    for (unsigned int i = 0; i < 2000000; i++)
        s = mix(s + i);
    sink = s;
    return sink != 447236269u;
}
