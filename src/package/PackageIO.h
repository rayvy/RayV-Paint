#pragma once
#include "PackageTypes.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace rvp {

// Binary package I/O (little-endian).
// Layout:
//   u32 magic = 'RVPK'
//   u32 version
//   u32 manifestBytes
//   u8  manifest[manifestBytes]   // UTF-8 JSON
//   u32 resourceCount
//   repeated:
//     u16 pathLen
//     u8  path[pathLen]           // UTF-8 relative path, '/' separators
//     u32 dataLen
//     u8  data[dataLen]

bool WritePackage(const std::string& filepath, const Package& pkg, std::string* err = nullptr);
bool ReadPackage(const std::string& filepath, Package& out, std::string* err = nullptr);
bool ReadPackageFromMemory(const uint8_t* data, size_t size, Package& out, std::string* err = nullptr);
bool WritePackageToMemory(const Package& pkg, std::vector<uint8_t>& out, std::string* err = nullptr);

// Helpers
bool ParseManifestFormat(const std::string& manifestJson, PackageFormat& outFmt);
// Build minimal manifest JSON for a format.
std::string MakeManifest(PackageFormat fmt, const std::string& id, const std::string& displayName,
                         const std::string& kind, int schemaVersion = 1);

// Convenience builders
bool BuildTexturePackage(Package& out, const std::string& id, const std::string& displayName,
                         const std::vector<uint8_t>& pngBytes,
                         const std::vector<uint8_t>& thumb32 = {},
                         const std::vector<uint8_t>& thumb128 = {},
                         int w = 0, int h = 0);
bool BuildBrushPackage(Package& out, const std::string& id, const std::string& displayName,
                       const std::string& configJson,
                       const std::vector<uint8_t>& tipRaw8 = {}, int tipSize = 0);
bool BuildConfigPackage(Package& out, ConfigKind kind, const std::string& id,
                        const std::string& displayName, const std::string& configJson,
                        const std::unordered_map<std::string, std::vector<uint8_t>>& extra = {});

// Detect package format from path extension (case-insensitive).
PackageFormat FormatFromPath(const std::string& path);

} // namespace rvp
