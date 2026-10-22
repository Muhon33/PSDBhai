// psdglance_tests.exe: generates synthetic PSD/PSB files covering every color
// mode, bit depth, compression and transparency case, decodes them and checks
// pixels; then checks thumbnail source selection, the reader cache, inflate,
// truncated files and a mutation fuzz loop.

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/codecs.h"
#include "../src/core/psd.h"
#include "../src/win/thumbnailer.h"

using Bytes = std::vector<uint8_t>;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);        \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

// ---------------------------------------------------------------------------
// Byte writer and encoders used to build test files.

struct Writer {
    Bytes b;
    void U8(uint32_t v) { b.push_back(uint8_t(v)); }
    void U16(uint32_t v) { U8(v >> 8); U8(v); }
    void U32(uint32_t v) { U16(v >> 16); U16(v); }
    void U64(uint64_t v) { U32(uint32_t(v >> 32)); U32(uint32_t(v)); }
    void Len(uint64_t v, bool wide) { if (wide) U64(v); else U32(uint32_t(v)); }
    void Raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void Raw(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
    void Str(const char* s) { Raw(s, std::strlen(s)); }
};

void PackBits(const uint8_t* s, size_t n, Bytes& out) {
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && s[i + run] == s[i]) ++run;
        if (run >= 2) {
            out.push_back(uint8_t(257 - run));
            out.push_back(s[i]);
            i += run;
            continue;
        }
        size_t start = i, lit = 0;
        while (i < n && lit < 128) {
            if (i + 1 < n && s[i] == s[i + 1]) break;
            ++i;
            ++lit;
        }
        out.push_back(uint8_t(lit - 1));
        out.insert(out.end(), s + start, s + start + lit);
    }
}

struct BitWriter {
    Bytes out;
    uint32_t buf = 0;
    int count = 0;
    void Bits(uint32_t v, int n) {  // LSB first
        for (int i = 0; i < n; ++i) Bit((v >> i) & 1);
    }
    void Code(uint32_t code, int len) {  // Huffman codes go MSB first
        for (int i = len - 1; i >= 0; --i) Bit((code >> i) & 1);
    }
    void Bit(uint32_t b) {
        buf |= b << count;
        if (++count == 8) Flush();
    }
    void Flush() {
        if (count) out.push_back(uint8_t(buf));
        buf = 0;
        count = 0;
    }
};

// zlib stream using one fixed-Huffman block of literals, or stored blocks.
Bytes Zlib(const Bytes& data, bool stored) {
    Bytes z = {0x78, 0x01};
    if (stored) {
        size_t pos = 0;
        do {
            size_t n = data.size() - pos < 65535 ? data.size() - pos : 65535;
            bool last = pos + n == data.size();
            z.push_back(last ? 1 : 0);
            z.push_back(uint8_t(n));
            z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n));
            z.push_back(uint8_t(~n >> 8));
            z.insert(z.end(), data.begin() + pos, data.begin() + pos + n);
            pos += n;
        } while (pos < data.size());
    } else {
        BitWriter w;
        w.Bits(1, 1);  // final block
        w.Bits(1, 2);  // fixed Huffman
        for (uint8_t v : data) {
            if (v < 144) w.Code(0x30 + v, 8);
            else w.Code(0x190 + (v - 144), 9);
        }
        w.Code(0, 7);  // end of block
        w.Flush();
        z.insert(z.end(), w.out.begin(), w.out.end());
    }
    uint32_t adler = psd::Adler32(data.data(), data.size());
    z.push_back(uint8_t(adler >> 24));
    z.push_back(uint8_t(adler >> 16));
    z.push_back(uint8_t(adler >> 8));
    z.push_back(uint8_t(adler));
    return z;
}

constexpr CLSID kClsidWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};

