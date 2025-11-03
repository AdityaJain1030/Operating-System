int add(int a, int b)
{
    return a + b;
}

int fib(int n)
{
    return 0;
}
// entry point is _start
void _start(void)
{
    int result = add(2, 3);
    result += fib(5);
}