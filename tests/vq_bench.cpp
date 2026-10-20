// How long VirtualQuery takes on an address inside one large committed, touched region (like a game heap), against a
// small region: the kernel walks the pages to the end of the region to report its size.
#include <windows.h>
#include <cstdio>
#include <random>

static double Bench(char* base, size_t size, int n) {
    std::mt19937_64 rng(7);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    size_t sum = 0;
    for (int i = 0; i < n; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(base + (rng() % (size / 64)) * 64, &mbi, sizeof(mbi));
        sum += mbi.RegionSize;
    }
    QueryPerformanceCounter(&b);
    if (!sum) printf(" ");
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1e6 / static_cast<double>(f.QuadPart) / n;
}

int main() {
    const size_t sizes[] = {1ull << 20, 64ull << 20, 512ull << 20, 2048ull << 20};
    for (size_t size : sizes) {
        char* p = static_cast<char*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!p) {
            printf("%zu MB: could not allocate\n", size >> 20);
            continue;
        }
        for (size_t o = 0; o < size; o += 4096) p[o] = 1;   // touched, like a heap in use
        const int n = size >= (512ull << 20) ? 200 : 2000;
        printf("committed region %5zu MB: VirtualQuery at a random address %8.1f us per call\n", size >> 20, Bench(p, size, n));
        VirtualFree(p, 0, MEM_RELEASE);
    }
    return 0;
}
