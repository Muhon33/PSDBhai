#pragma once
// Registry registration of the thumbnail handler, per user (HKCU, no admin)
// or per machine (HKLM, admin). Registers on the .psd/.psb extension keys,
// not Photoshop's ProgID, and restores any handler it replaced.

#include <windows.h>

namespace psdbhai {

enum class Scope { User, Machine };

inline constexpr const wchar_t* kExtensions[] = {L".psd", L".psb"};

bool IsProcessElevated();
const wchar_t* ScopeName(Scope scope);

// Writes the COM class (pointing at dllPath) and enables the chosen extensions.
LSTATUS Register(Scope scope, const wchar_t* dllPath, bool psd, bool psb);

// Removes everything Register wrote and restores replaced handlers.
LSTATUS Unregister(Scope scope);

// Turns the handler on or off for one extension (".psd" or ".psb").
LSTATUS SetExtensionEnabled(Scope scope, const wchar_t* extension, bool enable);
bool IsExtensionEnabled(Scope scope, const wchar_t* extension);

// True if our COM class is registered in that scope; optionally returns its DLL path.
bool IsClassRegistered(Scope scope, wchar_t* dllPath = nullptr, DWORD dllPathChars = 0);

// Tells Explorer that file associations changed so it picks up the handler.
void NotifyShell();

}  // namespace psdbhai
