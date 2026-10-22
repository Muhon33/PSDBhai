#pragma once
// Picks the cheapest image source that is sharp enough for the requested
// size (embedded JPEG, composite, or upscaled JPEG) and produces BGRA pixels.
// Shared by the shell extension and the CLI so both behave identically.

#include <windows.h>

#include "../core/psd.h"
#include "config.h"

namespace psdbhai {

enum class ThumbPath { None, EmbeddedJpeg, Composite, UpscaledJpeg };
const char* ThumbPathName(ThumbPath path);

enum class ForceSource { Auto, Jpeg, Composite };

struct ThumbRequest {
    uint32_t cx = 256;  // long side Explorer asked for
    Quality quality = Quality::Sharp;
    ForceSource force = ForceSource::Auto;
    psd::Limits limits;
};

struct ThumbOutcome {
    psd::Info info;
    psd::Result infoResult = psd::Result::Ok;
    bool compositeTried = false;
    psd::Result compositeResult = psd::Result::Ok;
    HRESULT jpegResult = S_OK;
    ThumbPath path = ThumbPath::None;
};

// Requires COM to be initialized on the calling thread (WIC).
HRESULT MakeThumbnail(psd::Reader& reader, const ThumbRequest& request, psd::Image& out,
                      ThumbOutcome* outcome = nullptr);

// Decodes a JPEG with WIC and scales it to fit maxSide.
HRESULT DecodeJpeg(const uint8_t* data, size_t size, uint32_t maxSide, bool allowUpscale, bool swapRedBlue,
                   psd::Image& out);

// 32-bit top-down DIB section, as IThumbnailProvider::GetThumbnail returns.
HRESULT ImageToHBitmap(const psd::Image& image, HBITMAP* out);

}  // namespace psdbhai
