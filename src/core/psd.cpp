#include "psd.h"

#include <chrono>
#include <cmath>
#include <cstring>

#include "codecs.h"

namespace psd {

const char* ResultName(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::BadFile: return "bad file";
        case Result::Unsupported: return "unsupported";
        case Result::TooBig: return "too big";
        case Result::TimedOut: return "timed out";
        case Result::NoMemory: return "out of memory";
    }
    return "?";
}

const char* ColorModeName(uint16_t mode) {
    switch (mode) {
        case kBitmap: return "Bitmap";
        case kGrayscale: return "Grayscale";
        case kIndexed: return "Indexed";
        case kRGB: return "RGB";
        case kCMYK: return "CMYK";
        case kMultichannel: return "Multichannel";
        case kDuotone: return "Duotone";
        case kLab: return "Lab";
    }
    return "Unknown";
}

bool Image::Alloc(uint32_t w, uint32_t h) {
    width = w;
    height = h;
    hasAlpha = false;
    return bgra.Alloc(size_t(w) * h * 4);
}

namespace {

// ---------------------------------------------------------------------------
// Header, resources and layer count.

bool ReadLength(Reader& r, uint64_t offset, bool psb, uint64_t* v) {
    if (psb) return r.U64(offset, v);
    uint32_t v32;
    if (!r.U32(offset, &v32)) return false;
    *v = v32;
    return true;
}

bool IsResourceSignature(const uint8_t* s) {
    return !std::memcmp(s, "8BIM", 4) || !std::memcmp(s, "MeSa", 4) || !std::memcmp(s, "AgHg", 4) ||
           !std::memcmp(s, "PHUT", 4) || !std::memcmp(s, "DCSR", 4);
}

void HandleResource(Reader& r, uint16_t id, uint64_t data, uint32_t size, Info& info, bool& have1036) {
    switch (id) {
        case 1033:  // Photoshop 4 thumbnail, BGR
        case 1036: {  // Photoshop 5+ thumbnail
            if (id == 1033 && have1036) return;
            if (size < 28 + 4) return;
            uint8_t h[28];
            if (!r.Read(data, h, sizeof(h))) return;
            uint32_t format = Be32(h);
            uint32_t w = Be32(h + 4);
            uint32_t hgt = Be32(h + 8);
            uint32_t compressed = Be32(h + 20);
            if (format != 1 || w == 0 || hgt == 0) return;  // 1 = JPEG
            uint32_t jpegSize = size - 28;
            if (compressed != 0 && compressed < jpegSize) jpegSize = compressed;
            info.hasThumb = true;
            info.thumbIsBgr = id == 1033;
            info.thumbWidth = w;
            info.thumbHeight = hgt;
            info.thumbOffset = data + 28;
            info.thumbSize = jpegSize;
            if (id == 1036) have1036 = true;
            return;
        }
        case 1057: {  // version info: 4-byte version, then hasRealMergedData
            uint8_t v[5];
            if (size >= 5 && r.Read(data, v, 5)) {
                info.hasVersionInfo = true;
                info.realMergedData = v[4] != 0;
            }
            return;
        }
        case 1047: {  // transparency index (Indexed mode)
            uint16_t index;
            if (size >= 2 && r.U16(data, &index)) info.transparentIndex = index;
            return;
        }
    }
}

void ParseResources(Reader& r, uint64_t p, uint64_t end, Info& info) {
    bool have1036 = false;
    while (p <= end && end - p >= 12) {
        uint8_t h[7];  // signature, id, name length
        if (!r.Read(p, h, sizeof(h)) || !IsResourceSignature(h)) return;
        uint16_t id = Be16(h + 4);
        uint64_t q = p + 6 + ((uint64_t(h[6]) + 2) & ~uint64_t(1));  // Pascal name, padded to even
        uint32_t size;
        if (q > end || end - q < 4 || !r.U32(q, &size)) return;
        uint64_t data = q + 4;
        if (size > end - data) return;
        HandleResource(r, id, data, size, info, have1036);
        p = data + size + (size & 1);
    }
}

bool IsLongLengthKey(const uint8_t* key) {
    // Tagged blocks that use an 8-byte length in PSB files.
    static const char kKeys[][5] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                    "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    for (const auto& k : kKeys)
        if (!std::memcmp(key, k, 4)) return true;
    return false;
}

void SetLayerCount(Info& info, uint16_t raw) {
    int16_t count = static_cast<int16_t>(raw);
    info.hasLayerCount = true;
    info.layerCount = count;
    info.mergedAlpha = count < 0;
}

bool IsTaggedSignature(const uint8_t* s) { return !std::memcmp(s, "8BIM", 4) || !std::memcmp(s, "8B64", 4); }

void ParseLayerCount(Reader& r, uint64_t p, uint64_t end, Info& info) {
    const uint64_t lenSize = info.isPsb ? 8 : 4;
    uint64_t infoLen;
    if (end - p < lenSize || !ReadLength(r, p, info.isPsb, &infoLen)) return;
    p += lenSize;
    if (infoLen > end - p) return;
    if (infoLen >= 2) {
        uint16_t count;
        if (r.U16(p, &count)) SetLayerCount(info, count);
        return;
    }

    // 16- and 32-bit files leave the layer info empty and put it in an
    // 'Lr16' / 'Lr32' tagged block after the global layer mask.
    p += infoLen;
    uint32_t maskLen;
    if (end - p < 4 || !r.U32(p, &maskLen)) return;
    p += 4;
    if (maskLen > end - p) return;
    p += maskLen;

    while (end - p >= 12) {
        uint8_t h[8];
        if (!r.Read(p, h, sizeof(h)) || !IsTaggedSignature(h)) return;
        uint64_t len;
        if (info.isPsb && IsLongLengthKey(h + 4)) {
            if (end - p < 16 || !r.U64(p + 8, &len)) return;
            p += 16;
        } else {
            uint32_t len32;
            if (!r.U32(p + 8, &len32)) return;
            len = len32;
            p += 12;
        }
        if (len > end - p) return;
        if (len >= 2 && (!std::memcmp(h + 4, "Lr16", 4) || !std::memcmp(h + 4, "Lr32", 4) ||
                         !std::memcmp(h + 4, "Layr", 4))) {
            uint16_t count;
            if (r.U16(p, &count)) SetLayerCount(info, count);
            return;
        }
        p += len;
        // Blocks are padded (to 2 or 4 bytes); step over padding to the next signature.
        for (int pad = 0; pad < 4 && end - p >= 4; ++pad) {
            uint8_t s[4];
            if (!r.Read(p, s, 4)) return;
            if (IsTaggedSignature(s)) break;
            ++p;
        }
    }
}

// ---------------------------------------------------------------------------
// Color conversion.

struct LinearToSrgbTable {
    uint8_t v[4096];
    LinearToSrgbTable() {
        for (int i = 0; i < 4096; ++i) {
            double x = i / 4095.0;
            double s = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
            v[i] = static_cast<uint8_t>(s * 255.0 + 0.5);
        }
    }
};

const uint8_t* LinearToSrgb() {
    static const LinearToSrgbTable table;  // thread-safe one-time init
    return table.v;
}

uint8_t LinearToSrgb8(double x) {
    if (!(x > 0.0)) return 0;
    if (x >= 1.0) return 255;
    return LinearToSrgb()[static_cast<int>(x * 4095.0 + 0.5)];
}

// CIE L*a*b* (D50, as Photoshop stores it) to 8-bit sRGB.
void LabToRgb(uint32_t l8, uint32_t a8, uint32_t b8, uint8_t* r, uint8_t* g, uint8_t* b) {
    double L = l8 * (100.0 / 255.0), A = double(a8) - 128.0, B = double(b8) - 128.0;
    double fy = (L + 16.0) / 116.0, fx = fy + A / 500.0, fz = fy - B / 200.0;
    auto finv = [](double t) {
        const double d = 6.0 / 29.0;
        return t > d ? t * t * t : 3.0 * d * d * (t - 4.0 / 29.0);
    };
    double X = 0.96422 * finv(fx), Y = finv(fy), Z = 0.82521 * finv(fz);
    // XYZ (D50) to linear sRGB, Bradford-adapted.
    *r = LinearToSrgb8(3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z);
    *g = LinearToSrgb8(-0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z);
    *b = LinearToSrgb8(0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z);
}

// Converts one decoded channel row to 8 bits per pixel.
void RowTo8(const uint8_t* row, uint16_t depth, uint32_t w, bool isAlpha, uint8_t* out) {
    switch (depth) {
        case 1:  // Bitmap mode: 1 = black
            for (uint32_t x = 0; x < w; ++x) out[x] = ((row[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 255;
            break;
        case 8:
            std::memcpy(out, row, w);
            break;
        case 16:
            for (uint32_t x = 0; x < w; ++x) out[x] = uint8_t((Be16(row + 2 * x) * 255u + 32767u) / 65535u);
            break;
        case 32: {  // linear float
            const uint8_t* lut = LinearToSrgb();
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t bits = Be32(row + 4 * x);
                float f;
                std::memcpy(&f, &bits, 4);
                if (!(f > 0.0f)) f = 0.0f;  // also catches NaN
                if (f > 1.0f) f = 1.0f;
                out[x] = isAlpha ? uint8_t(f * 255.0f + 0.5f) : lut[int(f * 4095.0f + 0.5f)];
            }
            break;
        }
    }
}

// Undoes ZIP-with-prediction delta coding for one row.
void Unpredict(uint8_t* row, size_t rowBytes, uint32_t w, uint16_t depth, uint8_t* tmp) {
    if (depth == 8) {
        for (size_t i = 1; i < rowBytes; ++i) row[i] = uint8_t(row[i] + row[i - 1]);
    } else if (depth == 16) {
        uint16_t prev = Be16(row);
        for (uint32_t x = 1; x < w; ++x) {
            uint16_t v = uint16_t(Be16(row + 2 * x) + prev);
            row[2 * x] = uint8_t(v >> 8);
            row[2 * x + 1] = uint8_t(v);
            prev = v;
        }
    } else if (depth == 32) {
        // Deltas run over the whole row of byte planes (all first bytes, then
        // all second bytes, ...); rebuild big-endian floats afterwards.
        for (size_t i = 1; i < rowBytes; ++i) row[i] = uint8_t(row[i] + row[i - 1]);
        for (uint32_t x = 0; x < w; ++x) {
            tmp[4 * x + 0] = row[x];
            tmp[4 * x + 1] = row[w + x];
            tmp[4 * x + 2] = row[2 * size_t(w) + x];
            tmp[4 * x + 3] = row[3 * size_t(w) + x];
        }
        std::memcpy(row, tmp, rowBytes);
    }
}

class Deadline {
public:
    explicit Deadline(uint32_t ms)
        : enabled_(ms != 0), end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)) {}
    bool Expired() const { return enabled_ && std::chrono::steady_clock::now() > end_; }

private:
    bool enabled_;
    std::chrono::steady_clock::time_point end_;
};

// ---------------------------------------------------------------------------
// Row access for each compression kind.

constexpr int kMaxSlots = 5;  // up to 4 color channels + alpha

class RowFetcher {
public:
    RowFetcher(Reader& r, const Info& info, uint64_t rowBytes) : r_(r), info_(info), rowBytes_(rowBytes) {}

