#include "TexconvHelper.h"
#include "Logger.h"
#include "PathUtil.h"
#include <windows.h>
#include <filesystem>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>

static std::wstring GetExecutableDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos);
}

static std::wstring FindTexconv() {
    std::wstring exeDir = GetExecutableDir();
    // Candidates relative to RayVPaint_Core.exe (тАж/build/Release/bin)
    std::wstring candidates[] = {
        exeDir + L"\\texconv.exe",
        exeDir + L"\\..\\texconv.exe",
        exeDir + L"\\..\\..\\texconv.exe",
        exeDir + L"\\..\\..\\..\\texconv.exe",
        exeDir + L"\\..\\..\\..\\..\\texconv.exe",
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec))
            return std::filesystem::weakly_canonical(c, ec).wstring();
    }
    return L"texconv.exe"; // PATH
}

// Map Paint.NET-style / UI format strings тЖТ texconv -f DXGI name
static std::wstring MapFormatToTexconv(const std::string& fmtIn) {
    std::string fmt = fmtIn;
    // normalize
    auto contains = [&](const char* s) { return fmt.find(s) != std::string::npos; };

    // BC compressed
    if (contains("BC1") && (contains("sRGB") || contains("SRGB"))) return L"BC1_UNORM_SRGB";
    if (contains("BC1") || fmt == "BC1_UNORM") return L"BC1_UNORM";
    if (contains("BC2") && (contains("sRGB") || contains("SRGB"))) return L"BC2_UNORM_SRGB";
    if (contains("BC2") || fmt == "BC2_UNORM") return L"BC2_UNORM";
    if (contains("RXGB")) return L"BC3_UNORM"; // approx; true RXGB rare
    if (contains("BC3") && (contains("sRGB") || contains("SRGB"))) return L"BC3_UNORM_SRGB";
    if (contains("BC3") || fmt == "BC3_UNORM") return L"BC3_UNORM";
    if (contains("ATI1") || (contains("BC4") && contains("Unsigned")) || fmt == "BC4_UNORM") return L"BC4_UNORM";
    if (contains("BC4") && contains("Signed")) return L"BC4_SNORM";
    if (contains("ATI2") || (contains("BC5") && contains("Unsigned")) || fmt == "BC5_UNORM") return L"BC5_UNORM";
    if (contains("BC5") && contains("Signed") || fmt == "BC5_SNORM") return L"BC5_SNORM";
    if (contains("BC6H") && contains("Signed")) return L"BC6H_SF16";
    if (contains("BC6H") || fmt == "BC6H_UF16") return L"BC6H_UF16";
    if (contains("BC7") && (contains("sRGB") || contains("SRGB") || fmt == "BC7_UNORM_SRGB")) return L"BC7_UNORM_SRGB";
    if (contains("BC7") || fmt == "BC7_UNORM") return L"BC7_UNORM";

    // Uncompressed
    if (contains("B8G8R8A8") && (contains("sRGB") || contains("SRGB"))) return L"B8G8R8A8_UNORM_SRGB";
    if (contains("B8G8R8A8") || contains("A8R8G8B8")) return L"B8G8R8A8_UNORM";
    if (contains("B8G8R8X8") && (contains("sRGB") || contains("SRGB"))) return L"B8G8R8X8_UNORM_SRGB";
    if (contains("B8G8R8X8") || contains("X8R8G8B8")) return L"B8G8R8X8_UNORM";
    if (contains("R8G8B8A8") && (contains("sRGB") || contains("SRGB"))) return L"R8G8B8A8_UNORM_SRGB";
    if (contains("R8G8B8A8") || contains("A8B8G8R8") || fmt == "RGBA8_UNORM") return L"R8G8B8A8_UNORM";
    if (contains("R8G8B8X8") || contains("X8B8G8R8")) return L"R8G8B8A8_UNORM"; // closest
    if (contains("B5G5R5A1") || contains("A1R5G5B5")) return L"B5G5R5A1_UNORM";
    if (contains("B4G4R4A4") || contains("A4R4G4B4")) return L"B4G4R4A4_UNORM";
    if (contains("B5G6R5") || contains("R5G6B5")) return L"B5G6R5_UNORM";
    if (contains("B8G8R8") && !contains("B8G8R8A") && !contains("B8G8R8X")) return L"R8G8B8A8_UNORM"; // expand
    if ((contains("R8 (") || fmt == "R8_UNORM" || contains("L8")) && !contains("R8G8")) return L"R8_UNORM";
    if (contains("R8G8") && contains("Signed")) return L"R8G8_SNORM";
    if (contains("R8G8") || contains("A8L8") || contains("V8U8")) return L"R8G8_UNORM";
    if (contains("R32") && contains("Float") || fmt == "R32_FLOAT") return L"R32_FLOAT";
    if (fmt == "RGBA16_FLOAT" || contains("R16G16B16A16_FLOAT")) return L"R16G16B16A16_FLOAT";
    if (fmt == "RGBA16_UNORM" || contains("R16G16B16A16_UNORM")) return L"R16G16B16A16_UNORM";
    if (fmt == "RGBA32_FLOAT" || contains("R32G32B32A32_FLOAT")) return L"R32G32B32A32_FLOAT";

    // Last resort: pass through as ASCII (may work for DXGI names)
    std::wstring w;
    w.reserve(fmt.size());
    for (char c : fmt) w.push_back((wchar_t)(unsigned char)c);
    return w.empty() ? L"BC7_UNORM_SRGB" : w;
}

