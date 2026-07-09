#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// UTF-8 path helpers for Windows (wide APIs) and other platforms.
// On Windows, all disk I/O should go through wide (_wfopen / filesystem::path)
// so Cyrillic/CJK/etc. paths work regardless of process ANSI code page.
namespace PathUtil {

inline std::wstring Utf8ToWide(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    // Prefer strict UTF-8
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n > 0) {
        std::wstring w((size_t)n, 0);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), w.data(), n);
        return w;
    }
    // Fallback: treat as active ANSI code page (legacy dialogs / broken drops)
    n = MultiByteToWideChar(CP_ACP, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_ACP, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

inline std::string WideToUtf8(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

// Convert any path-like narrow string to filesystem::path (UTF-8 preferred, ACP fallback).
inline std::filesystem::path FromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(Utf8ToWide(utf8));
#else
    return std::filesystem::u8path(utf8);
#endif
}

// Try open: UTF-8 → wide; if fail, ACP → wide (handles mixed sources).
inline std::wstring PathToWideForOpen(const std::string& path) {
#ifdef _WIN32
    std::wstring w = Utf8ToWide(path);
    // If file does not exist under UTF-8 interpretation, try pure ACP once more
    // (Utf8ToWide already falls back to ACP for invalid UTF-8).
    return w;
#else
    return std::wstring(path.begin(), path.end());
#endif
}

inline bool Exists(const std::string& utf8Path) {
    std::error_code ec;
    return std::filesystem::exists(FromUtf8(utf8Path), ec);
}

inline FILE* OpenRead(const std::string& utf8Path) {
#ifdef _WIN32
    std::wstring w = PathToWideForOpen(utf8Path);
    if (w.empty()) return nullptr;
    FILE* f = _wfopen(w.c_str(), L"rb");
    if (f) return f;
    // Last resort: ACP force
    int n = MultiByteToWideChar(CP_ACP, 0, utf8Path.data(), (int)utf8Path.size(), nullptr, 0);
    if (n > 0) {
        std::wstring wa((size_t)n, 0);
        MultiByteToWideChar(CP_ACP, 0, utf8Path.data(), (int)utf8Path.size(), wa.data(), n);
        return _wfopen(wa.c_str(), L"rb");
    }
    return nullptr;
#else
    return std::fopen(utf8Path.c_str(), "rb");
#endif
}

inline FILE* OpenWrite(const std::string& utf8Path) {
#ifdef _WIN32
    std::wstring w = PathToWideForOpen(utf8Path);
    if (w.empty()) return nullptr;
    return _wfopen(w.c_str(), L"wb");
#else
    return std::fopen(utf8Path.c_str(), "wb");
#endif
}

inline bool ReadFileBytes(const std::string& utf8Path, std::vector<uint8_t>& out) {
    FILE* f = OpenRead(utf8Path);
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return false; }
    std::rewind(f);
    out.resize((size_t)sz);
    size_t n = (sz > 0) ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    return n == out.size();
}

inline bool WriteFileBytes(const std::string& utf8Path, const void* data, size_t size) {
    FILE* f = OpenWrite(utf8Path);
    if (!f) return false;
    size_t n = std::fwrite(data, 1, size, f);
    std::fclose(f);
    return n == size;
}

// Normalize path from OS drop / dialog to UTF-8 for internal storage.
// On Windows: if input is valid UTF-8 keep it; else re-decode as ACP → UTF-8.
inline std::string NormalizeToUtf8Path(const std::string& maybeUtf8) {
#ifdef _WIN32
    if (maybeUtf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                maybeUtf8.data(), (int)maybeUtf8.size(), nullptr, 0);
    if (n > 0) {
        // Valid UTF-8 already
        return maybeUtf8;
    }
    n = MultiByteToWideChar(CP_ACP, 0, maybeUtf8.data(), (int)maybeUtf8.size(), nullptr, 0);
    if (n <= 0) return maybeUtf8;
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_ACP, 0, maybeUtf8.data(), (int)maybeUtf8.size(), w.data(), n);
    return WideToUtf8(w);
#else
    return maybeUtf8;
#endif
}

} // namespace PathUtil
