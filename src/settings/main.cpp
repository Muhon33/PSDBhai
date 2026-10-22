// PSDBhai.exe: turns thumbnails on/off per file type, picks the
// quality mode and refreshes the thumbnail cache. The installer also calls
// it with /register or /unregister so all registry logic lives in one place.
//
//   PSDBhai.exe                       open the settings window
//   PSDBhai.exe /register user|machine
//   PSDBhai.exe /unregister user|machine

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <tlhelp32.h>

#include "../win/config.h"
#include "../win/registration.h"
#include "resource.h"

using namespace psdbhai;

namespace {

HINSTANCE g_instance = nullptr;

bool DllPathNextToExe(wchar_t* out, DWORD cch) {
    DWORD n = GetModuleFileNameW(nullptr, out, cch);
    if (n == 0 || n >= cch) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    return SUCCEEDED(StringCchCatW(out, cch, kDllName));
}

bool FileExists(const wchar_t* path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

struct InstallState {
    bool registered = false;
    Scope scope = Scope::User;
    bool canWrite = true;
    wchar_t dllPath[MAX_PATH] = {};
};

InstallState DetectInstall() {
    InstallState s;
    // HKCU overrides HKLM in Explorer's merged view, so check it first.
    if (IsClassRegistered(Scope::User, s.dllPath, MAX_PATH)) {
        s.registered = true;
        s.scope = Scope::User;
    } else if (IsClassRegistered(Scope::Machine, s.dllPath, MAX_PATH)) {
        s.registered = true;
        s.scope = Scope::Machine;
    } else {
        DllPathNextToExe(s.dllPath, MAX_PATH);
    }
    s.canWrite = s.scope == Scope::User || IsProcessElevated();
    return s;
}

void UpdateUi(HWND dlg) {
    InstallState s = DetectInstall();
    CheckDlgButton(dlg, IDC_PSD, s.registered && IsExtensionEnabled(s.scope, L".psd") ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_PSB, s.registered && IsExtensionEnabled(s.scope, L".psb") ? BST_CHECKED : BST_UNCHECKED);

    const bool dllFound = FileExists(s.dllPath);
    wchar_t status[600];
    if (s.registered && !dllFound) {
        StringCchPrintfW(status, 600, L"PSDBhai.dll is missing from %s. Reinstall PSDBhai.", s.dllPath);
    } else if (s.registered && !s.canWrite) {
        StringCchCopyW(status, 600,
                       L"Installed for all users. To change file types, run PSDBhai as administrator.");
    } else if (s.registered) {
        StringCchPrintfW(status, 600, L"Installed for %s. Open a folder of PSD files to see thumbnails.",
                         ScopeName(s.scope));
    } else if (dllFound) {
        StringCchCopyW(status, 600, L"Not turned on yet. Tick a file type to show thumbnails for this user.");
    } else {
        StringCchCopyW(status, 600, L"PSDBhai.dll was not found next to this app. Reinstall PSDBhai.");
    }
    SetDlgItemTextW(dlg, IDC_STATUS, status);

    const bool canToggle = s.canWrite && dllFound;
    EnableWindow(GetDlgItem(dlg, IDC_PSD), canToggle);
    EnableWindow(GetDlgItem(dlg, IDC_PSB), canToggle);
    CheckRadioButton(dlg, IDC_SHARP, IDC_FAST, ReadQuality() == Quality::Fast ? IDC_FAST : IDC_SHARP);
}

void OnToggle(HWND dlg, int control) {
    const wchar_t* ext = control == IDC_PSD ? L".psd" : L".psb";
    const bool on = IsDlgButtonChecked(dlg, control) == BST_CHECKED;
    InstallState s = DetectInstall();
    LSTATUS st;
    if (s.registered) {
        st = SetExtensionEnabled(s.scope, ext, on);
        NotifyShell();
    } else {
        st = on ? Register(Scope::User, s.dllPath, control == IDC_PSD, control == IDC_PSB) : ERROR_SUCCESS;
    }
    if (st != ERROR_SUCCESS) {
        MessageBoxW(dlg,
                    st == ERROR_ACCESS_DENIED ? L"Windows denied the change. Run PSDBhai as administrator."
                                              : L"The change could not be saved to the registry.",
                    kProductName, MB_ICONWARNING);
    }
    UpdateUi(dlg);
}

bool IsExplorerRunning(DWORD session) {
    bool running = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        DWORD ps = 0;
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0 && ProcessIdToSessionId(pe.th32ProcessID, &ps) &&
            ps == session) {
            running = true;
            break;
        }
    }
    CloseHandle(snap);
    return running;
}

