// EshIniReader.cpp — Win32 INI API implementation (MBCS, ANSI 'A' functions).
#include "loaders/EshIniReader.h"

#include <Windows.h>
#include <stdlib.h>   // atof
#include <cstring>    // _stricmp, strlen
#include <vector>

std::string EshIniReader::ReadString(const char* section, const char* key, const char* def) const {
    char buf[1024];
    DWORD n = GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), m_path.c_str());
    return std::string(buf, (size_t)n);
}

int EshIniReader::ReadInt(const char* section, const char* key, int def) const {
    return static_cast<int>(GetPrivateProfileIntA(section, key, def, m_path.c_str()));
}

float EshIniReader::ReadFloat(const char* section, const char* key, float def) const {
    // AR reads floats as strings + atof (IniReader.h:219-222). Probe with an
    // empty default so a MISSING key returns the caller's default directly —
    // avoids the FLT_MAX string round-trip AR's std::to_string default needs.
    char buf[256];
    DWORD n = GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), m_path.c_str());
    if (n == 0)
        return def;
    return static_cast<float>(atof(buf));
}

bool EshIniReader::ReadFloatOpt(const char* section, const char* key, float* out) const {
    char buf[256];
    DWORD n = GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), m_path.c_str());
    if (n == 0)
        return false;
    *out = static_cast<float>(atof(buf));
    return true;
}

bool EshIniReader::ReadBool(const char* section, const char* key, bool def) const {
    char buf[32];
    DWORD n = GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), m_path.c_str());
    if (n == 0)
        return def;
    // AR semantics (IniReader.h:224-235): single char -> "!= '0'"; longer
    // strings -> case-insensitive compare. NOTE: AR's vendored reader returns
    // TRUE for the literal "false" (IniReader.h:232 `compare(val,"false",false)`
    // is an equality check) — that inversion is NOT ported; "false" means false.
    if (n == 1)
        return buf[0] != '0';
    return _stricmp(buf, "false") != 0;
}

bool EshIniReader::SectionExists(const char* section) const {
    // Enumerate section names (null-separated, double-null terminated) and
    // compare case-insensitively — mirrors AR CIniReader::SectionExists
    // (IniReader.h:103-133) without a custom parser. Win32 INI limit is 32k.
    std::vector<char> buf(32768);
    DWORD n = GetPrivateProfileSectionNamesA(buf.data(), static_cast<DWORD>(buf.size()), m_path.c_str());
    if (n == 0)
        return false;   // file missing/unreadable (AR IniReader.h:104-107)
    const char* p   = buf.data();
    const char* end = buf.data() + n;
    while (p < end && *p) {
        if (_stricmp(p, section) == 0)
            return true;
        p += strlen(p) + 1;
    }
    return false;
}
