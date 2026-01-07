// TAA Pixel Shader
cbuffer TAAConstants : register(b0) {
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float2 JitterOffset;
    float2 Resolution;
    float BlendFactor;
    float MotionScale;
    float2 Padding;
};

Texture2D CurrentFrame : register(t0);
Texture2D HistoryBuffer : register(t1);
Texture2D DepthBuffer : register(t2);
Texture2D VelocityBuffer : register(t3);  // Optional

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

// Color space conversions for better blending
float3 RGB_YCoCg(float3 rgb) {
    float Y = dot(rgb, float3(0.25, 0.5, 0.25));
    float Co = dot(rgb, float3(0.5, 0.0, -0.5));
    float Cg = dot(rgb, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

float3 YCoCg_RGB(float3 ycocg) {
    float Y = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

// Neighborhood clamping to reduce ghosting
float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 prevSample) {
    float3 center = 0.5 * (aabbMax + aabbMin);
    float3 extents = 0.5 * (aabbMax - aabbMin);
    
    float3 offset = prevSample - center;
    float3 ts = abs(extents / (offset + 0.0001));
    float t = saturate(min(min(ts.x, ts.y), ts.z));
    
    return center + offset * t;
}

float4 PSMain(float2 texCoord : TEXCOORD0) : SV_TARGET {
    float2 pixelSize = 1.0 / Resolution;
    
    // Remove jitter from current frame sampling
    float2 unjitteredUV = texCoord - JitterOffset;
    
    // Sample current frame
    float3 currentColor = CurrentFrame.Sample(LinearSampler, unjitteredUV).rgb;
    
    // Get depth and calculate world position
    float depth = DepthBuffer.Sample(PointSampler, texCoord).r;
    
    // Calculate velocity (motion vectors)
    float2 velocity = float2(0, 0);
    
    // Reprojection-based velocity calculation
    float4 clipPos = float4(texCoord * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;  // Flip Y for D3D
    
    float4 worldPos = mul(InvViewProj, clipPos);
    worldPos /= worldPos.w;
    
    float4 prevClipPos = mul(PrevViewProj, worldPos);
    prevClipPos /= prevClipPos.w;
    
    float2 prevUV = prevClipPos.xy * float2(0.5, -0.5) + 0.5;
    velocity = prevUV - texCoord;
    
    // Sample history at reprojected position
    float2 historyUV = texCoord + velocity;
    float3 historyColor = HistoryBuffer.Sample(LinearSampler, historyUV).rgb;
    
    // Neighborhood color clamping (3x3)
    float3 neighborMin = currentColor;
    float3 neighborMax = currentColor;
    
    [unroll]
    for (int x = -1; x <= 1; x++) {
        [unroll]
        for (int y = -1; y <= 1; y++) {
            if (x == 0 && y == 0) continue;
            float2 offset = float2(x, y) * pixelSize;
            float3 neighbor = CurrentFrame.Sample(LinearSampler, unjitteredUV + offset).rgb;
            neighborMin = min(neighborMin, neighbor);
            neighborMax = max(neighborMax, neighbor);
        }
    }
    
    // Convert to YCoCg for better clamping
    float3 historyYCoCg = RGB_YCoCg(historyColor);
    float3 minYCoCg = RGB_YCoCg(neighborMin);
    float3 maxYCoCg = RGB_YCoCg(neighborMax);
    
    // Clip history to neighborhood
    float3 clampedHistoryYCoCg = ClipAABB(minYCoCg, maxYCoCg, historyYCoCg);
    float3 clampedHistory = YCoCg_RGB(clampedHistoryYCoCg);
    
    // Reject history if outside screen
    if (historyUV.x < 0 || historyUV.x > 1 || historyUV.y < 0 || historyUV.y > 1) {
        clampedHistory = currentColor;
    }
    
    // Blend current and history
    float3 result = lerp(clampedHistory, currentColor, BlendFactor);
    
    return float4(result, 1.0);
}