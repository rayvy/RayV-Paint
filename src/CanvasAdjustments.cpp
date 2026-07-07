#include "Canvas.h"
#include "core/Logger.h"
#include <algorithm>
#include <cmath>
#include <random>

// --- Helpers ---
static std::vector<float> ExportLayerF(const Layer& layer, int w, int h) {
    std::vector<float> out((size_t)w * h * 4, 0.f);
    if (layer.tileCache) layer.tileCache->ExportRGBA32F(out.data(), w, h);
    return out;
}

static void SetLayerPixelsF(Layer& layer, const std::vector<float>& pixels, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
    layer.tileCache->ImportRGBA32F(pixels.data(), w, h);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    layer.filtersDirty = true;
}

static void EnsureLayerTileCache(Layer& layer, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
}

static float SelU82F(uint8_t v) {
    return v / 255.f;
}

static float GetSelWeight(const std::vector<uint8_t>& mask, int w, int x, int y, bool hasSel) {
    if (!hasSel || mask.empty()) return 1.f;
    return SelU82F(mask[(size_t)y * w + x]);
}

static bool LayerHasPixels(const Layer& layer) {
    return layer.tileCache && !layer.tileCache->IsEmpty();
}

static inline void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max({r,g,b}), mn = std::min({r,g,b});
    v = mx; float delta = mx - mn;
    s = (mx > 1e-6f) ? delta / mx : 0.f;
    if (delta < 1e-6f) { h = 0.f; return; }
    if      (mx == r) h = (g - b) / delta + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / delta + 2.f;
    else              h = (r - g) / delta + 4.f;
    h /= 6.f;
}

static inline void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b) {
    if (s < 1e-6f) { r = g = b = v; return; }
    float hh = h * 6.f; int i = (int)hh % 6; float f = hh - (int)hh;
    float p = v*(1.f-s), q = v*(1.f-s*f), t = v*(1.f-s*(1.f-f));
    switch(i) {
        case 0: r=v;g=t;b=p; break; case 1: r=q;g=v;b=p; break;
        case 2: r=p;g=v;b=t; break; case 3: r=p;g=q;b=v; break;
        case 4: r=t;g=p;b=v; break; default:r=v;g=p;b=q; break;
    }
}

static void BoxBlurH(std::vector<float>& px, int w, int h, int r) {
    std::vector<float> tmp(px.size());
    for (int y = 0; y < h; ++y) {
        float acc[4]={0,0,0,0}; int count=0;
        for (int kx=0; kx<=r; ++kx) { int cx=std::min(kx,w-1); for(int c=0;c<4;++c) acc[c]+=px[((size_t)y*w+cx)*4+c]; ++count; }
        for (int x = 0; x < w; ++x) {
            for(int c=0;c<4;++c) tmp[((size_t)y*w+x)*4+c]=acc[c]/(float)count;
            int add=x+r+1, rem=x-r;
            if (add<w) { for(int c=0;c<4;++c) acc[c]+=px[((size_t)y*w+add)*4+c]; ++count; }
            if (rem>=0) { for(int c=0;c<4;++c) acc[c]-=px[((size_t)y*w+rem)*4+c]; --count; }
        }
    }
    px=tmp;
}

static void BoxBlurV(std::vector<float>& px, int w, int h, int r) {
    std::vector<float> tmp(px.size());
    for (int x = 0; x < w; ++x) {
        float acc[4]={0,0,0,0}; int count=0;
        for (int ky=0; ky<=r; ++ky) { int cy=std::min(ky,h-1); for(int c=0;c<4;++c) acc[c]+=px[((size_t)cy*w+x)*4+c]; ++count; }
        for (int y = 0; y < h; ++y) {
            for(int c=0;c<4;++c) tmp[((size_t)y*w+x)*4+c]=acc[c]/(float)count;
            int add=y+r+1, rem=y-r;
            if (add<h) { for(int c=0;c<4;++c) acc[c]+=px[((size_t)add*w+x)*4+c]; ++count; }
            if (rem>=0) { for(int c=0;c<4;++c) acc[c]-=px[((size_t)rem*w+x)*4+c]; --count; }
        }
    }
    px=tmp;
}

