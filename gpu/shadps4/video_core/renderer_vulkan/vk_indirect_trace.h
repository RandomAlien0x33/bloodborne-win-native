// SPDX-License-Identifier: GPL-2.0-or-later
// bbport BB_INDIRECT_TRACE=1 (diagnostics): where the garbage in indirect arguments comes from.
//
// IndirectGuard (vk_indirect_guard.h) stops draws and dispatches whose counts are garbage, but
// not what put the garbage there. With the trace on, every page (4 KiB) indirect arguments were
// read from is watched, and what touches it is kept, newest last: shader bindings that write it,
// command processor writes (WRITE_DATA, DMA fills and copies, on the CPU or the GPU), guest
// writes (protection faults), uploads from the game's memory into its VRAM copy, downloads back,
// write twins, image copies. Each indirect command gets a number; the guard's shader keeps the
// arguments and the dwords around them of each command it stops, and the report prints them
// with the page's history as it was when the command was recorded, and the game's own bytes
// there at that moment. The page before the arguments' page is watched too (a neighbour written
// past its end).
#pragma once

#include "common/types.h"

namespace Vulkan::IndirectTrace {

enum class Event : u8 {
    ShaderWrite,  ///< a shader binding that writes: detail = shader hash, extra = stride
    Fill,         ///< DMA fill: detail = value, extra = 1 on the GPU
    Copy,         ///< DMA or COPY_DATA copy: detail = source, extra = 1 on the GPU
    CommandWrite, ///< WRITE_DATA and the like on the CPU: detail = first dword
    GpuCommandWrite, ///< the same, written by the GPU in stream order
    GuestWrite,   ///< a write fault (or a CPU write noted): extra = 1 from a command thread,
                  ///< 2 the fault window around a fault (pages marked CPU-modified)
    Twin,         ///< BB_GPU_WRITE_TWINS: a guest write next to GPU data, twinned
    Download,     ///< GPU data copied back into the game's memory
    Upload,       ///< the game's memory copied into the VRAM copy
    ImageToBuffer, ///< an image tiled into the VRAM copy (texel buffer)
    ImageDownload, ///< an image written back into the game's memory
    Evict,        ///< a VRAM block dropped (unmapped memory)
    Indirect,     ///< indirect arguments read: detail = shader hash, extra = 1 draw, 0 dispatch
};

[[nodiscard]] bool Enabled() noexcept;

/// Something touched [address, address + size).
void Note(Event event, VAddr address, u64 size, u64 detail = 0, u32 extra = 0);

/// The source of a copy (NoteCopy): its state when the copy was recorded.
enum SourceFlags : u32 {
    SourceGpuModified = 1, ///< the GPU wrote it (its VRAM copy is read)
    SourceImage = 2,       ///< an image of the texture cache lies there
    SourceInPlace = 4,     ///< used where it is, in the game's memory
    SourceTwin = 8,        ///< a write twin covers it (BB_GPU_WRITE_TWINS)
};

/// A copy into [dst, dst + size) from `src` (~0: GDS), on the GPU or the CPU. The source pages
/// of small copies are watched from then on.
void NoteCopy(VAddr dst, u64 size, VAddr src, bool gpu, u32 source_flags);

/// An indirect command reads `bytes` at `address`: watches its page and keeps its history.
/// Returns the number the guard's shader reports it by.
u32 OnIndirect(VAddr address, u32 bytes, bool draw, u64 shader_hash, bool gpu_modified,
               bool in_place);

/// The guard stopped command `seq`: `args` as the GPU read them, `around` the dwords from
/// `around_from` bytes before them (0 if not known).
void OnRejected(u32 seq, const u32* args, u32 words, const u32* around, u32 around_words,
                u32 around_from);

} // namespace Vulkan::IndirectTrace
