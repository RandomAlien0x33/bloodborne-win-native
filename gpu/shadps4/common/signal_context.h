// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);
/// bbport: the frame and stack pointers of a fault context (x86-64 Linux and Windows; 0 elsewhere).
u64 GetRbp(void* ctx);
u64 GetRsp(void* ctx);

void IncrementRip(void* ctx, u64 length);

/// bbport: emulates a faulted `mov reg, [mem]` as a load of zero: sets rdi (`rdi` true) or r10
/// and moves rip past the instruction. False where not implemented (only x86-64 Linux, Windows).
bool ZeroLoadAndSkip(void* ctx, bool rdi, u64 length);

bool IsWriteError(void* ctx);

} // namespace Common