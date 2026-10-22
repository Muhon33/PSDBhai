#include "thumbnailer.h"

#include <shlwapi.h>
#include <wincodec.h>

#include <cstring>

namespace psdbhai {

namespace {

// The original WIC factory class; available on every Windows version we support.
// {CACAF262-9370-4615-A13B-9F5539DA4C0A}
constexpr CLSID kClsidWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() { if (p_) { p_->Release(); p_ = nullptr; } return &p_; }

private:
    T* p_ = nullptr;
};

}  // namespace

const char* ThumbPathName(ThumbPath path) {
    switch (path) {
        case ThumbPath::None: return "none";
        case ThumbPath::EmbeddedJpeg: return "embedded-jpeg";
        case ThumbPath::Composite: return "composite";
        case ThumbPath::UpscaledJpeg: return "upscaled-jpeg";
    }
    return "?";
}

HRESULT DecodeJpeg(const uint8_t* data, size_t size, uint32_t maxSide, bool allowUpscale, bool swapRedBlue,
                   psd::Image& out) {
    if (!data || size == 0 || size > UINT_MAX) return E_INVALIDARG;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                  reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) return hr;

    ComPtr<IStream> stream;
    *stream.put() = SHCreateMemStream(data, static_cast<UINT>(size));
    if (!stream.get()) return E_OUTOFMEMORY;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.put());
    if (FAILED(hr)) return hr;

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) return FAILED(hr) ? hr : E_FAIL;

    UINT tw = w, th = h;
    const UINT longSide = w > h ? w : h;
    if (maxSide != 0 && (longSide > maxSide || (allowUpscale && longSide < maxSide))) {
        tw = UINT((uint64_t(w) * maxSide + longSide / 2) / longSide);
        th = UINT((uint64_t(h) * maxSide + longSide / 2) / longSide);
        if (tw == 0) tw = 1;
        if (th == 0) th = 1;
    }

    IWICBitmapSource* source = frame.get();
    ComPtr<IWICBitmapScaler> scaler;
    if (tw != w || th != h) {
        hr = factory->CreateBitmapScaler(scaler.put());
        if (FAILED(hr)) return hr;
        WICBitmapInterpolationMode mode = tw < w ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeCubic;
        hr = scaler->Initialize(frame.get(), tw, th, mode);
        if (FAILED(hr)) return hr;
        source = scaler.get();
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.put());
    if (FAILED(hr)) return hr;
    hr = converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    if (!out.Alloc(tw, th)) return E_OUTOFMEMORY;
    hr = converter->CopyPixels(nullptr, tw * 4, UINT(out.bgra.size()), out.bgra.data());
    if (FAILED(hr)) return hr;

    uint8_t* p = out.bgra.data();
    for (size_t i = 0, n = size_t(tw) * th; i < n; ++i, p += 4) {
        if (swapRedBlue) {
            uint8_t t = p[0];
            p[0] = p[2];
            p[2] = t;
        }
        p[3] = 255;
    }
    out.hasAlpha = false;
    return S_OK;
}

HRESULT ImageToHBitmap(const psd::Image& image, HBITMAP* out) {
    *out = nullptr;
    if (image.width == 0 || image.height == 0 || image.width > INT_MAX || image.height > INT_MAX)
        return E_INVALIDARG;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = LONG(image.width);
    bmi.bmiHeader.biHeight = -LONG(image.height);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return E_OUTOFMEMORY;
    std::memcpy(bits, image.bgra.data(), image.bgra.size());
    *out = bitmap;
    return S_OK;
}

HRESULT MakeThumbnail(psd::Reader& reader, const ThumbRequest& request, psd::Image& out, ThumbOutcome* outcome) {
    ThumbOutcome local;
    ThumbOutcome& o = outcome ? *outcome : local;
    o = ThumbOutcome();

    o.infoResult = psd::ReadInfo(reader, o.info);
    if (o.infoResult != psd::Result::Ok) return E_FAIL;
    const psd::Info& info = o.info;
    const uint32_t cx = request.cx ? request.cx : 256;
    const uint32_t jpegLongSide = info.thumbWidth > info.thumbHeight ? info.thumbWidth : info.thumbHeight;

    auto tryJpeg = [&]() -> bool {
        if (!info.hasThumb) return false;
        psd::Buf<uint8_t> jpeg;
        if (psd::ReadEmbeddedJpeg(reader, info, jpeg) != psd::Result::Ok) {
            o.jpegResult = E_FAIL;
            return false;
        }
        o.jpegResult = DecodeJpeg(jpeg.data(), jpeg.size(), cx, true, info.thumbIsBgr, out);
        if (FAILED(o.jpegResult)) return false;
        o.path = jpegLongSide >= cx ? ThumbPath::EmbeddedJpeg : ThumbPath::UpscaledJpeg;
        return true;
    };
    auto tryComposite = [&]() -> bool {
        // A file saved without "Maximize Compatibility" has no real composite.
        if (!info.hasImageData || !info.realMergedData) return false;
        o.compositeTried = true;
        o.compositeResult = psd::DecodeComposite(reader, info, cx, request.limits, out);
        if (o.compositeResult != psd::Result::Ok) return false;
        o.path = ThumbPath::Composite;
        return true;
    };

    switch (request.force) {
        case ForceSource::Jpeg:
            return tryJpeg() ? S_OK : E_FAIL;
        case ForceSource::Composite:
            return tryComposite() ? S_OK : E_FAIL;
        case ForceSource::Auto:
            break;
    }

    // The decision flow from the handoff: cheapest source that is sharp enough.
    if (info.hasThumb && (request.quality == Quality::Fast || jpegLongSide >= cx) && tryJpeg()) return S_OK;
    if (tryComposite()) return S_OK;
    if (tryJpeg()) return S_OK;
    return E_FAIL;
}

}  // namespace psdbhai