// Monotone cubic spline LUT — called from UI curves editor
std::vector<float> Canvas_BuildSplineLUT(const std::vector<std::pair<float,float>>& pts) {
    std::vector<float> lut(256);
    if (pts.size() < 2) { for(int i=0;i<256;++i) lut[i]=(float)i/255.f; return lut; }
    int n=(int)pts.size();
    std::vector<float> d(n-1),m2(n,0.f);
    for (int i=0;i<n-1;++i) d[i]=(pts[i+1].second-pts[i].second)/(pts[i+1].first-pts[i].first+1e-9f);
    m2[0]=d[0];
    for (int i=1;i<n-1;++i) m2[i]=(d[i-1]+d[i])*0.5f;
    m2[n-1]=d[n-2];
    for (int i=0;i<n-1;++i) {
        if (fabsf(d[i])<1e-9f){m2[i]=m2[i+1]=0.f;continue;}
        float a=m2[i]/d[i], b=m2[i+1]/d[i];
        float ab2=a*a+b*b;
        if (ab2>9.f){float s2=3.f/sqrtf(ab2);m2[i]=s2*a*d[i];m2[i+1]=s2*b*d[i];}
    }
    for (int xi=0;xi<256;++xi) {
        float t=(float)xi/255.f;
        t=std::clamp(t,pts.front().first,pts.back().first);
        int seg=0; for(int i=0;i<n-2;++i) if(t>=pts[i+1].first) seg=i+1;
        float hh=(pts[seg+1].first-pts[seg].first);
        float u=(t-pts[seg].first)/(hh+1e-9f);
        float u2=u*u,u3=u2*u;
        float vv=(2*u3-3*u2+1)*pts[seg].second+(u3-2*u2+u)*hh*m2[seg]
                +(-2*u3+3*u2)*pts[seg+1].second+(u3-u2)*hh*m2[seg+1];
        lut[xi]=std::clamp(vv,0.f,1.f);
    }
    return lut;
}

// ============================================================
// Destructive Operations
// ============================================================

void Canvas::InvertAlpha() {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<0.5f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        pixels[idx+3]=1.f-pixels[idx+3];
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    layer.filtersDirty=true;
    Logger::Get().Info("InvertAlpha");
}

void Canvas::ApplyBlur(float radius) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    int r=std::max(1,(int)radius);
    std::vector<float> blurred=pixels;
    for (int pass=0;pass<3;++pass){BoxBlurH(blurred,m_Width,m_Height,r);BoxBlurV(blurred,m_Width,m_Height,r);}
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        for(int c=0;c<4;++c) pixels[idx+c]=pixels[idx+c]*(1.f-sel)+blurred[idx+c]*sel;
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    layer.filtersDirty=true;
    Logger::Get().Info("ApplyBlur r="+std::to_string(r));
}

void Canvas::ApplyHSV(float dH, float dS, float dV) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        float rr=pixels[idx],gg=pixels[idx+1],bb=pixels[idx+2];
        float h,s,v; RGBtoHSV(rr,gg,bb,h,s,v);
        h=fmodf(h+dH+1.f,1.f); s=std::clamp(s+dS,0.f,1.f); v=std::clamp(v+dV,0.f,1.f);
        float nr,ng,nb; HSVtoRGB(h,s,v,nr,ng,nb);
        pixels[idx]  =pixels[idx]  *(1.f-sel)+nr*sel;
        pixels[idx+1]=pixels[idx+1]*(1.f-sel)+ng*sel;
        pixels[idx+2]=pixels[idx+2]*(1.f-sel)+nb*sel;
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    layer.filtersDirty=true;
    Logger::Get().Info("ApplyHSV");
}

