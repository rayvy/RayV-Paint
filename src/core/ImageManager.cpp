#include "ImageManager.h"
#include "Logger.h"
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

bool ImageManager::LoadImageFromFile(const std::string& filepath, std::vector<float>& outPixels, int& outWidth, int& outHeight) {
    int channels = 0;
    // Force 4 channels (RGBA)
    stbi_uc* data = stbi_load(filepath.c_str(), &outWidth, &outHeight, &channels, 4);
    if (!data) {
        Logger::Get().Error("Failed to load image file (STB): " + filepath);
        return false;
    }

    size_t numPixels = (size_t)outWidth * outHeight;
    outPixels.resize(numPixels * 4);

    for (size_t i = 0; i < numPixels * 4; ++i) {
        outPixels[i] = data[i] / 255.0f;
    }

    stbi_image_free(data);
    Logger::Get().Info("Image loaded successfully (" + std::to_string(outWidth) + "x" + std::to_string(outHeight) + "): " + filepath);
    return true;
}

bool ImageManager::SaveImageToFile(const std::string& filepath, const std::vector<float>& pixels, int width, int height, const std::string& iccProfilePath) {
    size_t numPixels = (size_t)width * height;
    std::vector<uint8_t> rawBytes(numPixels * 4);

    for (size_t i = 0; i < numPixels * 4; ++i) {
        float val = std::clamp(pixels[i], 0.0f, 1.0f);
        rawBytes[i] = static_cast<uint8_t>(val * 255.0f + 0.5f);
    }

    // Determine format by file extension
    std::string ext = "";
    size_t dotPos = filepath.find_last_of('.');
    if (dotPos != std::string::npos) {
        ext = filepath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    int result = 0;
    std::string targetPath = filepath;
    if (ext == "png") {
        result = stbi_write_png(filepath.c_str(), width, height, 4, rawBytes.data(), width * 4);
    } 
    else if (ext == "jpg" || ext == "jpeg") {
        result = stbi_write_jpg(filepath.c_str(), width, height, 4, rawBytes.data(), 90);
    } 
    else if (ext == "tga") {
        result = stbi_write_tga(filepath.c_str(), width, height, 4, rawBytes.data());
    } 
    else if (ext == "bmp") {
        result = stbi_write_bmp(filepath.c_str(), width, height, 4, rawBytes.data());
    } 
    else {
        // Default to PNG
        targetPath = filepath + ".png";
        ext = "png";
        Logger::Get().Warn("Unknown extension '" + ext + "', exporting as PNG to: " + targetPath);
        result = stbi_write_png(targetPath.c_str(), width, height, 4, rawBytes.data(), width * 4);
    }

    if (!result) {
        Logger::Get().Error("Failed to save image file: " + targetPath);
        return false;
    }

    // Inject ICC Profile if requested and output is PNG
    if (!iccProfilePath.empty() && ext == "png") {
        std::ifstream iccFile(iccProfilePath, std::ios::binary);
        if (iccFile.is_open()) {
            std::vector<uint8_t> profileData((std::istreambuf_iterator<char>(iccFile)),
                                             std::istreambuf_iterator<char>());
            iccFile.close();

            if (!profileData.empty()) {
                int compressedSize = 0;
                unsigned char* compressedData = stbi_zlib_compress(profileData.data(), static_cast<int>(profileData.size()), &compressedSize, 8);
                if (compressedData) {
                    std::ifstream pngFileIn(targetPath, std::ios::binary);
                    if (pngFileIn.is_open()) {
                        std::vector<uint8_t> pngBytes((std::istreambuf_iterator<char>(pngFileIn)),
                                                       std::istreambuf_iterator<char>());
                        pngFileIn.close();

                        // PNG Signature is 8 bytes, IHDR is 25 bytes.
                        if (pngBytes.size() >= 33 && 
                            pngBytes[0] == 0x89 && pngBytes[1] == 0x50 && pngBytes[2] == 0x4E && pngBytes[3] == 0x47) {
                            
                            std::vector<uint8_t> chunkBytes;
                            chunkBytes.push_back('i');
                            chunkBytes.push_back('C');
                            chunkBytes.push_back('C');
                            chunkBytes.push_back('P');
                            
                            std::string profileName = "sRGB";
                            size_t lastSlash = iccProfilePath.find_last_of("\\/");
                            if (lastSlash != std::string::npos) {
                                profileName = iccProfilePath.substr(lastSlash + 1);
                            } else {
                                profileName = iccProfilePath;
                            }
                            size_t dotPosName = profileName.find_last_of('.');
                            if (dotPosName != std::string::npos) {
                                profileName = profileName.substr(0, dotPosName);
                            }
                            if (profileName.size() > 79) {
                                profileName = profileName.substr(0, 79);
                            }
                            if (profileName.empty()) {
                                profileName = "Custom";
                            }

                            for (char c : profileName) {
                                chunkBytes.push_back(static_cast<uint8_t>(c));
                            }
                            chunkBytes.push_back(0); // Null terminator
                            chunkBytes.push_back(0); // Compression method: 0
                            
                            chunkBytes.insert(chunkBytes.end(), compressedData, compressedData + compressedSize);
                            
                            uint32_t crc = CalculateCRC32(chunkBytes.data(), chunkBytes.size());
                            
                            std::vector<uint8_t> iCCPBlock(4 + chunkBytes.size() + 4);
                            uint32_t chunkDataLen = static_cast<uint32_t>(chunkBytes.size() - 4); // exclude 'iCCP' type
                            WriteBigEndian32(iCCPBlock.data(), chunkDataLen);
                            std::memcpy(iCCPBlock.data() + 4, chunkBytes.data(), chunkBytes.size());
                            WriteBigEndian32(iCCPBlock.data() + 4 + chunkBytes.size(), crc);
                            
                            pngBytes.insert(pngBytes.begin() + 33, iCCPBlock.begin(), iCCPBlock.end());
                            
                            std::ofstream pngFileOut(targetPath, std::ios::binary);
                            if (pngFileOut.is_open()) {
                                pngFileOut.write(reinterpret_cast<const char*>(pngBytes.data()), pngBytes.size());
                                pngFileOut.close();
                                Logger::Get().Info("Embedded ICC profile '" + profileName + "' into PNG file: " + targetPath);
                            } else {
                                Logger::Get().Error("Failed to open PNG file for writing ICC profile: " + targetPath);
                            }
                        }
                    }
                    free(compressedData);
                } else {
                    Logger::Get().Error("Failed to compress ICC profile data: " + iccProfilePath);
                }
            }
        } else {
            Logger::Get().Error("Failed to open ICC profile file: " + iccProfilePath);
        }
    }

    Logger::Get().Info("Image saved successfully: " + targetPath);
    return true;
}
