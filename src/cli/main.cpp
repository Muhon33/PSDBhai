// psdbhai-cli.exe: developer tool for PSDBhai.
//
//   psdbhai-cli info   <file.psd>
//   psdbhai-cli render <file.psd> <out.png> [--size N] [--source auto|jpeg|composite] [--fast]
//   psdbhai-cli dll    <PSDBhai.dll> <file.psd> <out.png> [--size N]   (calls the DLL without registering it)
//   psdbhai-cli shell  <file.psd> <out.png> [--size N]                   (asks Explorer's thumbnail pipeline)
//   psdbhai-cli bench  <file-or-folder> [--size N]

#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../core/psd.h"
#include "../win/config.h"
#include "../win/sources.h"
#include "../win/thumbnailer.h"

using namespace psdbhai;

namespace {

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
    void** putVoid() { return reinterpret_cast<void**>(put()); }

private:
    T* p_ = nullptr;
};

constexpr CLSID kClsidWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};

const char* Utf8(const wchar_t* s) {
    static char buf[4][1024];
    static int slot = 0;
    char* out = buf[slot++ & 3];
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, 1024, nullptr, nullptr);
    return out;
}

double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

HRESULT SavePng(const wchar_t* path, uint32_t w, uint32_t h, const uint8_t* bgra) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                  factory.putVoid());
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4,
                                         const_cast<BYTE*>(bgra), bitmap.put());
    if (FAILED(hr)) return hr;
    ComPtr<IWICStream> stream;
    if (FAILED(hr = factory->CreateStream(stream.put()))) return hr;
    if (FAILED(hr = stream->InitializeFromFilename(path, GENERIC_WRITE))) return hr;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()))) return hr;
    if (FAILED(hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return hr;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(hr = encoder->CreateNewFrame(frame.put(), props.put()))) return hr;
    if (FAILED(hr = frame->Initialize(props.get()))) return hr;
    if (FAILED(hr = frame->SetSize(w, h))) return hr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(hr = frame->SetPixelFormat(&format))) return hr;
    if (FAILED(hr = frame->WriteSource(bitmap.get(), nullptr))) return hr;
    if (FAILED(hr = frame->Commit())) return hr;
    return encoder->Commit();
}

HRESULT SaveHBitmapPng(HBITMAP hbm, const wchar_t* path, uint32_t* w, uint32_t* h) {
    BITMAP bm = {};
    if (!GetObjectW(hbm, sizeof(bm), &bm)) return E_FAIL;
    psd::Image img;
    if (!img.Alloc(uint32_t(bm.bmWidth), uint32_t(bm.bmHeight))) return E_OUTOFMEMORY;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hbm, 0, UINT(bm.bmHeight), img.bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != bm.bmHeight) return E_FAIL;
    *w = img.width;
    *h = img.height;
    return SavePng(path, img.width, img.height, img.bgra.data());
}

uint32_t ParseSize(int argc, wchar_t** argv, uint32_t fallback) {
    for (int i = 0; i + 1 < argc; ++i)
        if (!wcscmp(argv[i], L"--size")) return uint32_t(_wtoi(argv[i + 1]));
    return fallback;
}

bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 0; i < argc; ++i)
        if (!wcscmp(argv[i], flag)) return true;
    return false;
}

const wchar_t* FlagValue(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 0; i + 1 < argc; ++i)
        if (!wcscmp(argv[i], flag)) return argv[i + 1];
    return nullptr;
}

int CmdInfo(const wchar_t* file) {
    FileSource src(file);
    if (!src.IsOpen()) { std::printf("cannot open %s\n", Utf8(file)); return 1; }
    psd::Reader reader(src);
    psd::Info info;
    psd::Result r = psd::ReadInfo(reader, info);
    std::printf("file:          %s\n", Utf8(file));
    std::printf("size:          %llu bytes\n", (unsigned long long)src.Size());
    if (r != psd::Result::Ok) { std::printf("parse:         %s\n", psd::ResultName(r)); return 1; }
    std::printf("format:        %s\n", info.isPsb ? "PSB (large document)" : "PSD");
    std::printf("dimensions:    %u x %u\n", info.width, info.height);
    std::printf("color mode:    %s, %u-bit, %u channels\n", psd::ColorModeName(info.mode), info.depth, info.channels);
    if (info.hasThumb)
        std::printf("embedded jpeg: %u x %u, %u bytes%s\n", info.thumbWidth, info.thumbHeight, info.thumbSize,
                    info.thumbIsBgr ? " (BGR, resource 1033)" : "");
    else
        std::printf("embedded jpeg: none\n");
    std::printf("version info:  %s, real composite: %s\n", info.hasVersionInfo ? "yes" : "no",
                info.realMergedData ? "yes" : "no (Maximize Compatibility was off)");
    if (info.hasLayerCount)
        std::printf("layers:        %d%s\n", info.layerCount < 0 ? -info.layerCount : info.layerCount,
                    info.mergedAlpha ? " (composite has transparency)" : "");
    if (info.hasImageData)
        std::printf("composite:     compression %u at offset %llu\n", info.compression,
                    (unsigned long long)info.imageDataOffset);
    std::printf("header bytes read: %llu in %llu reads\n", (unsigned long long)reader.BytesFetched(),
                (unsigned long long)reader.SourceReads());
    return 0;
}

