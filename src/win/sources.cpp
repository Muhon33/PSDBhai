#include "sources.h"

namespace psdbhai {

StreamSource::StreamSource(IStream* stream) : stream_(stream) {
    stream_->AddRef();
    STATSTG stat = {};
    if (SUCCEEDED(stream_->Stat(&stat, STATFLAG_NONAME))) {
        size_ = stat.cbSize.QuadPart;
    } else {
        ULARGE_INTEGER end = {};
        LARGE_INTEGER zero = {};
        if (SUCCEEDED(stream_->Seek(zero, STREAM_SEEK_END, &end))) size_ = end.QuadPart;
    }
}

StreamSource::~StreamSource() { stream_->Release(); }

bool StreamSource::ReadAt(uint64_t offset, void* buf, size_t len, size_t* got) {
    *got = 0;
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (FAILED(stream_->Seek(pos, STREAM_SEEK_SET, nullptr))) return false;
    uint8_t* out = static_cast<uint8_t*>(buf);
    while (*got < len) {
        size_t left = len - *got;
        ULONG chunk = left > (1u << 30) ? (1u << 30) : static_cast<ULONG>(left);
        ULONG read = 0;
        HRESULT hr = stream_->Read(out + *got, chunk, &read);
        if (FAILED(hr)) return false;
        if (read == 0) break;  // end of stream
        *got += read;
    }
    return true;
}

FileSource::FileSource(const wchar_t* path) {
    file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    LARGE_INTEGER size;
    if (file_ != INVALID_HANDLE_VALUE && GetFileSizeEx(file_, &size)) size_ = uint64_t(size.QuadPart);
}

FileSource::~FileSource() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool FileSource::ReadAt(uint64_t offset, void* buf, size_t len, size_t* got) {
    *got = 0;
    uint8_t* out = static_cast<uint8_t*>(buf);
    while (*got < len) {
        size_t left = len - *got;
        DWORD chunk = left > (1u << 30) ? (1u << 30) : static_cast<DWORD>(left);
        OVERLAPPED ov = {};
        uint64_t at = offset + *got;
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD read = 0;
        if (!ReadFile(file_, out + *got, chunk, &read, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return false;
        }
        if (read == 0) break;
        *got += read;
    }
    return true;
}

}  // namespace psdbhai
