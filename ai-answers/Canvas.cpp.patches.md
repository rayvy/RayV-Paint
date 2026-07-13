// ============================================================
//  Canvas.cpp — PATCH DIFF
//  Apply these changes to migrate Canvas.cpp to GPU Driven Render
//  with the new GpuFxDispatch and refactored LayerStyles.
//
//  Summary of changes:
//  1. REMOVE duplicate RGBtoHSV/HSVtoRGB (lines ~6461-6480) — use layer_fx::RGBtoHSV/HSVtoRGB
//  2. REMOVE duplicate BoxBlurH/V (lines ~6482-6509) — use layer_fx::BoxBlurH/V
//  3. ADD m_GpuFx member + Init/Shutdown
//  4. REPLACE BufferApplyHSV/Blur/Curves/Noise with GPU dispatch
//  5. OPTIMIZE RebuildChannelPreviews with GPU compositing
//  6. OPTIMIZE ApplyLayerMask with direct tile access instead of GetPixelF/SetPixelF
//  7. OPTIMIZE RebuildLayerPresentation to pass GpuFxContext
// ============================================================

// ============================================================
//  PATCH 1: Add GpuFxContext member to Canvas (Canvas.h)
// ============================================================
// In Canvas.h, add after #include "GpuFxDispatch.h":
//
//   #include "GpuFxDispatch.h"
//
// In Canvas class, add member:
//   gpu_fx::GpuFxContext m_GpuFx;  // GPU filter/style dispatch context
//   bool m_GpuFxReady = false;
//
// In Canvas constructor, call:
//   m_GpuFxReady = gpu_fx::Init(m_GpuFx, device, L"");
//
// In Canvas destructor, call:
//   if (m_GpuFxReady) gpu_fx::Shutdown(m_GpuFx);

// ============================================================
//  PATCH 2: REMOVE duplicate static functions (~line 6461-6509)
// ============================================================
// DELETE these entirely from Canvas.cpp:
//
//   static inline void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v) { ... }
//   static inline void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b) { ... }
//   static void BoxBlurH(std::vector<float>& px, int w, int h, int r) { ... }
//   static void BoxBlurV(std::vector<float>& px, int w, int h, int r) { ... }
//
// REPLACE all callers:
//   BufferApplyHSV:       use layer_fx::RGBtoHSV / layer_fx::HSVtoRGB
//   BufferApplyBlur:      use layer_fx::BoxBlurH / layer_fx::BoxBlurV (with channels=4)

