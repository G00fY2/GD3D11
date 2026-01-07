#include "pch.h"
#include "D3D11PFX_TAA.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ConstantBuffer.h"
#include "GothicAPI.h"

// Halton sequence generator for jitter
static float Halton(int index, int base) {
    float result = 0.0f;
    float f = 1.0f / base;
    int i = index;
    while (i > 0) {
        result += f * (i % base);
        i = i / base;
        f = f / base;
    }
    return result;
}

D3D11PFX_TAA::D3D11PFX_TAA(D3D11PfxRenderer* rnd) 
    : D3D11PFX_Effect(rnd)
    , m_JitterIndex(0)
    , m_Width(0)
    , m_Height(0)
    , m_FirstFrame(true) {
    
    m_CurrentJitter = XMFLOAT2(0, 0);
    m_PreviousJitter = XMFLOAT2(0, 0);
    XMStoreFloat4x4(&m_PrevViewProj, XMMatrixIdentity());
    
    // Generate Halton sequence (8 samples)
    const int JITTER_SAMPLES = 8;
    m_JitterSequence.resize(JITTER_SAMPLES);
    for (int i = 0; i < JITTER_SAMPLES; i++) {
        m_JitterSequence[i] = XMFLOAT2(
            Halton(i + 1, 2) - 0.5f,
            Halton(i + 1, 3) - 0.5f
        );
    }
}

D3D11PFX_TAA::~D3D11PFX_TAA() {}

bool D3D11PFX_TAA::Init() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    
    // Create constant buffer
    TAAConstantBuffer cb = {};
    m_TAAConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof(TAAConstantBuffer), &cb);
    
    return true;
}

void D3D11PFX_TAA::OnResize(const INT2& size) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    
    m_Width = size.x;
    m_Height = size.y;
    
    // Create history and output buffers
    DXGI_FORMAT format = engine->GetBackBufferFormat();
    m_HistoryBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), size.x, size.y, format);
    m_OutputBuffer = std::make_unique<RenderToTextureBuffer>(
        engine->GetDevice().Get(), size.x, size.y, format);
    
    // Reset state on resize
    m_FirstFrame = true;
    m_JitterIndex = 0;
}

void D3D11PFX_TAA::AdvanceJitter() {
    m_PreviousJitter = m_CurrentJitter;
    m_JitterIndex = (m_JitterIndex + 1) % m_JitterSequence.size();
    m_UnjitteredViewProj = Engine::GAPI->GetProjectionMatrix();
    
    // Scale jitter to pixel size
    m_CurrentJitter = XMFLOAT2(
        m_JitterSequence[m_JitterIndex].x / m_Width,
        m_JitterSequence[m_JitterIndex].y / m_Height
    );
}

void D3D11PFX_TAA::RenderPostFX(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& currentFrameSRV,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& depthSRV,
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& velocitySRV) {
    
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    
    engine->SetDefaultStates();
    engine->UpdateRenderStates();
    
    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf());
    
    // Update constant buffer
    TAAConstantBuffer cb;
    
    // Get current view-projection and compute inverse
    XMMATRIX view = XMLoadFloat4x4(&Engine::GAPI->GetRendererState().TransformState.TransformView);
    XMMATRIX proj = XMLoadFloat4x4(&Engine::GAPI->GetProjectionMatrix() );
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);
    
    XMStoreFloat4x4(&cb.InvViewProj, XMMatrixTranspose(invViewProj));
    cb.PrevViewProj = m_UnjitteredViewProj;
    cb.JitterOffset = m_CurrentJitter;
    cb.Resolution = XMFLOAT2(static_cast<float>(m_Width), static_cast<float>(m_Height));
    cb.BlendFactor = m_FirstFrame ? 1.0f : 0.1f;  // Blend 10% of current frame
    cb.MotionScale = 1.0f;
    
    m_TAAConstantBuffer->UpdateBuffer(&cb);
    m_TAAConstantBuffer->BindToPixelShader(0);
    
    // Store current viewproj for next frame
    XMStoreFloat4x4(&m_PrevViewProj, XMMatrixTranspose(viewProj));

    // Copy the original image to our temp-buffer
    FxRenderer->CopyTextureToRTV( engine->GetHDRBackBuffer().GetShaderResView(),
    FxRenderer->GetTempBuffer().GetRenderTargetView(), engine->GetResolution() );

    // Bind shaders
    engine->GetShaderManager().GetVShader("VS_PFX")->Apply();
    auto taaPS = engine->GetShaderManager().GetPShader("PS_PFX_TAA");
    taaPS->Apply();

    // Bind textures
    // Slot 0: Current frame
    // Slot 1: History buffer
    // Slot 2: Depth buffer
    // Slot 3: Velocity buffer (optional)
    ID3D11ShaderResourceView* srvs[4] = {
        FxRenderer->GetTempBuffer().GetShaderResView().Get(),
        m_FirstFrame ? FxRenderer->GetTempBuffer().GetShaderResView().Get() : m_HistoryBuffer->GetShaderResView().Get(),
        depthSRV.Get(),
        velocitySRV ? velocitySRV.Get() : nullptr
    };
    context->PSSetShaderResources(0, 4, srvs);
    
    // Render to output buffer
    context->OMSetRenderTargets(1, m_OutputBuffer->GetRenderTargetView().GetAddressOf(), nullptr);
    FxRenderer->DrawFullScreenQuad();
    
    // Copy output to history for next frame
    context->CopyResource(m_HistoryBuffer->GetTexture().Get(), 
                          m_OutputBuffer->GetTexture().Get());

    if ( Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor > 0.0f ) {
        auto sharpenPS = engine->GetShaderManager().GetPShader( "PS_PFX_Sharpen" );
        sharpenPS->Apply();

        GammaCorrectConstantBuffer gcb;
        gcb.G_Gamma = Engine::GAPI->GetGammaValue();
        gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();
        gcb.G_TextureSize = engine->GetResolution();
        gcb.G_SharpenStrength = Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor;

        sharpenPS->GetConstantBuffer()[0]->UpdateBuffer( &gcb );
        sharpenPS->GetConstantBuffer()[0]->BindToPixelShader( 0 );

        FxRenderer->CopyTextureToRTV( m_OutputBuffer->GetShaderResView(), oldRTV, INT2( 0, 0 ), true );
    } else {
        FxRenderer->CopyTextureToRTV( m_OutputBuffer->GetShaderResView(), oldRTV );
    }
    
    //// Copy to original render target
    //context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), nullptr);
    //FxRenderer->CopyTextureToRTV(m_OutputBuffer->GetShaderResView(), oldRTV);
    
    // Cleanup
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    context->PSSetShaderResources(0, 4, nullSRVs);
    context->OMSetRenderTargets(1, oldRTV.GetAddressOf(), oldDSV.Get());
    
    m_FirstFrame = false;
}