Bytes WicEncode(const GUID& container, uint32_t w, uint32_t h, const Bytes& bgra) {
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)&f);
    IStream* stream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    IWICBitmapEncoder* enc = nullptr;
    f->CreateEncoder(container, nullptr, &enc);
    enc->Initialize(stream, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    enc->CreateNewFrame(&frame, &props);
    frame->Initialize(props);
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    Bytes pixels = bgra;
    if (fmt != GUID_WICPixelFormat32bppBGRA) {  // JPEG wants 24bpp BGR
        pixels.clear();
        for (size_t i = 0; i < bgra.size(); i += 4) pixels.insert(pixels.end(), {bgra[i], bgra[i + 1], bgra[i + 2]});
    }
    UINT stride = fmt == GUID_WICPixelFormat32bppBGRA ? w * 4 : w * 3;
    frame->WritePixels(h, stride, UINT(pixels.size()), pixels.data());
    frame->Commit();
    enc->Commit();
    STATSTG st = {};
    stream->Stat(&st, STATFLAG_NONAME);
    Bytes out(size_t(st.cbSize.QuadPart));
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    stream->Read(out.data(), ULONG(out.size()), &got);
    props->Release();
    frame->Release();
    enc->Release();
    stream->Release();
    f->Release();
    return out;
}

// ---------------------------------------------------------------------------
// Synthetic PSD/PSB files: four quadrants with known colors.

enum class AlphaKind { None, Merged, SelectionOnly };

struct Rgb { int r, g, b; };

struct Spec {
    std::string name;
    bool psb = false;
    uint16_t mode = psd::kRGB;
    uint16_t depth = 8;
    uint16_t compression = psd::kRLE;
    uint32_t w = 64, h = 48;
    AlphaKind alpha = AlphaKind::None;
    bool lr16 = false;        // layer count in an Lr16 tagged block
    bool thumb = false;       // embedded JPEG (orange)
    bool thumbBgr = false;    // resource 1033 instead of 1036
    bool versionInfo = true;  // resource 1057
    bool realMerged = true;
    int tolerance = 1;
};

const Rgb kOrange = {255, 128, 0};

int ColorChannels(uint16_t mode) {
    return mode == psd::kRGB || mode == psd::kLab ? 3 : mode == psd::kCMYK ? 4 : 1;
}

// Per quadrant (TL, TR, BL, BR): stored 8-bit channel values and the RGB we expect back.
void QuadrantValues(uint16_t mode, int q, uint8_t ch[4], Rgb* expect) {
    static const Rgb kRgb[4] = {{255, 0, 0}, {0, 200, 0}, {30, 60, 250}, {255, 255, 255}};
    static const uint8_t kGray[4] = {0, 85, 170, 255};
    switch (mode) {
        case psd::kRGB:
            ch[0] = uint8_t(kRgb[q].r); ch[1] = uint8_t(kRgb[q].g); ch[2] = uint8_t(kRgb[q].b);
            *expect = kRgb[q];
            break;
        case psd::kCMYK: {  // stored inverted; K' = 255 means no black
            static const uint8_t kCmyk[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {255, 255, 255, 128}, {255, 255, 255, 255}};
            static const Rgb kExpect[4] = {{255, 0, 0}, {0, 255, 0}, {128, 128, 128}, {255, 255, 255}};
            std::memcpy(ch, kCmyk[q], 4);
            *expect = kExpect[q];
            break;
        }
        case psd::kLab: {  // white, black, mid gray, red (D50)
            static const uint8_t kLabv[4][3] = {{255, 128, 128}, {0, 128, 128}, {128, 128, 128}, {138, 209, 198}};
            static const Rgb kExpect[4] = {{255, 255, 255}, {0, 0, 0}, {119, 119, 119}, {255, 0, 0}};
            std::memcpy(ch, kLabv[q], 3);
            *expect = kExpect[q];
            break;
        }
        case psd::kIndexed:
            ch[0] = uint8_t(q);  // palette entry q = kRgb[q]
            *expect = kRgb[q];
            break;
        case psd::kBitmap: {
            static const int kBlack[4] = {1, 0, 0, 1};
            ch[0] = kBlack[q] ? 0 : 255;  // 8-bit stand-in; packed to bits later
            int v = kBlack[q] ? 0 : 255;
            *expect = {v, v, v};
            break;
        }
        default:  // Grayscale, Duotone, Multichannel
            ch[0] = kGray[q];
            *expect = {kGray[q], kGray[q], kGray[q]};
            break;
    }
}

double SrgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

void PutSample(Bytes& row, uint32_t x, uint16_t depth, uint8_t v, bool isAlpha) {
    switch (depth) {
        case 1:
            if (v == 0) row[x >> 3] |= uint8_t(0x80 >> (x & 7));  // 1 = black
            break;
        case 8:
            row[x] = v;
            break;
        case 16: {
            uint32_t v16 = v * 257u;
            row[2 * x] = uint8_t(v16 >> 8);
            row[2 * x + 1] = uint8_t(v16);
            break;
        }
        case 32: {
            float f = isAlpha ? v / 255.0f : float(SrgbToLinear(v / 255.0));
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            for (int i = 0; i < 4; ++i) row[4 * x + i] = uint8_t(bits >> (24 - 8 * i));
            break;
        }
    }
}

void Predict(Bytes& row, uint32_t w, uint16_t depth) {
    if (depth == 8) {
        for (size_t i = row.size() - 1; i > 0; --i) row[i] = uint8_t(row[i] - row[i - 1]);
    } else if (depth == 16) {
        for (uint32_t x = w - 1; x > 0; --x) {
            uint16_t cur = uint16_t((row[2 * x] << 8) | row[2 * x + 1]);
            uint16_t prev = uint16_t((row[2 * x - 2] << 8) | row[2 * x - 1]);
            uint16_t d = uint16_t(cur - prev);
            row[2 * x] = uint8_t(d >> 8);
            row[2 * x + 1] = uint8_t(d);
        }
    } else if (depth == 32) {
        Bytes planes(row.size());
        for (uint32_t x = 0; x < w; ++x)
            for (int k = 0; k < 4; ++k) planes[size_t(k) * w + x] = row[4 * x + k];
        for (size_t i = planes.size() - 1; i > 0; --i) planes[i] = uint8_t(planes[i] - planes[i - 1]);
        row = planes;
    }
}

