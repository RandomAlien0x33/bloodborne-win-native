// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>
#include "common/rdtsc.h"
#include "common/uint128.h"

#ifdef _WIN64
#include <windows.h>
#endif

namespace Common {

static constexpr size_t SecondToNanoseconds = 1000000000ULL;

template <u64 Nearest>
static u64 RoundToNearest(u64 value) {
    const auto mod = value % Nearest;
    return mod >= (Nearest / 2) ? (value - mod + Nearest) : (value - mod);
}

// bbport: the steady clock: the system clock can step while the frequency is measured.
static u64 GetTimeNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

u64 EstimateRDTSCFrequency() {
    // Discard the first result measuring the rdtsc.
    FencedRDTSC();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    FencedRDTSC();

    // Get the current time.
    const auto start_time = GetTimeNs();
    const u64 tsc_start = FencedRDTSC();
    // Wait for 100 milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto end_time = GetTimeNs();
    const u64 tsc_end = FencedRDTSC();
    // Calculate differences.
    const u64 tsc_diff = tsc_end - tsc_start;
    const u64 tsc_freq = MultiplyAndDivide64(tsc_diff, 1000000000ULL, end_time - start_time);
    return RoundToNearest<100'000>(tsc_freq);
}

} // namespace Common
