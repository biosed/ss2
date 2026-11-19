#pragma once
#include "assets/map_image.hpp"

namespace ss2::assets {
// Resamples a premultiplied-RGBA infantry sprite frame to targetWidth x targetHeight.
// Preserves full palette and alpha, smooths diagonals, and avoids transparent-edge halos.
MapImage resampleInfantry(const MapImage& premultipliedRgba, unsigned targetWidth, unsigned targetHeight);
}
