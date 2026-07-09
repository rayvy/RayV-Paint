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
namespace PathUtil {

inline std::wstring Utf8ToWide(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
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

inline std::filesystem::path FromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(Utf8ToWide(utf8));
#else
    return std::filesystem::u8path(utf8);
#endif
}

inline bool Exists(const std::string& utf8Path) {
    std::error_code ec;
    return std::filesystem::exists(FromUtf8(utf8Path), ec);
}

inline FILE* OpenRead(const std::string& utf8Path) {
#ifdef _WIN32
    return _wfopen(Utf8ToWide(utf8Path).c_str(), L"rb");
#else
    return std::fopen(utf8Path.c_str(), "rb");
#endif
}

inline FILE* OpenWrite(const std::string& utf8Path) {
#ifdef _WIN32
    return _wfopen(Utf8ToWide(utf8Path).c_str(), L"wb");
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

} // namespace PathUtil