// ============================================================
//  PATCH 3: BufferApplyHSV — GPU dispatch
// ============================================================
// REPLACE lines ~6698-6717:
//
// BEFORE:
//   static void BufferApplyHSV(std::vector<float>& pixels, int w, int h, float dH, float dS, float dV,
//                              const std::vector<uint8_t>& selMask, bool hasSel) {
//       for (int y = 0; y < h; ++y)
//           for (int x = 0; x < w; ++x) {
//               float sel = GetSelWeight(selMask, w, x, y, hasSel);
//               if (sel < 1e-4f) continue;
//               size_t idx = ((size_t)y * w + x) * 4;
//               float rr = pixels[idx], gg = pixels[idx + 1], bb = pixels[idx + 2];
//               float hv, s, v;
//               RGBtoHSV(rr, gg, bb, hv, s, v);
//               ...
//           }
//   }
//
// AFTER:
static void BufferApplyHSV(std::vector<float>& pixels, int w, int h, float dH, float dS, float dV,
                           const std::vector<uint8_t>& selMask, bool hasSel,
                           gpu_fx::GpuFxContext* gpuCtx) {
    // GPU path: single dispatch, no per-pixel loop
    if (gpuCtx && gpuCtx->device && gpuCtx->psHSV) {
        auto src = gpu_fx::UploadBuffer(*gpuCtx, pixels.data(), w, h);
        if (src.Valid()) {
            auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);

            // Upload selection mask if present
            gpu_fx::GpuFxTarget maskGPU;
            ID3D11ShaderResourceView* maskSRV = nullptr;
            if (hasSel && !selMask.empty() && selMask.size() == (size_t)w * h) {
                maskGPU = gpu_fx::UploadMask(*gpuCtx, selMask.data(), w, h);
                maskSRV = maskGPU.srv;
            }

            gpu_fx::ApplyHSV(*gpuCtx, src.srv, dst, w, h,
                             dH, dS, dV, maskSRV, 1.0f);
            if (dst.Valid())
                gpu_fx::ReadBack(*gpuCtx, dst, pixels.data(), w, h);
            src.Release(); dst.Release(); maskGPU.Release();
            return;
        }
    }

    // CPU fallback — uses shared layer_fx functions (no duplicate)
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            float rr = pixels[idx], gg = pixels[idx + 1], bb = pixels[idx + 2];
            float hv, s, v;
            layer_fx::RGBtoHSV(rr, gg, bb, hv, s, v);  // <-- shared, not duplicate
            hv = fmodf(hv + dH + 1.f, 1.f);
            s = std::clamp(s + dS, 0.f, 1.f);
            v = std::clamp(v + dV, 0.f, 1.f);
            float nr, ng, nb;
            layer_fx::HSVtoRGB(hv, s, v, nr, ng, nb);  // <-- shared, not duplicate
            pixels[idx]     = pixels[idx]     * (1.f - sel) + nr * sel;
            pixels[idx + 1] = pixels[idx + 1] * (1.f - sel) + ng * sel;
            pixels[idx + 2] = pixels[idx + 2] * (1.f - sel) + nb * sel;
        }
}

// ============================================================
//  PATCH 4: BufferApplyBlur — GPU dispatch
// ============================================================
// REPLACE lines ~6680-6696:
//
// BEFORE:
//   static void BufferApplyBlur(std::vector<float>& pixels, int w, int h, float radius,
//                               const std::vector<uint8_t>& selMask, bool hasSel) {
//       int r = std::max(1, (int)radius);
//       std::vector<float> blurred = pixels;
//       for (int pass = 0; pass < 3; ++pass) {
//           BoxBlurH(blurred, w, h, r);
//           BoxBlurV(blurred, w, h, r);
//       }
//       ...
//   }
//
// AFTER:
static void BufferApplyBlur(std::vector<float>& pixels, int w, int h, float radius,
                            const std::vector<uint8_t>& selMask, bool hasSel,
                            gpu_fx::GpuFxContext* gpuCtx) {
    int r = std::max(1, (int)radius);

    if (gpuCtx && gpuCtx->device && gpuCtx->psBoxBlurH) {
        auto src = gpu_fx::UploadBuffer(*gpuCtx, pixels.data(), w, h);
        if (src.Valid()) {
            auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);
            gpu_fx::ApplyBoxBlur(*gpuCtx, src.srv, dst, w, h, r, 3, 4);
            if (dst.Valid()) {
                std::vector<float> blurred((size_t)w * h * 4);
                gpu_fx::ReadBack(*gpuCtx, dst, blurred.data(), w, h);

                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        float sel = GetSelWeight(selMask, w, x, y, hasSel);
                        if (sel < 1e-4f) continue;
                        size_t idx = ((size_t)y * w + x) * 4;
                        for (int c = 0; c < 4; ++c)
                            pixels[idx + c] = pixels[idx + c] * (1.f - sel) + blurred[idx + c] * sel;
                    }
                dst.Release();
            }
            src.Release();
            return;
        }
    }

    // CPU fallback — uses shared layer_fx::BoxBlur (no duplicate)
    std::vector<float> blurred = pixels;
    layer_fx::BoxBlur(blurred, w, h, r, 4, 3);  // <-- shared, channels=4
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            for (int c = 0; c < 4; ++c)
                pixels[idx + c] = pixels[idx + c] * (1.f - sel) + blurred[idx + c] * sel;
        }
}

