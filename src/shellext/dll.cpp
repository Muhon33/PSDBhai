// PSDBhai.dll: the Explorer thumbnail handler (COM in-process server).
// Implements only IInitializeWithStream, so Windows runs it in an isolated
// COM surrogate (dllhost.exe): a bad file can never take Explorer down.

#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <thumbcache.h>

#include <new>

#include "../core/reader.h"
#include "../win/config.h"
#include "../win/registration.h"
#include "../win/sources.h"
#include "../win/thumbnailer.h"

namespace {

HINSTANCE g_module = nullptr;
LONG g_objects = 0;
LONG g_locks = 0;

class ThumbnailProvider final : public IInitializeWithStream, public IThumbnailProvider {
public:
    ThumbnailProvider() { InterlockedIncrement(&g_objects); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInitializeWithStream)) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else if (riid == __uuidof(IThumbnailProvider)) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return refs;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (!stream) return E_INVALIDARG;
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = stream;
        stream_->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
        if (!bitmap || !alpha) return E_POINTER;
        *bitmap = nullptr;
        *alpha = WTSAT_UNKNOWN;
        if (!stream_) return E_UNEXPECTED;

        psdbhai::StreamSource source(stream_);
        psd::Reader reader(source);
        psdbhai::ThumbRequest request;
        request.cx = cx;
        request.quality = psdbhai::ReadQuality();
        psd::Image image;
        HRESULT hr = psdbhai::MakeThumbnail(reader, request, image);
        if (FAILED(hr)) return hr;  // Explorer falls back to the normal icon
        hr = psdbhai::ImageToHBitmap(image, bitmap);
        if (SUCCEEDED(hr)) *alpha = image.hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
        return hr;
    }

private:
    ~ThumbnailProvider() {
        if (stream_) stream_->Release();
        InterlockedDecrement(&g_objects);
    }

    LONG refs_ = 1;
    IStream* stream_ = nullptr;
};

class ClassFactory final : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Static instance: lifetime is governed by LockServer and g_objects.
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        ThumbnailProvider* provider = new (std::nothrow) ThumbnailProvider();
        if (!provider) return E_OUTOFMEMORY;
        HRESULT hr = provider->QueryInterface(riid, ppv);
        provider->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_locks);
        else InterlockedDecrement(&g_locks);
        return S_OK;
    }
};

ClassFactory g_factory;

psdbhai::Scope RegistrationScope() {
    // regsvr32 from an elevated prompt installs for everyone; otherwise per user.
    return psdbhai::IsProcessElevated() ? psdbhai::Scope::Machine : psdbhai::Scope::User;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != psdbhai::kClsidThumbProvider) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return E_FAIL;
    LSTATUS st = psdbhai::Register(RegistrationScope(), path, true, true);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer() { return HRESULT_FROM_WIN32(psdbhai::Unregister(RegistrationScope())); }
