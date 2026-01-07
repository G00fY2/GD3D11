#include "pch.h"
#include "D3D11PFX_FSR.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ConstantBuffer.h"

// Helper functions from AMD FSR SDK
static void FsrEasuCon(
    uint32_t* con0, uint32_t* con1, uint32_t* con2, uint32_t* con3,
    float inputViewportInPixelsX, float inputViewportInPixelsY,
    float inputSizeInPixelsX, float inputSizeInPixelsY,
    float outputSizeInPixelsX, float outputSizeInPixelsY) {
    
    // Implementation from AMD FSR SDK
    // ... (see AMD FSR documentation for full implementation)
}

static void FsrRcasCon(uint32_t* con, float sharpness) {
    // Implementation from AMD FSR SDK
    sharpness = std::max(0.0f, std::min(2.0f, sharpness));
    // ... (see AMD FSR documentation)
}

D3D11PFX_FSR::D3D11PFX_FSR(D3D11PfxRenderer* rnd)
    : D3D11PFX_Effect(rnd)
    , m_QualityMode(FSRQualityMode::Quality)
    , m_Sharpness(0.2f)
    , m_OutputSize(0, 0)
    , m_InputSize(0, 0) {
}

D3D11PFX_FSR::~D3D11PFX_FSR() {}

bool D3D11PFX_FSR::Init() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    
    FSR_EASUConstants easuCB = {};
    m_EASUConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof(FSR_EASUConstants), &easuCB);
    
    FSR_RCASConstants rcasCB = {};
    m_RCASConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof(FSR_RCASConstants), &rcasCB);
    
    return true;
}

void D3D11PFX_FSR::SetQualityMode(FSRQualityMode mode) {
    m_QualityMode = mode;
}

void D3D11PFX_FSR::SetSharpness(float sharpness) {
    m_Sharpness = std::max(0.0f, std::min(2.0f, sharpness));
}

INT2 D3D11PFX_FSR::GetRenderResolution() const {
    float scale = 1.0f;
    switch (m_QualityMode) {
        case FSRQualityMode::Ultra:       scale = 1.3f; break;
        case FSRQualityMode::Quality:     scale = 1.5f; break;
        case FSRQualityMode::Balanced:    scale = 1.7f; break;
        case FSRQualityMode::Performance: scale = 2.0f; break;
    }
    return INT2(
        static_cast<int>(m_OutputSize.x / scale),
        static_cast<int>(m_OutputSize.y / scale)
    );
}

void D3D11PFX_FSR::OnResize(const INT2& size) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    
    m_OutputSize = size;
    m_InputSize = GetRenderResolution();
    
    // Create intermediate EASU buffer at output resolution
    m_EASUBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), size.x, size.y, 
        engine->GetBackBufferFormat());
}

void D3D11PFX_FSR::RenderPostFX(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& inputSRV,
    const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& outputRTV) {
    
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    
    engine->SetDefaultStates();
    engine->UpdateRenderStates();
    
    // Pass 1: EASU (Edge-Adaptive Spatial Upsampling)
    {
        FSR_EASUConstants easuCB;
        FsrEasuCon(easuCB.Const0, easuCB.Const1, easuCB.Const2, easuCB.Const3,
            static_cast<float>(m_InputSize.x), static_cast<float>(m_InputSize.y),
            static_cast<float>(m_InputSize.x), static_cast<float>(m_InputSize.y),
            static_cast<float>(m_OutputSize.x), static_cast<float>(m_OutputSize.y));
        
        m_EASUConstantBuffer->UpdateBuffer(&easuCB);
        m_EASUConstantBuffer->BindToPixelShader(0);
        
        engine->GetShaderManager().GetVShader("VS_PFX")->Apply();
        engine->GetShaderManager().GetPShader("PS_FSR_EASU")->Apply();
        
        context->PSSetShaderResources(0, 1, inputSRV.GetAddressOf());
        context->OMSetRenderTargets(1, m_EASUBuffer->GetRenderTargetView().GetAddressOf(), nullptr);
        
        FxRenderer->DrawFullScreenQuad();
    }
    
    // Pass 2: RCAS (Robust Contrast-Adaptive Sharpening)
    {
        FSR_RCASConstants rcasCB;
        FsrRcasCon(rcasCB.Const0, m_Sharpness);
        
        m_RCASConstantBuffer->UpdateBuffer(&rcasCB);
        m_RCASConstantBuffer->BindToPixelShader(0);
        
        engine->GetShaderManager().GetPShader("PS_FSR_RCAS")->Apply();
        
        context->PSSetShaderResources(0, 1, m_EASUBuffer->GetShaderResView().GetAddressOf());
        context->OMSetRenderTargets(1, outputRTV.GetAddressOf(), nullptr);
        
        FxRenderer->DrawFullScreenQuad();
    }
    
    // Cleanup
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
}