void Canvas::ApplyCurves(const std::vector<float>& lut256) {
    if ((int)lut256.size()<256||m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    auto sample=[&](float v)->float{
        float fi=v*255.f; int i=std::clamp((int)fi,0,254); float t=fi-i;
        return lut256[i]*(1.f-t)+lut256[i+1]*t;
    };
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        for(int c=0;c<3;++c) pixels[idx+c]=pixels[idx+c]*(1.f-sel)+sample(pixels[idx+c])*sel;
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    layer.filtersDirty=true;
    Logger::Get().Info("ApplyCurves");
}

void Canvas::ApplyNoise(float strength, bool colorNoise) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);
    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1.f,1.f);
    for (int y=0;y<m_Height;++y) for (int x=0;x<m_Width;++x) {
        float sel = GetSelWeight(m_SelectionMask, m_Width, x, y, m_HasSelection);
        if (sel<1e-4f) continue;
        size_t idx=((size_t)y*m_Width+x)*4;
        if (colorNoise) { for(int c=0;c<3;++c) pixels[idx+c]=std::clamp(pixels[idx+c]+dist(rng)*strength*sel,0.f,1.f); }
        else { float n=dist(rng)*strength*sel; for(int c=0;c<3;++c) pixels[idx+c]=std::clamp(pixels[idx+c]+n,0.f,1.f); }
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    layer.filtersDirty=true;
    Logger::Get().Info("ApplyNoise");
}

// ============================================================
// Non-destructive Filters
// ============================================================

void Canvas::RebuildFilteredPixels(Layer& layer) {
    if (!layer.filtersDirty) return;
    if (layer.filters.empty() || !LayerHasPixels(layer)) {
        layer.filteredCache.reset();
        layer.filtersDirty=false;
        return;
    }
    std::vector<float> tmp = ExportLayerF(layer, m_Width, m_Height);
    int w=m_Width,h=m_Height;
    for (auto& f : layer.filters) {
        if (!f.enabled) continue;
        switch (f.type) {
        case FilterType::Blur: { int rr=std::max(1,(int)f.p[0]); for(int p=0;p<3;++p){BoxBlurH(tmp,w,h,rr);BoxBlurV(tmp,w,h,rr);} } break;
        case FilterType::HSV: {
            for(int i=0;i<w*h;++i){
                size_t idx=(size_t)i*4; float hr,hs,hv;
                RGBtoHSV(tmp[idx],tmp[idx+1],tmp[idx+2],hr,hs,hv);
                hr=fmodf(hr+f.p[0]+1.f,1.f); hs=std::clamp(hs+f.p[1],0.f,1.f); hv=std::clamp(hv+f.p[2],0.f,1.f);
                float r2,g2,b2; HSVtoRGB(hr,hs,hv,r2,g2,b2); tmp[idx]=r2;tmp[idx+1]=g2;tmp[idx+2]=b2;
            }
        } break;
        case FilterType::Curves: {
            if ((int)f.lut.size()==256) {
                auto sam=[&](float v)->float{ float fi=v*255.f; int ii=std::clamp((int)fi,0,254); float t=fi-ii; return f.lut[ii]*(1.f-t)+f.lut[ii+1]*t; };
                for(int i=0;i<w*h;++i){ size_t idx=(size_t)i*4; for(int c=0;c<3;++c) tmp[idx+c]=sam(tmp[idx+c]); }
            }
        } break;
        case FilterType::AlphaInvert: for(int i=0;i<w*h;++i) tmp[(size_t)i*4+3]=1.f-tmp[(size_t)i*4+3]; break;
        case FilterType::Noise: {
            std::mt19937 rng2(1337); std::uniform_real_distribution<float> dist2(-1.f,1.f);
            bool col=(f.p[1]>0.5f);
            for(int i=0;i<w*h;++i){ size_t idx=(size_t)i*4;
                if(col){for(int c=0;c<3;++c) tmp[idx+c]=std::clamp(tmp[idx+c]+dist2(rng2)*f.p[0],0.f,1.f);}
                else { float n=dist2(rng2)*f.p[0]; for(int c=0;c<3;++c) tmp[idx+c]=std::clamp(tmp[idx+c]+n,0.f,1.f); }
            }
        } break;
        }
    }
    if (!layer.filteredCache) {
        layer.filteredCache = std::make_unique<TileCache>();
    }
    layer.filteredCache->Init(m_Width, m_Height, m_CanvasFormat);
    layer.filteredCache->ImportRGBA32F(tmp.data(), m_Width, m_Height);
    layer.filteredCache->MarkAllDirty();
    layer.filtersDirty=false;
}
