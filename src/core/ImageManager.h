#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <opencv2/core.hpp>

class ImageManager {
public:
    // Load PNG, JPG, TGA, BMP, PSD etc. into RGBA8 buffer.
    static bool LoadImageFromFile(const std::string& filepath, std::vector<uint8_t>& outPixels, int& outWidth, int& outHeight);

    // Save internal float RGBA buffer into PNG/JPG/TGA/BMP (converts to u8 first).
    static bool SaveImageToFile(const std::string& filepath, const std::vector<float>& pixels, int width, int height, const std::string& iccProfilePath = "");

    // Save pre-quantised RGBA8 (preferred for large docs — no float intermediate).
    static bool SaveRGBA8ToFile(const std::string& filepath, const uint8_t* rgba, int width, int height,
                                int rowStrideBytes = 0, const std::string& iccProfilePath = "");

    // Converts float RGBA pixels to an 8-bit 3-channel RGB cv::Mat
    static cv::Mat PixelsToMat8UC3(const std::vector<float>& pixels, int width, int height);

    // Converts float RGBA pixels to an 8-bit 1-channel Grayscale cv::Mat
    static cv::Mat PixelsToMat8UC1(const std::vector<float>& pixels, int width, int height);
};
