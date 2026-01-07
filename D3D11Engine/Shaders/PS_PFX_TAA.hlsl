// TAA Pixel Shader - Improved with Velocity Buffer Support
cbuffer TAAConstants : register(b0) {
    float4x4 InvViewProj;      // Current frame's UNJITTERED inverse view-projection
    float4x4 PrevViewProj;     // Previous frame's UNJITTERED view-projection
    float2 JitterOffset;       // Current jitter in UV space
    float2 Resolution;
    float BlendFactor;
    float MotionScale;
    float2 Padding;
};

SamplerState SS_Linear : register(s0);
SamplerState SS_Point : register(s1);

Texture2D TX_Texture0 : register(t0);  // Current frame
Texture2D TX_Texture1 : register(t1);  // History buffer
Texture2D TX_Texture2 : register(t2);  // Depth buffer
Texture2D TX_Texture3 : register(t3);  // Velocity buffer (RG16F)

struct PS_INPUT
{
    float2 vTexcoord   : TEXCOORD0;
    float3 vEyeRay     : TEXCOORD1;
    float4 vPosition   : SV_POSITION;
};

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

// Soft neighborhood clamping using clip towards center
float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 prevSample) {
    float3 center = 0.5 * (aabbMax + aabbMin);
    float3 extents = 0.5 * (aabbMax - aabbMin) + 0.001;
    
    float3 offset = prevSample - center;
    float3 ts = abs(extents / (offset + 0.0001));
    float t = saturate(min(min(ts.x, ts.y), ts.z));
    
    return center + offset * t;
}

