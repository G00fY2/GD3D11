#include "pch.h"
#include "D3D11Texture.h"
#include "Engine.h"
#include "D3D11GraphicsEngineBase.h"
#include "GothicAPI.h"
#include <DDSTextureLoader.h>
#include "RenderToTextureBuffer.h"
#include <d3dcompiler.h>
#include "D3D11_Helpers.h"

using namespace DirectX;

D3D11Texture::D3D11Texture() {
    // Insert into state-map
    ID = D3D11ObjectIDs::Counters.TextureCounter++;

    D3D11ObjectIDs::TextureByID[ID] = this;
}

D3D11Texture::~D3D11Texture() {
    // Remove from state map
    Toolbox::EraseByElement( D3D11ObjectIDs::TextureByID, this );

    Thumbnail.Reset();
    Texture.Reset();
    ShaderResourceView.Reset();
}

/** Initializes the texture object */
XRESULT D3D11Texture::Init( INT2 size, ETextureFormat format, UINT mipMapCount, void* data, const std::string& fileName ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    //Engine::GAPI->EnterResourceCriticalSection();

    TextureFormat = (DXGI_FORMAT)format;
    TextureSize = size;
    MipMapCount = mipMapCount;

    CD3D11_TEXTURE2D_DESC textureDesc(
        (DXGI_FORMAT)format,
        size.x,
        size.y,
        1,
        mipMapCount,
        D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, 1, 0, 0 );

    LE( engine->GetDevice()->CreateTexture2D( &textureDesc, nullptr, Texture.ReleaseAndGetAddressOf() ) );
    SetDebugName( Texture.Get(), "D3D11Texture(\"" + fileName + "\")->Texture" );

    D3D11_SHADER_RESOURCE_VIEW_DESC descRV = {};
    descRV.Format = DXGI_FORMAT_UNKNOWN;
    descRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    descRV.Texture2D.MipLevels = mipMapCount;
    descRV.Texture2D.MostDetailedMip = 0;
    LE( engine->GetDevice()->CreateShaderResourceView( Texture.Get(), &descRV, ShaderResourceView.ReleaseAndGetAddressOf() ) );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + fileName + "\")->ShaderResourceView" );

    //Engine::GAPI->LeaveResourceCriticalSection();

    return XR_SUCCESS;
}

/** Initializes the texture from a file */
XRESULT D3D11Texture::Init( const std::string& file ) {
    HRESULT hr;
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    //LogInfo() << "Loading Engine-Texture: " << file;

    //Engine::GAPI->EnterResourceCriticalSection();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> res;
    LE( CreateDDSTextureFromFile( engine->GetDevice().Get(), Toolbox::ToWideChar( file.c_str() ).c_str(), (ID3D11Resource**)res.ReleaseAndGetAddressOf(), ShaderResourceView.GetAddressOf() ) );

    if ( !ShaderResourceView.Get() || !res.Get() )
        return XR_FAILED;

    D3D11_TEXTURE2D_DESC desc;
    res->GetDesc( &desc );

    Texture = res;
    TextureFormat = desc.Format;

    TextureSize.x = desc.Width;
    TextureSize.y = desc.Height;
    SetDebugName( res.Get(), "D3D11Texture(\"" + file + "\")->Texture" );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + file + "\")->ShaderResourceView" );

    //Engine::GAPI->LeaveResourceCriticalSection();

    return XR_SUCCESS;
}

/** Updates the Texture-Object */
XRESULT D3D11Texture::UpdateData( void* data, int mip ) {
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    // Enter the critical section for safety while executing the deferred command list
    Engine::GAPI->EnterResourceCriticalSection();
    engine->GetContext()->UpdateSubresource( Texture.Get(), mip, nullptr, data, GetRowPitchBytes( mip ), GetSizeInBytes( mip ) );
    Engine::GAPI->LeaveResourceCriticalSection();

    return XR_SUCCESS;
}

/** Updates the Texture-Object using the deferred context (For loading in an other thread) */
XRESULT D3D11Texture::UpdateDataDeferred( void* data, int mip, bool noLock ) {
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    // Enter the critical section for safety while executing the deferred command list
    if ( !noLock )
        Engine::GAPI->EnterResourceCriticalSection();

    engine->GetDeferredMediaContext1()->UpdateSubresource( Texture.Get(), mip, nullptr, data, GetRowPitchBytes( mip ), GetSizeInBytes( mip ) );

    if ( !noLock )
        Engine::GAPI->LeaveResourceCriticalSection();

    return XR_SUCCESS;
}

/** Returns the RowPitch-Bytes */
UINT D3D11Texture::GetRowPitchBytes( int mip ) {
    int px = static_cast<int>( std::max<float>( 1.0f, floor( TextureSize.x / pow( 2.0f, mip ) ) ) );
    //int py = (int)std::max( 1.0, floor( TextureSize.y / pow( 2.0f, mip ) ) );
    //int px = TextureSize.x;
    //int py = TextureSize.y;

    if ( TextureFormat == DXGI_FORMAT_BC1_UNORM || TextureFormat == DXGI_FORMAT_BC2_UNORM ||
        TextureFormat == DXGI_FORMAT_BC3_UNORM ) {
        return Toolbox::GetDDSRowPitchSize( px, TextureFormat == DXGI_FORMAT_BC1_UNORM );
    } else // Use R8G8B8A8
    {
        return px * 4;
    }
}