Bytes MakePsd(const Spec& s) {
    const int colorCh = ColorChannels(s.mode);
    const int channels = colorCh + (s.alpha != AlphaKind::None ? 1 : 0);
    const size_t rowBytes = s.depth == 1 ? (s.w + 7) / 8 : size_t(s.w) * (s.depth / 8);

    // Planar rows: rows[c * h + y]
    std::vector<Bytes> rows(size_t(channels) * s.h, Bytes(rowBytes, 0));
    for (uint32_t y = 0; y < s.h; ++y) {
        for (uint32_t x = 0; x < s.w; ++x) {
            int q = (y >= s.h / 2 ? 2 : 0) + (x >= s.w / 2 ? 1 : 0);
            uint8_t ch[4] = {};
            Rgb dummy;
            QuadrantValues(s.mode, q, ch, &dummy);
            for (int c = 0; c < colorCh; ++c) PutSample(rows[size_t(c) * s.h + y], x, s.depth, ch[c], false);
            if (s.alpha == AlphaKind::Merged)
                PutSample(rows[size_t(colorCh) * s.h + y], x, s.depth, x < s.w / 2 ? 255 : 0, true);
            // SelectionOnly leaves its extra channel at 0: it must not be read as transparency.
        }
    }

    Writer w;
    w.Str("8BPS");
    w.U16(s.psb ? 2 : 1);
    w.Raw("\0\0\0\0\0\0", 6);
    w.U16(uint32_t(channels));
    w.U32(s.h);
    w.U32(s.w);
    w.U16(s.depth);
    w.U16(s.mode);

    // Color mode data
    if (s.mode == psd::kIndexed) {
        static const Rgb kRgb[4] = {{255, 0, 0}, {0, 200, 0}, {30, 60, 250}, {255, 255, 255}};
        Bytes pal(768, 0);
        for (int i = 0; i < 4; ++i) {
            pal[i] = uint8_t(kRgb[i].r);
            pal[256 + i] = uint8_t(kRgb[i].g);
            pal[512 + i] = uint8_t(kRgb[i].b);
        }
        w.U32(768);
        w.Raw(pal);
    } else {
        w.U32(0);
    }

    // Image resources
    Writer res;
    auto resource = [&](uint16_t id, const Bytes& data) {
        res.Str("8BIM");
        res.U16(id);
        res.U16(0);  // empty Pascal name, padded
        res.U32(uint32_t(data.size()));
        res.Raw(data);
        if (data.size() & 1) res.U8(0);
    };
    if (s.versionInfo) {
        Writer v;
        v.U32(1);
        v.U8(s.realMerged ? 1 : 0);
        v.U32(0);  // empty writer name
        v.U32(0);  // empty reader name
        v.U32(1);
        resource(1057, v.b);
    }
    if (s.thumb) {
        const uint32_t tw = 64, th = 48;
        Bytes px(size_t(tw) * th * 4);
        for (size_t i = 0; i < px.size(); i += 4) {
            Rgb c = kOrange;
            if (s.thumbBgr) std::swap(c.r, c.b);  // 1033 stores BGR
            px[i] = uint8_t(c.b);
            px[i + 1] = uint8_t(c.g);
            px[i + 2] = uint8_t(c.r);
            px[i + 3] = 255;
        }
        Bytes jpeg = WicEncode(GUID_ContainerFormatJpeg, tw, th, px);
        Writer t;
        t.U32(1);  // kJpegRGB
        t.U32(tw);
        t.U32(th);
        t.U32((tw * 24 + 31) / 32 * 4);
        t.U32((tw * 24 + 31) / 32 * 4 * th);
        t.U32(uint32_t(jpeg.size()));
        t.U16(24);
        t.U16(1);
        t.Raw(jpeg);
        resource(s.thumbBgr ? 1033 : 1036, t.b);
    }
    w.U32(uint32_t(res.b.size()));
    w.Raw(res.b);

    // Layer and mask information
    if (s.alpha == AlphaKind::None) {
        w.Len(0, s.psb);
    } else {
        int16_t count = s.alpha == AlphaKind::Merged ? -1 : 1;
        Writer lm;
        if (!s.lr16) {
            lm.Len(2, s.psb);
            lm.U16(uint16_t(count));
            lm.U32(0);  // global layer mask
        } else {
            lm.Len(0, s.psb);
            lm.U32(0);
            // An unrelated block with odd length and a pad byte first.
            lm.Str("8BIMlnsr");
            lm.U32(3);
            lm.Raw("abc", 3);
            lm.U8(0);
            lm.Str("8BIMLr16");
            lm.Len(2, s.psb);  // Lr16 uses an 8-byte length in PSB
            lm.U16(uint16_t(count));
        }
        w.Len(lm.b.size(), s.psb);
        w.Raw(lm.b);
    }

    // Image data
    w.U16(s.compression);
    if (s.compression == psd::kRaw) {
        for (const Bytes& r : rows) w.Raw(r);
    } else if (s.compression == psd::kRLE) {
        std::vector<Bytes> packed(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) PackBits(rows[i].data(), rows[i].size(), packed[i]);
        for (const Bytes& p : packed) {
            if (s.psb) w.U32(uint32_t(p.size()));
            else w.U16(uint32_t(p.size()));
        }
        for (const Bytes& p : packed) w.Raw(p);
    } else {
        Bytes all;
        for (Bytes r : rows) {
            if (s.compression == psd::kZipPredict) Predict(r, s.w, s.depth);
            all.insert(all.end(), r.begin(), r.end());
        }
        w.Raw(Zlib(all, s.compression == psd::kZip));
    }
    return w.b;
}

void Pixel(const psd::Image& img, uint32_t x, uint32_t y, Rgb* c, int* a) {
    const uint8_t* p = img.bgra.data() + (size_t(y) * img.width + x) * 4;
    *c = {p[2], p[1], p[0]};
    *a = p[3];
}