int CmdRender(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    FileSource src(argv[2]);
    if (!src.IsOpen()) { std::printf("cannot open %s\n", Utf8(argv[2])); return 1; }
    psd::Reader reader(src);
    ThumbRequest req;
    req.cx = ParseSize(argc, argv, 256);
    if (HasFlag(argc, argv, L"--fast")) req.quality = Quality::Fast;
    if (const wchar_t* s = FlagValue(argc, argv, L"--source")) {
        if (!wcscmp(s, L"jpeg")) req.force = ForceSource::Jpeg;
        else if (!wcscmp(s, L"composite")) req.force = ForceSource::Composite;
    }
    psd::Image img;
    ThumbOutcome oc;
    double t0 = NowMs();
    HRESULT hr = MakeThumbnail(reader, req, img, &oc);
    double ms = NowMs() - t0;
    if (FAILED(hr)) {
        std::printf("no thumbnail (info: %s, composite: %s, jpeg hr=0x%08lx)\n", psd::ResultName(oc.infoResult),
                    oc.compositeTried ? psd::ResultName(oc.compositeResult) : "not tried", (unsigned long)oc.jpegResult);
        return 1;
    }
    hr = SavePng(argv[3], img.width, img.height, img.bgra.data());
    if (FAILED(hr)) { std::printf("saving PNG failed: 0x%08lx\n", (unsigned long)hr); return 1; }
    std::printf("%s: %ux%u via %s in %.1f ms, %.2f MB read in %llu reads%s\n", Utf8(argv[3]), img.width,
                img.height, ThumbPathName(oc.path), ms, reader.BytesFetched() / 1048576.0,
                (unsigned long long)reader.SourceReads(), img.hasAlpha ? ", has alpha" : "");
    return 0;
}

int CmdDll(int argc, wchar_t** argv) {
    if (argc < 5) return 2;
    UINT cx = ParseSize(argc, argv, 256);
    HMODULE dll = LoadLibraryW(argv[2]);
    if (!dll) { std::printf("cannot load %s (error %lu)\n", Utf8(argv[2]), GetLastError()); return 1; }
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
    if (!getClassObject) { std::printf("DllGetClassObject not exported\n"); return 1; }

    ComPtr<IClassFactory> factory;
    HRESULT hr = getClassObject(kClsidThumbProvider, __uuidof(IClassFactory), factory.putVoid());
    if (FAILED(hr)) { std::printf("DllGetClassObject failed: 0x%08lx\n", (unsigned long)hr); return 1; }
    ComPtr<IInitializeWithStream> init;
    hr = factory->CreateInstance(nullptr, __uuidof(IInitializeWithStream), init.putVoid());
    if (FAILED(hr)) { std::printf("CreateInstance failed: 0x%08lx\n", (unsigned long)hr); return 1; }
    ComPtr<IStream> stream;
    hr = SHCreateStreamOnFileEx(argv[3], STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                stream.put());
    if (FAILED(hr)) { std::printf("cannot open %s\n", Utf8(argv[3])); return 1; }
    if (FAILED(hr = init->Initialize(stream.get(), STGM_READ))) { std::printf("Initialize failed\n"); return 1; }
    ComPtr<IThumbnailProvider> provider;
    if (FAILED(hr = init->QueryInterface(__uuidof(IThumbnailProvider), provider.putVoid()))) {
        std::printf("no IThumbnailProvider\n");
        return 1;
    }
    HBITMAP hbm = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    double t0 = NowMs();
    hr = provider->GetThumbnail(cx, &hbm, &alpha);
    double ms = NowMs() - t0;
    if (FAILED(hr)) { std::printf("GetThumbnail failed: 0x%08lx (Explorer would show the normal icon)\n", (unsigned long)hr); return 1; }
    uint32_t w = 0, h = 0;
    hr = SaveHBitmapPng(hbm, argv[4], &w, &h);
    DeleteObject(hbm);
    if (FAILED(hr)) { std::printf("saving PNG failed\n"); return 1; }
    std::printf("%s: %ux%u from the DLL in %.1f ms, alpha type %s\n", Utf8(argv[4]), w, h, ms,
                alpha == WTSAT_ARGB ? "ARGB" : alpha == WTSAT_RGB ? "RGB" : "unknown");
    return 0;
}

