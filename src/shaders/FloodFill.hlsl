Texture2D<float4> gInputAlbedo : register(t0);
RWTexture2D<float> gOutputMask : register(u0);

cbuffer FloodFillParams : register(b0) {
    float4 seedColor;
    float tolerance;
    int2 seedPos;
    int2 canvasSize;
    int passIndex;
};

[numthreads(16, 16, 1)]
void CSInit(uint3 id : SV_DispatchThreadID) {
    int2 pos = int2(id.xy);
    if (pos.x >= canvasSize.x || pos.y >= canvasSize.y) return;

    if (pos.x == seedPos.x && pos.y == seedPos.y) {
        gOutputMask[pos] = 1.0f;
    } else {
        gOutputMask[pos] = 0.0f;
    }
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 pos = int2(id.xy);
    if (pos.x >= canvasSize.x || pos.y >= canvasSize.y) return;

    float currentMask = gOutputMask[pos];
    if (currentMask > 0.0f) return;

    int2 neighbors[4] = {
        pos + int2(1, 0),
        pos + int2(-1, 0),
        pos + int2(0, 1),
        pos + int2(0, -1)
    };

    bool hasFilledNeighbor = false;
    for (int i = 0; i < 4; ++i) {
        int2 n = neighbors[i];
        if (n.x >= 0 && n.x < canvasSize.x && n.y >= 0 && n.y < canvasSize.y) {
            if (gOutputMask[n] > 0.0f) {
                hasFilledNeighbor = true;
                break;
            }
        }
    }

    if (hasFilledNeighbor) {
        float4 color = gInputAlbedo[pos];
        float diff = sqrt(
            pow(color.r - seedColor.r, 2) +
            pow(color.g - seedColor.g, 2) +
            pow(color.b - seedColor.b, 2)
        );
        if (diff <= tolerance * sqrt(3.0f)) {
            gOutputMask[pos] = 1.0f;
        }
    }
}

[numthreads(16, 16, 1)]
void CSGlobalThreshold(uint3 id : SV_DispatchThreadID) {
    int2 pos = int2(id.xy);
    if (pos.x >= canvasSize.x || pos.y >= canvasSize.y) return;

    float4 color = gInputAlbedo[pos];
    float diff = sqrt(
        pow(color.r - seedColor.r, 2) +
        pow(color.g - seedColor.g, 2) +
        pow(color.b - seedColor.b, 2)
    );
    if (diff <= tolerance * sqrt(3.0f)) {
        gOutputMask[pos] = 1.0f;
    } else {
        gOutputMask[pos] = 0.0f;
    }
}
