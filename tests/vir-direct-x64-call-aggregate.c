struct result {
    int value;
};

struct result helper(void);

int main(void)
{
    return helper().value;
}
struct result helper(void)
{
    struct result r;

    r.value = 29;
    return r;
}
