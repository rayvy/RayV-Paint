#include "Win32FileDialogs.h"
#include "../../core/PathUtil.h"
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

namespace {
std::wstring ConvertFilterToWString(const char* filter) {
    if (!filter) return L"";
    std::vector<char> filterBuffer;
    const char* p = filter;
    while (true) {
        if (*p == '\0' && *(p + 1) == '\0') {
            filterBuffer.push_back('\0');
            filterBuffer.push_back('\0');
            break;
        }
        filterBuffer.push_back(*p);
        p++;
    }
    int size = static_cast<int>(filterBuffer.size());
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, filterBuffer.data(), size, NULL, 0);
    std::wstring wfilter(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, filterBuffer.data(), size, &wfilter[0], size_needed);
    return wfilter;
}
} // namespace

namespace Ui {

bool ShowOpenFile(char* outPath, size_t maxLen, const char* filter) {
    if (!outPath || maxLen == 0) return false;
    OPENFILENAMEW ofn;
    wchar_t szFile[512] = { 0 };
    if (outPath[0]) {
        std::wstring wpath = PathUtil::Utf8ToWide(outPath);
        wcsncpy_s(szFile, wpath.c_str(), _TRUNCATE);
    }
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    std::wstring wfilter = ConvertFilterToWString(filter);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) != TRUE) return false;
    std::string utf8 = PathUtil::WideToUtf8(ofn.lpstrFile);
    std::strncpy(outPath, utf8.c_str(), maxLen - 1);
    outPath[maxLen - 1] = '\0';
    return true;
}

bool ShowSaveFile(char* outPath, size_t maxLen, const char* filter) {
    if (!outPath || maxLen == 0) return false;
    OPENFILENAMEW ofn;
    wchar_t szFile[512] = { 0 };
    if (outPath[0]) {
        std::wstring wpath = PathUtil::Utf8ToWide(outPath);
        wcsncpy_s(szFile, wpath.c_str(), _TRUNCATE);
    }
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    std::wstring wfilter = ConvertFilterToWString(filter);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn) != TRUE) return false;
    std::string utf8 = PathUtil::WideToUtf8(ofn.lpstrFile);
    std::strncpy(outPath, utf8.c_str(), maxLen - 1);
    outPath[maxLen - 1] = '\0';
    return true;
}

} // namespace Ui

#else

namespace Ui {
bool ShowOpenFile(char*, size_t, const char*) { return false; }
bool ShowSaveFile(char*, size_t, const char*) { return false; }
} // namespace Ui

#endif