// ============================================================
//  PATCH 5: BufferApplyCurves — GPU dispatch
// ============================================================
// REPLACE lines ~6719-6738:
static void BufferApplyCurves(std::vector<float>& pixels, int w, int h,
                              const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha,
                              const std::vector<uint8_t>& selMask, bool hasSel,
                              gpu_fx::GpuFxContext* gpuCtx) {
    if (gpuCtx && gpuCtx->device && gpuCtx->psCurves && lutRGB.size() == 256) {
        auto src = gpu_fx::UploadBuffer(*gpuCtx, pixels.data(), w, h);
        if (src.Valid()) {
            auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);
            gpu_fx::GpuFxTarget maskGPU;
            ID3D11ShaderResourceView* maskSRV = nullptr;
            if (hasSel && !selMask.empty() && selMask.size() == (size_t)w * h) {
                maskGPU = gpu_fx::UploadMask(*gpuCtx, selMask.data(), w, h);
                maskSRV = maskGPU.srv;
            }
            // RGB channels (bits 0,1,2 = 0x7)
            gpu_fx::ApplyCurves(*gpuCtx, src.srv, dst, w, h,
                                lutRGB.data(), 0x7, maskSRV, 1.0f);
            if (dst.Valid())
                gpu_fx::ReadBack(*gpuCtx, dst, pixels.data(), w, h);

            // Alpha LUT separate pass
            if (lutAlpha.size() == 256) {
                auto src2 = gpu_fx::UploadBuffer(*gpuCtx, pixels.data(), w, h);
                auto dst2 = gpu_fx::CreateTarget(*gpuCtx, w, h);
                gpu_fx::ApplyCurves(*gpuCtx, src2.srv, dst2, w, h,
                                    lutAlpha.data(), 0x8, maskSRV, 1.0f);
                if (dst2.Valid())
                    gpu_fx::ReadBack(*gpuCtx, dst2, pixels.data(), w, h);
                src2.Release(); dst2.Release();
            }

            src.Release(); dst.Release(); maskGPU.Release();
            return;
        }
    }

    // CPU fallback (original, no change needed)
    auto sample = [&](const std::vector<float>& lut, float v) -> float {
        float fi = v * 255.f;
        int i = std::clamp((int)fi, 0, 254);
        float t = fi - i;
        return lut[i] * (1.f - t) + lut[i + 1] * t;
    };
    const bool hasA = (int)lutAlpha.size() >= 256;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            for (int c = 0; c < 3; ++c)
                pixels[idx + c] = pixels[idx + c] * (1.f - sel) + sample(lutRGB, pixels[idx + c]) * sel;
            if (hasA)
                pixels[idx + 3] = pixels[idx + 3] * (1.f - sel) + sample(lutAlpha, pixels[idx + 3]) * sel;
        }
}

// ============================================================
//  PATCH 6: BufferApplyNoise — GPU dispatch
// ============================================================
static void BufferApplyNoise(std::vector<float>& pixels, int w, int h, float strength, bool colorNoise,
                             uint32_t seed,
                             const std::vector<uint8_t>& selMask, bool hasSel,
                             gpu_fx::GpuFxContext* gpuCtx) {
    if (gpuCtx && gpuCtx->device && gpuCtx->psNoise) {
        auto src = gpu_fx::UploadBuffer(*gpuCtx, pixels.data(), w, h);
        if (src.Valid()) {
            auto dst = gpu_fx::CreateTarget(*gpuCtx, w, h);
            gpu_fx::GpuFxTarget maskGPU;
            ID3D11ShaderResourceView* maskSRV = nullptr;
            if (hasSel && !selMask.empty() && selMask.size() == (size_t)w * h) {
                maskGPU = gpu_fx::UploadMask(*gpuCtx, selMask.data(), w, h);
                maskSRV = maskGPU.srv;
            }
            gpu_fx::ApplyNoise(*gpuCtx, src.srv, dst, w, h, strength, colorNoise, seed,
                               maskSRV, 1.0f);
            if (dst.Valid())
                gpu_fx::ReadBack(*gpuCtx, dst, pixels.data(), w, h);
            src.Release(); dst.Release(); maskGPU.Release();
            return;
        }
    }

    // CPU fallback (original)
    std::mt19937 rng(seed ? seed : 1u);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sel = GetSelWeight(selMask, w, x, y, hasSel);
            if (sel < 1e-4f) continue;
            size_t idx = ((size_t)y * w + x) * 4;
            if (colorNoise) {
                for (int c = 0; c < 3; ++c)
                    pixels[idx + c] = std::clamp(pixels[idx + c] + dist(rng) * strength * sel, 0.f, 1.f);
            } else {
                float n = dist(rng) * strength * sel;
                for (int c = 0; c < 3; ++c)
                    pixels[idx + c] = std::clamp(pixels[idx + c] + n, 0.f, 1.f);
            }
        }
}