// Compute luminance for weighting
float Luminance(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

// Get the closest depth in a 3x3 neighborhood (for better motion vector sampling)
// Note: This engine uses REVERSED-Z: depth 0 = far (sky), depth 1 = near
float2 GetClosestDepthOffset(float2 texCoord, float2 pixelSize) {
    float closestDepth = 0.0;  // Start at far (0 in reversed-Z)
    float2 closestOffset = float2(0, 0);
    
    [unroll]
    for (int x = -1; x <= 1; x++) {
        [unroll]
        for (int y = -1; y <= 1; y++) {
            float2 offset = float2(x, y) * pixelSize;
            float depth = TX_Texture2.Sample(SS_Point, texCoord + offset).r;
            // In reversed-Z, larger depth = closer to camera
            if (depth > closestDepth) {
                closestDepth = depth;
                closestOffset = offset;
            }
        }
    }
    
    return closestOffset;
}

// Catmull-Rom filtering for sharper history sampling
float3 SampleHistoryCatmullRom(float2 uv) {
    float2 texSize = Resolution;
    float2 invTexSize = 1.0 / texSize;
    
    // Compute sample position
    float2 position = uv * texSize;
    float2 centerPosition = floor(position - 0.5) + 0.5;
    float2 f = position - centerPosition;
    float2 f2 = f * f;
    float2 f3 = f * f2;
    
    // Catmull-Rom weights
    float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * f3 - 0.5 * f2;
    
    // Optimized bilinear samples
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    
    float2 tc0 = (centerPosition - 1.0) * invTexSize;
    float2 tc3 = (centerPosition + 2.0) * invTexSize;
    float2 tc12 = (centerPosition + offset12) * invTexSize;
    
    // Sample using bilinear filtering
    float3 result = float3(0, 0, 0);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y);
    result += TX_Texture1.SampleLevel(SS_Linear, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    
    return max(result, 0.0);
}

float4 PSMain(PS_INPUT Input) : SV_TARGET {
    float2 texCoord = Input.vTexcoord;
    float2 pixelSize = 1.0 / Resolution;
    
    // Sample current frame at jitter-corrected position for sharper output
    float2 unjitteredUV = texCoord - JitterOffset;
    float3 currentColor = TX_Texture0.Sample(SS_Linear, unjitteredUV).rgb;
    
    // Also sample at exact texCoord for neighborhood
    float3 centerColor = TX_Texture0.Sample(SS_Linear, texCoord).rgb;
    
    // Find the closest depth in neighborhood for motion vector sampling
    float2 closestOffset = GetClosestDepthOffset(texCoord, pixelSize);
    
    // Sample velocity from velocity buffer at closest depth location
    float2 velocity = TX_Texture3.Sample(SS_Point, texCoord + closestOffset).rg;
    
    // Scale velocity if needed
    velocity *= MotionScale;
    
    // Velocity magnitude in pixels
    float velocityLengthPixels = length(velocity * Resolution);
    
    // Calculate reprojected UV using velocity
    float2 prevUV = texCoord - velocity;
    
    // Sample history with Catmull-Rom for sharper result
    float3 historyColor = SampleHistoryCatmullRom(prevUV);
    
    // Collect neighborhood samples for variance clipping
    float3 m1 = float3(0, 0, 0);  // First moment (mean)
    float3 m2 = float3(0, 0, 0);  // Second moment
    float3 neighborMin = float3(1e10, 1e10, 1e10);
    float3 neighborMax = float3(-1e10, -1e10, -1e10);
    
    // 3x3 neighborhood sampling with cross pattern weighted more
    static const float weights[9] = { 0.5, 1.0, 0.5, 1.0, 1.0, 1.0, 0.5, 1.0, 0.5 };
    float totalWeight = 0.0;
    int idx = 0;
    
    [unroll]
    for (int y = -1; y <= 1; y++) {
        [unroll]
        for (int x = -1; x <= 1; x++) {
            float2 offset = float2(x, y) * pixelSize;
            float3 neighbor = TX_Texture0.Sample(SS_Linear, texCoord + offset).rgb;
            
            float w = weights[idx++];
            m1 += neighbor * w;
            m2 += neighbor * neighbor * w;
            totalWeight += w;
            
            neighborMin = min(neighborMin, neighbor);
            neighborMax = max(neighborMax, neighbor);
        }
    }
    
    m1 /= totalWeight;
    m2 /= totalWeight;
    
    // Calculate standard deviation
    float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    
    // Tighter clipping for high velocity - reduces ghosting on moving objects
    // Use smaller gamma for faster rejection of invalid history
    float velocityFactor = saturate(velocityLengthPixels * 0.15);
    float gamma = lerp(0.75, 0.25, velocityFactor);  // Tighter clipping with motion
    
    // Variance-based clipping bounds
    float3 clipMin = m1 - gamma * sigma;
    float3 clipMax = m1 + gamma * sigma;
    
    // Also constrain to neighborhood min/max
    clipMin = max(clipMin, neighborMin);
    clipMax = min(clipMax, neighborMax);
    
    // Convert to YCoCg for perceptually better clamping
    float3 historyYCoCg = RGB_YCoCg(historyColor);
    float3 clipMinYCoCg = RGB_YCoCg(clipMin);
    float3 clipMaxYCoCg = RGB_YCoCg(clipMax);
    
    // Clip history to neighborhood bounds
    float3 clampedHistoryYCoCg = ClipAABB(clipMinYCoCg, clipMaxYCoCg, historyYCoCg);
    float3 clampedHistory = YCoCg_RGB(clampedHistoryYCoCg);
    
    // Calculate how much history was clipped (for adaptive blending)
    float clipDistance = length(historyColor - clampedHistory);
    float clipAmount = saturate(clipDistance / (Luminance(m1) + 0.001));
    
    // Reject history if reprojected position is outside screen
    bool offScreen = prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0;
    if (offScreen) {
        clampedHistory = currentColor;
        clipAmount = 1.0;
    }
    
    // Adaptive blend factor:
    // - Base: BlendFactor (typically 0.05-0.1 for stable TAA)
    // - Increase for high motion to reduce smearing
    // - Increase when history is heavily clipped
    float motionBlend = saturate(velocityLengthPixels * 0.04);  // More aggressive motion rejection
    float clipBlend = clipAmount * 0.5;
    
    float adaptiveBlend = max(BlendFactor, max(motionBlend, clipBlend));
    
    // For very high velocity, blend even more towards current frame
    if (velocityLengthPixels > 4.0) {
        adaptiveBlend = lerp(adaptiveBlend, 0.5, saturate((velocityLengthPixels - 4.0) * 0.1));
    }
    
    adaptiveBlend = clamp(adaptiveBlend, 0.04, 1.0);
    
    // Luminance-weighted blending to reduce flickering on high-contrast edges
    float lumCurrent = Luminance(currentColor);
    float lumHistory = Luminance(clampedHistory);
    float lumDiff = abs(lumCurrent - lumHistory) / max(lumCurrent, max(lumHistory, 0.2));
    
    // Increase blend towards current when there's high luminance difference
    adaptiveBlend = lerp(adaptiveBlend, max(adaptiveBlend, 0.25), saturate(lumDiff * 0.5));
    
    // Final blend
    float3 result = lerp(clampedHistory, currentColor, adaptiveBlend);
    
    return float4(result, 1.0);
}