#pragma once
// Small decompressors used by the composite decoder.

#include <cstddef>
#include <cstdint>

namespace psd {

// PackBits (Apple RLE) as used by PSD. Decodes into dst, stops when dst is
// full or src is exhausted, and returns the number of bytes written.
size_t UnpackBits(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen);

// Inflates a zlib stream (or, failing the header check, a raw deflate
// stream). Returns false on corrupt data or if dst is too small.
bool Inflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen, size_t* produced);

uint32_t Adler32(const uint8_t* data, size_t len);

}  // namespace psd
