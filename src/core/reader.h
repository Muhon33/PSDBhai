#pragma once
// Random-access byte sources and a caching reader on top of them.
// The IStream Explorer hands us may be a cross-process proxy, so the decoder
// must not issue thousands of tiny reads; but row skipping also means we must
// not fetch far more than we need. Reader keeps a few independent read-ahead
// blocks: each grows while its reads are sequential (one per channel stream
// during composite decoding) and stays small for scattered reads.

#include <cstddef>
#include <cstdint>

#include "buffer.h"

namespace psd {

class Source {
public:
    virtual ~Source() = default;
    // Reads up to len bytes at offset. Returns false on I/O error; a short
    // read at end of data is not an error (got < len).
    virtual bool ReadAt(uint64_t offset, void* buf, size_t len, size_t* got) = 0;
    virtual uint64_t Size() const = 0;
};

class MemorySource final : public Source {
public:
    MemorySource(const void* data, size_t size)
        : data_(static_cast<const uint8_t*>(data)), size_(size) {}
    bool ReadAt(uint64_t offset, void* buf, size_t len, size_t* got) override;
    uint64_t Size() const override { return size_; }

private:
    const uint8_t* data_;
    size_t size_;
};

class Reader {
public:
    explicit Reader(Source& src, size_t blockCapacity = 256 * 1024);

    // Exact read: false unless all len bytes were read.
    bool Read(uint64_t offset, void* buf, size_t len);
    bool U8(uint64_t offset, uint8_t* v) { return Read(offset, v, 1); }
    bool U16(uint64_t offset, uint16_t* v);
    bool U32(uint64_t offset, uint32_t* v);
    bool U64(uint64_t offset, uint64_t* v);

    uint64_t Size() const { return size_; }
    uint64_t BytesFetched() const { return fetched_; }  // bytes pulled from the source
    uint64_t SourceReads() const { return reads_; }

private:
    static constexpr int kBlocks = 6;
    static constexpr size_t kMinWindow = 8 * 1024;

    struct Block {
        Buf<uint8_t> data;
        uint64_t offset = 0;
        size_t len = 0;
        size_t window = kMinWindow;
        uint64_t lastUse = 0;
    };

    bool DirectRead(uint64_t offset, void* buf, size_t len);

    Source& src_;
    uint64_t size_;
    size_t capacity_;
    Block blocks_[kBlocks];
    uint64_t tick_ = 0;
    uint64_t fetched_ = 0;
    uint64_t reads_ = 0;
};

}  // namespace psd
