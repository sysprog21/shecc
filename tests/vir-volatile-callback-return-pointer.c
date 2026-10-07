static char text[1] = {'a'};
char *get(int n)
{
    return text + n;
}
static char *(*volatile callback)(int) = get;
int touch(void)
{
    callback;
    return callback(0) == text ? 0 : 1;
}
int main(void)
{
    return touch();
}
