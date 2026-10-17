int sccp_branch(void);

int main(void)
{
    return sccp_branch();
}
int sccp_branch(void)
{
    int first = 8;
    int second = 3;

    if (first == 8)
        return second * 7;
    return first;
}
