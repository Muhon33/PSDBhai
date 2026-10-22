#pragma once
// Owning buffer for trivially copyable types that never throws. Allocation
// failure is reported by Alloc() so the shell extension can fail gracefully
// instead of terminating the host process.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace psd {

template <typename T>
class Buf {
public:
    Buf() = default;
    ~Buf() { std::free(p_); }
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
    Buf(Buf&& o) noexcept : p_(o.p_), n_(o.n_) { o.p_ = nullptr; o.n_ = 0; }
    Buf& operator=(Buf&& o) noexcept {
        if (this != &o) {
            std::free(p_);
            p_ = o.p_; n_ = o.n_;
            o.p_ = nullptr; o.n_ = 0;
        }
        return *this;
    }

    bool Alloc(size_t n, bool zero = false) {
        Reset();
        if (n == 0) return true;
        if (n > SIZE_MAX / sizeof(T)) return false;
        p_ = static_cast<T*>(zero ? std::calloc(n, sizeof(T)) : std::malloc(n * sizeof(T)));
        if (!p_) return false;
        n_ = n;
        return true;
    }
    void Reset() { std::free(p_); p_ = nullptr; n_ = 0; }
    void Zero() { if (p_) std::memset(p_, 0, n_ * sizeof(T)); }

    T* data() { return p_; }
    const T* data() const { return p_; }
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    T& operator[](size_t i) { return p_[i]; }
    const T& operator[](size_t i) const { return p_[i]; }

private:
    T* p_ = nullptr;
    size_t n_ = 0;
};

inline uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

}  // namespace psd
