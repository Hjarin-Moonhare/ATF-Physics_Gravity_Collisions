// What one guarded read of bad memory costs: SafeCopy catching an access violation, without and with a vectored
// exception handler installed first (crash loggers install one; it runs on every exception before SafeCopy's guard).
// Build and run: tests\run_benchmarks.bat
#include "../src/plugin.cpp"

#include <chrono>
#include <cstdio>

static LONG CALLBACK CrashLoggerLike(EXCEPTION_POINTERS* info) {
    void* frames[62];   // what a crash logger typically does first: capture the stack
    CaptureStackBackTrace(0, 62, frames, nullptr);
    (void)info;
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool PlainSafeCopy(void* dst, const void* src, size_t n) {   // the plain version: exception only, no check first
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static double Cost(void* bad, int n, bool old) {
    std::uint64_t v = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) old ? PlainSafeCopy(&v, bad, 8) : SafeCopy(&v, bad, 8);
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
}

int main() {
    void* bad = reinterpret_cast<void*>(0x3F800000ull);   // a float's bits read as a pointer: unmapped
    std::uint64_t v = 0;
    std::uint64_t good = 42;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) SafeCopy(&v, &good, 8);
    const double okCost = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 100000;
    printf("guarded read of good memory:                        %8.3f us\n", okCost);
    printf("bad memory, exception caught:                        %8.2f us\n", Cost(bad, 2000, true));
    printf("bad memory, checked first (no exception):            %8.2f us\n", Cost(bad, 2000, false));
    void* h = AddVectoredExceptionHandler(1, CrashLoggerLike);
    printf("with a crash-logger-like handler installed: caught   %8.2f us\n", Cost(bad, 2000, true));
    printf("                                            checked  %8.2f us\n", Cost(bad, 2000, false));
    RemoveVectoredExceptionHandler(h);
    return 0;
}
