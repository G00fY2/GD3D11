#pragma once

#include "pch.h"
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <DirectXMath.h>
#include "RenderToTextureBuffer.h"
#include "D3D11CascadedShadowMapBuffer.h"
#include "D3D11_Helpers.h"
#include "WorldObjects.h"
#include "D3D11PointLight.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "GSky.h"

struct RenderToDepthStencilBuffer;
struct RenderToTextureBuffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11SamplerState;

class zCVob;
class zCVobLight;
class GSky;

enum PS_DS_AtmosphericScatteringSlots {
    TX_ShadowmapArray = 3,
    TX_RainShadowmap = 4,
    TX_ReflectionCube = 5,
    TX_Distortion = 6,
    TX_SI_SP = 7,
};

const int POINTLIGHT_SHADOWMAP_SIZE = 64;

class D3D11ShadowMap {
public:
    D3D11ShadowMap();
    ~D3D11ShadowMap();

    // Initialize resources. `size` is the initial square shadowmap size.
    // This will create a set of cascades (default cascades count used internally).
    void Init( Microsoft::WRL::ComPtr<ID3D11Device1>& device, Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int size );

    // Resize world shadowmap to a new size
    void Resize( int size );

    RenderToTextureBuffer* GetDummyCubeRT() { return m_dummyCubeRT.get(); }

    // Get the cascaded shadow map
    D3D11CascadedShadowMapBuffer* GetCascadedShadowMap() { return m_cascadedShadowMap.get(); }

    // Get DSV for a specific cascade
    ID3D11DepthStencilView* GetCascadeDSV( UINT cascadeIndex ) {
        return m_cascadedShadowMap ? m_cascadedShadowMap->GetCascadeDSV( cascadeIndex ) : nullptr;
    }

    int GetSizeX() const {
        if ( m_cascadedShadowMap ) return m_cascadedShadowMap->GetSize();
        return 0;
    }

    // Bind world shadowmap SRV to a pixel shader slot (binds entire cascade array)
    void BindToPixelShader( ID3D11DeviceContext1* context, UINT slot );

    // Bind the shadowmap sampler to the given slot
    void BindSampler( ID3D11DeviceContext1* context, UINT slot );

    // Compute cascade split distances.
    // Returns a vector of size (numCascades + 1) where:
    //  splits[0] == nearPlane, splits[numCascades] == farPlane
    //  For cascade i: near = splits[i], far = splits[i+1]
    //  lambda in [0,1] interpolates between logarithmic (1.0) and uniform (0.0) splits.
    static std::vector<float> ComputeCascadeSplits( float nearPlane, float farPlane, size_t numCascades, float lambda = 0.95f );

    /** Renders the shadowmaps for the sun */
    void XM_CALLCONV RenderShadowmaps( FXMVECTOR cameraPosition, RenderToDepthStencilBuffer* target = nullptr, bool cullFront = true, bool dontCull = false, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite = nullptr, Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV = nullptr, float cascadeFar = 0.0f );

    XRESULT DrawLighting( std::vector<VobLightInfo*>& lights );

    void XM_CALLCONV RenderShadowCube( FXMVECTOR position,
        float range,
        const RenderToDepthStencilBuffer& targetCube,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV,
        bool cullFront = true,
        bool indoor = false,
        bool noNPCs = false,
        std::list<VobInfo*>* renderedVobs = nullptr, 
        std::list<SkeletalVobInfo*>* renderedMobs = nullptr,
        std::map<MeshKey,
        WorldMeshInfo*,
        cmpMeshKey>* worldMeshCache = nullptr );

private:
    Microsoft::WRL::ComPtr<ID3D11Device1> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> m_context;

    // CSM using texture array
    std::unique_ptr<D3D11CascadedShadowMapBuffer> m_cascadedShadowMap;

    std::unique_ptr<RenderToTextureBuffer> m_dummyCubeRT;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_shadowmapSampler;
};
