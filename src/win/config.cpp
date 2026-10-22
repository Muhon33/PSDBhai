#include "config.h"

namespace psdglance {

Quality ReadQuality() {
    DWORD value = 0, size = sizeof(value);
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Quality", RRF_RT_REG_DWORD, nullptr,
                              &value, &size);
    return st == ERROR_SUCCESS && value == DWORD(Quality::Fast) ? Quality::Fast : Quality::Sharp;
}

bool WriteQuality(Quality quality) {
    DWORD value = DWORD(quality);
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Quality", REG_DWORD, &value,
                           sizeof(value)) == ERROR_SUCCESS;
}

}  // namespace psdglance
