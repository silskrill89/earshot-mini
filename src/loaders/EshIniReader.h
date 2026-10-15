// EshIniReader.h — Win32 GetPrivateProfileString/IntA wrapper (MBCS).
//
// Replaces AR's custom CIniReader string parser
// (external/EarShotOpenAL/source/IniReader.h) with the Win32 INI API for the
// EarShot.ini reads (AR Loaders.cpp:454-570). Section/key lookup is
// case-insensitive (Win32 behavior). The INI file lives at
// PLUGIN_PATH/EarShot.ini (AR Loaders.cpp:456).
#pragma once
#include <string>

class EshIniReader {
public:
    explicit EshIniReader(const char* path) : m_path(path) {}

    const std::string& Path() const { return m_path; }

    std::string ReadString(const char* section, const char* key, const char* def) const;
    int   ReadInt(const char* section, const char* key, int def) const;
    float ReadFloat(const char* section, const char* key, float def) const;
    bool  ReadBool(const char* section, const char* key, bool def) const;

    // Presence-probing float read (AR's readOpt NaN-sentinel lambda,
    // Loaders.cpp:470-475): false = key absent/empty, *out untouched.
    bool  ReadFloatOpt(const char* section, const char* key, float* out) const;

    // Exact section-name probe (AR CIniReader::SectionExists, IniReader.h:103-133
    // — used for the EXPLOSION_TYPE_i / RICOCHET_SURFACE_TYPE_i fallback,
    // Loaders.cpp:538-541, 562-565). false when the file is unreadable.
    bool  SectionExists(const char* section) const;

private:
    std::string m_path;
};