bool Near(const Rgb& a, const Rgb& b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

void CheckQuadrants(const Spec& s, const psd::Image& img, const char* label) {
    const uint32_t xs[2] = {img.width / 4, img.width * 3 / 4};
    const uint32_t ys[2] = {img.height / 4, img.height * 3 / 4};
    for (int q = 0; q < 4; ++q) {
        uint8_t ch[4];
        Rgb expect;
        QuadrantValues(s.mode, q, ch, &expect);
        Rgb got;
        int alpha;
        Pixel(img, xs[q & 1], ys[q >> 1], &got, &alpha);
        const bool transparent = s.alpha == AlphaKind::Merged && (q & 1);
        if (transparent) {
            CHECK(alpha == 0, "%s %s q%d: alpha %d, expected 0", s.name.c_str(), label, q, alpha);
            continue;
        }
        CHECK(alpha == 255, "%s %s q%d: alpha %d, expected 255", s.name.c_str(), label, q, alpha);
        int tol = s.mode == psd::kLab && q == 3 ? 40 : s.tolerance;
        CHECK(Near(got, expect, tol), "%s %s q%d: got (%d,%d,%d) expected (%d,%d,%d)", s.name.c_str(), label, q,
              got.r, got.g, got.b, expect.r, expect.g, expect.b);
    }
    CHECK(img.hasAlpha == (s.alpha == AlphaKind::Merged), "%s %s: hasAlpha=%d", s.name.c_str(), label, img.hasAlpha);
}

std::vector<Spec> AllSpecs() {
    std::vector<Spec> specs;
    const uint16_t comps[4] = {psd::kRaw, psd::kRLE, psd::kZip, psd::kZipPredict};
    const char* compNames[4] = {"raw", "rle", "zip", "zip-predict"};
    struct ModeDepth { uint16_t mode; uint16_t depth; const char* name; int tol; };
    const ModeDepth mds[] = {
        {psd::kRGB, 8, "rgb8", 1},       {psd::kRGB, 16, "rgb16", 1},     {psd::kRGB, 32, "rgb32", 2},
        {psd::kGrayscale, 8, "gray8", 1}, {psd::kGrayscale, 16, "gray16", 1}, {psd::kGrayscale, 32, "gray32", 2},
        {psd::kCMYK, 8, "cmyk8", 1},     {psd::kCMYK, 16, "cmyk16", 1},   {psd::kLab, 8, "lab8", 3},
        {psd::kLab, 16, "lab16", 3},     {psd::kIndexed, 8, "indexed", 0}, {psd::kBitmap, 1, "bitmap", 0},
        {psd::kDuotone, 8, "duotone", 1}, {psd::kMultichannel, 8, "multichannel", 1},
    };
    for (const ModeDepth& md : mds) {
        for (int c = 0; c < 4; ++c) {
            for (int psb = 0; psb < 2; ++psb) {
                Spec s;
                s.name = std::string(md.name) + "-" + compNames[c] + (psb ? "-psb" : "");
                s.mode = md.mode;
                s.depth = md.depth;
                s.compression = comps[c];
                s.psb = psb != 0;
                s.tolerance = md.tol;
                s.w = 67;  // odd width exercises bit packing and column math
                s.h = 41;
                specs.push_back(s);
            }
        }
    }
    // Transparency cases
    for (uint16_t depth : {8, 16, 32}) {
        for (int psb = 0; psb < 2; ++psb) {
            Spec s;
            s.depth = depth;
            s.psb = psb != 0;
            s.alpha = AlphaKind::Merged;
            s.lr16 = depth != 8;
            s.tolerance = depth == 32 ? 2 : 1;
            s.name = "rgb" + std::to_string(depth) + "-alpha" + (s.lr16 ? "-lr16" : "") + (psb ? "-psb" : "");
            specs.push_back(s);
        }
    }
    Spec sel;
    sel.name = "rgb8-selection-channel-stays-opaque";
    sel.alpha = AlphaKind::SelectionOnly;
    specs.push_back(sel);
    Spec gra;
    gra.name = "gray8-alpha";
    gra.mode = psd::kGrayscale;
    gra.alpha = AlphaKind::Merged;
    specs.push_back(gra);
    return specs;
}

// ---------------------------------------------------------------------------

void TestComposites() {
    std::printf("composite decoding\n");
    int files = 0;
    for (const Spec& s : AllSpecs()) {
        Bytes file = MakePsd(s);
        psd::MemorySource src(file.data(), file.size());
        psd::Reader r(src, 4096);  // small blocks exercise the cache paths
        psd::Info info;
        psd::Result res = psd::ReadInfo(r, info);
        CHECK(res == psd::Result::Ok, "%s: ReadInfo %s", s.name.c_str(), psd::ResultName(res));
        if (res != psd::Result::Ok) continue;
        CHECK(info.isPsb == s.psb && info.width == s.w && info.height == s.h, "%s: header", s.name.c_str());
        CHECK(info.mergedAlpha == (s.alpha == AlphaKind::Merged), "%s: mergedAlpha=%d", s.name.c_str(), info.mergedAlpha);

        psd::Limits limits;
        psd::Image full, small;
        res = psd::DecodeComposite(r, info, 0, limits, full);
        CHECK(res == psd::Result::Ok, "%s: full decode %s", s.name.c_str(), psd::ResultName(res));
        if (res == psd::Result::Ok) {
            CHECK(full.width == s.w && full.height == s.h, "%s: full size %ux%u", s.name.c_str(), full.width, full.height);
            CheckQuadrants(s, full, "full");
        }
        res = psd::DecodeComposite(r, info, 20, limits, small);
        CHECK(res == psd::Result::Ok, "%s: reduced decode %s", s.name.c_str(), psd::ResultName(res));
        if (res == psd::Result::Ok) {
            const uint32_t longSide = s.w > s.h ? s.w : s.h;
            const uint32_t ew = (s.w * 20 + longSide / 2) / longSide, eh = (s.h * 20 + longSide / 2) / longSide;
            CHECK(small.width == ew && small.height == eh, "%s: reduced size %ux%u, expected %ux%u", s.name.c_str(),
                  small.width, small.height, ew, eh);
            CheckQuadrants(s, small, "reduced");
        }
        ++files;
    }
    std::printf("  %d synthetic files\n", files);
}

void TestSourceSelection() {
    std::printf("thumbnail source selection\n");
    using psdglance::ThumbPath;
    struct Case { const char* name; bool thumb, bgr, realMerged; uint32_t cx; psdglance::Quality q; ThumbPath expect; uint32_t w; Rgb color; };
    const Rgb red = {255, 0, 0};
    const Case cases[] = {
        {"small icon uses embedded jpeg", true, false, true, 48, psdglance::Quality::Sharp, ThumbPath::EmbeddedJpeg, 48, kOrange},
        {"large icon uses composite", true, false, true, 256, psdglance::Quality::Sharp, ThumbPath::Composite, 64, red},
        {"no real composite falls back to upscaled jpeg", true, false, false, 256, psdglance::Quality::Sharp, ThumbPath::UpscaledJpeg, 256, kOrange},
        {"fast mode uses jpeg", true, false, true, 256, psdglance::Quality::Fast, ThumbPath::UpscaledJpeg, 256, kOrange},
        {"BGR thumbnail (1033) colors", true, true, true, 32, psdglance::Quality::Sharp, ThumbPath::EmbeddedJpeg, 32, kOrange},
        {"no jpeg uses composite at small size", false, false, true, 32, psdglance::Quality::Sharp, ThumbPath::Composite, 32, red},
    };
    for (const Case& c : cases) {
        Spec s;
        s.thumb = c.thumb;
        s.thumbBgr = c.bgr;
        s.realMerged = c.realMerged;
        Bytes file = MakePsd(s);
        psd::MemorySource src(file.data(), file.size());
        psd::Reader r(src);
        psdglance::ThumbRequest req;
        req.cx = c.cx;
        req.quality = c.q;
        psd::Image img;
        psdglance::ThumbOutcome oc;
        HRESULT hr = psdglance::MakeThumbnail(r, req, img, &oc);
        CHECK(SUCCEEDED(hr), "%s: hr=0x%08lx", c.name, (unsigned long)hr);
        if (FAILED(hr)) continue;
        CHECK(oc.path == c.expect, "%s: path %s", c.name, psdglance::ThumbPathName(oc.path));
        CHECK(img.width == c.w, "%s: width %u, expected %u", c.name, img.width, c.w);
        Rgb got;
        int a;
        Pixel(img, img.width / 8, img.height / 4, &got, &a);  // inside the top-left quadrant
        CHECK(Near(got, c.color, 14), "%s: color (%d,%d,%d)", c.name, got.r, got.g, got.b);
    }

    Spec none;
    none.realMerged = false;  // no composite and no jpeg
    Bytes file = MakePsd(none);
    psd::MemorySource src(file.data(), file.size());
    psd::Reader r(src);
    psd::Image img;
    psdglance::ThumbRequest req;
    CHECK(FAILED(psdglance::MakeThumbnail(r, req, img)), "nothing usable must fail so Explorer shows the icon");

    // HBITMAP conversion
    Spec s;
    Bytes f2 = MakePsd(s);
    psd::MemorySource src2(f2.data(), f2.size());
    psd::Reader r2(src2);
    psd::Image img2;
    if (SUCCEEDED(psdglance::MakeThumbnail(r2, req, img2))) {
        HBITMAP hbm = nullptr;
        CHECK(SUCCEEDED(psdglance::ImageToHBitmap(img2, &hbm)) && hbm, "ImageToHBitmap");
        BITMAP bm = {};
        GetObjectW(hbm, sizeof(bm), &bm);
        CHECK(bm.bmWidth == 64 && bm.bmHeight == 48 && bm.bmBitsPixel == 32, "HBITMAP is 64x48x32");
        DeleteObject(hbm);
    }
}

uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t Rand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return uint32_t(g_rng >> 16);
}

