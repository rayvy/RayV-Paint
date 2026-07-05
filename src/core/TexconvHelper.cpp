#include "TexconvHelper.h"
#include "Logger.h"
#include <windows.h>
#include <filesystem>
#include <sstream>
#include <vector>
#include <codecvt>
#include <locale>

static std::wstring GetExecutableDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos);
}

bool TexconvHelper::CompressDDS(const std::string& srcImage, const std::string& destDds, const ExportSettings& settings) {
    // 1. Locate texconv.exe
    std::wstring exeDir = GetExecutableDir();
    std::wstring texconvPath = exeDir + L"\\..\\texconv.exe";
    if (!std::filesystem::exists(texconvPath)) {
        texconvPath = exeDir + L"\\texconv.exe";
        if (!std::filesystem::exists(texconvPath)) {
            texconvPath = L"texconv.exe"; // Fallback to PATH/working directory
        }
    }

    // 2. Build command arguments
    // Usage: texconv.exe -f <format> -m <mip-levels> -if <filter> -y -o <output-dir> <srcImage>
    std::wstring format = L"BC7_UNORM_SRGB";
    std::string fmt = settings.ddsFormatStr;
    
    if (fmt.find("BC7 (sRGB") != std::string::npos || fmt == "BC7_UNORM_SRGB") format = L"BC7_UNORM_SRGB";
    else if (fmt.find("BC7 (Linear") != std::string::npos || fmt == "BC7_UNORM") format = L"BC7_UNORM";
    else if (fmt.find("BC1 (Linear") != std::string::npos || fmt == "BC1_UNORM") format = L"BC1_UNORM";
    else if (fmt.find("BC1 (sRGB") != std::string::npos || fmt == "BC1_UNORM_SRGB") format = L"BC1_UNORM_SRGB";
    else if (fmt.find("BC2 (Linear") != std::string::npos || fmt == "BC2_UNORM") format = L"BC2_UNORM";
    else if (fmt.find("BC2 (sRGB") != std::string::npos || fmt == "BC2_UNORM_SRGB") format = L"BC2_UNORM_SRGB";
    else if (fmt.find("BC3 (Linear, DXT5)") != std::string::npos || fmt == "BC3_UNORM") format = L"BC3_UNORM";
    else if (fmt.find("BC3 (sRGB") != std::string::npos || fmt == "BC3_UNORM_SRGB") format = L"BC3_UNORM_SRGB";
    else if (fmt.find("BC3 (Linear, RXGB)") != std::string::npos || fmt == "RXGB") format = L"RXGB";
    else if (fmt.find("BC4 (Linear, Unsigned)") != std::string::npos || fmt == "BC4_UNORM") format = L"BC4_UNORM";
    else if (fmt.find("BC5 (Linear, Unsigned)") != std::string::npos || fmt == "BC5_UNORM") format = L"BC5_UNORM";
    else if (fmt.find("BC5 (Linear, Signed)") != std::string::npos || fmt == "BC5_SNORM") format = L"BC5_SNORM";
    else if (fmt.find("BC6H (Linear, Unsigned") != std::string::npos || fmt == "BC6H_UF16") format = L"BC6H_UF16";
    else if (fmt.find("BC6H (Linear, Signed") != std::string::npos || fmt == "BC6H_SF16") format = L"BC6H_SF16";
    else if (fmt.find("B8G8R8A8 (Linear") != std::string::npos || fmt == "B8G8R8A8_UNORM") format = L"B8G8R8A8_UNORM";
    else if (fmt.find("B8G8R8A8 (sRGB") != std::string::npos || fmt == "B8G8R8A8_UNORM_SRGB") format = L"B8G8R8A8_UNORM_SRGB";
    else if (fmt.find("B8G8R8X8 (Linear") != std::string::npos || fmt == "B8G8R8X8_UNORM") format = L"B8G8R8X8_UNORM";
    else if (fmt.find("B8G8R8X8 (sRGB") != std::string::npos || fmt == "B8G8R8X8_UNORM_SRGB") format = L"B8G8R8X8_UNORM_SRGB";
    else if (fmt.find("R8G8B8A8 (Linear") != std::string::npos || fmt == "R8G8B8A8_UNORM") format = L"R8G8B8A8_UNORM";
    else if (fmt.find("R8G8B8A8 (sRGB") != std::string::npos || fmt == "R8G8B8A8_UNORM_SRGB") format = L"R8G8B8A8_UNORM_SRGB";
    else if (fmt.find("B5G5R5A1") != std::string::npos || fmt == "B5G5R5A1_UNORM") format = L"B5G5R5A1_UNORM";
    else if (fmt.find("B4G4R4A4") != std::string::npos || fmt == "B4G4R4A4_UNORM") format = L"B4G4R4A4_UNORM";
    else if (fmt.find("B5G6R5") != std::string::npos || fmt == "B5G6R5_UNORM") format = L"B5G6R5_UNORM";
    else if (fmt.find("R8 (Linear, Unsigned") != std::string::npos || fmt == "R8_UNORM") format = L"R8_UNORM";
    else if (fmt.find("R8G8 (Linear, Unsigned") != std::string::npos || fmt == "R8G8_UNORM") format = L"R8G8_UNORM";
    else if (fmt.find("R8G8 (Linear, Signed") != std::string::npos || fmt == "R8G8_SNORM") format = L"R8G8_SNORM";
    else if (fmt.find("R32 (Linear, Float") != std::string::npos || fmt == "R32_FLOAT") format = L"R32_FLOAT";
    else {
        // Fallback
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        format = converter.from_bytes(fmt);
    }

    std::wstring cmd = L"\"" + texconvPath + L"\"";
    cmd += L" -f " + format;

    if (settings.generateMipMaps) {
        cmd += L" -m 0";
    } else {
        cmd += L" -m 1";
    }

    std::wstring filter = L"CUBIC";
    std::string f = settings.mipFilter;
    if (f == "Nearest Neighbor") filter = L"POINT";
    else if (f == "Bilinear") filter = L"LINEAR";
    else if (f == "Bilinear (Low Quality)") filter = L"BOX";
    else if (f == "Fant") filter = L"FANT";
    else if (f == "BOX") filter = L"BOX";
    cmd += L" -if " + filter;

    if (format.find(L"BC6H") != std::wstring::npos || format.find(L"BC7") != std::wstring::npos) {
        if (settings.compressionSpeed == "Fast") {
            cmd += L" -bc d";
        } else if (settings.compressionSpeed == "Slow") {
            cmd += L" -bc q";
        }
    }

    cmd += L" -y";
    
    std::filesystem::path destPath(destDds);
    std::wstring outDir = destPath.parent_path().wstring();
    cmd += L" -o \"" + outDir + L"\"";
    
    std::filesystem::path srcPath(srcImage);
    cmd += L" \"" + srcPath.wstring() + L"\"";

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(L'\0');

    Logger::Get().Info("Executing: texconv compression for DDS");
    
    if (!CreateProcessW(
        nullptr,
        cmdBuffer.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        Logger::Get().Error("Failed to launch texconv.exe. Error code: " + std::to_string(GetLastError()));
        return false;
    }

    WaitForSingleObject(pi.hProcess, 30000);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        Logger::Get().Error("texconv.exe returned non-zero exit code: " + std::to_string(exitCode));
        return false;
    }

    std::wstring defaultOutName = srcPath.stem().wstring() + L".dds";
    std::filesystem::path defaultOutPath = destPath.parent_path() / defaultOutName;
    
    if (std::filesystem::exists(defaultOutPath) && defaultOutPath != destPath) {
        std::error_code ec;
        std::filesystem::rename(defaultOutPath, destPath, ec);
        if (ec) {
            Logger::Get().Error("Failed to rename temp compressed file to final destination: " + ec.message());
            return false;
        }
    }

    Logger::Get().Info("DDS texture successfully compressed using texconv");
    return true;
}