/** Returns the size of the texture in bytes */
UINT D3D11Texture::GetSizeInBytes( int mip ) {
    int px = static_cast<int>( std::max<float>( 1.0f, floor( TextureSize.x / pow( 2.0f, mip ) ) ) );
    int py = static_cast<int>( std::max<float>( 1.0f, floor( TextureSize.y / pow( 2.0f, mip ) ) ) );
    //int px = TextureSize.x;
    //int py = TextureSize.y;

    if ( TextureFormat == DXGI_FORMAT_BC1_UNORM || TextureFormat == DXGI_FORMAT_BC2_UNORM ||
        TextureFormat == DXGI_FORMAT_BC3_UNORM ) {
        return Toolbox::GetDDSStorageRequirements( px, py, TextureFormat == DXGI_FORMAT_BC1_UNORM );
    } else // Use R8G8B8A8
    {
        return px * py * 4;
    }
}

/** Binds this texture to a pixelshader */
XRESULT D3D11Texture::BindToPixelShader( int slot ) {
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    engine->GetContext()->PSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );

    return XR_SUCCESS;
}

/** Binds this texture to a pixelshader */
XRESULT D3D11Texture::BindToVertexShader( int slot ) {
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    engine->GetContext()->VSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );
    engine->GetContext()->DSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );

    return XR_SUCCESS;
}

/** Binds this texture to a domainshader */
XRESULT D3D11Texture::BindToDomainShader( int slot ) {
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    engine->GetContext()->DSSetShaderResources( slot, 1, ShaderResourceView.GetAddressOf() );

    return XR_SUCCESS;
}

/** Creates a thumbnail for this */
XRESULT D3D11Texture::CreateThumbnail() {
    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;
    HRESULT hr;
    // Since D2D can't load DXTn-Textures on Windows 7, we copy it over to a smaller texture here in d3d11

    // Create texture
    CD3D11_TEXTURE2D_DESC textureDesc(
        (DXGI_FORMAT)DXGI_FORMAT_R8G8B8A8_UNORM,
        256,
        256,
        1,
        1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, 1, 0, 0 );

    LE( engine->GetDevice()->CreateTexture2D( &textureDesc, nullptr, Thumbnail.ReleaseAndGetAddressOf() ) );

    // Create temporary RTV
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> tempRTV;
    LE( engine->GetDevice()->CreateRenderTargetView( Thumbnail.Get(), nullptr, tempRTV.GetAddressOf() ) );
    if ( !tempRTV.Get() )
        return XR_FAILED;

    engine->GetContext()->ClearRenderTargetView( tempRTV.Get(), (float*)&float4( 1, 0, 0, 1 ) );

    // Copy main texture to it
    engine->GetContext()->PSSetShaderResources( 0, 1, ShaderResourceView.GetAddressOf() );

    ID3D11RenderTargetView* oldRTV[2];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;

    engine->GetContext()->OMGetRenderTargets( 2, oldRTV, oldDSV.GetAddressOf() );

    engine->GetContext()->OMSetRenderTargets( 1, tempRTV.GetAddressOf(), nullptr );
    engine->DrawQuad( INT2( 0, 0 ), INT2( 256, 256 ) );

    engine->GetContext()->OMSetRenderTargets( 2, oldRTV, oldDSV.Get() );

    return XR_SUCCESS;
}

/** Returns the thumbnail of this texture. If this returns nullptr, you need to create one first */
const Microsoft::WRL::ComPtr<ID3D11Texture2D>& D3D11Texture::GetThumbnail() {
    return Thumbnail;
}

/** Generates mipmaps for this texture (may be slow!) */
XRESULT D3D11Texture::GenerateMipMaps() {
    if ( MipMapCount == 1 )
        return XR_SUCCESS;

    D3D11GraphicsEngineBase* engine = (D3D11GraphicsEngineBase*)Engine::GraphicsEngine;

    Engine::GAPI->EnterResourceCriticalSection();

    RenderToTextureBuffer* b = new RenderToTextureBuffer( engine->GetDevice().Get(), TextureSize.x, TextureSize.y, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, MipMapCount );

    engine->GetDeferredMediaContext1()->CopySubresourceRegion( b->GetTexture().Get(), 0, 0, 0, 0, Texture.Get(), 0, nullptr );

    // Generate mips
    engine->GetDeferredMediaContext1()->GenerateMips( b->GetShaderResView().Get() );

    // Copy the full chain back
    engine->GetDeferredMediaContext1()->CopyResource( Texture.Get(), b->GetTexture().Get() );
    delete b;

    Engine::GAPI->LeaveResourceCriticalSection();

    return XR_SUCCESS;
}