    // channelOfSlot maps each slot we decode to a file channel index.
    Result Init(const int* channelOfSlot, int slots, const Limits& limits, const Deadline& deadline);
    bool Get(int slot, uint32_t y, uint8_t* dst);

private:
    Result InitRle(const Limits& limits, const Deadline& deadline);
    Result InitZip(const Limits& limits);

    Reader& r_;
    const Info& info_;
    uint64_t rowBytes_;
    uint64_t dataStart_ = 0;
    int slots_ = 0;
    int channelOfSlot_[kMaxSlots] = {};

    Buf<uint64_t> rleOffset_;
    Buf<uint32_t> rleLength_;
    Buf<uint8_t> packed_;
    Buf<uint8_t> inflated_;
    Buf<uint8_t> tmp_;
};

Result RowFetcher::Init(const int* channelOfSlot, int slots, const Limits& limits, const Deadline& deadline) {
    slots_ = slots;
    for (int i = 0; i < slots; ++i) channelOfSlot_[i] = channelOfSlot[i];
    dataStart_ = info_.imageDataOffset + 2;
    if (dataStart_ > r_.Size()) return Result::BadFile;
    const uint64_t rows = uint64_t(info_.channels) * info_.height;

    switch (info_.compression) {
        case kRaw:
            if (rowBytes_ != 0 && rows > (r_.Size() - dataStart_) / rowBytes_) return Result::BadFile;
            return Result::Ok;
        case kRLE:
            return InitRle(limits, deadline);
        case kZip:
        case kZipPredict:
            return InitZip(limits);
    }
    return Result::Unsupported;
}

Result RowFetcher::InitRle(const Limits& limits, const Deadline& deadline) {
    const uint32_t H = info_.height;
    const uint64_t countSize = info_.isPsb ? 4 : 2;
    const uint64_t tableBytes = uint64_t(info_.channels) * H * countSize;
    if (tableBytes > limits.maxRleTableBytes) return Result::TooBig;
    if (tableBytes > r_.Size() - dataStart_) return Result::BadFile;
    if (!rleOffset_.Alloc(size_t(slots_) * H) || !rleLength_.Alloc(size_t(slots_) * H)) return Result::NoMemory;

    int slotOfChannel[56];
    int lastChannel = 0;
    for (int& s : slotOfChannel) s = -1;
    for (int s = 0; s < slots_; ++s) {
        slotOfChannel[channelOfSlot_[s]] = s;
        if (channelOfSlot_[s] > lastChannel) lastChannel = channelOfSlot_[s];
    }

    // Worst-case PackBits output is slightly larger than the input.
    const uint64_t maxPacked = rowBytes_ + rowBytes_ / 64 + 256;
    if (!packed_.Alloc(size_t(maxPacked))) return Result::NoMemory;

    // Walk the byte-count table only as far as the last channel we need,
    // turning counts into absolute row offsets.
    const uint64_t entries = uint64_t(lastChannel + 1) * H;
    uint64_t pos = dataStart_ + tableBytes;
    uint8_t chunk[16384];
    const uint64_t perChunk = sizeof(chunk) / countSize;
    for (uint64_t i = 0; i < entries;) {
        uint64_t n = entries - i < perChunk ? entries - i : perChunk;
        if (!r_.Read(dataStart_ + i * countSize, chunk, size_t(n * countSize))) return Result::BadFile;
        for (uint64_t k = 0; k < n; ++k, ++i) {
            uint32_t count = countSize == 2 ? Be16(chunk + k * 2) : Be32(chunk + k * 4);
            if (count > maxPacked) return Result::BadFile;
            int slot = slotOfChannel[i / H];
            if (slot >= 0) {
                size_t at = size_t(slot) * H + size_t(i % H);
                rleOffset_[at] = pos;
                rleLength_[at] = count;
            }
            pos += count;
        }
        if (deadline.Expired()) return Result::TimedOut;
    }
    return Result::Ok;
}

Result RowFetcher::InitZip(const Limits& limits) {
    const uint64_t total = uint64_t(info_.channels) * info_.height * rowBytes_;
    const uint64_t compressed = r_.Size() - dataStart_;
    if (total > limits.maxZipBytes || compressed > limits.maxZipBytes) return Result::TooBig;
    Buf<uint8_t> src;
    if (!src.Alloc(size_t(compressed)) || !inflated_.Alloc(size_t(total), true)) return Result::NoMemory;
    if (!r_.Read(dataStart_, src.data(), src.size())) return Result::BadFile;
    size_t produced = 0;
    if (!Inflate(src.data(), src.size(), inflated_.data(), inflated_.size(), &produced)) return Result::BadFile;
    if (info_.compression == kZipPredict && info_.depth == 32 && !tmp_.Alloc(size_t(rowBytes_)))
        return Result::NoMemory;
    return Result::Ok;
}

bool RowFetcher::Get(int slot, uint32_t y, uint8_t* dst) {
    const uint64_t rowIndex = uint64_t(channelOfSlot_[slot]) * info_.height + y;
    switch (info_.compression) {
        case kRaw:
            return r_.Read(dataStart_ + rowIndex * rowBytes_, dst, size_t(rowBytes_));
        case kRLE: {
            size_t at = size_t(slot) * info_.height + y;
            uint32_t len = rleLength_[at];
            if (!r_.Read(rleOffset_[at], packed_.data(), len)) return false;
            size_t n = UnpackBits(packed_.data(), len, dst, size_t(rowBytes_));
            if (n < rowBytes_) std::memset(dst + n, 0, size_t(rowBytes_ - n));
            return true;
        }
        default:
            std::memcpy(dst, inflated_.data() + rowIndex * rowBytes_, size_t(rowBytes_));
            if (info_.compression == kZipPredict)
                Unpredict(dst, size_t(rowBytes_), info_.width, info_.depth, tmp_.data());
            return true;
    }
}

int ColorChannelCount(uint16_t mode) {
    switch (mode) {
        case kRGB:
        case kLab: return 3;
        case kCMYK: return 4;
        default: return 1;
    }
}

// Adds one source row into the per-output-column sums. px() yields a pixel's
// three color values and alpha; colors are weighted by alpha so transparent
// pixels do not darken edges.
template <typename PixelFn>
void AccumulateRow(uint32_t outW, const uint32_t* colStart, uint64_t* acc, uint32_t* count, PixelFn px) {
    for (uint32_t ox = 0; ox < outW; ++ox) {
        uint64_t s0 = 0, s1 = 0, s2 = 0, sa = 0;
        const uint32_t x1 = colStart[ox + 1];
        for (uint32_t x = colStart[ox]; x < x1; ++x) {
            uint32_t c0, c1, c2, a;
            px(x, c0, c1, c2, a);
            s0 += uint64_t(c0) * a;
            s1 += uint64_t(c1) * a;
            s2 += uint64_t(c2) * a;
            sa += a;
        }
        acc[4 * ox + 0] += s0;
        acc[4 * ox + 1] += s1;
        acc[4 * ox + 2] += s2;
        acc[4 * ox + 3] += sa;
        count[ox] += x1 - colStart[ox];
    }
}

}  // namespace