int CmdShell(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    int cx = int(ParseSize(argc, argv, 256));
    wchar_t full[MAX_PATH];
    GetFullPathNameW(argv[2], MAX_PATH, full, nullptr);
    ComPtr<IShellItemImageFactory> factory;
    HRESULT hr = SHCreateItemFromParsingName(full, nullptr, __uuidof(IShellItemImageFactory), factory.putVoid());
    if (FAILED(hr)) { std::printf("cannot open %s\n", Utf8(full)); return 1; }
    HBITMAP hbm = nullptr;
    SIZE size = {cx, cx};
    double t0 = NowMs();
    hr = factory->GetImage(size, SIIGBF_THUMBNAILONLY, &hbm);
    double ms = NowMs() - t0;
    if (FAILED(hr)) { std::printf("Explorer returned no thumbnail (0x%08lx): handler not registered or failed\n", (unsigned long)hr); return 1; }
    uint32_t w = 0, h = 0;
    hr = SaveHBitmapPng(hbm, argv[3], &w, &h);
    DeleteObject(hbm);
    if (FAILED(hr)) { std::printf("saving PNG failed\n"); return 1; }
    std::printf("%s: %ux%u from Explorer's thumbnail pipeline in %.1f ms\n", Utf8(argv[3]), w, h, ms);
    return 0;
}

void BenchFile(const wchar_t* file, uint32_t cx) {
    FileSource src(file);
    if (!src.IsOpen()) return;
    psd::Reader reader(src);
    ThumbRequest req;
    req.cx = cx;
    psd::Image img;
    ThumbOutcome oc;
    double t0 = NowMs();
    HRESULT hr = MakeThumbnail(reader, req, img, &oc);
    double ms = NowMs() - t0;
    const wchar_t* name = wcsrchr(file, L'\\');
    std::printf("%8.1f ms  %10.1f MB  %8.2f MB read  %-14s %s\n", ms, src.Size() / 1048576.0,
                reader.BytesFetched() / 1048576.0, SUCCEEDED(hr) ? ThumbPathName(oc.path) : "FAILED",
                Utf8(name ? name + 1 : file));
}

int CmdBench(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    uint32_t cx = ParseSize(argc, argv, 256);
    std::printf("size %u px\n    time        file    read          source         name\n", cx);
    DWORD attrs = GetFileAttributesW(argv[2]);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 1;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        BenchFile(argv[2], cx);
        return 0;
    }
    static const wchar_t* const kPatterns[] = {L"*.psd", L"*.psb"};
    for (const wchar_t* ext : kPatterns) {
        wchar_t pattern[MAX_PATH], path[MAX_PATH];
        swprintf(pattern, MAX_PATH, L"%ls\\%ls", argv[2], ext);
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(pattern, &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            swprintf(path, MAX_PATH, L"%ls\\%ls", argv[2], fd.cFileName);
            BenchFile(path, cx);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return 0;
}

void Usage() {
    std::printf(
        "psdbhai-cli - PSDBhai developer tool\n\n"
        "  psdbhai-cli info   <file.psd>\n"
        "  psdbhai-cli render <file.psd> <out.png> [--size N] [--source auto|jpeg|composite] [--fast]\n"
        "  psdbhai-cli dll    <PSDBhai.dll> <file.psd> <out.png> [--size N]\n"
        "  psdbhai-cli shell  <file.psd> <out.png> [--size N]\n"
        "  psdbhai-cli bench  <file-or-folder> [--size N]\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { Usage(); return 2; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int rc = 2;
    const wchar_t* cmd = argv[1];
    if (!wcscmp(cmd, L"info")) rc = CmdInfo(argv[2]);
    else if (!wcscmp(cmd, L"render")) rc = CmdRender(argc, argv);
    else if (!wcscmp(cmd, L"dll")) rc = CmdDll(argc, argv);
    else if (!wcscmp(cmd, L"shell")) rc = CmdShell(argc, argv);
    else if (!wcscmp(cmd, L"bench")) rc = CmdBench(argc, argv);
    if (rc == 2) Usage();
    CoUninitialize();
    return rc;
}
