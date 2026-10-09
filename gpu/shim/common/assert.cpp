// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#include "bbport_platform.h"
#include <chrono>
#include <thread>

// bb-probe (probe.c): set while the port restarts itself through run.sh. The device fd is closed
// before exec; Vulkan calls failing then are not errors: this thread waits for the exec instead.
extern "C" __attribute__((weak)) volatile int runtime_restarting; // absent in the tests

// bbport_write_log.cpp: with BB_WRITE_LOG, the latest guest memory writes (no-op otherwise).
extern "C" void bbgpu_dump_guest_writes(void* ucontext);

void assert_fail_impl() {
    if (&runtime_restarting && runtime_restarting) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }
    std::fflush(stdout);
    // bbport: where the assertion fired (module+offset; llvm-symbolizer on bb-probe.exe).
    {
        void* frames[24];
        const int depth = BbPlatform::Backtrace(frames, 24);
        for (int i = 0; i < depth; ++i) {
            char where[160];
#ifdef _WIN32
            BbPlatform::DescribeAddress(frames[i], where, sizeof(where));
#else
            std::snprintf(where, sizeof(where), "%p", frames[i]);
#endif
            std::fprintf(stderr, "  assert #%d %s\n", i, where);
        }
    }
    bbgpu_dump_guest_writes(nullptr);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
