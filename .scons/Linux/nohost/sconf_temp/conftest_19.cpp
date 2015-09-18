
#include <atomic>
int main(int argc, char **argv) {
    std::atomic<int> a(0);
    return a.fetch_add(1);
}
