#include "registration.h"

#include <shlobj.h>
#include <strsafe.h>

#include <cwchar>

#include "config.h"

namespace psdbhai {

namespace {

HKEY Root(Scope scope) { return scope == Scope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

// Software\Classes\CLSID\{ours}
void ClassKeyPath(wchar_t* out, size_t cch) {
    StringCchPrintfW(out, cch, L"Software\\Classes\\CLSID\\%s", kClsidString);
}

// Software\Classes\.psd\ShellEx\{E357FCCD-...}
void HandlerKeyPath(const wchar_t* ext, wchar_t* out, size_t cch) {
    StringCchPrintfW(out, cch, L"Software\\Classes\\%s\\ShellEx\\%s", ext, kThumbnailHandlerKey);
}

// Value under Software\PSDBhai remembering a handler we replaced.
void BackupValueName(const wchar_t* ext, wchar_t* out, size_t cch) {
    StringCchPrintfW(out, cch, L"PreviousHandler%s", ext);
}

bool ReadString(HKEY root, const wchar_t* key, const wchar_t* value, wchar_t* out, DWORD cch) {
    DWORD bytes = cch * sizeof(wchar_t);
    out[0] = 0;
    return RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, out, &bytes) == ERROR_SUCCESS && out[0];
}

LSTATUS WriteString(HKEY root, const wchar_t* key, const wchar_t* value, const wchar_t* data) {
    return RegSetKeyValueW(root, key, value, REG_SZ, data, DWORD((wcslen(data) + 1) * sizeof(wchar_t)));
}

// Deletes key if it has no values and no subkeys, then walks up to stopAt.
void DeleteEmptyKeys(HKEY root, wchar_t* path, size_t stopAtLength) {
    while (wcslen(path) > stopAtLength) {
        HKEY key;
        if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
        DWORD subkeys = 0, values = 0;
        LSTATUS st = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                      nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(key);
        if (st != ERROR_SUCCESS || subkeys != 0 || values != 0) return;
        if (RegDeleteKeyW(root, path) != ERROR_SUCCESS) return;
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash) return;
        *slash = 0;
    }
}

}  // namespace

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

const wchar_t* ScopeName(Scope scope) { return scope == Scope::Machine ? L"all users" : L"this user"; }

bool IsClassRegistered(Scope scope, wchar_t* dllPath, DWORD dllPathChars) {
    wchar_t key[256], path[MAX_PATH];
    ClassKeyPath(key, 256);
    StringCchCatW(key, 256, L"\\InprocServer32");
    if (!ReadString(Root(scope), key, nullptr, path, MAX_PATH)) return false;
    if (dllPath && dllPathChars) StringCchCopyW(dllPath, dllPathChars, path);
    return true;
}

bool IsExtensionEnabled(Scope scope, const wchar_t* ext) {
    wchar_t key[256], value[64];
    HandlerKeyPath(ext, key, 256);
    return ReadString(Root(scope), key, nullptr, value, 64) && _wcsicmp(value, kClsidString) == 0;
}

LSTATUS SetExtensionEnabled(Scope scope, const wchar_t* ext, bool enable) {
    const HKEY root = Root(scope);
    wchar_t key[256], current[64], backupName[64], backup[64];
    HandlerKeyPath(ext, key, 256);
    BackupValueName(ext, backupName, 64);
    const bool hasCurrent = ReadString(root, key, nullptr, current, 64);
    const bool isOurs = hasCurrent && _wcsicmp(current, kClsidString) == 0;

    if (enable) {
        if (isOurs) return ERROR_SUCCESS;
        if (hasCurrent) {
            LSTATUS st = WriteString(root, kSettingsKey, backupName, current);
            if (st != ERROR_SUCCESS) return st;
        }
        return WriteString(root, key, nullptr, kClsidString);
    }

    if (!isOurs) return ERROR_SUCCESS;  // someone else's handler: leave it alone
    if (ReadString(root, kSettingsKey, backupName, backup, 64)) {
        LSTATUS st = WriteString(root, key, nullptr, backup);
        if (st != ERROR_SUCCESS) return st;
        RegDeleteKeyValueW(root, kSettingsKey, backupName);
        return ERROR_SUCCESS;
    }
    LSTATUS st = RegDeleteKeyW(root, key);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) return st;
    // Remove the ShellEx and extension keys too if we left them empty.
    wchar_t parent[256];
    StringCchPrintfW(parent, 256, L"Software\\Classes\\%s\\ShellEx", ext);
    DeleteEmptyKeys(root, parent, wcslen(L"Software\\Classes"));
    return ERROR_SUCCESS;
}

LSTATUS Register(Scope scope, const wchar_t* dllPath, bool psd, bool psb) {
    const HKEY root = Root(scope);
    wchar_t key[256];
    ClassKeyPath(key, 256);
    LSTATUS st = WriteString(root, key, nullptr, kHandlerName);
    if (st != ERROR_SUCCESS) return st;
    StringCchCatW(key, 256, L"\\InprocServer32");
    if ((st = WriteString(root, key, nullptr, dllPath)) != ERROR_SUCCESS) return st;
    if ((st = WriteString(root, key, L"ThreadingModel", L"Apartment")) != ERROR_SUCCESS) return st;

    if (scope == Scope::Machine) {
        // Needed where policy only allows approved shell extensions.
        WriteString(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                    kClsidString, kHandlerName);
    }
    if ((st = SetExtensionEnabled(scope, L".psd", psd)) != ERROR_SUCCESS) return st;
    if ((st = SetExtensionEnabled(scope, L".psb", psb)) != ERROR_SUCCESS) return st;
    NotifyShell();
    return ERROR_SUCCESS;
}

LSTATUS Unregister(Scope scope) {
    const HKEY root = Root(scope);
    LSTATUS result = ERROR_SUCCESS;
    for (const wchar_t* ext : kExtensions) {
        LSTATUS st = SetExtensionEnabled(scope, ext, false);
        if (st != ERROR_SUCCESS) result = st;
    }
    wchar_t key[256];
    ClassKeyPath(key, 256);
    LSTATUS st = RegDeleteTreeW(root, key);
    if (st == ERROR_SUCCESS) RegDeleteKeyW(root, key);
    else if (st != ERROR_FILE_NOT_FOUND) result = st;
    if (scope == Scope::Machine) {
        RegDeleteKeyValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                           kClsidString);
    }
    // The quality preference lives in HKCU for every install type.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Quality");
    wchar_t settings[64];
    StringCchCopyW(settings, 64, kSettingsKey);
    DeleteEmptyKeys(root, settings, wcslen(L"Software"));
    StringCchCopyW(settings, 64, kSettingsKey);
    DeleteEmptyKeys(HKEY_CURRENT_USER, settings, wcslen(L"Software"));
    NotifyShell();
    return result;
}

void NotifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

}  // namespace psdbhai