static std::wstring MapMipFilter(const std::string& f) {
    if (f == "Nearest Neighbor" || f == "Point" || f == "POINT") return L"POINT";
    if (f == "Bilinear" || f == "Linear" || f == "LINEAR") return L"LINEAR";
    if (f == "Bilinear (Low Quality)" || f == "Box" || f == "BOX") return L"BOX";
    if (f == "Fant" || f == "FANT") return L"FANT";
    if (f == "Lanczos") return L"LANCZOS";
    if (f == "Bicubic (Smooth)" || f == "Cubic" || f == "Bicubic" || f == "CUBIC") return L"CUBIC";
    if (f == "Adaptive (Sharp)") return L"FANT";
    return L"CUBIC";
}

bool TexconvHelper::CompressDDS(const std::string& srcImage, const std::string& destDds, const ExportSettings& settings) {
    std::wstring texconvPath = FindTexconv();
    std::wstring format = MapFormatToTexconv(settings.ddsFormatStr);

    std::filesystem::path srcPath = PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(srcImage));
    std::filesystem::path destPath = PathUtil::FromUtf8(PathUtil::NormalizeToUtf8Path(destDds));
    std::error_code ec;
    if (!std::filesystem::exists(srcPath, ec)) {
        Logger::Get().Error("Texconv: source missing: " + srcImage);
        return false;
    }

    std::filesystem::path outDir = destPath.parent_path();
    if (outDir.empty()) outDir = std::filesystem::current_path();
    std::filesystem::create_directories(outDir, ec);

    // Unique work name so we can detect output even if dest name differs
    std::filesystem::path workSrc = srcPath;
    // texconv always writes <stem>.dds into -o directory
    std::filesystem::path texOut = outDir / (srcPath.stem().wstring() + L".dds");

    // Remove stale output
    std::filesystem::remove(texOut, ec);
    if (destPath != texOut)
        std::filesystem::remove(destPath, ec);

    std::wstring cmd = L"\"" + texconvPath + L"\"";
    cmd += L" -f " + format;

    if (settings.generateMipMaps)
        cmd += L" -m 0"; // full chain
    else
        cmd += L" -m 1";

    cmd += L" -if " + MapMipFilter(settings.mipFilter);

    // BC6H/BC7 quality
    if (format.find(L"BC6H") != std::wstring::npos || format.find(L"BC7") != std::wstring::npos) {
        if (settings.compressionSpeed == "Fast")
            cmd += L" -bc d";
        else if (settings.compressionSpeed == "Slow" || settings.compressionSpeed == "Best")
            cmd += L" -bc x"; // max quality
        // Medium = default
    }

    // Color: our composite is already display-referred sRGB bytes in R8G8B8A8_UNORM.
    // Targeting *_SRGB without -srgbi makes texconv treat input as linear and apply
    // sRGB OETF тЖТ washed/bleached colors. Tag both ends as sRGB (identity transfer).
    if (format.find(L"_SRGB") != std::wstring::npos)
        cmd += L" -srgbi -srgbo";

    cmd += L" -y";
    cmd += L" -o \"" + outDir.wstring() + L"\"";
    cmd += L" \"" + workSrc.wstring() + L"\"";

    Logger::Get().Info("Texconv: " + PathUtil::WideToUtf8(cmd));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Logger::Get().Error("Failed to launch texconv.exe (hr=" + std::to_string(GetLastError()) +
                            "). Looked for: " + PathUtil::WideToUtf8(texconvPath));
        return false;
    }

    // Long enough for large 16K BC7
    WaitForSingleObject(pi.hProcess, 600000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        Logger::Get().Error("texconv.exe exit code " + std::to_string(exitCode) +
                            " format=" + PathUtil::WideToUtf8(format));
        return false;
    }

    if (!std::filesystem::exists(texOut, ec)) {
        Logger::Get().Error("texconv produced no output at " + PathUtil::WideToUtf8(texOut.wstring()));
        return false;
    }

    auto outSize = std::filesystem::file_size(texOut, ec);
    if (ec || outSize < 128) {
        Logger::Get().Error("texconv output too small/invalid");
        return false;
    }

    if (texOut != destPath) {
        std::filesystem::remove(destPath, ec);
        std::filesystem::rename(texOut, destPath, ec);
        if (ec) {
            // copy fallback
            std::filesystem::copy_file(texOut, destPath, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(texOut, ec);
            if (ec) {
                Logger::Get().Error("Failed to move texconv output to destination: " + ec.message());
                return false;
            }
        }
    }

    auto finalSize = std::filesystem::file_size(destPath, ec);
    Logger::Get().Info("DDS compressed OK format=" + PathUtil::WideToUtf8(format) +
                       " mips=" + std::string(settings.generateMipMaps ? "yes" : "no") +
                       " bytes=" + std::to_string((unsigned long long)finalSize) +
                       " тЖТ " + destDds);
    return true;
}
