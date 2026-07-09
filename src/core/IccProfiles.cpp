#include "IccProfiles.h"
#include <cstring>
#include <cmath>
#include <array>

// Minimal matrix RGB ICC (v2) builder — no external .icc files required.
namespace {

void WriteBE32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

void WriteBE16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

// s15Fixed16
int32_t ToS15(double v) {
    return (int32_t)std::llround(v * 65536.0);
}

void AppendBE32(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t b[4];
    WriteBE32(b, v);
    o.insert(o.end(), b, b + 4);
}

void AppendBytes(std::vector<uint8_t>& o, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    o.insert(o.end(), p, p + n);
}

void Pad4(std::vector<uint8_t>& o) {
    while (o.size() % 4) o.push_back(0);
}

// Build a compact RGB matrix profile with gamma curve.
std::vector<uint8_t> BuildMatrixProfile(
    const char* desc,
    double wx, double wy, double wz,
    double rx, double ry, double rz,
    double gx, double gy, double gz,
    double bx, double by, double bz,
    double gamma)
{
    // Tag data blobs (without 4-byte pad between tags — we pad each blob)
    auto makeDesc = [](const char* name) {
        std::vector<uint8_t> t;
        AppendBE32(t, 0x64657363); // 'desc'
        AppendBE32(t, 0);          // reserved
        uint32_t len = (uint32_t)std::strlen(name) + 1;
        AppendBE32(t, len);
        AppendBytes(t, name, len);
        Pad4(t);
        return t;
    };
    auto makeXYZ = [](double x, double y, double z) {
        std::vector<uint8_t> t;
        AppendBE32(t, 0x58595A20); // 'XYZ '
        AppendBE32(t, 0);
        AppendBE32(t, (uint32_t)ToS15(x));
        AppendBE32(t, (uint32_t)ToS15(y));
        AppendBE32(t, (uint32_t)ToS15(z));
        return t;
    };
    auto makeGamma = [&](double g) {
        std::vector<uint8_t> t;
        AppendBE32(t, 0x63757276); // 'curv'
        AppendBE32(t, 0);
        AppendBE32(t, 1); // count=1 → gamma
        AppendBE32(t, (uint32_t)ToS15(g));
        return t;
    };

    std::vector<uint8_t> descTag = makeDesc(desc);
    std::vector<uint8_t> wtpt = makeXYZ(wx, wy, wz);
    std::vector<uint8_t> rXYZ = makeXYZ(rx, ry, rz);
    std::vector<uint8_t> gXYZ = makeXYZ(gx, gy, gz);
    std::vector<uint8_t> bXYZ = makeXYZ(bx, by, bz);
    std::vector<uint8_t> trc  = makeGamma(gamma);

    // 9 tags
    struct Tag { uint32_t sig; const std::vector<uint8_t>* data; };
    Tag tags[] = {
        { 0x64657363, &descTag }, // desc
        { 0x77747074, &wtpt },    // wtpt
        { 0x7258595A, &rXYZ },    // rXYZ
        { 0x6758595A, &gXYZ },    // gXYZ
        { 0x6258595A, &bXYZ },    // bXYZ
        { 0x72545243, &trc },     // rTRC
        { 0x67545243, &trc },     // gTRC
        { 0x62545243, &trc },     // bTRC
        { 0x63707274, &descTag }, // cprt (reuse desc)
    };
    const uint32_t tagCount = 9;
    const uint32_t tagTableSize = 4 + tagCount * 12;
    const uint32_t dataStart = 128 + tagTableSize;

    std::vector<uint8_t> profile(128, 0);
    // Header fields filled after size known
    // Preferred CMM: 'none'
    std::memcpy(profile.data() + 4, "none", 4);
    WriteBE32(profile.data() + 8, 0x02400000); // version 2.4.0
    std::memcpy(profile.data() + 12, "mntr", 4); // class
    std::memcpy(profile.data() + 16, "RGB ", 4);
    std::memcpy(profile.data() + 20, "XYZ ", 4);
    // date leave 0
    std::memcpy(profile.data() + 36, "acsp", 4);
    // platform
    std::memcpy(profile.data() + 40, "MSFT", 4);
    WriteBE32(profile.data() + 64, 0); // rendering intent perceptual
    // illuminant D50 in header (required)
    WriteBE32(profile.data() + 68, (uint32_t)ToS15(0.9642));
    WriteBE32(profile.data() + 72, (uint32_t)ToS15(1.0000));
    WriteBE32(profile.data() + 76, (uint32_t)ToS15(0.8249));
    std::memcpy(profile.data() + 80, "RayV", 4); // creator

    // Tag count
    std::vector<uint8_t> body;
    AppendBE32(body, tagCount);

    uint32_t offset = dataStart;
    std::vector<uint8_t> tagData;
    for (uint32_t i = 0; i < tagCount; ++i) {
        AppendBE32(body, tags[i].sig);
        AppendBE32(body, offset);
        uint32_t sz = (uint32_t)tags[i].data->size();
        AppendBE32(body, sz);
        AppendBytes(tagData, tags[i].data->data(), tags[i].data->size());
        // pad to 4
        while (tagData.size() % 4) tagData.push_back(0);
        // next offset = dataStart + current tagData size... recompute carefully
        offset = dataStart + (uint32_t)tagData.size();
    }

    // Rebuild offsets correctly (first pass above used running offset wrong for pad).
    // Simpler: sequential append
    body.clear();
    tagData.clear();
    AppendBE32(body, tagCount);
    offset = dataStart;
    for (uint32_t i = 0; i < tagCount; ++i) {
        AppendBE32(body, tags[i].sig);
        AppendBE32(body, offset);
        uint32_t rawSz = (uint32_t)tags[i].data->size();
        AppendBE32(body, rawSz);
        size_t before = tagData.size();
        AppendBytes(tagData, tags[i].data->data(), tags[i].data->size());
        while (tagData.size() % 4) tagData.push_back(0);
        offset = dataStart + (uint32_t)tagData.size();
        (void)before;
    }

    profile.insert(profile.end(), body.begin(), body.end());
    profile.insert(profile.end(), tagData.begin(), tagData.end());

    WriteBE32(profile.data(), (uint32_t)profile.size());

    // Profile ID leave zeros (optional MD5)
    return profile;
}

const std::vector<uint8_t>& Empty() {
    static std::vector<uint8_t> e;
    return e;
}

const std::vector<uint8_t>& Srgb() {
    // sRGB primaries + D65 + gamma 2.2 (approx; good enough for export tagging)
    static std::vector<uint8_t> p = BuildMatrixProfile(
        "sRGB",
        0.9505, 1.0000, 1.0891,
        0.4124, 0.2126, 0.0193,
        0.3576, 0.7152, 0.1192,
        0.1805, 0.0722, 0.9505,
        2.2);
    return p;
}

const std::vector<uint8_t>& DisplayP3() {
    static std::vector<uint8_t> p = BuildMatrixProfile(
        "Display P3",
        0.9505, 1.0000, 1.0891,
        0.5151, 0.2412, -0.0011,
        0.2920, 0.6922, 0.0419,
        0.1571, 0.0666, 0.7841,
        2.2);
    return p;
}

const std::vector<uint8_t>& AdobeRGB() {
    static std::vector<uint8_t> p = BuildMatrixProfile(
        "Adobe RGB",
        0.9505, 1.0000, 1.0891,
        0.6097, 0.3111, 0.0195,
        0.2053, 0.6257, 0.0609,
        0.1492, 0.0632, 0.7448,
        2.2);
    return p;
}

} // namespace

namespace IccProfiles {

const char* Name(Preset p) {
    switch (p) {
    case Preset::None: return "None";
    case Preset::sRGB: return "sRGB";
    case Preset::DisplayP3: return "Display P3";
    case Preset::AdobeRGB: return "Adobe RGB";
    }
    return "sRGB";
}

const char* ProfileTagName(Preset p) {
    return Name(p);
}

const std::vector<uint8_t>& GetProfileBytes(Preset p) {
    switch (p) {
    case Preset::None: return Empty();
    case Preset::sRGB: return Srgb();
    case Preset::DisplayP3: return DisplayP3();
    case Preset::AdobeRGB: return AdobeRGB();
    }
    return Srgb();
}

} // namespace IccProfiles