// ---------------------------------------------------------------------------

Result ReadInfo(Reader& r, Info& info) {
    info = Info();
    uint8_t h[26];
    if (!r.Read(0, h, sizeof(h)) || std::memcmp(h, "8BPS", 4) != 0) return Result::BadFile;
    uint16_t version = Be16(h + 4);
    if (version != 1 && version != 2) return Result::BadFile;
    info.isPsb = version == 2;
    info.channels = Be16(h + 12);
    info.height = Be32(h + 14);
    info.width = Be32(h + 18);
    info.depth = Be16(h + 22);
    info.mode = Be16(h + 24);

    const uint32_t maxDim = info.isPsb ? 300000 : 30000;
    if (info.channels < 1 || info.channels > 56) return Result::BadFile;
    if (info.width == 0 || info.height == 0 || info.width > maxDim || info.height > maxDim)
        return Result::BadFile;
    if (info.depth != 1 && info.depth != 8 && info.depth != 16 && info.depth != 32) return Result::BadFile;
    switch (info.mode) {
        case kBitmap: case kGrayscale: case kIndexed: case kRGB:
        case kCMYK: case kMultichannel: case kDuotone: case kLab:
            break;
        default:
            return Result::Unsupported;
    }

    const uint64_t size = r.Size();
    uint64_t off = 26;

    uint32_t colorModeLen;
    if (!r.U32(off, &colorModeLen)) return Result::BadFile;
    off += 4;
    if (colorModeLen > size - off) return Result::BadFile;
    if (info.mode == kIndexed && colorModeLen >= 768 && r.Read(off, info.palette, 768)) info.hasPalette = true;
    off += colorModeLen;

    uint32_t resourcesLen;
    if (!r.U32(off, &resourcesLen)) return Result::BadFile;
    off += 4;
    if (resourcesLen > size - off) return Result::BadFile;
    ParseResources(r, off, off + resourcesLen, info);
    off += resourcesLen;

    uint64_t layerMaskLen;
    const uint64_t lenSize = info.isPsb ? 8 : 4;
    if (!ReadLength(r, off, info.isPsb, &layerMaskLen)) return Result::BadFile;
    off += lenSize;
    if (layerMaskLen > size - off) return Result::BadFile;
    if (layerMaskLen > 0) ParseLayerCount(r, off, off + layerMaskLen, info);
    off += layerMaskLen;

    uint16_t compression;
    if (size - off >= 2 && r.U16(off, &compression)) {
        info.hasImageData = true;
        info.imageDataOffset = off;
        info.compression = compression;
    }
    return Result::Ok;
}

