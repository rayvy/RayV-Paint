#pragma once
#include "PathTypes.h"
#include <string>

namespace vec {

// Parse SVG file or memory → Document (shapes). Best-effort (nanosvg).
// scale: multiply coordinates (1 = as in file).
bool LoadSvgFile(const std::string& path, Document& out, std::string* err = nullptr);
bool LoadSvgMemory(const char* data, size_t len, Document& out, std::string* err = nullptr);

// Write a simple SVG of the document (paths/rects/ellipses/lines).
bool SaveSvgFile(const std::string& path, const Document& doc, int width, int height,
                 std::string* err = nullptr);
std::string SaveSvgString(const Document& doc, int width, int height);

} // namespace vec
