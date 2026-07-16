#include "ScriptImage.h"
#include "../core/Logger.h"
#include "../core/ImageManager.h"
#include "../core/PathUtil.h"
#include "../core/MemoryStats.h"

// Implementation lives in ImageManager.cpp — headers only here.
#include <stb_image.h>
#include <stb_image_write.h>

#include <cstring>

namespace script {
namespace {

static constexpr size_t kMaxDecodePixels = (size_t)8192 * 8192;

static void Fail(std::string* err, const std::string& msg) {
    Logger::Get().ErrorTag("script.image", msg);
    if (err) *err = msg;
}

} // namespace

bool ImageDecodeMemory(const uint8_t* data, size_t size,
                       std::vector<uint8_t>& outRgba, int& outW, int& outH,
                       std::string* outError) {
    outRgba.clear();
    outW = outH = 0;
    if (!data || size == 0) {
        Fail(outError, "decode: empty buffer");
        return false;
    }
    if (size > (size_t)256 * 1024 * 1024) {
        Fail(outError, "decode: buffer too large (>256 MiB)");
        return false;
    }
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(data, (int)size, &w, &h, &ch, 4);
    if (!px) {
        Fail(outError, std::string("decode: stbi failed: ") + (stbi_failure_reason() ? stbi_failure_reason() : "?"));
        return false;
    }
    if (w < 1 || h < 1 || (size_t)w * (size_t)h > kMaxDecodePixels) {
        stbi_image_free(px);
        Fail(outError, "decode: invalid/too large dimensions");
        return false;
    }
    const size_t bytes = (size_t)w * h * 4;
    if (MemoryStats::ExceedsRamBudget(bytes, 0.35)) {
        stbi_image_free(px);
        Fail(outError, "decode: exceeds RAM budget");
        return false;
    }
    outRgba.resize(bytes);
    std::memcpy(outRgba.data(), px, bytes);
    stbi_image_free(px);
    outW = w;
    outH = h;
    return true;
}

static void StbiWriteVec(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    auto* p = static_cast<const uint8_t*>(data);
    v->insert(v->end(), p, p + size);
}

bool ImageEncodePng(const uint8_t* rgba, int w, int h, int rowStride,
                    std::vector<uint8_t>& outPng, std::string* outError) {
    outPng.clear();
    if (!rgba || w < 1 || h < 1) {
        Fail(outError, "encode_png: invalid args");
        return false;
    }
    if ((size_t)w * h > kMaxDecodePixels) {
        Fail(outError, "encode_png: too large");
        return false;
    }
    int stride = rowStride > 0 ? rowStride : w * 4;
    int ok = stbi_write_png_to_func(StbiWriteVec, &outPng, w, h, 4, rgba, stride);
    if (!ok || outPng.empty()) {
        Fail(outError, "encode_png: stbi_write failed");
        outPng.clear();
        return false;
    }
    return true;
}

bool ImageLoadFile(const std::string& path,
                   std::vector<uint8_t>& outRgba, int& outW, int& outH,
                   std::string* outError) {
    if (!ImageManager::LoadImageFromFile(path, outRgba, outW, outH)) {
        Fail(outError, "load_file failed: " + path);
        return false;
    }
    return true;
}

bool ImageSaveFile(const std::string& path, const uint8_t* rgba, int w, int h,
                   std::string* outError) {
    if (!rgba || w < 1 || h < 1) {
        Fail(outError, "save_file: invalid args");
        return false;
    }
    if (!ImageManager::SaveRGBA8ToFile(path, rgba, w, h, w * 4)) {
        Fail(outError, "save_file failed: " + path);
        return false;
    }
    return true;
}

} // namespace script
