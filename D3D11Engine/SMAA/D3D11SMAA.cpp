#include "D3D11SMAA.h"
#include <d3dcompiler.h>
#include <vector>
#include <iostream>

// Include DirectXTK or your preferred texture loader
#include "DDSTextureLoader.h" // Assuming DirectXTK availability

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using namespace Microsoft::WRL;

D3D11SMAA::D3D11SMAA(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_context(context), m_width(0), m_height(0)
{
}

D3D11SMAA::~D3D11SMAA()
{
}

HRESULT D3D11SMAA::CompileShader(const std::wstring& path, const std::string& entryPoint, const std::string& profile, ID3DBlob** blob)
{
    ComPtr<ID3DBlob> errorBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(), profile.c_str(), flags, 0, blob, errorBlob.GetAddressOf());

    if (FAILED(hr) && errorBlob)
    {
        OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    return hr;
}

bool D3D11SMAA::Init(const std::wstring& shaderPath, const std::wstring& areaTexPath, const std::wstring& searchTexPath)
{
    HRESULT hr;
    ComPtr<ID3DBlob> blob;

    // 1. Compile Shaders
    // Edge Detection
    if (FAILED(CompileShader(shaderPath, "EdgeDetectionVS", "vs_5_0", &blob))) return false;
    m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_vsEdge.GetAddressOf());

    if (FAILED(CompileShader(shaderPath, "LumaEdgeDetectionPS", "ps_5_0", &blob))) return false;
    m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_psLumaEdge.GetAddressOf());

    // Blending Weight
    if (FAILED(CompileShader(shaderPath, "BlendingWeightCalculationVS", "vs_5_0", &blob))) return false;
    m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_vsBlend.GetAddressOf());

    if (FAILED(CompileShader(shaderPath, "BlendingWeightCalculationPS", "ps_5_0", &blob))) return false;
    m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_psBlend.GetAddressOf());

    // Neighborhood Blending
    if (FAILED(CompileShader(shaderPath, "NeighborhoodBlendingVS", "vs_5_0", &blob))) return false;
    m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_vsNeighbor.GetAddressOf());

    if (FAILED(CompileShader(shaderPath, "NeighborhoodBlendingPS", "ps_5_0", &blob))) return false;
    m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_psNeighbor.GetAddressOf());

    // 2. Load Textures
    // Use DirectXTK CreateDDSTextureFromFile
    hr = CreateDDSTextureFromFile(m_device.Get(), areaTexPath.c_str(), nullptr, m_areaTexSRV.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = CreateDDSTextureFromFile(m_device.Get(), searchTexPath.c_str(), nullptr, m_searchTexSRV.GetAddressOf());
    if (FAILED(hr)) return false;

    // 3. Create Constant Buffer
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(SMAAConstants);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = 0;
    hr = m_device->CreateBuffer(&bd, nullptr, m_constantBuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    // 4. Create Sampler States
    // SMAA requires specific samplers (Linear Clamp and Point Clamp)
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&sampDesc, m_samplerLinear.GetAddressOf());

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // Point filter
    m_device->CreateSamplerState(&sampDesc, m_samplerPoint.GetAddressOf());

    // 5. Create Helper States
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = true;
    m_device->CreateRasterizerState(&rasterDesc, m_rasterizerState.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    m_device->CreateDepthStencilState(&dsDesc, m_disableDepthState.GetAddressOf());
    
    // Default blend state (Opaque/Overwrite)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&blendDesc, m_blendState.GetAddressOf());

    return true;
}

void D3D11SMAA::OnResize(int width, int height)
{
    if (m_width == width && m_height == height) return;
    
    m_width = width;
    m_height = height;

    // Release old
    m_edgesTex.Reset(); m_edgesRTV.Reset(); m_edgesSRV.Reset();
    m_blendTex.Reset(); m_blendRTV.Reset(); m_blendSRV.Reset();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // SMAA requires specific formats, RGBA8 UNORM is generally safe
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    // Create Edges Texture
    m_device->CreateTexture2D(&desc, nullptr, m_edgesTex.GetAddressOf());
    m_device->CreateRenderTargetView(m_edgesTex.Get(), nullptr, m_edgesRTV.GetAddressOf());
    m_device->CreateShaderResourceView(m_edgesTex.Get(), nullptr, m_edgesSRV.GetAddressOf());

    // Create Blend Texture
    m_device->CreateTexture2D(&desc, nullptr, m_blendTex.GetAddressOf());
    m_device->CreateRenderTargetView(m_blendTex.Get(), nullptr, m_blendRTV.GetAddressOf());
    m_device->CreateShaderResourceView(m_blendTex.Get(), nullptr, m_blendSRV.GetAddressOf());

    // Update Constant Buffer
    SMAAConstants constants;
    constants.RT_Metrics = XMFLOAT4(1.0f / width, 1.0f / height, (float)width, (float)height);
    m_context->UpdateSubresource(m_constantBuffer.Get(), 0, nullptr, &constants, 0, 0);
}

void D3D11SMAA::Render(ID3D11ShaderResourceView* inputSRV, ID3D11RenderTargetView* outputRTV)
{
    if (!m_width || !m_height) return;

    // Save old state (Optional, but good practice in a library)
    // For performance in an engine, you usually don't save/restore but assume state flow.
    // Here we just set what we need.

    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };

    // Common State Setup
    m_context->IASetInputLayout(nullptr); // Using VertexID generation
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->RSSetState(m_rasterizerState.Get());
    m_context->OMSetDepthStencilState(m_disableDepthState.Get(), 0);
    m_context->OMSetBlendState(m_blendState.Get(), nullptr, 0xFFFFFFFF);
    
    ID3D11SamplerState* samplers[] = { m_samplerLinear.Get(), m_samplerPoint.Get() };
    m_context->PSSetSamplers(0, 2, samplers);
    m_context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);

    // -----------------------------------------------------------
    // Pass 1: Edge Detection
    // Input: Color (t0)
    // Output: EdgesTex
    // -----------------------------------------------------------
    m_context->ClearRenderTargetView(m_edgesRTV.Get(), clearColor);
    m_context->OMSetRenderTargets(1, m_edgesRTV.GetAddressOf(), nullptr);
    
    m_context->VSSetShader(m_vsEdge.Get(), nullptr, 0);
    m_context->PSSetShader(m_psLumaEdge.Get(), nullptr, 0);
    
    // Bind Input Color to Slot t0
    m_context->PSSetShaderResources(0, 1, &inputSRV);

    m_context->Draw(3, 0); // Draw Fullscreen Triangle

    // Unbind SRV to avoid hazard
    m_context->PSSetShaderResources(0, 1, nullSRVs);

    // -----------------------------------------------------------
    // Pass 2: Blending Weight Calculation
    // Input: EdgesTex (t1), AreaTex (t3), SearchTex (t4)
    // Output: BlendTex
    // -----------------------------------------------------------
    m_context->ClearRenderTargetView(m_blendRTV.Get(), clearColor);
    m_context->OMSetRenderTargets(1, m_blendRTV.GetAddressOf(), nullptr);

    m_context->VSSetShader(m_vsBlend.Get(), nullptr, 0);
    m_context->PSSetShader(m_psBlend.Get(), nullptr, 0);

    // Bind resources
    ID3D11ShaderResourceView* pass2SRVs[] = { nullptr, m_edgesSRV.Get(), nullptr, m_areaTexSRV.Get(), m_searchTexSRV.Get() };
    // We start at slot 0 to clear slot 0, or just bind specifically
    // Map: t1=Edges, t3=Area, t4=Search. t0 is unused here.
    m_context->PSSetShaderResources(0, 5, pass2SRVs);

    m_context->Draw(3, 0);

    m_context->PSSetShaderResources(0, 5, pass2SRVs ); // Unbind

    // -----------------------------------------------------------
    // Pass 3: Neighborhood Blending
    // Input: Color (t0), BlendTex (t2)
    // Output: Target RTV
    // -----------------------------------------------------------
    m_context->OMSetRenderTargets(1, &outputRTV, nullptr);

    m_context->VSSetShader(m_vsNeighbor.Get(), nullptr, 0);
    m_context->PSSetShader(m_psNeighbor.Get(), nullptr, 0);

    ID3D11ShaderResourceView* pass3SRVs[] = { inputSRV, nullptr, m_blendSRV.Get() };
    m_context->PSSetShaderResources(0, 3, pass3SRVs);

    m_context->Draw(3, 0);

    // Cleanup
    m_context->PSSetShaderResources(0, 3, nullSRVs);
}
