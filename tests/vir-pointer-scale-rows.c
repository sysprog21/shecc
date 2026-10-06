int first(int x)
{
    return x + 1;
}
int second(int x)
{
    return x + 3;
}
int main(void)
{
    int rows[3][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
    int (*row)[4] = rows + 2;
    volatile int offset = -1;
    if (row[offset][2] != 7 || row + offset != rows + 1)
        return 1;
    typedef int (*callback)(int);
    callback table[3] = {first, second, first};
    callback *middle = table + 1;
    if (middle[offset](4) != 5 || middle[1](6) != 7)
        return 2;
    int (*raw[3])(int) = {first, second, first};
    int (**raw_middle)(int) = raw + 1;
    if (raw_middle[offset](4) != 5 || raw_middle[1](6) != 7)
        return 3;
    return 0;
}
