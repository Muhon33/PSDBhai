#include "reader.h"

#include <cstring>

namespace psd {

bool MemorySource::ReadAt(uint64_t offset, void* buf, size_t len, size_t* got) {
    *got = 0;
    if (offset >= size_) return true;
    size_t avail = size_ - static_cast<size_t>(offset);
    size_t n = len < avail ? len : avail;
    std::memcpy(buf, data_ + offset, n);
    *got = n;
    return true;
}

Reader::Reader(Source& src, size_t blockCapacity)
    : src_(src), size_(src.Size()), capacity_(blockCapacity < kMinWindow ? kMinWindow : blockCapacity) {}

bool Reader::DirectRead(uint64_t offset, void* buf, size_t len) {
    size_t got = 0;
    ++reads_;
    if (!src_.ReadAt(offset, buf, len, &got) || got != len) return false;
    fetched_ += got;
    return true;
}

bool Reader::Read(uint64_t offset, void* buf, size_t len) {
    if (len == 0) return true;
    if (offset > size_ || len > size_ - offset) return false;
    uint8_t* out = static_cast<uint8_t*>(buf);

    while (len > 0) {
        Block* hit = nullptr;
        for (Block& b : blocks_) {
            if (b.len && offset >= b.offset && offset - b.offset < b.len) {
                hit = &b;
                break;
            }
        }
        if (hit) {
            size_t pos = static_cast<size_t>(offset - hit->offset);
            size_t n = hit->len - pos < len ? hit->len - pos : len;
            std::memcpy(out, hit->data.data() + pos, n);
            hit->lastUse = ++tick_;
            out += n;
            offset += n;
            len -= n;
            continue;
        }
        if (len >= capacity_) return DirectRead(offset, out, len);

        // A block that ends exactly here is a sequential stream: reuse it and
        // double its read-ahead. Otherwise recycle the least recently used
        // block with a small window.
        Block* b = nullptr;
        size_t window = kMinWindow;
        for (Block& c : blocks_) {
            if (c.len && c.offset + c.len == offset) {
                b = &c;
                window = c.window * 2 < capacity_ ? c.window * 2 : capacity_;
                break;
            }
        }
        if (!b) {
            b = &blocks_[0];
            for (Block& c : blocks_)
                if (c.lastUse < b->lastUse) b = &c;
        }
        if (b->data.empty() && !b->data.Alloc(capacity_)) return DirectRead(offset, out, len);

        size_t want = (len + 4095) & ~size_t(4095);
        if (want < window) want = window;
        if (want > capacity_) want = capacity_;
        if (want > size_ - offset) want = static_cast<size_t>(size_ - offset);

        size_t got = 0;
        ++reads_;
        b->len = 0;
        if (!src_.ReadAt(offset, b->data.data(), want, &got) || got == 0) return false;
        fetched_ += got;
        b->offset = offset;
        b->len = got;
        b->window = window;
        b->lastUse = ++tick_;
    }
    return true;
}

bool Reader::U16(uint64_t offset, uint16_t* v) {
    uint8_t b[2];
    if (!Read(offset, b, 2)) return false;
    *v = Be16(b);
    return true;
}

bool Reader::U32(uint64_t offset, uint32_t* v) {
    uint8_t b[4];
    if (!Read(offset, b, 4)) return false;
    *v = Be32(b);
    return true;
}

bool Reader::U64(uint64_t offset, uint64_t* v) {
    uint8_t b[8];
    if (!Read(offset, b, 8)) return false;
    *v = Be64(b);
    return true;
}

}  // namespace psd
