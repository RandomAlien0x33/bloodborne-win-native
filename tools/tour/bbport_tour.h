// SPDX-License-Identifier: GPL-2.0-or-later
// bbport tour (a separate build, tools/tour/build.sh; not in the game's own build): the
// character is sent by the game's own warp to points all over every map, one after another, and
// looks around at each, so the port meets and compiles the graphics pipelines the game uses there
// (they stay in the shader cache).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace BbTour {
/// The game's image (the loader calls it once, with the other image patches).
void PatchImage(unsigned char* image, std::uint64_t size);
/// The pipeline cache saves a blob (shader meta, pipeline key, device profile). The tour keeps
/// its own copy (BB_TOUR_PIPELINES), which the cache's rebuild for a new build or GPU does not
/// clear, with the point each one was first met at.
void OnCacheBlob(int type, const std::string& name, const std::string& extension,
                 const unsigned char* data, std::size_t size);
} // namespace BbTour