Result ReadEmbeddedJpeg(Reader& r, const Info& info, Buf<uint8_t>& jpeg) {
    if (!info.hasThumb) return Result::Unsupported;
    if (info.thumbSize > (16u << 20)) return Result::TooBig;
    if (!jpeg.Alloc(info.thumbSize)) return Result::NoMemory;
    if (!r.Read(info.thumbOffset, jpeg.data(), jpeg.size())) return Result::BadFile;
    return Result::Ok;
}

Result DecodeComposite(Reader& r, const Info& info, uint32_t maxSide, const Limits& limits, Image& out) {
    if (!info.hasImageData) return Result::BadFile;
    if (info.compression > kZipPredict) return Result::Unsupported;
    const Deadline deadline(limits.timeBudgetMs);
    const uint32_t W = info.width, H = info.height;
    const uint16_t mode = info.mode, depth = info.depth;

    // Combinations Photoshop can produce.
    if ((mode == kBitmap) != (depth == 1)) return Result::Unsupported;
    if (mode == kIndexed && (depth != 8 || !info.hasPalette)) return Result::Unsupported;
    if ((mode == kCMYK || mode == kLab) && depth == 32) return Result::Unsupported;

    const int colorChannels = ColorChannelCount(mode);
    if (info.channels < colorChannels) return Result::BadFile;
    const bool hasAlphaChannel =
        info.mergedAlpha && info.channels > colorChannels && mode != kBitmap && mode != kIndexed;
    const int slots = colorChannels + (hasAlphaChannel ? 1 : 0);
    const int alphaSlot = hasAlphaChannel ? colorChannels : -1;
    int channelOfSlot[kMaxSlots];
    for (int s = 0; s < slots; ++s) channelOfSlot[s] = s;  // color channels first, then alpha

    // Output size: fit the long side into maxSide, never upscale.
    uint32_t outW = W, outH = H;
    const uint32_t longSide = W > H ? W : H;
    if (maxSide != 0 && longSide > maxSide) {
        outW = uint32_t((uint64_t(W) * maxSide + longSide / 2) / longSide);
        outH = uint32_t((uint64_t(H) * maxSide + longSide / 2) / longSide);
        if (outW == 0) outW = 1;
        if (outH == 0) outH = 1;
    }

    const uint64_t rowBytes = depth == 1 ? (uint64_t(W) + 7) / 8 : uint64_t(W) * (depth / 8);

    RowFetcher fetcher(r, info, rowBytes);
    Result res = fetcher.Init(channelOfSlot, slots, limits, deadline);
    if (res != Result::Ok) return res;

    Buf<uint8_t> raw[kMaxSlots], plane[kMaxSlots];
    for (int s = 0; s < slots; ++s)
        if (!raw[s].Alloc(size_t(rowBytes)) || !plane[s].Alloc(W)) return Result::NoMemory;
    Buf<uint32_t> colStart;
    Buf<uint64_t> acc;
    Buf<uint32_t> count;
    if (!colStart.Alloc(size_t(outW) + 1) || !acc.Alloc(size_t(outW) * 4) || !count.Alloc(outW))
        return Result::NoMemory;
    for (uint32_t i = 0; i <= outW; ++i) colStart[i] = uint32_t(uint64_t(i) * W / outW);
    if (!out.Alloc(outW, outH)) return Result::NoMemory;

    const uint8_t* pal = info.palette;
    const int transparentIndex = info.transparentIndex;
    const uint32_t kRowsPerOutput = 3;
    bool anyTransparent = false;

    for (uint32_t oy = 0; oy < outH; ++oy) {
        if (deadline.Expired()) return Result::TimedOut;
        acc.Zero();
        count.Zero();

        uint64_t y0 = uint64_t(oy) * H / outH;
        uint64_t y1 = uint64_t(oy + 1) * H / outH;
        if (y1 <= y0) y1 = y0 + 1;
        const uint64_t span = y1 - y0;
        const uint64_t samples = span < kRowsPerOutput ? span : kRowsPerOutput;

        for (uint64_t k = 0; k < samples; ++k) {
            uint32_t y = uint32_t(samples == span ? y0 + k : y0 + span * (2 * k + 1) / (2 * samples));
            for (int s = 0; s < slots; ++s) {
                if (!fetcher.Get(s, y, raw[s].data())) return Result::BadFile;
                RowTo8(raw[s].data(), depth, W, s == alphaSlot, plane[s].data());
            }
            const uint8_t* p0 = plane[0].data();
            const uint8_t* p1 = slots > 1 ? plane[1].data() : p0;
            const uint8_t* p2 = slots > 2 ? plane[2].data() : p0;
            const uint8_t* p3 = slots > 3 ? plane[3].data() : p0;
            const uint8_t* pa = alphaSlot >= 0 ? plane[alphaSlot].data() : nullptr;

            switch (mode) {
                case kRGB:
                case kLab:  // L, a, b are averaged here and converted per output pixel
                    AccumulateRow(outW, colStart.data(), acc.data(), count.data(),
                                  [&](uint32_t x, uint32_t& c0, uint32_t& c1, uint32_t& c2, uint32_t& a) {
                                      c0 = p0[x]; c1 = p1[x]; c2 = p2[x];
                                      a = pa ? pa[x] : 255;
                                  });
                    break;
                case kCMYK:  // stored inverted: 255 = no ink
                    AccumulateRow(outW, colStart.data(), acc.data(), count.data(),
                                  [&](uint32_t x, uint32_t& c0, uint32_t& c1, uint32_t& c2, uint32_t& a) {
                                      uint32_t k = p3[x];
                                      c0 = (p0[x] * k + 127) / 255;
                                      c1 = (p1[x] * k + 127) / 255;
                                      c2 = (p2[x] * k + 127) / 255;
                                      a = pa ? pa[x] : 255;
                                  });
                    break;
                case kIndexed:
                    AccumulateRow(outW, colStart.data(), acc.data(), count.data(),
                                  [&](uint32_t x, uint32_t& c0, uint32_t& c1, uint32_t& c2, uint32_t& a) {
                                      uint32_t i = p0[x];
                                      c0 = pal[i]; c1 = pal[256 + i]; c2 = pal[512 + i];
                                      a = int(i) == transparentIndex ? 0 : 255;
                                  });
                    break;
                default:  // Grayscale, Bitmap, Duotone, Multichannel
                    AccumulateRow(outW, colStart.data(), acc.data(), count.data(),
                                  [&](uint32_t x, uint32_t& c0, uint32_t& c1, uint32_t& c2, uint32_t& a) {
                                      c0 = c1 = c2 = p0[x];
                                      a = pa ? pa[x] : 255;
                                  });
                    break;
            }
        }

        uint8_t* dst = out.bgra.data() + size_t(oy) * outW * 4;
        for (uint32_t ox = 0; ox < outW; ++ox, dst += 4) {
            const uint64_t sa = acc[4 * ox + 3];
            const uint32_t n = count[ox] ? count[ox] : 1;
            uint8_t alpha = uint8_t((sa + n / 2) / n);
            uint8_t c[3] = {0, 0, 0};
            if (sa != 0)
                for (int i = 0; i < 3; ++i) c[i] = uint8_t((acc[4 * ox + i] + sa / 2) / sa);
            if (mode == kLab) LabToRgb(c[0], c[1], c[2], &c[0], &c[1], &c[2]);
            dst[0] = c[2];
            dst[1] = c[1];
            dst[2] = c[0];
            dst[3] = alpha;
            if (alpha != 255) anyTransparent = true;
        }
    }
    out.hasAlpha = anyTransparent;
    return Result::Ok;
}

}  // namespace psd
