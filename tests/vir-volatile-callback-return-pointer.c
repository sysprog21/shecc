static char text[1] = {'a'};
char *get(int n);
int touch(void);
static char *(*volatile callback)(int) = get;
int main(void)
{
    return touch();
}
char *get(int n)
{
    return text + n;
}
int touch(void)
{
    callback;
    return callback(0) == text ? 0 : 1;
}
