#include "ImageManager.h"
#include "Logger.h"
#include "PathUtil.h"
#include <algorithm>
#include <fstream>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

static uint32_t CalculateCRC32(const uint8_t* data, size_t length) {
    static uint32_t table[256];
    static bool tableInitialized = false;
    if (!tableInitialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                if (c & 1) {
                    c = 0xEDB88320L ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[i] = c;
        }
        tableInitialized = true;
    }

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

static void WriteBigEndian32(uint8_t* dest, uint32_t val) {
    dest[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    dest[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    dest[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dest[3] = static_cast<uint8_t>(val & 0xFF);
}

struct SaveContext {
    std::vector<uint8_t> bytes;
};

static void stbi_write_func_vector(void* context, void* data, int size) {
    auto ctx = static_cast<SaveContext*>(context);
    auto bytes = static_cast<const uint8_t*>(data);
    ctx->bytes.insert(ctx->bytes.end(), bytes, bytes + size);
}

bool ImageManager::LoadImageFromFile(const std::string& filepath, std::vector<uint8_t>& outPixels, int& outWidth, int& outHeight) {
    // Normalize to UTF-8 then read via wide _wfopen (Cyrillic/CJK-safe on Windows).
    const std::string path = PathUtil::NormalizeToUtf8Path(filepath);
    std::vector<uint8_t> buffer;
    bool readOk = PathUtil::ReadFileBytes(path, buffer);
    if (!readOk)
        readOk = PathUtil::ReadFileBytes(filepath, buffer);
    if (!readOk || buffer.empty()) {
        Logger::Get().Error("Failed to open image file for loading: " + filepath
            + " (normalized=" + path + ")");
        return false;
    }

    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(
        buffer.data(), static_cast<int>(buffer.size()), &outWidth, &outHeight, &channels, 4);
    if (!data) {
        Logger::Get().Error("Failed to decode image file (STB): " + filepath);
        return false;
    }

    size_t numPixels = (size_t)outWidth * outHeight;
    outPixels.resize(numPixels * 4);
    std::memcpy(outPixels.data(), data, numPixels * 4);

    stbi_image_free(data);
    Logger::Get().Info("Image loaded successfully (" + std::to_string(outWidth) + "x" +
                       std::to_string(outHeight) + "): " + path);
    return true;
}

static bool InjectIccBytesIntoPng(std::vector<uint8_t>& pngBytes,
                                  const uint8_t* profileData, size_t profileSize,
                                  const char* profileNameIn) {
    if (!profileData || profileSize == 0) return true;

    int compressedSize = 0;
    unsigned char* compressedData = stbi_zlib_compress(
        const_cast<uint8_t*>(profileData), static_cast<int>(profileSize), &compressedSize, 8);
    if (!compressedData) {
        Logger::Get().Error("Failed to compress ICC profile data");
        return false;
    }

    if (pngBytes.size() >= 33 &&
        pngBytes[0] == 0x89 && pngBytes[1] == 0x50 && pngBytes[2] == 0x4E && pngBytes[3] == 0x47) {

        std::vector<uint8_t> chunkBytes;
        chunkBytes.push_back('i');
        chunkBytes.push_back('C');
        chunkBytes.push_back('C');
        chunkBytes.push_back('P');

        std::string profileName = profileNameIn ? profileNameIn : "Embedded";
        if (profileName.size() > 79) profileName = profileName.substr(0, 79);
        if (profileName.empty()) profileName = "Embedded";

        for (char c : profileName) chunkBytes.push_back(static_cast<uint8_t>(c));
        chunkBytes.push_back(0); // null terminator for name
        chunkBytes.push_back(0); // compression method zlib
        chunkBytes.insert(chunkBytes.end(), compressedData, compressedData + compressedSize);

        uint32_t crc = CalculateCRC32(chunkBytes.data(), chunkBytes.size());
        std::vector<uint8_t> iCCPBlock(4 + chunkBytes.size() + 4);
        uint32_t chunkDataLen = static_cast<uint32_t>(chunkBytes.size() - 4);
        WriteBigEndian32(iCCPBlock.data(), chunkDataLen);
        std::memcpy(iCCPBlock.data() + 4, chunkBytes.data(), chunkBytes.size());
        WriteBigEndian32(iCCPBlock.data() + 4 + chunkBytes.size(), crc);
        pngBytes.insert(pngBytes.begin() + 33, iCCPBlock.begin(), iCCPBlock.end());
    }
    free(compressedData);
    return true;
}

static bool InjectIccIntoPng(std::vector<uint8_t>& pngBytes, const std::string& iccProfilePath) {
    if (iccProfilePath.empty()) return true;
    std::vector<uint8_t> profileData;
    if (!PathUtil::ReadFileBytes(PathUtil::NormalizeToUtf8Path(iccProfilePath), profileData)) {
        Logger::Get().Error("Failed to open ICC profile file: " + iccProfilePath);
        return false;
    }
    if (profileData.empty()) return true;

    std::string profileName = "sRGB";
    size_t lastSlash = iccProfilePath.find_last_of("\\/");
    if (lastSlash != std::string::npos) profileName = iccProfilePath.substr(lastSlash + 1);
    else profileName = iccProfilePath;
    size_t dotPosName = profileName.find_last_of('.');
    if (dotPosName != std::string::npos) profileName = profileName.substr(0, dotPosName);

    return InjectIccBytesIntoPng(pngBytes, profileData.data(), profileData.size(), profileName.c_str());
}

bool ImageManager::SaveRGBA8ToFile(const std::string& filepath, const uint8_t* rgba, int width, int height,
                                   int rowStrideBytes, const std::string& iccProfilePath) {
    if (!rgba || width <= 0 || height <= 0) {
        Logger::Get().Error("SaveRGBA8ToFile: invalid buffer/dimensions");
        return false;
    }
    if (rowStrideBytes <= 0) rowStrideBytes = width * 4;

    std::string ext;
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    int result = 0;
    std::string targetPath = filepath;
    SaveContext saveCtx;

    if (ext == "png") {
        result = stbi_write_png_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba, rowStrideBytes);
    } else if (ext == "jpg" || ext == "jpeg") {
        result = stbi_write_jpg_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba, 90);
    } else if (ext == "tga") {
        result = stbi_write_tga_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba);
    } else if (ext == "bmp") {
        result = stbi_write_bmp_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba);
    } else {
        targetPath = filepath + ".png";
        ext = "png";
        Logger::Get().Warn("Unknown extension, exporting as PNG to: " + targetPath);
        result = stbi_write_png_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba, rowStrideBytes);
    }

    if (!result || saveCtx.bytes.empty()) {
        Logger::Get().Error("Failed to encode image file data: " + targetPath);
        return false;
    }

    if (!iccProfilePath.empty() && ext == "png") {
        InjectIccIntoPng(saveCtx.bytes, iccProfilePath);
    }

    if (!PathUtil::WriteFileBytes(PathUtil::NormalizeToUtf8Path(targetPath),
                                  saveCtx.bytes.data(), saveCtx.bytes.size())) {
        Logger::Get().Error("Failed to open image file for writing: " + targetPath);
        return false;
    }

    Logger::Get().Info("Image saved successfully: " + targetPath);
    return true;
}

