#pragma once
// PSD/PSB parsing and reduced-size decoding of the merged (composite) image.
// Portable C++: no Windows or COM code, so it can be unit-tested and fuzzed.
// Spec: Adobe Photoshop File Formats Specification.

#include <cstdint>

#include "buffer.h"
#include "reader.h"

namespace psd {

enum ColorMode : uint16_t {
    kBitmap = 0,
    kGrayscale = 1,
    kIndexed = 2,
    kRGB = 3,
    kCMYK = 4,
    kMultichannel = 7,
    kDuotone = 8,
    kLab = 9,
};

enum CompressionKind : uint16_t {
    kRaw = 0,
    kRLE = 1,
    kZip = 2,
    kZipPredict = 3,
};

enum class Result {
    Ok,
    BadFile,      // malformed, truncated or not a PSD/PSB
    Unsupported,  // valid but a variant we do not decode
    TooBig,       // over a configured limit
    TimedOut,     // over the time budget
    NoMemory,
};

const char* ResultName(Result r);
const char* ColorModeName(uint16_t mode);

struct Info {
    bool isPsb = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t channels = 0;
    uint16_t depth = 0;
    uint16_t mode = 0;

    // Image resources.
    bool hasVersionInfo = false;     // resource 1057 present
    bool realMergedData = true;      // 1057 hasRealMergedData; true when absent
    bool hasThumb = false;           // JPEG thumbnail from 1036 (or 1033)
    bool thumbIsBgr = false;         // 1033 stores BGR
    uint32_t thumbWidth = 0;
    uint32_t thumbHeight = 0;
    uint64_t thumbOffset = 0;
    uint32_t thumbSize = 0;
    int transparentIndex = -1;       // resource 1047, Indexed mode

    bool hasPalette = false;
    uint8_t palette[768] = {};       // 256 R, 256 G, 256 B

    // Layer info: a negative count means the first extra channel of the
    // composite holds its transparency.
    bool hasLayerCount = false;
    int32_t layerCount = 0;
    bool mergedAlpha = false;

    // Image data section.
    bool hasImageData = false;
    uint64_t imageDataOffset = 0;    // offset of the 2-byte compression code
    uint16_t compression = 0;
};

// Straight (non-premultiplied) BGRA, top-down rows, stride = width * 4.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasAlpha = false;
    Buf<uint8_t> bgra;

    bool Alloc(uint32_t w, uint32_t h);
};

struct Limits {
    uint32_t timeBudgetMs = 4000;
    uint64_t maxZipBytes = 192ull << 20;       // inflated composite size cap
    uint64_t maxRleTableBytes = 64ull << 20;   // row byte-count table cap
};

// Reads the header, resources and layer count. Reads only a few KB.
Result ReadInfo(Reader& r, Info& info);

// Copies the embedded JPEG thumbnail bytes.
Result ReadEmbeddedJpeg(Reader& r, const Info& info, Buf<uint8_t>& jpeg);

// Decodes the composite so its long side is at most maxSide (0 = full size),
// reading only the rows it needs. Never upscales.
Result DecodeComposite(Reader& r, const Info& info, uint32_t maxSide, const Limits& limits,
                       Image& out);

}  // namespace psd
