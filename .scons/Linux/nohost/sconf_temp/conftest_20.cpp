
int main(int argc, char **argv) {
    int a = 0;
    int b = 0;
    int c = 0;

    __atomic_compare_exchange(&a, &b, &c, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return 0;
}