bool ImageManager::SaveRGBA8ToFile(const std::string& filepath, const uint8_t* rgba, int width, int height,
                                   int rowStrideBytes, const uint8_t* iccBytes, size_t iccSize,
                                   const char* iccProfileName) {
    if (!rgba || width <= 0 || height <= 0) {
        Logger::Get().Error("SaveRGBA8ToFile: invalid buffer/dimensions");
        return false;
    }
    if (rowStrideBytes <= 0) rowStrideBytes = width * 4;

    std::string ext;
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    int result = 0;
    std::string targetPath = filepath;
    SaveContext saveCtx;

    if (ext == "png") {
        result = stbi_write_png_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba, rowStrideBytes);
    } else if (ext == "jpg" || ext == "jpeg") {
        result = stbi_write_jpg_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba, 90);
    } else if (ext == "tga") {
        result = stbi_write_tga_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba);
    } else if (ext == "bmp") {
        result = stbi_write_bmp_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba);
    } else {
        targetPath = filepath + ".png";
        ext = "png";
        result = stbi_write_png_to_func(stbi_write_func_vector, &saveCtx, width, height, 4, rgba, rowStrideBytes);
    }

    if (!result || saveCtx.bytes.empty()) {
        Logger::Get().Error("Failed to encode image file data: " + targetPath);
        return false;
    }

    if (iccBytes && iccSize > 0 && ext == "png") {
        InjectIccBytesIntoPng(saveCtx.bytes, iccBytes, iccSize, iccProfileName);
    }

    if (!PathUtil::WriteFileBytes(PathUtil::NormalizeToUtf8Path(targetPath),
                                  saveCtx.bytes.data(), saveCtx.bytes.size())) {
        Logger::Get().Error("Failed to open image file for writing: " + targetPath);
        return false;
    }
    Logger::Get().Info("Image saved successfully: " + targetPath);
    return true;
}

bool ImageManager::SaveImageToFile(const std::string& filepath, const std::vector<float>& pixels, int width, int height, const std::string& iccProfilePath) {
    size_t numPixels = (size_t)width * height;
    if (pixels.size() < numPixels * 4) {
        Logger::Get().Error("SaveImageToFile: pixel buffer too small");
        return false;
    }
    std::vector<uint8_t> rawBytes(numPixels * 4);
    for (size_t i = 0; i < numPixels * 4; ++i) {
        float val = std::clamp(pixels[i], 0.0f, 1.0f);
        rawBytes[i] = static_cast<uint8_t>(val * 255.0f + 0.5f);
    }
    return SaveRGBA8ToFile(filepath, rawBytes.data(), width, height, width * 4, iccProfilePath);
}

cv::Mat ImageManager::PixelsToMat8UC3(const std::vector<float>& pixels, int width, int height) {
    cv::Mat mat(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t srcIdx = ((size_t)y * width + x) * 4;
            float r = pixels[srcIdx + 0];
            float g = pixels[srcIdx + 1];
            float b = pixels[srcIdx + 2];
            
            mat.at<cv::Vec3b>(y, x) = cv::Vec3b(
                static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f)
            );
        }
    }
    return mat;
}

cv::Mat ImageManager::PixelsToMat8UC1(const std::vector<float>& pixels, int width, int height) {
    cv::Mat mat(height, width, CV_8UC1);
    bool isRGBA = (pixels.size() == (size_t)width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (isRGBA) {
                size_t srcIdx = ((size_t)y * width + x) * 4;
                float r = pixels[srcIdx + 0];
                float g = pixels[srcIdx + 1];
                float b = pixels[srcIdx + 2];
                float gray = 0.299f * r + 0.587f * g + 0.114f * b;
                mat.at<uint8_t>(y, x) = static_cast<uint8_t>(std::clamp(gray, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                size_t srcIdx = ((size_t)y * width + x);
                float val = pixels[srcIdx];
                mat.at<uint8_t>(y, x) = static_cast<uint8_t>(std::clamp(val, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    return mat;
}
