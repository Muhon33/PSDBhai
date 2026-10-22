#pragma once
// psd::Source implementations over an IStream (what Explorer gives us) and
// over a Win32 file handle (CLI and tests).

#include <windows.h>
#include <objidl.h>

#include "../core/reader.h"

namespace psdbhai {

class StreamSource final : public psd::Source {
public:
    explicit StreamSource(IStream* stream);
    ~StreamSource() override;
    bool ReadAt(uint64_t offset, void* buf, size_t len, size_t* got) override;
    uint64_t Size() const override { return size_; }

private:
    IStream* stream_;
    uint64_t size_ = 0;
};

class FileSource final : public psd::Source {
public:
    explicit FileSource(const wchar_t* path);
    ~FileSource() override;
    bool IsOpen() const { return file_ != INVALID_HANDLE_VALUE; }
    bool ReadAt(uint64_t offset, void* buf, size_t len, size_t* got) override;
    uint64_t Size() const override { return size_; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

}  // namespace psdbhai
