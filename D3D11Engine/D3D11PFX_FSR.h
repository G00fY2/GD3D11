#pragma once
#include "pch.h"
#include "D3D11PFX_Effect.h"
#include "D3D11ConstantBuffer.h"

struct RenderToTextureBuffer;

// FSR constant buffer structures (from AMD SDK)
struct FSR_EASUConstants {
    uint32_t Const0[4];
    uint32_t Const1[4];
    uint32_t Const2[4];
    uint32_t Const3[4];
};

struct FSR_RCASConstants {
    uint32_t Const0[4];
};

// FSR Quality modes
enum class FSRQualityMode {
    Ultra,      // 1.3x scale (77% resolution)
    Quality,    // 1.5x scale (67% resolution)
    Balanced,   // 1.7x scale (59% resolution)
    Performance // 2.0x scale (50% resolution)
};

class D3D11PFX_FSR : public D3D11PFX_Effect {
public:
    D3D11PFX_FSR(D3D11PfxRenderer* rnd);
    ~D3D11PFX_FSR();

    /** Initialize FSR resources */
    bool Init();

    /** Called on resize */
    void OnResize(const INT2& size);

    /** Renders FSR (EASU + RCAS passes) */
    void RenderPostFX(const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& inputSRV,
                      const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& outputRTV);

    /** Sets the quality mode */
    void SetQualityMode(FSRQualityMode mode);

    /** Sets sharpness (0.0 - 2.0, default 0.2) */
    void SetSharpness(float sharpness);

    /** Gets the render resolution based on quality mode */
    INT2 GetRenderResolution() const;

    /** Draws this effect to the given buffer */
    XRESULT Render(RenderToTextureBuffer* fxbuffer) override { return XR_SUCCESS; }

private:
    // Intermediate buffer after EASU (upscaling)
    std::unique_ptr<RenderToTextureBuffer> m_EASUBuffer;
    
    // Constant buffers
    std::unique_ptr<D3D11ConstantBuffer> m_EASUConstantBuffer;
    std::unique_ptr<D3D11ConstantBuffer> m_RCASConstantBuffer;
    
    FSRQualityMode m_QualityMode;
    float m_Sharpness;
    
    INT2 m_OutputSize;
    INT2 m_InputSize;
};
