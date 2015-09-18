
int main(int argc, char **argv) {
    int a = 0;
    return __sync_fetch_and_add(&a, 1);
}

//
// Figure out if we are using gcc older than 4.2 to target 32-bit x86. If so, error out
// even if we were able to compile the __sync statement, due to
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=40693
//
#if defined(__i386__)
#if !defined(__clang__)
#if defined(__GNUC__) && (__GNUC__ == 4) && (__GNUC_MINOR__ < 2)
#error "Refusing to use __sync in 32-bit mode with gcc older than 4.2"
#endif
#endif
#endif
