#pragma once
#include <string>
#include <vector>
#include <opencv2/core.hpp>

class ImageManager {
public:
    // Load PNG, JPG, TGA, BMP, PSD etc. into internal float RGBA buffer
    static bool LoadImageFromFile(const std::string& filepath, std::vector<float>& outPixels, int& outWidth, int& outHeight);

    // Save internal float RGBA buffer into PNG or JPG, with optional ICC profile
    static bool SaveImageToFile(const std::string& filepath, const std::vector<float>& pixels, int width, int height, const std::string& iccProfilePath = "");

    // Converts float RGBA pixels to an 8-bit 3-channel RGB cv::Mat
    static cv::Mat PixelsToMat8UC3(const std::vector<float>& pixels, int width, int height);

    // Converts float RGBA pixels to an 8-bit 1-channel Grayscale cv::Mat (for selection masks, etc.)
    static cv::Mat PixelsToMat8UC1(const std::vector<float>& pixels, int width, int height);
};