// ============================================================
//  PATCH 7: UpdateAdjustPreviewHSV — pass GPU context
// ============================================================
// BEFORE (line ~6810):
//   void Canvas::UpdateAdjustPreviewHSV(float dH, float dS, float dV) {
//       if (!m_AdjustPreviewActive || m_ActiveLayerIdx != m_AdjustPreviewLayerIdx) return;
//       Layer& layer = m_Layers[m_ActiveLayerIdx];
//       auto pixels = m_AdjustPreviewBase;
//       BufferApplyHSV(pixels, m_Width, m_Height, dH, dS, dV, m_SelectionMask, m_HasSelection);
//       ...
//   }
//
// AFTER:
//   void Canvas::UpdateAdjustPreviewHSV(float dH, float dS, float dV) {
//       if (!m_AdjustPreviewActive || m_ActiveLayerIdx != m_AdjustPreviewLayerIdx) return;
//       Layer& layer = m_Layers[m_ActiveLayerIdx];
//       auto pixels = m_AdjustPreviewBase;
//       BufferApplyHSV(pixels, m_Width, m_Height, dH, dS, dV,
//                      m_SelectionMask, m_HasSelection,
//                      m_GpuFxReady ? &m_GpuFx : nullptr);  // <-- GPU
//       SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat, false);
//       m_CompositeDirty = true;
//   }

// ============================================================
//  PATCH 8: UpdateAdjustPreviewCurves — pass GPU context
// ============================================================
// Similar pattern:
//   BufferApplyCurves(pixels, m_Width, m_Height, lutRGB, lutAlpha,
//                     m_SelectionMask, m_HasSelection,
//                     m_GpuFxReady ? &m_GpuFx : nullptr);

// ============================================================
//  PATCH 9: InvertColors — GPU dispatch
// ============================================================
// BEFORE (line ~6658):
//   void Canvas::InvertColors() {
//       ...
//       auto pixels = ExportLayerF(layer, m_Width, m_Height);
//       for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) { ... }
//       SetLayerPixelsF(layer, pixels, ...);
//   }
//
// AFTER:
//   void Canvas::InvertColors() {
//       if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
//       Layer& layer=m_Layers[m_ActiveLayerIdx];
//       if (layer.isGroup) return;
//       EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
//       BackupAllActiveLayerTiles();
//       auto pixels = ExportLayerF(layer, m_Width, m_Height);
//
//       // GPU path
//       if (m_GpuFxReady && m_GpuFx.psInvertColors) {
//           auto src = gpu_fx::UploadBuffer(m_GpuFx, pixels.data(), m_Width, m_Height);
//           if (src.Valid()) {
//               gpu_fx::GpuFxTarget maskGPU;
//               ID3D11ShaderResourceView* maskSRV = nullptr;
//               if (m_HasSelection && !m_SelectionMask.empty()) {
//                   maskGPU = gpu_fx::UploadMask(m_GpuFx, m_SelectionMask.data(), m_Width, m_Height);
//                   maskSRV = maskGPU.srv;
//               }
//               auto dst = gpu_fx::CreateTarget(m_GpuFx, m_Width, m_Height);
//               gpu_fx::ApplyInvertColors(m_GpuFx, src.srv, dst, m_Width, m_Height,
//                                         maskSRV, 1.0f);
//               if (dst.Valid())
//                   gpu_fx::ReadBack(m_GpuFx, dst, pixels.data(), m_Width, m_Height);
//               src.Release(); dst.Release(); maskGPU.Release();
//           }
//       } else {
//           // CPU fallback
//           for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
//               float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
//               if (sel<0.5f) continue;
//               size_t idx=((size_t)y*m_Width+x)*4;
//               pixels[idx+0]=1.f-pixels[idx+0];
//               pixels[idx+1]=1.f-pixels[idx+1];
//               pixels[idx+2]=1.f-pixels[idx+2];
//           }
//       }
//
//       SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
//       CommitActiveLayerMutation("Invert Colors");
//   }