void TestReaderCache() {
    std::printf("reader cache\n");
    Bytes data(3 * 1024 * 1024 + 123);
    for (uint8_t& b : data) b = uint8_t(Rand());
    psd::MemorySource src(data.data(), data.size());
    for (size_t capacity : {size_t(8192), size_t(65536), size_t(262144)}) {
        psd::Reader r(src, capacity);
        Bytes buf;
        bool ok = true;
        for (int i = 0; i < 20000 && ok; ++i) {
            size_t len = Rand() % (i % 7 == 0 ? 400000 : 3000) + 1;
            uint64_t off = Rand() % data.size();
            if (i % 3 == 0) off = (off / 1000) * 1000;  // some sequential-ish
            buf.resize(len);
            bool inRange = off + len <= data.size();
            bool got = r.Read(off, buf.data(), len);
            if (got != inRange || (got && std::memcmp(buf.data(), data.data() + off, len) != 0)) ok = false;
        }
        CHECK(ok, "random reads match source (capacity %zu)", capacity);
    }
}

void TestInflateDynamic() {
    std::printf("inflate (dynamic Huffman via a WIC PNG)\n");
    const uint32_t w = 96, h = 64;
    Bytes px(size_t(w) * h * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t((i % 4 == 3) ? 255 : (Rand() % 4 ? (i / 7) : Rand()));
    Bytes png = WicEncode(GUID_ContainerFormatPng, w, h, px);
    Bytes z;
    for (size_t p = 8; p + 12 <= png.size();) {
        uint32_t len = psd::Be32(&png[p]);
        if (!std::memcmp(&png[p + 4], "IDAT", 4)) z.insert(z.end(), png.begin() + p + 8, png.begin() + p + 8 + len);
        p += 12 + len;
    }
    CHECK(z.size() > 6, "PNG has IDAT data");
    if (z.size() <= 6) return;
    Bytes out(size_t(h) * (1 + w * 4) + 16);
    size_t produced = 0;
    bool ok = psd::Inflate(z.data(), z.size(), out.data(), out.size(), &produced);
    CHECK(ok && produced == size_t(h) * (1 + w * 4), "inflate ok=%d produced=%zu", ok, produced);
    uint32_t adler = psd::Be32(&z[z.size() - 4]);
    CHECK(ok && psd::Adler32(out.data(), produced) == adler, "Adler-32 matches");
}

void TestPackBits() {
    std::printf("packbits\n");
    bool ok = true;
    for (int i = 0; i < 2000 && ok; ++i) {
        Bytes in(Rand() % 700 + 1);
        for (uint8_t& b : in) b = uint8_t(Rand() % (i % 3 + 2));
        Bytes packed, out(in.size());
        PackBits(in.data(), in.size(), packed);
        size_t n = psd::UnpackBits(packed.data(), packed.size(), out.data(), out.size());
        if (n != in.size() || out != in) ok = false;
        // Garbage input must stay in bounds.
        Bytes junk(Rand() % 64);
        for (uint8_t& b : junk) b = uint8_t(Rand());
        psd::UnpackBits(junk.data(), junk.size(), out.data(), out.size());
    }
    CHECK(ok, "PackBits round trip");
}

void RunThumbnail(const Bytes& file, uint32_t cx) {
    psd::MemorySource src(file.data(), file.size());
    psd::Reader r(src, 8192);
    psdglance::ThumbRequest req;
    req.cx = cx;
    req.limits.timeBudgetMs = 2000;
    psd::Image img;
    psdglance::MakeThumbnail(r, req, img);
}

void TestTruncation() {
    std::printf("truncated files\n");
    int runs = 0;
    for (const Spec& s : AllSpecs()) {
        if (runs > 4000) break;
        Bytes file = MakePsd(s);
        for (size_t len = 0; len < file.size(); len += 1 + file.size() / 60) {
            Bytes cut(file.begin(), file.begin() + len);
            RunThumbnail(cut, 64);
            ++runs;
        }
    }
    CHECK(true, "survived");
    std::printf("  %d truncated files, no crash\n", runs);
}

void TestFuzz(int iterations) {
    std::printf("mutation fuzzing\n");
    std::vector<Bytes> seeds;
    for (const Spec& s : AllSpecs())
        if (seeds.size() < 80) seeds.push_back(MakePsd(s));
    Spec t;
    t.thumb = true;
    seeds.push_back(MakePsd(t));
    const uint32_t sizes[] = {32, 96, 256, 1024};
    double worst = 0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    for (int i = 0; i < iterations; ++i) {
        Bytes f = seeds[Rand() % seeds.size()];
        int mutations = 1 + Rand() % 8;
        for (int m = 0; m < mutations; ++m) {
            size_t pos = Rand() % (Rand() % 4 == 0 ? (f.size() < 64 ? f.size() : 64) : f.size());
            switch (Rand() % 4) {
                case 0: f[pos] = uint8_t(Rand()); break;
                case 1: f[pos] ^= uint8_t(1 << (Rand() % 8)); break;
                case 2: f[pos] = Rand() % 2 ? 0xFF : 0x00; break;
                case 3: if (f.size() > 30) f.resize(f.size() - Rand() % (f.size() / 4)); break;
            }
            if (f.empty()) break;
        }
        QueryPerformanceCounter(&t0);
        RunThumbnail(f, sizes[Rand() % 4]);
        QueryPerformanceCounter(&t1);
        double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        if (ms > worst) worst = ms;
    }
    CHECK(worst < 3000, "slowest mutated file took %.0f ms", worst);
    std::printf("  %d mutated files, no crash, slowest %.1f ms\n", iterations, worst);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int fuzz = argc > 1 ? _wtoi(argv[1]) : 20000;
    TestComposites();
    TestSourceSelection();
    TestReaderCache();
    TestInflateDynamic();
    TestPackBits();
    TestTruncation();
    TestFuzz(fuzz);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