void StopExplorer(DWORD session) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        DWORD ps = 0;
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0 || !ProcessIdToSessionId(pe.th32ProcessID, &ps) ||
            ps != session)
            continue;
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (process) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
    }
    CloseHandle(snap);
}

// Deletes thumbcache_*.db; returns the number of files still in use.
int DeleteThumbnailCache(int* deleted) {
    wchar_t dir[MAX_PATH], pattern[MAX_PATH], file[MAX_PATH];
    *deleted = 0;
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir))) return 0;
    StringCchCatW(dir, MAX_PATH, L"\\Microsoft\\Windows\\Explorer\\");
    StringCchPrintfW(pattern, MAX_PATH, L"%sthumbcache_*.db", dir);
    int failed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        StringCchPrintfW(file, MAX_PATH, L"%s%s", dir, fd.cFileName);
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
            ok = DeleteFileW(file) || GetLastError() == ERROR_FILE_NOT_FOUND;
            if (!ok) Sleep(200);
        }
        if (ok) ++*deleted;
        else ++failed;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return failed;
}

void RefreshThumbnails(HWND dlg) {
    if (MessageBoxW(dlg,
                    L"This closes all File Explorer windows, clears the Windows thumbnail cache and restarts "
                    L"File Explorer. Thumbnails are then rebuilt as you browse.\n\nContinue?",
                    kProductName, MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
        return;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    StopExplorer(session);
    int deleted = 0;
    int inUse = DeleteThumbnailCache(&deleted);

    // Windows usually restarts the shell by itself; start it if it did not.
    for (int i = 0; i < 20 && !IsExplorerRunning(session); ++i) Sleep(150);
    if (!IsExplorerRunning(session)) {
        wchar_t explorer[MAX_PATH];
        GetWindowsDirectoryW(explorer, MAX_PATH);
        StringCchCatW(explorer, MAX_PATH, L"\\explorer.exe");
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(explorer, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    SetCursor(old);

    wchar_t msg[256];
    if (inUse == 0)
        StringCchPrintfW(msg, 256, L"Thumbnail cache cleared (%d files).", deleted);
    else
        StringCchPrintfW(msg, 256,
                         L"Cleared %d cache files; %d were in use. Sign out and back in to finish the refresh.",
                         deleted, inUse);
    MessageBoxW(dlg, msg, kProductName, MB_ICONINFORMATION);
}

INT_PTR CALLBACK DialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            // ("small" is a macro in the Windows headers, hence the longer names.)
            HICON bigIcon = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                          GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
            HICON smallIcon = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
            SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
            wchar_t version[128];
            StringCchPrintfW(version, 128, L"Version %s. Free, no telemetry, nothing runs in the background.",
                             kVersion);
            SetDlgItemTextW(dlg, IDC_VERSION, version);
            UpdateUi(dlg);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_PSD:
                case IDC_PSB:
                    if (HIWORD(wp) == BN_CLICKED) OnToggle(dlg, LOWORD(wp));
                    return TRUE;
                case IDC_SHARP:
                case IDC_FAST:
                    if (HIWORD(wp) == BN_CLICKED)
                        WriteQuality(LOWORD(wp) == IDC_FAST ? Quality::Fast : Quality::Sharp);
                    return TRUE;
                case IDC_REFRESH:
                    RefreshThumbnails(dlg);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, 0);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

int RunCommand(int argc, wchar_t** argv) {
    const Scope scope = argc >= 3 && _wcsicmp(argv[2], L"machine") == 0 ? Scope::Machine : Scope::User;
    if (_wcsicmp(argv[1], L"/register") == 0) {
        wchar_t dll[MAX_PATH];
        if (!DllPathNextToExe(dll, MAX_PATH) || !FileExists(dll)) return 2;
        return Register(scope, dll, true, true) == ERROR_SUCCESS ? 0 : 1;
    }
    if (_wcsicmp(argv[1], L"/unregister") == 0) return Unregister(scope) == ERROR_SUCCESS ? 0 : 1;
    return -1;  // not a command: show the window
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2) {
        int code = RunCommand(argc, argv);
        if (code >= 0) {
            LocalFree(argv);
            return code;
        }
    }
    if (argv) LocalFree(argv);

    // Lets the installer ask the user to close this window before updating.
    HANDLE running = CreateMutexW(nullptr, FALSE, L"PSDBhaiRunning");

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, DialogProc, 0);
    if (running) CloseHandle(running);
    return 0;
}