// ============================================================
//  PATCH 10: ApplyLayerMask — eliminate GetPixelF/SetPixelF per pixel
// ============================================================
// BEFORE (line ~1094-1101):
//   for (int y = 0; y < m_Height; ++y) {
//       for (int x = 0; x < m_Width; ++x) {
//           float rgba[4];
//           layer.tileCache->GetPixelF(x, y, rgba);      // virtual call per pixel!
//           rgba[3] *= SelU82F(layer.mask[(size_t)y * m_Width + x]);
//           layer.tileCache->SetPixelF(x, y, rgba);      // virtual call per pixel!
//       }
//   }
//
// AFTER (use tile data directly):
//   {
//       const size_t n = (size_t)m_Width * m_Height;
//       for (int ty = 0; ty < (m_Height + 255) / 256; ++ty) {
//           for (int tx = 0; tx < (m_Width + 255) / 256; ++tx) {
//               if (!layer.tileCache->HasTile(tx, ty)) continue;
//               uint8_t* tileData = layer.tileCache->GetMutableTileData(tx, ty);
//               if (!tileData) continue;
//
//               int x0 = tx * 256, y0 = ty * 256;
//               int x1 = std::min(x0 + 256, m_Width);
//               int y1 = std::min(y0 + 256, m_Height);
//
//               for (int y = y0; y < y1; ++y) {
//                   for (int x = x0; x < x1; ++x) {
//                       int lx = x - x0, ly = y - y0;
//                       size_t tileIdx = ((size_t)ly * 256 + lx) * 4;
//                       float* px = (float*)(tileData + tileIdx);
//                       px[3] *= layer.mask[(size_t)y * m_Width + x] / 255.f;
//                   }
//               }
//           }
//       }
//       layer.tileCache->MarkAllDirty();
//   }

// ============================================================
//  PATCH 11: RebuildFilteredPixels — pass GpuFxContext
// ============================================================
// BEFORE (line ~6975):
//   void Canvas::RebuildFilteredPixels(Layer& layer) {
//       ...
//       std::vector<float> tmp = ResolveLayerContentF(layer);
//       layer_fx::ApplyPixelFilters(tmp, m_Width, m_Height, layer.filters);
//       ...
//   }
//
// AFTER:
//   void Canvas::RebuildFilteredPixels(Layer& layer) {
//       if (!layer.filtersDirty) return;
//       if (layer.filters.empty() || (!LayerHasPixels(layer) && !layer.IsFill())) {
//           layer.filteredCache.reset();
//           layer.filtersDirty = false;
//           layer.presentationDirty = true;
//           return;
//       }
//       std::vector<float> tmp = ResolveLayerContentF(layer);
//       layer_fx::ApplyPixelFilters(tmp, m_Width, m_Height, layer.filters,
//                                   m_GpuFxReady ? &m_GpuFx : nullptr);  // <-- GPU
//       if (!layer.filteredCache)
//           layer.filteredCache = std::make_unique<TileCache>();
//       layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
//       layer.filteredCache->ImportRGBA32F(tmp.data(), m_Width, m_Height);
//       layer.filteredCache->MarkAllDirty();
//       layer.filtersDirty = false;
//       layer.presentationDirty = true;
//   }

