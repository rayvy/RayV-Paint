#include "TexconvRunner.h"
#include "../common/Utf8.h"

#include <windows.h>
#include <filesystem>
#include <vector>

namespace helpers {
namespace {

bool ExistsFile(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

} // namespace

std::wstring FindTexconvPath() {
    std::wstring exeDir = ExeDirW();
    const std::wstring candidates[] = {
        exeDir + L"\\texconv.exe",
        exeDir + L"\\..\\texconv.exe",
        exeDir + L"\\..\\..\\texconv.exe",
        exeDir + L"\\..\\..\\..\\texconv.exe",
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        auto p = std::filesystem::path(c);
        if (std::filesystem::exists(p, ec)) {
            auto canon = std::filesystem::weakly_canonical(p, ec);
            return ec ? p.wstring() : canon.wstring();
        }
    }
    return L"texconv.exe";
}

ConvertResult ConvertOne(const std::string& srcUtf8, const ConvertOptions& opt,
                         const std::string& outDdsUtf8) {
    ConvertResult r;
    r.srcPath = srcUtf8;

    std::filesystem::path src = Utf8ToWide(srcUtf8);
    std::error_code ec;
    if (!ExistsFile(src)) {
        r.message = "Source missing";
        return r;
    }

    std::filesystem::path dest;
    if (!outDdsUtf8.empty())
        dest = Utf8ToWide(outDdsUtf8);
    else
        dest = src.parent_path() / (src.stem().wstring() + L".dds");

    r.dstPath = WideToUtf8(dest.wstring());

    std::filesystem::path outDir = dest.parent_path();
    if (outDir.empty()) outDir = std::filesystem::current_path();
    std::filesystem::create_directories(outDir, ec);

    // texconv writes <stem>.dds into -o dir using source stem
    std::filesystem::path texOut = outDir / (src.stem().wstring() + L".dds");
    std::filesystem::remove(texOut, ec);
    if (texOut != dest)
        std::filesystem::remove(dest, ec);

    std::wstring texconv = FindTexconvPath();
    std::wstring format = Utf8ToWide(opt.format);

    std::wstring cmd = L"\"" + texconv + L"\" -f " + format;
    cmd += opt.generateMips ? L" -m 0" : L" -m 1";
    cmd += L" -if " + Utf8ToWide(opt.mipFilter.empty() ? "CUBIC" : opt.mipFilter);

    if (format.find(L"BC6H") != std::wstring::npos || format.find(L"BC7") != std::wstring::npos) {
        if (opt.compressionSpeed == "Fast")
            cmd += L" -bc d";
        else if (opt.compressionSpeed == "Slow" || opt.compressionSpeed == "Best")
            cmd += L" -bc x";
    }

    // sRGB targets: tag input as sRGB so colors are not double-encoded
    if (format.find(L"_SRGB") != std::wstring::npos)
        cmd += L" -srgbi -srgbo";

    cmd += L" -y -o \"" + outDir.wstring() + L"\"";
    cmd += L" \"" + src.wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        r.message = "Failed to launch texconv.exe (is it next to RayVHelpers?)";
        return r;
    }

    WaitForSingleObject(pi.hProcess, 600000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        r.message = "texconv exit code " + std::to_string(exitCode);
        return r;
    }

    if (!ExistsFile(texOut)) {
        r.message = "texconv produced no output";
        return r;
    }

    auto outSize = std::filesystem::file_size(texOut, ec);
    if (ec || outSize < 128) {
        r.message = "texconv output too small";
        return r;
    }

    if (texOut != dest) {
        std::filesystem::remove(dest, ec);
        std::filesystem::rename(texOut, dest, ec);
        if (ec) {
            std::filesystem::copy_file(texOut, dest,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(texOut, ec);
            if (ec) {
                r.message = "Failed to place DDS at destination";
                return r;
            }
        }
    }

    r.ok = true;
    r.message = "OK";
    return r;
}

std::vector<ConvertResult> ConvertMany(
    const std::vector<std::string>& sources,
    const ConvertOptions& opt,
    const std::function<void(int, int, const std::string&)>& onProgress) {
    std::vector<ConvertResult> out;
    out.reserve(sources.size());
    const int total = (int)sources.size();
    for (int i = 0; i < total; ++i) {
        if (onProgress) onProgress(i, total, sources[i]);
        out.push_back(ConvertOne(sources[i], opt));
    }
    if (onProgress && total > 0) onProgress(total, total, {});
    return out;
}

} // namespace helpers
