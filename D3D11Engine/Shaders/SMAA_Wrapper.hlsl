// SMAA_Wrapper.hlsl
// ---------------------------------------------------
// Connects the header-only SMAA.hlsl to the D3D11 Pipeline
// using SMAA_CUSTOM_SL to avoid sampler redefinition conflicts.
// ---------------------------------------------------

// 1. Configuration
#define SMAA_PRESET_HIGH // Adjust: LOW, MEDIUM, HIGH, ULTRA
#define SMAA_CUSTOM_SL    // We will define the macros manually below

// ---------------------------------------------------
// 2. Resources & Constants
// ---------------------------------------------------
cbuffer cbSMAA : register(b0)
{
    float4 SMAA_RT_METRICS; // (1/w, 1/h, w, h)
};

// Texture slots
Texture2D    colorTex    : register(t0);
Texture2D    edgesTex    : register(t1);
Texture2D    blendTex    : register(t2);
Texture2D    areaTex     : register(t3);
Texture2D    searchTex   : register(t4);

// Samplers (Explicitly bound)
SamplerState LinearSampler : register(s0);
SamplerState PointSampler  : register(s1);

// ---------------------------------------------------
// 3. Custom Porting Macros (Required for SMAA_CUSTOM_SL)
//    These map SMAA functions to standard HLSL syntax.
// ---------------------------------------------------

#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)

// ---------------------------------------------------
// 4. Include Core Logic
// ---------------------------------------------------
#include "SMAA.hlsl"

// ---------------------------------------------------
// 5. Entry Points (Vertex Shaders)
// ---------------------------------------------------

struct VS_SimpleOut {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};

struct VS_EdgeOut {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
    float4 Offsets[3] : TEXCOORD1;
};

struct VS_BlendOut {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
    float2 Pix : TEXCOORD1;
    float4 Offsets[3] : TEXCOORD2;
};

struct VS_NeighborOut {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
    float4 Offset : TEXCOORD1;
};

// Helper for Fullscreen Triangle (0,1,2)
void GetQuadAttributes(uint id, out float4 pos, out float2 tex) {
    tex = float2((id << 1) & 2, id & 2);
    pos = float4(tex * float2(2, -2) + float2(-1, 1), 0, 1);
}

VS_EdgeOut EdgeDetectionVS(uint id : SV_VertexID) {
    VS_EdgeOut o;
    GetQuadAttributes(id, o.Pos, o.Tex);
    SMAAEdgeDetectionVS(o.Tex, o.Offsets);
    return o;
}

VS_BlendOut BlendingWeightCalculationVS(uint id : SV_VertexID) {
    VS_BlendOut o;
    GetQuadAttributes(id, o.Pos, o.Tex);
    SMAABlendingWeightCalculationVS(o.Tex, o.Pix, o.Offsets);
    return o;
}

VS_NeighborOut NeighborhoodBlendingVS(uint id : SV_VertexID) {
    VS_NeighborOut o;
    GetQuadAttributes(id, o.Pos, o.Tex);
    SMAANeighborhoodBlendingVS(o.Tex, o.Offset);
    return o;
}

// ---------------------------------------------------
// 6. Entry Points (Pixel Shaders)
// ---------------------------------------------------

float4 LumaEdgeDetectionPS(VS_EdgeOut input) : SV_TARGET {
    return float4(SMAALumaEdgeDetectionPS(input.Tex, input.Offsets, colorTex), 0, 0);
}

float4 BlendingWeightCalculationPS(VS_BlendOut input) : SV_TARGET {
    return SMAABlendingWeightCalculationPS(input.Tex, input.Pix, input.Offsets, edgesTex, areaTex, searchTex, float4(0,0,0,0));
}

float4 NeighborhoodBlendingPS(VS_NeighborOut input) : SV_TARGET {
    return SMAANeighborhoodBlendingPS(input.Tex, input.Offset, colorTex, blendTex);
}