// ============================================================
//  PATCH 12: RebuildLayerPresentation — pass GpuFxContext
// ============================================================
// In RebuildLayerPresentation, change the BuildPresentation call:
//
// BEFORE:
//   presSmall = layer_fx::BuildPresentation(
//       bakeContent, bakeW, bakeH, {}, scaledStyles, pp);
//
// AFTER:
//   presSmall = layer_fx::BuildPresentation(
//       bakeContent, bakeW, bakeH, {}, scaledStyles, pp,
//       m_GpuFxReady ? &m_GpuFx : nullptr);  // <-- GPU shadow/outline

// ============================================================
//  PATCH 13: FillSolidBuffer calls — pass GpuFxContext
// ============================================================
// All calls like:
//   layer_fx::FillSolidBuffer(buf, m_Width, m_Height, L.fill);
// Change to:
//   layer_fx::FillSolidBuffer(buf, m_Width, m_Height, L.fill,
//                             m_GpuFxReady ? &m_GpuFx : nullptr);

// ============================================================
//  PATCH 14: Proxy upscale — replace CPU NN with GPU
// ============================================================
// BEFORE (line ~7155-7165):
//   for (int y = 0; y < m_Height; ++y) {
//       int sy = std::min(bakeH - 1, (int)(y * scaleY));
//       for (int x = 0; x < m_Width; ++x) {
//           int sx = std::min(bakeW - 1, (int)(x * scaleX));
//           size_t di = ((size_t)y * m_Width + x) * 4;
//           size_t si = ((size_t)sy * bakeW + sx) * 4;
//           presFull[di + 0] = presSmall[si + 0];
//           ...
//       }
//   }
//
// AFTER:
//   if (m_GpuFxReady && m_GpuFx.psNearestUp) {
//       auto src = gpu_fx::UploadBuffer(m_GpuFx, presSmall.data(), bakeW, bakeH);
//       if (src.Valid()) {
//           auto dst = gpu_fx::CreateTarget(m_GpuFx, m_Width, m_Height);
//           if (dst.Valid()) {
//               // Use nearest upscale shader
//               gpu_fx::DispatchPass(m_GpuFx, m_GpuFx.psNearestUp,
//                   src.srv, dst, nullptr, nullptr);
//               gpu_fx::ReadBack(m_GpuFx, dst, presFull.data(), m_Width, m_Height);
//               dst.Release();
//           }
//           src.Release();
//       }
//   } else {
//       // CPU fallback (original)
//       for (int y = 0; y < m_Height; ++y) { ... }
//   }

// ============================================================
//  PATCH 15: ComposeLayers — use GPU for fill layer texture
// ============================================================
// In ComposeLayers, the fill layer GPU path already uses EnsureFillLayerGpu.
// But when filters/styles are present, it falls back to CPU BuildPresentation + upload.
// With the new GpuFxContext, the BuildPresentation call now internally uses GPU
// for shadow/outline generation, reducing the CPU work.
// No additional changes needed — the GpuFxContext is passed through
// RebuildFilteredPixels and RebuildLayerPresentation.

// ============================================================
//  PATCH 16: RGBtoHSV/HSVtoRGB made non-static in LayerStyles.cpp
// ============================================================
// In LayerStyles.refactored.cpp, the functions are static.
// To make them accessible from Canvas, move to header as inline or
// add to layer_fx namespace as non-static.
// Recommended: declare in LayerStyles.h:
//   namespace layer_fx {
//       void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v);
//       void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b);
//   }
// And use layer_fx::RGBtoHSV / layer_fx::HSVtoRGB in Canvas.cpp.