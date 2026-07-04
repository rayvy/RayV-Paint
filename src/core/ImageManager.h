#pragma once
#include <string>
#include <vector>

class ImageManager {
public:
    // Load PNG, JPG, TGA, BMP, PSD etc. into internal float RGBA buffer
    static bool LoadImageFromFile(const std::string& filepath, std::vector<float>& outPixels, int& outWidth, int& outHeight);

    // Save internal float RGBA buffer into PNG or JPG, with optional ICC profile
    static bool SaveImageToFile(const std::string& filepath, const std::vector<float>& pixels, int width, int height, const std::string& iccProfilePath = "");
};
