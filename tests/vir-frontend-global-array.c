/*
 * Global arrays addressed from VIR-inspected functions: an array is a pointer
 * slot followed by its elements, and the array names the elements.
 */
int table[4] = {1, 2, 3, 4};
char bytes[16];

int sum(int *p)
{
    return p[0] + p[3] * 10;
}

int pick(int i)
{
    return bytes[i];
}

int main()
{
    bytes[3] = 5;
    return sum(table) + pick(3);
}
