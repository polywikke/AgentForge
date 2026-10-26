#pragma once

// ============================================================================
// UTF-8 <-> Windows 宽字符 / std::filesystem::path 转换
//
// 约定: 工程内部所有 std::string 路径/文本均为 UTF-8;
// Windows API 与 std::filesystem 需要宽字符, 统一通过本文件转换,
// 避免在中文路径下出现乱码或文件打不开的问题。
// ============================================================================

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

inline std::wstring utf8ToWide(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], needed);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

inline std::string wideToUtf8(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &out[0], needed, nullptr, nullptr);
    return out;
#else
    return std::string(w.begin(), w.end());
#endif
}

inline std::filesystem::path utf8ToPath(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

// ============================================================================
// 路径便捷处理: 展开 %ENVVAR% 与开头的 ~
// 例: %USERPROFILE%\Desktop\sort.py  ->  C:\Users\xxx\Desktop\sort.py
// ============================================================================
inline std::string expandEnvVars(const std::string& input) {
#ifdef _WIN32
    if (input.find('%') == std::string::npos) return input;

    std::wstring w = utf8ToWide(input);
    std::wstring out;

    for (size_t i = 0; i < w.size();) {
        if (w[i] == L'%') {
            size_t end = w.find(L'%', i + 1);
            if (end != std::wstring::npos && end > i + 1) {
                std::wstring name = w.substr(i + 1, end - i - 1);
                DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
                if (needed > 0) {
                    std::wstring value(needed, L'\0');
                    DWORD written = GetEnvironmentVariableW(name.c_str(), &value[0], needed);
                    if (written > 0) {
                        value.resize(written);
                        out += value;
                        i = end + 1;
                        continue;
                    }
                }
                // 变量不存在: 原样保留
                out += w.substr(i, end - i + 1);
                i = end + 1;
                continue;
            }
        }
        out += w[i++];
    }
    return wideToUtf8(out);
#else
    return input;
#endif
}

inline std::string resolveUserPath(const std::string& input) {
    std::string expanded = expandEnvVars(input);
#ifdef _WIN32
    if (!expanded.empty() && expanded[0] == '~') {
        DWORD needed = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
        if (needed > 0) {
            std::wstring profile(needed, L'\0');
            DWORD written = GetEnvironmentVariableW(L"USERPROFILE", &profile[0], needed);
            if (written > 0) {
                profile.resize(written);
                std::wstring rest = utf8ToWide(expanded.substr(1));
                if (!rest.empty() && rest[0] != L'\\' && rest[0] != L'/') {
                    rest = L"\\" + rest;
                }
                return wideToUtf8(profile + rest);
            }
        }
    }
#endif
    return expanded;
}
