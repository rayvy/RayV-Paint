#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

// IEEE 754 binary16 helpers (shared by TileCache, PaintEngine, DDS).
namespace HalfFloat {

inline float ToFloat(uint16_t h) {
    uint32_t sign = (uint32_t(h) & 0x8000u) << 16;
    uint32_t exp  = (uint32_t(h) & 0x7C00u) >> 10;
    uint32_t mant = uint32_t(h) & 0x03FFu;

    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03FFu;
            bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

inline uint16_t FromFloat(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(float));
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exponent = ((x >> 23) & 0xFF) - 127;
    uint32_t mantissa = x & 0x007FFFFF;

    if (exponent == -127) {
        // Zero or subnormal float → half zero (sign preserved)
        return static_cast<uint16_t>(sign);
    }
    if (exponent > 15) {
        // Overflow → infinity
        return static_cast<uint16_t>(sign | 0x7C00);
    }
    if (exponent < -14) {
        // Underflow → zero
        return static_cast<uint16_t>(sign);
    }
    exponent += 15;
    mantissa >>= 13;
    return static_cast<uint16_t>(sign | (exponent << 10) | mantissa);
}

inline void StoreRGBA16F(uint8_t* dst, const float rgba[4]) {
    uint16_t* h = reinterpret_cast<uint16_t*>(dst);
    h[0] = FromFloat(rgba[0]);
    h[1] = FromFloat(rgba[1]);
    h[2] = FromFloat(rgba[2]);
    h[3] = FromFloat(rgba[3]);
}

inline void LoadRGBA16F(const uint8_t* src, float rgba[4]) {
    const uint16_t* h = reinterpret_cast<const uint16_t*>(src);
    rgba[0] = ToFloat(h[0]);
    rgba[1] = ToFloat(h[1]);
    rgba[2] = ToFloat(h[2]);
    rgba[3] = ToFloat(h[3]);
}

inline uint8_t FloatToU8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace HalfFloat
