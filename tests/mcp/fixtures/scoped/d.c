/* Test fixture for test_dbgsym.py (module D): a C function, for C source lines. */
unsigned char counter;

void count3(void)
{
    ++counter;
    ++counter;
    counter += 3;
}
