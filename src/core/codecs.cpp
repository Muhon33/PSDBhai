#include "codecs.h"

#include <cstring>

namespace psd {

size_t UnpackBits(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    size_t in = 0, out = 0;
    while (in < srcLen && out < dstLen) {
        int8_t n = static_cast<int8_t>(src[in++]);
        if (n >= 0) {
            size_t count = static_cast<size_t>(n) + 1;
            if (count > srcLen - in) count = srcLen - in;
            if (count > dstLen - out) count = dstLen - out;
            std::memcpy(dst + out, src + in, count);
            in += count;
            out += count;
        } else if (n != -128) {
            if (in >= srcLen) break;
            size_t count = static_cast<size_t>(1 - n);
            if (count > dstLen - out) count = dstLen - out;
            std::memset(dst + out, src[in++], count);
            out += count;
        }
    }
    return out;
}

uint32_t Adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    while (len > 0) {
        size_t n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

// ---------------------------------------------------------------------------
// Inflate: a compact canonical-Huffman decoder (RFC 1950/1951). Composites
// stored with ZIP are rare, so clarity wins over speed here.

namespace {

struct Huffman {
    uint16_t count[16];    // number of codes of each length
    uint16_t symbol[288];  // symbols ordered by code
};

struct InflateState {
    const uint8_t* in;
    size_t inLen;
    size_t inPos;
    uint8_t* out;
    size_t outLen;
    size_t outPos;
    uint32_t bitBuf;
    int bitCount;
    bool error;
};

int Bits(InflateState& s, int need) {
    uint32_t val = s.bitBuf;
    while (s.bitCount < need) {
        if (s.inPos >= s.inLen) {
            s.error = true;
            return 0;
        }
        val |= static_cast<uint32_t>(s.in[s.inPos++]) << s.bitCount;
        s.bitCount += 8;
    }
    s.bitBuf = val >> need;
    s.bitCount -= need;
    return static_cast<int>(val & ((1u << need) - 1));
}

int Decode(InflateState& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= Bits(s, 1);
        if (s.error) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// Returns 0 for a complete code, > 0 for incomplete, < 0 for over-subscribed.
int Build(Huffman& h, const uint16_t* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i] != 0) h.symbol[offs[lengths[i]]++] = uint16_t(i);
    return left;
}

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(InflateState& s, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = Decode(s, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (s.outPos >= s.outLen) return false;
            s.out[s.outPos++] = static_cast<uint8_t>(sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + static_cast<size_t>(Bits(s, kLenExtra[sym]));
            int dsym = Decode(s, dist);
            if (dsym < 0 || dsym >= 30) return false;
            size_t d = kDistBase[dsym] + static_cast<size_t>(Bits(s, kDistExtra[dsym]));
            if (s.error || d > s.outPos || len > s.outLen - s.outPos) return false;
            uint8_t* o = s.out + s.outPos;
            const uint8_t* from = o - d;
            for (size_t i = 0; i < len; ++i) o[i] = from[i];
            s.outPos += len;
        }
    }
}

bool Stored(InflateState& s) {
    s.bitBuf = 0;  // skip to the byte boundary
    s.bitCount = 0;
    if (s.inLen - s.inPos < 4) return false;
    const uint8_t* p = s.in + s.inPos;
    unsigned len = p[0] | (p[1] << 8);
    unsigned nlen = p[2] | (p[3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xFFFF)) return false;
    if (len > s.inLen - s.inPos || len > s.outLen - s.outPos) return false;
    std::memcpy(s.out + s.outPos, s.in + s.inPos, len);
    s.inPos += len;
    s.outPos += len;
    return true;
}

struct FixedTables {
    Huffman lit, dist;
    FixedTables() {
        uint16_t lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        Build(lit, lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        Build(dist, lengths, 30);
    }
};

bool Fixed(InflateState& s) {
    static const FixedTables tables;  // thread-safe one-time init
    return Codes(s, tables.lit, tables.dist);
}

bool Dynamic(InflateState& s) {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = Bits(s, 5) + 257;
    int ndist = Bits(s, 5) + 1;
    int ncode = Bits(s, 4) + 4;
    if (s.error || nlen > 286 || ndist > 30) return false;

    uint16_t lengths[320];
    int i = 0;
    for (; i < ncode; ++i) lengths[kOrder[i]] = static_cast<uint16_t>(Bits(s, 3));
    for (; i < 19; ++i) lengths[kOrder[i]] = 0;
    if (s.error) return false;

    Huffman lit, dist;
    if (Build(lit, lengths, 19) != 0) return false;

    int index = 0;
    while (index < nlen + ndist) {
        int sym = Decode(s, lit);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[index++] = static_cast<uint16_t>(sym);
            continue;
        }
        uint16_t len = 0;
        int repeat;
        if (sym == 16) {
            if (index == 0) return false;
            len = lengths[index - 1];
            repeat = 3 + Bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + Bits(s, 3);
        } else {
            repeat = 11 + Bits(s, 7);
        }
        if (s.error || index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = len;
    }
    if (lengths[256] == 0) return false;

    int err = Build(lit, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lit.count[0] != 1)) return false;
    err = Build(dist, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dist.count[0] != 1)) return false;
    return Codes(s, lit, dist);
}

bool InflateRaw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen, size_t* produced) {
    InflateState s{src, srcLen, 0, dst, dstLen, 0, 0, 0, false};
    int last;
    do {
        last = Bits(s, 1);
        int type = Bits(s, 2);
        if (s.error) return false;
        bool ok = false;
        if (type == 0) ok = Stored(s);
        else if (type == 1) ok = Fixed(s);
        else if (type == 2) ok = Dynamic(s);
        if (!ok) return false;
    } while (!last);
    *produced = s.outPos;
    return true;
}

}  // namespace

bool Inflate(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen, size_t* produced) {
    *produced = 0;
    if (srcLen >= 2) {
        unsigned cmf = src[0], flg = src[1];
        bool zlib = (cmf & 0x0F) == 8 && ((cmf << 8) | flg) % 31 == 0 && !(flg & 0x20);
        if (zlib) return InflateRaw(src + 2, srcLen - 2, dst, dstLen, produced);
    }
    return InflateRaw(src, srcLen, dst, dstLen, produced);
}

}  // namespace psd
