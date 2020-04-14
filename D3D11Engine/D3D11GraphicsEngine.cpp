#include "D3D11GraphicsEngine.h"

#include "AlignedAllocator.h"
#include "BaseAntTweakBar.h"
#include "D2DEditorView.h"
#include "D2DView.h"
#include "D3D11Effect.h"
#include "D3D11GShader.h"
#include "D3D11HDShader.h"
#include "D3D11LineRenderer.h"
#include "D3D11OcclusionQuerry.h"
#include "D3D11PShader.h"
#include "D3D11PfxRenderer.h"
#include "D3D11PipelineStates.h"
#include "D3D11PointLight.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "GMesh.h"
#include "GOcean.h"
#include "GSky.h"
#include "RenderToTextureBuffer.h"
#include "zCDecal.h"
#include "zCMaterial.h"
#include "zCQuadMark.h"
#include "zCTexture.h"
#include "zCView.h"
#include "zCVobLight.h"
#include <DDSTextureLoader.h>
#include <ScreenGrab.h>
#include <wincodec.h>
#include <SpriteFont.h>
#include <SpriteBatch.h>
#include <locale>
#include <codecvt>
#include <wrl\client.h>

#pragma comment(lib, "dxguid.lib")

namespace wrl = Microsoft::WRL;
using namespace DirectX;
using namespace DirectX::SimpleMath;

const int RES_UPSCALE = 1;
const INT2 DEFAULT_RESOLUTION = INT2(1920 * RES_UPSCALE, 1080 * RES_UPSCALE);
// const int WORLD_SHADOWMAP_SIZE = 1024;

const int NUM_UNLOADEDTEXCOUNT_FORCE_LOAD_TEXTURES = 100;

const float DEFAULT_NORMALMAP_STRENGTH = 0.10f;
const float DEFAULT_FAR_PLANE = 50000.0f;
const XMFLOAT4 UNDERWATER_COLOR_MOD = XMFLOAT4(0.5f, 0.7f, 1.0f, 1.0f);

const float NUM_FRAME_SHADOW_UPDATES =
2;  // Fraction of lights to update per frame
const int NUM_MIN_FRAME_SHADOW_UPDATES =
4;  // Minimum lights to update per frame
const int MAX_IMPORTANT_LIGHT_UPDATES = 1;
//#define DEBUG_D3D11

D3D11GraphicsEngine::D3D11GraphicsEngine() {
	Resolution = DEFAULT_RESOLUTION;
	DebugPointlight = nullptr;
	OutputWindow = nullptr;
	ActiveHDS = nullptr;
	ActivePS = nullptr;
	InverseUnitSphereMesh = nullptr;

	Effects = std::make_unique<D3D11Effect>();
	RenderingStage = DES_MAIN;
	PresentPending = false;
	SaveScreenshotNextFrame = false;
	LineRenderer = std::make_unique<D3D11LineRenderer>();
	Occlusion = std::make_unique<D3D11OcclusionQuerry>();

	m_FrameLimiter = std::make_unique<FpsLimiter>();
	m_LastFrameLimit = 0;

	RECT desktopRect;
	GetClientRect(GetDesktopWindow(), &desktopRect);

	// Match the resolution with the current desktop resolution
	Resolution =
		Engine::GAPI->GetRendererState()->RendererSettings.LoadedResolution;
}

D3D11GraphicsEngine::~D3D11GraphicsEngine() {
	GothicDepthBufferStateInfo::DeleteCachedObjects();
	GothicBlendStateInfo::DeleteCachedObjects();
	GothicRasterizerStateInfo::DeleteCachedObjects();

	SAFE_DELETE(InverseUnitSphereMesh);

	SAFE_DELETE(QuadVertexBuffer);
	SAFE_DELETE(QuadIndexBuffer);

	ID3D11Debug* d3dDebug;
	Device->QueryInterface(__uuidof(ID3D11Debug),
		reinterpret_cast<void**>(&d3dDebug));

	if (d3dDebug) {
		d3dDebug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
		d3dDebug->Release();
	}

	// MemTrackerFinalReport();
}

/** Called when the game created it's window */
XRESULT D3D11GraphicsEngine::Init() {
	HRESULT hr;

	LogInfo() << "Initializing Device...";

	// Create DXGI factory
	LE(CreateDXGIFactory(__uuidof(IDXGIFactory), &DXGIFactory));
	LE(DXGIFactory->EnumAdapters(0, &DXGIAdapter));  // Get first adapter

	// Find out what we are rendering on to write it into the logfile
	DXGI_ADAPTER_DESC adpDesc;
	DXGIAdapter->GetDesc(&adpDesc);

	std::wstring wDeviceDescription(adpDesc.Description);
	std::string deviceDescription(wDeviceDescription.begin(),
		wDeviceDescription.end());
	DeviceDescription = deviceDescription;
	LogInfo() << "Rendering on: " << deviceDescription.c_str();

	int flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	
	D3D_FEATURE_LEVEL featurelevel = D3D_FEATURE_LEVEL_11_0;

	// Create D3D11-Device
#ifndef DEBUG_D3D11
	LE(D3D11CreateDevice(DXGIAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
		&featurelevel, 1, D3D11_SDK_VERSION, &Device, nullptr,
		&Context));
#else
	LE(D3D11CreateDevice(DXGIAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		flags | D3D11_CREATE_DEVICE_DEBUG, &featurelevel, 1,
		D3D11_SDK_VERSION, &Device, nullptr, &Context));
#endif

	if (hr == DXGI_ERROR_UNSUPPORTED) {
		LogErrorBox() << "Your GPU (" << deviceDescription.c_str()
			<< ") does not support Direct3D 11, so it can't run GD3D11!\n"
			"It has to be at least Featurelevel 11_0 compatible, "
			"which requires at least:\n"
			" *	Nvidia GeForce GTX4xx or higher\n"
			" *	AMD Radeon 5xxx or higher\n\n"
			"The game will now close.";
		exit(0);
	}

	LE(GetDevice()->CreateDeferredContext(0, &DeferredContext));  // Used for multithreaded texture loading

	LogInfo() << "Creating ShaderManager";

	ShaderManager = std::make_unique<D3D11ShaderManager>();
	ShaderManager->Init();
	ShaderManager->LoadShaders();

	PS_DiffuseNormalmapped = ShaderManager->GetPShader("PS_DiffuseNormalmapped");
	PS_Diffuse = ShaderManager->GetPShader("PS_Diffuse");
	PS_DiffuseNormalmappedAlphatest =
		ShaderManager->GetPShader("PS_DiffuseNormalmappedAlphaTest");
	PS_DiffuseAlphatest = ShaderManager->GetPShader("PS_DiffuseAlphaTest");

	TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();
	TempVertexBuffer->Init(
		nullptr, DRAWVERTEXARRAY_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
		D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE);

	DynamicInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
	DynamicInstancingBuffer->Init(
		nullptr, INSTANCING_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
		D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE);

	D3D11_SAMPLER_DESC samplerDesc;
	samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MipLODBias = 0;
	samplerDesc.MaxAnisotropy = 16;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.BorderColor[0] = 1.0f;
	samplerDesc.BorderColor[1] = 1.0f;
	samplerDesc.BorderColor[2] = 1.0f;
	samplerDesc.BorderColor[3] = 1.0f;
	samplerDesc.MinLOD = -3.402823466e+38F;  // -FLT_MAX
	samplerDesc.MaxLOD = 3.402823466e+38F;   // FLT_MAX

	LE(GetDevice()->CreateSamplerState(&samplerDesc, &DefaultSamplerState));
	GetContext()->PSSetSamplers(0, 1, DefaultSamplerState.GetAddressOf());
	GetContext()->VSSetSamplers(0, 1, DefaultSamplerState.GetAddressOf());
	GetContext()->DSSetSamplers(0, 1, DefaultSamplerState.GetAddressOf());
	GetContext()->HSSetSamplers(0, 1, DefaultSamplerState.GetAddressOf());

	samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	//TODO: NVidia PCSS
	// samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	GetDevice()->CreateSamplerState(&samplerDesc, &ShadowmapSamplerState);
	GetContext()->PSSetSamplers(2, 1, ShadowmapSamplerState.GetAddressOf());
	GetContext()->VSSetSamplers(2, 1, ShadowmapSamplerState.GetAddressOf());

	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	GetDevice()->CreateSamplerState(&samplerDesc, &ClampSamplerState);

	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	GetDevice()->CreateSamplerState(&samplerDesc, &CubeSamplerState);

	D3D11_RASTERIZER_DESC rasterizerDesc;
	rasterizerDesc.CullMode = D3D11_CULL_BACK;
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.FrontCounterClockwise =
		true;  // Gothics world vertices are CCW. That get's set by the
			   // GAPI-Graphics-State as well but is hardcoded here
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0;
	rasterizerDesc.SlopeScaledDepthBias = 0;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = true;

	LE(GetDevice()->CreateRasterizerState(&rasterizerDesc, &WorldRasterizerState));
	GetContext()->RSSetState(WorldRasterizerState.Get());

	rasterizerDesc.FrontCounterClockwise = false;
	LE(GetDevice()->CreateRasterizerState(&rasterizerDesc, &HUDRasterizerState));

	D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
	// Depth test parameters
	depthStencilDesc.DepthEnable = true;
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

	// Stencil test parameters
	depthStencilDesc.StencilEnable = false;
	depthStencilDesc.StencilReadMask = 0xFF;
	depthStencilDesc.StencilWriteMask = 0xFF;

	// Stencil operations if pixel is front-facing
	depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
	depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	// Stencil operations if pixel is back-facing
	depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
	depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	LE(GetDevice()->CreateDepthStencilState(&depthStencilDesc,
		&DefaultDepthStencilState));

	SetActivePixelShader("PS_Simple");
	SetActiveVertexShader("VS_Ex");

	DistortionTexture = std::make_unique<D3D11Texture>();
	DistortionTexture->Init("system\\GD3D11\\textures\\distortion2.dds");

	NoiseTexture = std::make_unique<D3D11Texture>();
	NoiseTexture->Init("system\\GD3D11\\textures\\noise.dds");

	WhiteTexture = std::make_unique<D3D11Texture>();
	WhiteTexture->Init("system\\GD3D11\\textures\\white.dds");

	InverseUnitSphereMesh = new GMesh;
	InverseUnitSphereMesh->LoadMesh("system\\GD3D11\\meshes\\icoSphere.obj");

	// Create distance-buffers
	CreateConstantBuffer((D3D11ConstantBuffer * *)& InfiniteRangeConstantBuffer,
		nullptr, sizeof(float4));
	CreateConstantBuffer((D3D11ConstantBuffer * *)& OutdoorSmallVobsConstantBuffer,
		nullptr, sizeof(float4));
	CreateConstantBuffer((D3D11ConstantBuffer * *)& OutdoorVobsConstantBuffer,
		nullptr, sizeof(float4));

	// Init inf-buffer now
	InfiniteRangeConstantBuffer->UpdateBuffer(float4(FLT_MAX, 0, 0, 0).toPtr());

	// Load reflectioncube
	
	if (S_OK != CreateDDSTextureFromFile(
		GetDevice(), L"system\\GD3D11\\Textures\\reflect_cube.dds",
		nullptr,
		&ReflectionCube))
		LogWarn()
		<< "Failed to load file: system\\GD3D11\\Textures\\reflect_cube.dds";

	if (S_OK != CreateDDSTextureFromFile(
		GetDevice(), L"system\\GD3D11\\Textures\\SkyCubemap2.dds", 
		nullptr, &ReflectionCube2))
		LogWarn()
		<< "Failed to load file: system\\GD3D11\\Textures\\SkyCubemap2.dds";

	// Init quad buffers
	Engine::GraphicsEngine->CreateVertexBuffer(&QuadVertexBuffer);
	QuadVertexBuffer->Init(nullptr, 6 * sizeof(ExVertexStruct),
		D3D11VertexBuffer::EBindFlags::B_VERTEXBUFFER,
		D3D11VertexBuffer::EUsageFlags::U_DYNAMIC,
		D3D11VertexBuffer::CA_WRITE);

	Engine::GraphicsEngine->CreateVertexBuffer(&QuadIndexBuffer);
	QuadIndexBuffer->Init(nullptr, 6 * sizeof(VERTEX_INDEX),
		D3D11VertexBuffer::EBindFlags::B_INDEXBUFFER,
		D3D11VertexBuffer::EUsageFlags::U_DYNAMIC,
		D3D11VertexBuffer::CA_WRITE);

	ExVertexStruct vx[6];
	ZeroMemory(vx, sizeof(vx));

	const float scale = 1.0f;
	vx[0].Position = Vector3(-scale * 0.5f, -scale * 0.5f, 0.0f);
	vx[1].Position = Vector3(scale * 0.5f, -scale * 0.5f, 0.0f);
	vx[2].Position = Vector3(-scale * 0.5f, scale * 0.5f, 0.0f);

	vx[0].TexCoord = Vector2(0, 0);
	vx[1].TexCoord = Vector2(1, 0);
	vx[2].TexCoord = Vector2(0, 1);

	vx[0].Color = 0xFFFFFFFF;
	vx[1].Color = 0xFFFFFFFF;
	vx[2].Color = 0xFFFFFFFF;

	vx[3].Position = Vector3(scale * 0.5f, -scale * 0.5f, 0.0f);
	vx[4].Position = Vector3(scale * 0.5f, scale * 0.5f, 0.0f);
	vx[5].Position = Vector3(-scale * 0.5f, scale * 0.5f, 0.0f);

	vx[3].TexCoord = Vector2(1, 0);
	vx[4].TexCoord = Vector2(1, 1);
	vx[5].TexCoord = Vector2(0, 1);

	vx[3].Color = 0xFFFFFFFF;
	vx[4].Color = 0xFFFFFFFF;
	vx[5].Color = 0xFFFFFFFF;

	QuadVertexBuffer->UpdateBuffer(vx);

	VERTEX_INDEX indices[] = {0, 1, 2, 3, 4, 5};
	QuadIndexBuffer->UpdateBuffer(indices, sizeof(indices));

	// Create dummy rendertarget for shadowcubes
	DummyShadowCubemapTexture = std::make_unique<RenderToTextureBuffer>(
		GetDevice(), POINTLIGHT_SHADOWMAP_SIZE, POINTLIGHT_SHADOWMAP_SIZE,
		DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, DXGI_FORMAT_UNKNOWN,
		DXGI_FORMAT_UNKNOWN, 1, 6);
	
	try
	{
		auto fontFilePath = L"system\\GD3D11\\Fonts\\" + Toolbox::ToWideChar(Engine::GAPI->GetRendererState()->RendererSettings.FontFileDefault);
		m_font = std::make_unique<SpriteFont>(GetDevice(), fontFilePath.c_str());
		m_spriteBatch = std::make_unique<SpriteBatch>(GetContext());
	}
	catch (const std::exception& ex)
	{
		Engine::GAPI->GetRendererState()->RendererSettings.EnableCustomFontRendering = false;
		LogError() << ex.what() << std::endl;
	}
	states = std::make_unique<CommonStates>(GetDevice());

	return XR_SUCCESS;
}

/** Called when the game created its window */
XRESULT D3D11GraphicsEngine::SetWindow(HWND hWnd) {
	LogInfo() << "Creating swapchain";
	OutputWindow = hWnd;

	const INT2 res = Resolution;

#ifdef BUILD_SPACER
	RECT r;
	GetClientRect(hWnd, &r);

	res.x = r.right;
	res.y = r.bottom;
#endif

	if (res.x != 0 && res.y != 0) OnResize(res);

	return XR_SUCCESS;
}

/** Called on window resize/resolution change */
XRESULT D3D11GraphicsEngine::OnResize(INT2 newSize) {
	HRESULT hr;

	if (memcmp(&Resolution, &newSize, sizeof(newSize)) == 0 && SwapChain)
		return XR_SUCCESS;  // Don't resize if we don't have to

	Resolution = newSize;
	INT2 bbres = GetBackbufferResolution();

#ifndef BUILD_SPACER
	RECT desktopRect;
	GetClientRect(GetDesktopWindow(), &desktopRect);
	SetWindowPos(OutputWindow, nullptr, 0, 0, desktopRect.right,
		desktopRect.bottom, 0);
#endif

	// Release all referenced buffer resources before we can resize the swapchain. Needed!
	BackbufferRTV.Reset();
	BackbufferSRV.Reset();
	DepthStencilBuffer.reset();

	if (UIView) UIView->PrepareResize();

	const DXGI_SWAP_CHAIN_FLAG& scflags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	if (!SwapChain) {
		LogInfo() << "Creating new swapchain! (Format: DXGI_FORMAT_R8G8B8A8_UNORM)";

		DXGI_SWAP_CHAIN_DESC scd;
		ZeroMemory(&scd, sizeof(DXGI_SWAP_CHAIN_DESC));

		scd.Flags = scflags;
		scd.BufferCount = 1;
		scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
		scd.OutputWindow = OutputWindow;
		scd.SampleDesc.Count = 1;
		scd.SampleDesc.Quality = 0;
		scd.BufferDesc.Height = bbres.y;
		scd.BufferDesc.Width = bbres.x;
		scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		bool windowed = Engine::GAPI->HasCommandlineParameter("ZWINDOW") ||
			Engine::GAPI->GetIntParamFromConfig("zStartupWindowed");
		scd.Windowed = windowed;

#ifndef PUBLIC_RELEASE
		scd.Windowed = true;
#endif

#ifdef BUILD_GOTHIC_1_08k
#ifdef PUBLIC_RELEASE
		scd.Windowed = false;
#else
		scd.Windowed = true;
#endif
#endif

		LE(DXGIFactory->CreateSwapChain(GetDevice(), &scd, &SwapChain));

		if (!SwapChain) {
			LogError() << "Failed to create Swapchain! Program will now exit!";
			exit(0);
		}

		// Need to init AntTweakBar now that we have a working swapchain
		XLE(Engine::AntTweakBar->Init());

	}
	else {
		LogInfo() << "Resizing swapchain  (Format: DXGI_FORMAT_R8G8B8A8_UNORM)";

		if (FAILED(SwapChain->ResizeBuffers(0, bbres.x, bbres.y,
			DXGI_FORMAT_R8G8B8A8_UNORM, scflags))) {
			LogError() << "Failed to resize swapchain!";
			return XR_FAILED;
		}
	}

	// Successfully resized swapchain, re-get buffers
	wrl::ComPtr<ID3D11Texture2D> backbuffer;
	SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backbuffer);
	
	// Recreate RenderTargetView
	LE(GetDevice()->CreateRenderTargetView(backbuffer.Get(), nullptr, &BackbufferRTV));
	LE(GetDevice()->CreateShaderResourceView(backbuffer.Get(), nullptr, &BackbufferSRV));

	if (UIView) {
		UIView->Resize(Resolution, backbuffer.Get());
	}

	// Recreate DepthStencilBuffer
	DepthStencilBuffer = std::make_unique<RenderToDepthStencilBuffer>(
		GetDevice(), Resolution.x, Resolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
		DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT);

	DepthStencilBufferCopy = std::make_unique<RenderToTextureBuffer>(
		GetDevice(), Resolution.x, Resolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
		DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT);

	// Bind our newly created resources
	GetContext()->OMSetRenderTargets(1, BackbufferRTV.GetAddressOf(),
		DepthStencilBuffer->GetDepthStencilView());

	// Set the viewport
	D3D11_VIEWPORT viewport;
	ZeroMemory(&viewport, sizeof(D3D11_VIEWPORT));

	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.Width = static_cast<float>(bbres.x);
	viewport.Height = static_cast<float>(bbres.y);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	GetContext()->RSSetViewports(1, &viewport);

	// Create PFX-Renderer
	if (!PfxRenderer) PfxRenderer = std::make_unique<D3D11PfxRenderer>();

	PfxRenderer->OnResize(Resolution);

	CloudBuffer = std::make_unique<RenderToTextureBuffer>(GetDevice(), bbres.x, bbres.y, DXGI_FORMAT_R8G8B8A8_UNORM);

	GBuffer1_Normals_SpecIntens_SpecPower = std::make_unique<RenderToTextureBuffer>(
		GetDevice(), Resolution.x, Resolution.y, DXGI_FORMAT_R16G16B16A16_FLOAT);

	GBuffer0_Diffuse = std::make_unique<RenderToTextureBuffer>(
		GetDevice(), Resolution.x, Resolution.y, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

	HDRBackBuffer = std::make_unique<RenderToTextureBuffer>(GetDevice(), Resolution.x, Resolution.y,
		DXGI_FORMAT_R16G16B16A16_FLOAT);

	int s = Engine::GAPI->GetRendererState()->RendererSettings.ShadowMapSize;
	WorldShadowmap1 = std::make_unique<RenderToDepthStencilBuffer>(
		GetDevice(), s, s, DXGI_FORMAT_R32_TYPELESS, nullptr, DXGI_FORMAT_D32_FLOAT,
		DXGI_FORMAT_R32_FLOAT);

	Engine::AntTweakBar->OnResize(newSize);

	return XR_SUCCESS;
}

/** Called when the game wants to render a new frame */
XRESULT D3D11GraphicsEngine::OnBeginFrame() {
	if (Engine::GAPI->GetRendererState()->RendererSettings.FpsLimit != 0)
	{
		if (m_LastFrameLimit != Engine::GAPI->GetRendererState()->RendererSettings.FpsLimit)
		{
			m_LastFrameLimit = Engine::GAPI->GetRendererState()->RendererSettings.FpsLimit;
			m_FrameLimiter->SetLimit(m_LastFrameLimit);
		}
		m_FrameLimiter->Start();
	}
	else {
		m_FrameLimiter->Reset();
	}
	Engine::GAPI->GetRendererState()->RendererInfo.Timing.StartTotal();

	static bool s_firstFrame = true;
	if (s_firstFrame) {
	}

	s_firstFrame = false;

	// Enter the critical section for safety while executing the deferred command
	// list
	Engine::GAPI->EnterResourceCriticalSection();
	ID3D11CommandList* dc_cl = nullptr;

	GetDeferredMediaContext()->FinishCommandList(true, &dc_cl);

	// Copy list of textures we are operating on
	Engine::GAPI->MoveLoadedTexturesToProcessedList();
	Engine::GAPI->ClearFrameLoadedTextures();

	Engine::GAPI->LeaveResourceCriticalSection();
	if (dc_cl) {
		// LogInfo() << "Executing command list";
		GetContext()->ExecuteCommandList(dc_cl, true);
		dc_cl->Release();
	}

	// Check for editorpanel
	if (!UIView) {
		if (Engine::GAPI->GetRendererState()->RendererSettings.EnableEditorPanel) {
			CreateMainUIView();
		}
	}

	// Check for shadowmap resize
	int s = Engine::GAPI->GetRendererState()->RendererSettings.ShadowMapSize;
	switch (s) {
		case 0:
		case 1: s = 512; break;
		case 2: s = 1024; break;
		case 3: s = 2048; break;
		case 4: s = 4096; break;
		case 5: s = 8192; break;
	}

	if (WorldShadowmap1->GetSizeX() != s) {
		int old = WorldShadowmap1->GetSizeX();
		LogInfo() << "Shadowmapresolution changed to: " << s << "x" << s;
		WorldShadowmap1 = std::make_unique<RenderToDepthStencilBuffer>(
			GetDevice(), s, s, DXGI_FORMAT_R32_TYPELESS, nullptr, DXGI_FORMAT_D32_FLOAT,
			DXGI_FORMAT_R32_FLOAT);

		Engine::GAPI->GetRendererState()->RendererSettings.WorldShadowRangeScale *= old / static_cast<float>(s);
	}

	// Force the mode
	zCView::SetMode(
		static_cast<int>(Resolution.x / Engine::GAPI->GetRendererState()->RendererSettings.GothicUIScale),
		static_cast<int>(Resolution.y / Engine::GAPI->GetRendererState()->RendererSettings.GothicUIScale),
		32);

	// Notify the shader manager
	ShaderManager->OnFrameStart();

	// Enable blending, in case gothic want's to render its save-screen
	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.BlendEnabled = true;
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();
	UpdateRenderStates();

	// Bind GBuffers
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(), DepthStencilBuffer->GetDepthStencilView());

	SetActivePixelShader("PS_Simple");
	SetActiveVertexShader("VS_Ex");

	PS_DiffuseNormalmappedFxMap				= ShaderManager->GetPShader("PS_DiffuseNormalmappedFxMap");
	PS_DiffuseNormalmappedAlphatestFxMap	= ShaderManager->GetPShader("PS_DiffuseNormalmappedAlphatestFxMap");
	PS_DiffuseNormalmapped					= ShaderManager->GetPShader("PS_DiffuseNormalmapped");
	PS_Diffuse								= ShaderManager->GetPShader("PS_Diffuse");
	PS_DiffuseNormalmappedAlphatest			= ShaderManager->GetPShader("PS_DiffuseNormalmappedAlphaTest");
	PS_DiffuseAlphatest						= ShaderManager->GetPShader("PS_DiffuseAlphaTest");
	PS_Simple								= ShaderManager->GetPShader("PS_Simple");
	GS_Billboard							= ShaderManager->GetGShader("GS_Billboard");
	PS_LinDepth								= ShaderManager->GetPShader("PS_LinDepth");
	return XR_SUCCESS;
}

/** Called when the game ended it's frame */
XRESULT D3D11GraphicsEngine::OnEndFrame() {
	Present();

	// At least Present should have flushed the pipeline, so these textures should
	// be ready by now
	Engine::GAPI->SetFrameProcessedTexturesReady();

	Engine::GAPI->GetRendererState()->RendererInfo.Timing.StopTotal();
	m_FrameLimiter->Wait();
	return XR_SUCCESS;
}

/** Called when the game wants to clear the bound rendertarget */
XRESULT D3D11GraphicsEngine::Clear(const float4& color) {
	GetContext()->ClearDepthStencilView(DepthStencilBuffer->GetDepthStencilView(),
		D3D11_CLEAR_DEPTH, 1.0f, 0);

	GetContext()->ClearRenderTargetView(GBuffer0_Diffuse->GetRenderTargetView(),
		color.toPtr());
	GetContext()->ClearRenderTargetView(
		GBuffer1_Normals_SpecIntens_SpecPower->GetRenderTargetView(),
		float4(0, 0, 0, 0).toPtr());
	GetContext()->ClearRenderTargetView(HDRBackBuffer->GetRenderTargetView(),
		float4(0, 0, 0, 0).toPtr());

	return XR_SUCCESS;
}

/** Creates a vertexbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngine::CreateVertexBuffer(D3D11VertexBuffer** outBuffer) {
	*outBuffer = new D3D11VertexBuffer;
	return XR_SUCCESS;
}

/** Creates a texture object (Not registered inside) */
XRESULT D3D11GraphicsEngine::CreateTexture(D3D11Texture** outTexture) {
	*outTexture = new D3D11Texture;
	return XR_SUCCESS;
}

/** Creates a constantbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngine::CreateConstantBuffer(D3D11ConstantBuffer** outCB,
	void* data, int size) {
	*outCB = new D3D11ConstantBuffer(size, data);
	return XR_SUCCESS;
}

/** Returns a list of available display modes */
XRESULT
D3D11GraphicsEngine::GetDisplayModeList(std::vector<DisplayModeInfo>* modeList,
	bool includeSuperSampling) {
	UINT numModes = 0;
	std::unique_ptr<DXGI_MODE_DESC[]> displayModes = nullptr;
	const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
	IDXGIOutput* output;

	// Get desktop rect
	RECT desktop;
	GetClientRect(GetDesktopWindow(), &desktop);

	if (!DXGIAdapter) return XR_FAILED;

	DXGIAdapter->EnumOutputs(0, &output);

	if (!output) return XR_FAILED;

	HRESULT hr = output->GetDisplayModeList(format, 0, &numModes, nullptr);

	displayModes = std::make_unique<DXGI_MODE_DESC[]>(numModes);

	// Get the list
	hr = output->GetDisplayModeList(format, 0, &numModes, displayModes.get());
	for (unsigned int i = 0; i < numModes; i++) {
		if (displayModes[i].Format != format) continue;

		DisplayModeInfo info;
		info.Height = displayModes[i].Height;
		info.Width = displayModes[i].Width;
		info.Bpp = 32;
		// TODO: find out what is causing issues, this has issues with exclusive fullscreen
		//if (info.Width > (unsigned long)desktop.right ||
		//	info.Height > (unsigned long)desktop.bottom)
		//	continue;  // Skip bigger sizes than the desktop rect, because DXGI
					   // doesn't like them apparently
					   // TODO: Fix this if possible!

		if (!modeList->empty() &&
			memcmp(&modeList->back(), &info, sizeof(info)) == 0)
			continue;  // Already have that in list

		modeList->push_back(info);
	}
	if (includeSuperSampling) {
		// Put supersampling resolutions in, up to just below 8k
		int i = 2;
		DisplayModeInfo ssBase = modeList->back();
		while (ssBase.Width * i < 8192 && ssBase.Height * i < 8192) {
			DisplayModeInfo info;
			info.Height = ssBase.Height * i;
			info.Width = ssBase.Width * i;
			info.Bpp = 32;

			modeList->push_back(info);
			i++;
		}
	}

	output->Release();

	return XR_SUCCESS;
}

/** Presents the current frame to the screen */
XRESULT D3D11GraphicsEngine::Present() {
	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 0.0f;
	vp.Width = static_cast<float>(GetBackbufferResolution().x);
	vp.Height = static_cast<float>(GetBackbufferResolution().y);

	GetContext()->RSSetViewports(1, &vp);

	// Copy HDR scene to backbuffer
	SetDefaultStates();

	if (Engine::GAPI->GetRendererState()->RendererSettings.EnableCustomFontRendering && !textToDraw.empty())
	{
		auto r = RECT{};
		r.top = 0;
		r.left = 0;
		r.bottom = Resolution.y;
		r.right = Resolution.x;

		//m_spriteBatch->Begin();
		m_spriteBatch->Begin(SpriteSortMode_Deferred, states->NonPremultiplied());
		wchar_t buf[255];
		static XMVECTOR gothicColor = XMVectorSet(203.f / 255.f, 186.f / 255.f, 158.f / 255.f, 1);

		zTRnd_AlphaBlendFunc lastBlendState = zRND_ALPHA_FUNC_NONE;

		for (auto const& txt : textToDraw) {
			// TODO: Fix Alpha Blending for SpriteFont.
			/*if (lastBlendState != zRND_ALPHA_FUNC_BLEND && txt.blendState == zRND_ALPHA_FUNC_BLEND)
			{
				lastBlendState = txt.blendState;
				m_spriteBatch->End();

				Engine::GAPI->GetRendererState()->BlendState.SetAlphaBlending();
				Engine::GAPI->GetRendererState()->BlendState.SetDirty();
				UpdateRenderStates();
				m_spriteBatch->Begin(SpriteSortMode_Deferred, states->NonPremultiplied());
			}*/

			auto wSize = MultiByteToWideChar(CP_ACP, 0, txt.str.c_str(), txt.str.length(), buf, 255);
			// TODO: What are we gonna do with too long texts?
			if (!wSize) {
				continue;
			}

			auto color = XMLoadFloat4(txt.color.toXMFLOAT4());

			std::wstring output = std::wstring(buf, wSize);
			auto fontPos = XMVectorSet(txt.x, txt.y, 0, 0);

			// Drop-Shadow
			//m_font->DrawString(m_spriteBatch.get(), output.c_str(), fontPos + XMVectorSet(1.f, 1.f, 0, 0), Colors::Black, 0.f);
			//m_font->DrawString(m_spriteBatch.get(), output.c_str(), fontPos + XMVectorSet(-1.f, 1.f, 0, 0), Colors::Black, 0.f);

			m_font->DrawString(m_spriteBatch.get(), output.c_str(), fontPos, color, 0.f);
		}

		m_spriteBatch->Draw(HDRBackBuffer->GetShaderResView(), r);

		m_spriteBatch->End();

		textToDraw.clear();
		SetDefaultStates();
	}

	SetActivePixelShader("PS_PFX_GammaCorrectInv");

	ActivePS->Apply();

	GammaCorrectConstantBuffer gcb = {};
	gcb.G_Gamma = Engine::GAPI->GetGammaValue();
	gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();
	gcb.G_TextureSize = GetResolution();
	gcb.G_SharpenStrength =
		Engine::GAPI->GetRendererState()->RendererSettings.SharpenFactor;

	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(&gcb);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	PfxRenderer->CopyTextureToRTV(HDRBackBuffer->GetShaderResView(),
		BackbufferRTV.Get(), INT2(0, 0), true);

	// GetContext()->ClearState();

	GetContext()->OMSetRenderTargets(1, BackbufferRTV.GetAddressOf(), nullptr);

	// Check for movie-frame
	if (Engine::GAPI->GetPendingMovieFrame()) {
		Engine::GAPI->GetPendingMovieFrame()->BindToPixelShader(0);
		DrawQuad(INT2(0, 0), GetBackbufferResolution());

		Engine::GAPI->SetPendingMovieFrame(nullptr);
	}

	SetDefaultStates();

	Engine::AntTweakBar->Draw();
	SetDefaultStates();
	
	if (UIView) UIView->Render(Engine::GAPI->GetFrameTimeSec());

	bool vsync = Engine::GAPI->GetRendererState()->RendererSettings.EnableVSync;

	Engine::GAPI->EnterResourceCriticalSection();
	if (SwapChain->Present(vsync ? 1 : 0, 0) == DXGI_ERROR_DEVICE_REMOVED) {
		switch (GetDevice()->GetDeviceRemovedReason()) {
			case DXGI_ERROR_DEVICE_HUNG:
				LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_HUNG)";
				exit(0);
				break;

			case DXGI_ERROR_DEVICE_REMOVED:
				LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_REMOVED)";
				exit(0);
				break;

			case DXGI_ERROR_DEVICE_RESET:
				LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_RESET)";
				exit(0);
				break;

			case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
				LogErrorBox() << "Device Removed! (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
				exit(0);
				break;

			case DXGI_ERROR_INVALID_CALL:
				LogErrorBox() << "Device Removed! (DXGI_ERROR_INVALID_CALL)";
				exit(0);
				break;

			case S_OK:
				LogInfo() << "Device removed, but we're fine!";
				break;

			default:
				LogWarnBox() << "Device Removed! (Unknown reason)";
		}
	}
	Engine::GAPI->LeaveResourceCriticalSection();

	PresentPending = false;

	return XR_SUCCESS;
}

/** Called to set the current viewport */
XRESULT D3D11GraphicsEngine::SetViewport(const ViewportInfo& viewportInfo) {
	// Set the viewport
	D3D11_VIEWPORT viewport;
	ZeroMemory(&viewport, sizeof(D3D11_VIEWPORT));

	viewport.TopLeftX = static_cast<float>(viewportInfo.TopLeftX);
	viewport.TopLeftY = static_cast<float>(viewportInfo.TopLeftY);
	viewport.Width = static_cast<float>(viewportInfo.Width);
	viewport.Height = static_cast<float>(viewportInfo.Height);
	viewport.MinDepth = viewportInfo.MinZ;
	viewport.MaxDepth = viewportInfo.MaxZ;

	GetContext()->RSSetViewports(1, &viewport);

	return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed (World)*/
XRESULT D3D11GraphicsEngine::DrawVertexBuffer(D3D11VertexBuffer* vb,
	unsigned int numVertices,
	unsigned int stride) {
#ifdef RECORD_LAST_DRAWCALL
	g_LastDrawCall.Type = DrawcallInfo::VB;
	g_LastDrawCall.NumElements = numVertices;
	g_LastDrawCall.BaseVertexLocation = 0;
	g_LastDrawCall.BaseIndexLocation = 0;
	if (!g_LastDrawCall.Check()) return XR_SUCCESS;
#endif

	UINT offset = 0;
	UINT uStride = stride;
	ID3D11Buffer* buffer = vb->GetVertexBuffer();
	GetContext()->IASetVertexBuffers(0, 1, &buffer, &uStride, &offset);

	// Draw the mesh
	GetContext()->Draw(numVertices, 0);

	Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnTriangles +=
		numVertices;

	return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed (VOBs)*/
XRESULT D3D11GraphicsEngine::DrawVertexBufferIndexed(D3D11VertexBuffer* vb,
	D3D11VertexBuffer* ib,
	unsigned int numIndices,
	unsigned int indexOffset) {
#ifdef RECORD_LAST_DRAWCALL
	g_LastDrawCall.Type               = DrawcallInfo::VB_IX;
	g_LastDrawCall.NumElements        = numIndices;
	g_LastDrawCall.BaseVertexLocation = 0;
	g_LastDrawCall.BaseIndexLocation  = indexOffset;
	if (!g_LastDrawCall.Check()) return XR_SUCCESS;
#endif

	if (vb) {
		UINT offset = 0;
		UINT uStride = sizeof(ExVertexStruct);
		ID3D11Buffer* buffer = vb->GetVertexBuffer();
		GetContext()->IASetVertexBuffers(0, 1, &buffer, &uStride, &offset);

		if (sizeof(VERTEX_INDEX) == sizeof(unsigned short)) {
			GetContext()->IASetIndexBuffer(ib->GetVertexBuffer(),
				DXGI_FORMAT_R16_UINT, 0);
		}
		else {
			GetContext()->IASetIndexBuffer(ib->GetVertexBuffer(),
				DXGI_FORMAT_R32_UINT, 0);
		}
	}

	if (numIndices) {
		// Draw the mesh
		GetContext()->DrawIndexed(numIndices, indexOffset, 0);

		Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnTriangles +=
			numIndices / 3;
	}
	return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferIndexedUINT(
	D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
	unsigned int indexOffset) {
#ifdef RECORD_LAST_DRAWCALL
	g_LastDrawCall.Type               = DrawcallInfo::VB_IX_UINT;
	g_LastDrawCall.NumElements        = numIndices;
	g_LastDrawCall.BaseVertexLocation = 0;
	g_LastDrawCall.BaseIndexLocation  = indexOffset;
	if (!g_LastDrawCall.Check()) return XR_SUCCESS;
#endif

	if (vb) {
		UINT offset          = 0;
		UINT uStride         = sizeof(ExVertexStruct);
		ID3D11Buffer* buffer = ((D3D11VertexBuffer*)vb)->GetVertexBuffer();
		GetContext()->IASetVertexBuffers(0, 1, &buffer, &uStride, &offset);
		GetContext()->IASetIndexBuffer(((D3D11VertexBuffer*)ib)->GetVertexBuffer(),
			DXGI_FORMAT_R32_UINT, 0);
	}

	if (numIndices) {
		// Draw the mesh
		GetContext()->DrawIndexed(numIndices, indexOffset, 0);

		Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnTriangles +=
			numIndices / 3;
	}

	return XR_SUCCESS;
}

/** Binds viewport information to the given constantbuffer slot */
XRESULT D3D11GraphicsEngine::BindViewportInformation(const std::string& shader,
	int slot) {
	D3D11_VIEWPORT vp;
	UINT num = 1;
	GetContext()->RSGetViewports(&num, &vp);

	// Update viewport information
	float scale =
		Engine::GAPI->GetRendererState()->RendererSettings.GothicUIScale;
	Temp2Float2[0].x = vp.TopLeftX / scale;
	Temp2Float2[0].y = vp.TopLeftY / scale;
	Temp2Float2[1].x = vp.Width / scale;
	Temp2Float2[1].y = vp.Height / scale;

	D3D11PShader* ps = ShaderManager->GetPShader(shader);
	D3D11VShader* vs = ShaderManager->GetVShader(shader);

	if (vs) {
		vs->GetConstantBuffer()[slot]->UpdateBuffer(Temp2Float2);
		vs->GetConstantBuffer()[slot]->BindToVertexShader(slot);
	}

	if (ps) {
		ps->GetConstantBuffer()[slot]->UpdateBuffer(Temp2Float2);
		ps->GetConstantBuffer()[slot]->BindToVertexShader(slot);
	}

	return XR_SUCCESS;
}

int D3D11GraphicsEngine::MeasureString(const std::string& str, int font)
{
	if (font == 0)
	{
		XMVECTOR measure = m_font->MeasureString(str.c_str());
		float width; XMStoreFloat(&width, measure);
		return static_cast<int>(width);
	}
	return 0;
}

/** Draws a vertexarray, non-indexed (HUD, 2D)*/
XRESULT D3D11GraphicsEngine::DrawVertexArray(ExVertexStruct* vertices,
	unsigned int numVertices,
	unsigned int startVertex,
	unsigned int stride) {
	UpdateRenderStates();
	D3D11VShader* vShader = ActiveVS;
	// ShaderManager->GetVShader("VS_TransformedEx");

	// Bind the FF-Info to the first PS slot
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	SetupVS_ExMeshDrawCall();

	D3D11_BUFFER_DESC desc;
	TempVertexBuffer->GetVertexBuffer()->GetDesc(&desc);

	if (desc.ByteWidth < stride * numVertices) {
		if (Engine::GAPI->GetRendererState()->RendererSettings.EnableDebugLog)
			LogInfo() << "TempVertexBuffer too small (" << desc.ByteWidth << "), need "
			<< stride * numVertices << " bytes. Recreating buffer.";

		// Buffer too small, recreate it
		TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();

		TempVertexBuffer->Init(
			nullptr, stride * numVertices, D3D11VertexBuffer::B_VERTEXBUFFER,
			D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE);
	}

	TempVertexBuffer->UpdateBuffer(vertices, stride * numVertices);

	UINT offset          = 0;
	UINT uStride         = stride;
	ID3D11Buffer* buffer = TempVertexBuffer->GetVertexBuffer();
	GetContext()->IASetVertexBuffers(0, 1, &buffer, &uStride, &offset);

	// Draw the mesh
	GetContext()->Draw(numVertices, startVertex);

	Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnTriangles +=
		numVertices;

	return XR_SUCCESS;
}

/** Draws a vertexarray, indexed */
XRESULT D3D11GraphicsEngine::DrawIndexedVertexArray(ExVertexStruct* vertices,
	unsigned int numVertices,
	D3D11VertexBuffer* ib,
	unsigned int numIndices,
	unsigned int stride) {
	UpdateRenderStates();
	D3D11VShader* vShader =
		ActiveVS;  // ShaderManager->GetVShader("VS_TransformedEx");

	// Bind the FF-Info to the first PS slot

	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	SetupVS_ExMeshDrawCall();

	D3D11_BUFFER_DESC desc;
	TempVertexBuffer->GetVertexBuffer()->GetDesc(&desc);

	if (desc.ByteWidth < stride * numVertices) {
		if (Engine::GAPI->GetRendererState()->RendererSettings.EnableDebugLog)
			LogInfo() << "TempVertexBuffer too small (" << desc.ByteWidth << "), need "
			<< stride * numVertices << " bytes. Recreating buffer.";

		// Buffer too small, recreate it
		TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();

		TempVertexBuffer->Init(
			nullptr, stride * numVertices, D3D11VertexBuffer::B_VERTEXBUFFER,
			D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE);
	}

	TempVertexBuffer->UpdateBuffer(vertices, stride * numVertices);

	UINT offset = 0;
	UINT uStride = stride;
	ID3D11Buffer* buffers[] = {
		TempVertexBuffer->GetVertexBuffer(),
		ib->GetVertexBuffer()
	};
	GetContext()->IASetVertexBuffers(0, 2, buffers, &uStride, &offset);

	// Draw the mesh
	GetContext()->DrawIndexed(numIndices, 0, 0);

	Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnTriangles +=
		numVertices;

	return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
XRESULT D3D11GraphicsEngine::DrawVertexBufferFF(D3D11VertexBuffer* vb,
	unsigned int numVertices,
	unsigned int startVertex,
	unsigned int stride) {
	SetupVS_ExMeshDrawCall();

	// Bind the FF-Info to the first PS slot
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	UINT offset = 0;
	UINT uStride = stride;
	ID3D11Buffer* buffer = vb->GetVertexBuffer();
	GetContext()->IASetVertexBuffers(0, 1, &buffer, &uStride, &offset);

	// Draw the mesh
	GetContext()->Draw(numVertices, startVertex);

	Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnTriangles +=
		numVertices;

	return XR_SUCCESS;
}

/** Draws a skeletal mesh */
XRESULT  D3D11GraphicsEngine::DrawSkeletalMesh(
	D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
	const std::vector<XMFLOAT4X4>& transforms, float fatness) {
	GetContext()->RSSetState(WorldRasterizerState.Get());
	GetContext()->OMSetDepthStencilState(DefaultDepthStencilState.Get(), 0);

	if (GetRenderingStage() == DES_SHADOWMAP_CUBE) {
		SetActiveVertexShader("VS_ExSkeletalCube");
	}
	else {
		SetActiveVertexShader("VS_ExSkeletal");
	}
	SetActivePixelShader("PS_AtmosphereGround");
	const D3D11PShader* nrmPS = ActivePS;
	SetActivePixelShader("PS_World");
	const D3D11PShader* defaultPS = ActivePS;

	InfiniteRangeConstantBuffer->BindToPixelShader(3);

	const auto& world = Engine::GAPI->GetRendererState()->TransformState.TransformWorld;

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Get currently bound texture name
	zCTexture* tex = Engine::GAPI->GetBoundTexture(0);

	const bool tesselationEnabled = Engine::GAPI->GetRendererState()->RendererSettings.EnableTesselation;

	if (tex) {
		MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(tex);
		if (!info->Constantbuffer)
			info->UpdateConstantbuffer();
		// TODO: Slow, save this somewhere!

		// TODO: TODO: Currently bodies and faces look really glossy.
		//		  This is only a temporary fix!
		if (info->buffer.SpecularIntensity != 0.05f) {
			info->buffer.SpecularIntensity = 0.05f;
			info->UpdateConstantbuffer();
		}

		info->Constantbuffer->BindToPixelShader(2);

		// Bind a default normalmap in case the scene is wet and we currently have
		// none
		if (!tex->GetSurface()->GetNormalmap()) {
			// Modify the strength of that default normalmap for the material info
			if (info->buffer.NormalmapStrength /* * Engine::GAPI->GetSceneWetness()*/
				!= DEFAULT_NORMALMAP_STRENGTH) {
				info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
				info->UpdateConstantbuffer();
			}

			DistortionTexture->BindToPixelShader(1);
		}

		// Select shader
		BindShaderForTexture(tex);

		if (RenderingStage == DES_MAIN) {
			if (ActiveHDS) {
				GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				GetContext()->DSSetShader(nullptr, nullptr, 0);
				GetContext()->HSSetShader(nullptr, nullptr, 0);
				ActiveHDS = nullptr;
			}
		}
	}

	VS_ExConstantBuffer_PerInstanceSkeletal cb2 = {};
	cb2.World = world;
	cb2.PI_ModelFatness = fatness;

	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&cb2);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	// Copy bones
	memcpy(TempBonesMatrix, &transforms[0], sizeof(XMFLOAT4X4) * std::min(transforms.size(), sizeof(TempBonesMatrix) / sizeof(TempBonesMatrix[0])));
	ActiveVS->GetConstantBuffer()[2]->UpdateBuffer(TempBonesMatrix);
	ActiveVS->GetConstantBuffer()[2]->BindToVertexShader(2);

	if (transforms.size() >=
		sizeof(TempBonesMatrix) / sizeof(TempBonesMatrix[0])) {
		LogWarn() << "SkeletalMesh has more than "
			<< sizeof(TempBonesMatrix) / sizeof(TempBonesMatrix[0])
			<< " bones! (" << transforms.size() << ")Up this limit!";
	}

	ActiveVS->Apply();
	ActivePS->Apply();

	UINT offset = 0;
	UINT uStride = sizeof(ExSkelVertexStruct);
	ID3D11Buffer* buffer = vb->GetVertexBuffer();
	GetContext()->IASetVertexBuffers(0, 1, &buffer, &uStride, &offset);

	if (sizeof(VERTEX_INDEX) == sizeof(unsigned short)) {
		GetContext()->IASetIndexBuffer(ib->GetVertexBuffer(),
			DXGI_FORMAT_R16_UINT, 0);
	}
	else {
		GetContext()->IASetIndexBuffer(ib->GetVertexBuffer(),
			DXGI_FORMAT_R32_UINT, 0);
	}

	if (RenderingStage == DES_SHADOWMAP) {
		// Unbind PixelShader in this case
		GetContext()->PSSetShader(nullptr, nullptr, 0);
		ActivePS = nullptr;
	}

	bool linearDepth = (Engine::GAPI->GetRendererState()->GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
	if (linearDepth) {
		ActivePS = PS_LinDepth;
		ActivePS->Apply();
	}

	// Draw the mesh
	GetContext()->DrawIndexed(numIndices, 0, 0);

	if (ActiveHDS) {
		GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		GetContext()->DSSetShader(nullptr, nullptr, 0);
		GetContext()->HSSetShader(nullptr, nullptr, 0);
		ActiveHDS = nullptr;
	}

	return XR_SUCCESS;
}

/** Draws a batch of instanced geometry */
XRESULT D3D11GraphicsEngine::DrawInstanced(
	D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
	void* instanceData, unsigned int instanceDataStride,
	unsigned int numInstances, unsigned int vertexStride) {
	UpdateRenderStates();

	// Check buffersize
	D3D11_BUFFER_DESC desc;
	DynamicInstancingBuffer->GetVertexBuffer()->GetDesc(&desc);

	if (desc.ByteWidth < instanceDataStride * numInstances) {
		if (Engine::GAPI->GetRendererState()->RendererSettings.EnableDebugLog)
			LogInfo() << "Instancing buffer too small (" << desc.ByteWidth << "), need "
			<< instanceDataStride * numInstances
			<< " bytes. Recreating buffer.";

		// Buffer too small, recreate it
		DynamicInstancingBuffer = std::make_unique<D3D11VertexBuffer>();

		// Put in some little extra space (32) so we don't need to recreate this
		// every frame when approaching a field of stones or something.
		DynamicInstancingBuffer->Init(
			nullptr, instanceDataStride * (numInstances + 32),
			D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
			D3D11VertexBuffer::CA_WRITE);
	}

	// Update the vertexbuffer
	DynamicInstancingBuffer->UpdateBuffer(instanceData,
		instanceDataStride * numInstances);

	// Bind shader and pipeline flags
	D3D11VShader* vShader = ShaderManager->GetVShader("VS_ExInstanced");

	auto* world =
		&Engine::GAPI->GetRendererState()->TransformState.TransformWorld;
	auto& view =
		Engine::GAPI->GetRendererState()->TransformState.TransformView;
	auto& proj = Engine::GAPI->GetProjectionMatrixDX();

	VS_ExConstantBuffer_PerFrame cb = {};
	cb.View = view;
	cb.Projection = proj;

	VS_ExConstantBuffer_PerInstance cbb = {};
	cbb.World = *(DirectX::XMFLOAT4X4*)world;

	vShader->GetConstantBuffer()[0]->UpdateBuffer(&cb);
	vShader->GetConstantBuffer()[0]->BindToVertexShader(0);

	vShader->Apply();

	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	UINT offset[] = {0, 0};
	UINT uStride[] = {vertexStride, instanceDataStride};
	ID3D11Buffer* buffers[] = {
		((D3D11VertexBuffer*)vb)->GetVertexBuffer(),
		DynamicInstancingBuffer->GetVertexBuffer()};
	GetContext()->IASetVertexBuffers(0, 2, buffers, uStride, offset);

	if (sizeof(VERTEX_INDEX) == sizeof(unsigned short)) {
		GetContext()->IASetIndexBuffer(((D3D11VertexBuffer*)ib)->GetVertexBuffer(),
			DXGI_FORMAT_R16_UINT, 0);
	}
	else {
		GetContext()->IASetIndexBuffer(((D3D11VertexBuffer*)ib)->GetVertexBuffer(),
			DXGI_FORMAT_R32_UINT, 0);
	}

	// Draw the batch
	GetContext()->DrawIndexedInstanced(numIndices, numInstances, 0, 0, 0);

	return XR_SUCCESS;
}

/** Draws a batch of instanced geometry */
XRESULT D3D11GraphicsEngine::DrawInstanced(
	D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
	D3D11VertexBuffer* instanceData, unsigned int instanceDataStride,
	unsigned int numInstances, unsigned int vertexStride,
	unsigned int startInstanceNum, unsigned int indexOffset) {
	// Bind shader and pipeline flags
	UINT offset[] = {0, 0};
	UINT uStride[] = {vertexStride, instanceDataStride};
	ID3D11Buffer* buffers[] = {
		((D3D11VertexBuffer*)vb)->GetVertexBuffer(),
		((D3D11VertexBuffer*)instanceData)->GetVertexBuffer()};
	GetContext()->IASetVertexBuffers(0, 2, buffers, uStride, offset);

	if (sizeof(VERTEX_INDEX) == sizeof(unsigned short)) {
		GetContext()->IASetIndexBuffer(((D3D11VertexBuffer*)ib)->GetVertexBuffer(),
			DXGI_FORMAT_R16_UINT, 0);
	}
	else {
		GetContext()->IASetIndexBuffer(((D3D11VertexBuffer*)ib)->GetVertexBuffer(),
			DXGI_FORMAT_R32_UINT, 0);
	}

	unsigned int max =
		Engine::GAPI->GetRendererState()->RendererSettings.MaxNumFaces * 3;
	numIndices = max != 0 ? (numIndices < max ? numIndices : max) : numIndices;

	// Draw the batch
	GetContext()->DrawIndexedInstanced(numIndices, numInstances, indexOffset, 0,
		startInstanceNum);

	Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnVobs++;

	return XR_SUCCESS;
}

/** Sets the active pixel shader object */
XRESULT D3D11GraphicsEngine::SetActivePixelShader(const std::string& shader) {
	ActivePS = ShaderManager->GetPShader(shader);

	return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::SetActiveVertexShader(const std::string& shader) {
	ActiveVS = ShaderManager->GetVShader(shader);

	return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::SetActiveHDShader(const std::string& shader) {
	ActiveHDS = ShaderManager->GetHDShader(shader);

	return XR_SUCCESS;
}

/** Binds the active PixelShader */
XRESULT D3D11GraphicsEngine::BindActivePixelShader() {
	if (ActiveVS) ActiveVS->Apply();
	return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::BindActiveVertexShader() {
	if (ActivePS) ActivePS->Apply();
	return XR_SUCCESS;
}

/** Unbinds the texture at the given slot */
XRESULT D3D11GraphicsEngine::UnbindTexture(int slot) {
	ID3D11ShaderResourceView* srv = nullptr;
	GetContext()->PSSetShaderResources(slot, 1, &srv);
	GetContext()->VSSetShaderResources(slot, 1, &srv);

	return XR_SUCCESS;
}

/** Recreates the renderstates */
XRESULT D3D11GraphicsEngine::UpdateRenderStates() {
	if (Engine::GAPI->GetRendererState()->BlendState.StateDirty &&
		Engine::GAPI->GetRendererState()->BlendState.Hash != FFBlendStateHash) {
		D3D11BlendStateInfo* state = (D3D11BlendStateInfo*)
			GothicStateCache::s_BlendStateMap[Engine::GAPI->GetRendererState()
			->BlendState];

		if (!state) {
			// Create new state
			state =
				new D3D11BlendStateInfo(Engine::GAPI->GetRendererState()->BlendState);

			GothicStateCache::s_BlendStateMap[Engine::GAPI->GetRendererState()
				->BlendState] = state;
		}

		FFBlendState = state->State;
		FFRasterizerStateHash = Engine::GAPI->GetRendererState()->BlendState.Hash;

		Engine::GAPI->GetRendererState()->BlendState.StateDirty = false;
		GetContext()->OMSetBlendState(FFBlendState.Get(), float4(0, 0, 0, 0).toPtr(),
			0xFFFFFFFF);
	}

	if (Engine::GAPI->GetRendererState()->RasterizerState.StateDirty &&
		Engine::GAPI->GetRendererState()->RasterizerState.Hash !=
		FFRasterizerStateHash) {
		D3D11RasterizerStateInfo* state = (D3D11RasterizerStateInfo*)
			GothicStateCache::s_RasterizerStateMap[Engine::GAPI->GetRendererState()
			->RasterizerState];

		if (!state) {
			// Create new state
			state = new D3D11RasterizerStateInfo(
				Engine::GAPI->GetRendererState()->RasterizerState);

			GothicStateCache::s_RasterizerStateMap[Engine::GAPI->GetRendererState()
				->RasterizerState] = state;
		}

		FFRasterizerState = state->State;
		FFRasterizerStateHash = Engine::GAPI->GetRendererState()->DepthState.Hash;

		Engine::GAPI->GetRendererState()->RasterizerState.StateDirty = false;
		GetContext()->RSSetState(FFRasterizerState.Get());
	}

	if (Engine::GAPI->GetRendererState()->DepthState.StateDirty &&
		Engine::GAPI->GetRendererState()->DepthState.Hash !=
		FFDepthStencilStateHash) {
		D3D11DepthBufferState* state = (D3D11DepthBufferState*)
			GothicStateCache::s_DepthBufferMap[Engine::GAPI->GetRendererState()
			->DepthState];

		if (!state) {
			// Create new state
			state = new D3D11DepthBufferState(
				Engine::GAPI->GetRendererState()->DepthState);

			GothicStateCache::s_DepthBufferMap[Engine::GAPI->GetRendererState()
				->DepthState] = state;
		}

		FFDepthStencilState = state->State;
		FFDepthStencilStateHash = Engine::GAPI->GetRendererState()->DepthState.Hash;

		Engine::GAPI->GetRendererState()->DepthState.StateDirty = false;
		GetContext()->OMSetDepthStencilState(FFDepthStencilState.Get(), 0);
	}

	return XR_SUCCESS;
}

/** Called when we started to render the world */
XRESULT D3D11GraphicsEngine::OnStartWorldRendering() {
	SetDefaultStates();

	if (Engine::GAPI->GetRendererState()->RendererSettings.DisableRendering)
		return XR_SUCCESS;

	// return XR_SUCCESS;
	if (PresentPending) return XR_SUCCESS;

	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	vp.Width = static_cast<float>(GetResolution().x);
	vp.Height = static_cast<float>(GetResolution().y);

	GetContext()->RSSetViewports(1, &vp);

	ID3D11RenderTargetView* rtvs[] = {
		GBuffer0_Diffuse->GetRenderTargetView(),
		GBuffer1_Normals_SpecIntens_SpecPower->GetRenderTargetView()};
	GetContext()->OMSetRenderTargets(2, rtvs,
		DepthStencilBuffer->GetDepthStencilView());

	Engine::GAPI->SetFarPlane(
		Engine::GAPI->GetRendererState()->RendererSettings.SectionDrawRadius *
		WORLD_SECTION_SIZE);

	Clear(float4(Engine::GAPI->GetRendererState()->GraphicsState.FF_FogColor,
		0.0f));

	// Clear textures from the last frame
	FrameTextures.clear();
	RenderedVobs.resize(0);  // Keep memory allocated on this
	FrameWaterSurfaces.clear();
	FrameTransparencyMeshes.clear();

	// TODO: TODO: Hack for texture caching!
	zCTextureCacheHack::NumNotCachedTexturesInFrame = 0;

	// Re-Bind the default sampler-state in case it was overwritten
	GetContext()->PSSetSamplers(0, 1, DefaultSamplerState.GetAddressOf());

	// Update view distances
	InfiniteRangeConstantBuffer->UpdateBuffer(float4(FLT_MAX, 0, 0, 0).toPtr());
	OutdoorSmallVobsConstantBuffer->UpdateBuffer(
		float4(Engine::GAPI->GetRendererState()
			->RendererSettings.OutdoorSmallVobDrawRadius,
			0, 0, 0).toPtr());
	OutdoorVobsConstantBuffer->UpdateBuffer(float4(
		Engine::GAPI->GetRendererState()->RendererSettings.OutdoorVobDrawRadius,
		0, 0, 0).toPtr());

	// Update editor
	if (UIView) {
		UIView->Update(Engine::GAPI->GetFrameTimeSec());
	}

	Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
		false;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawSky) {
		// Draw back of the sky if outdoor
		DrawSky();
	}

	// Draw world
	Engine::GAPI->DrawWorldMeshNaive();

	// Draw HBAO
	if (Engine::GAPI->GetRendererState()->RendererSettings.HbaoSettings.Enabled)
		PfxRenderer->DrawHBAO(HDRBackBuffer->GetRenderTargetView());

	SetDefaultStates();

	// PfxRenderer->RenderDistanceBlur();

	SetActivePixelShader("PS_Simple");
	SetActiveVertexShader("VS_Ex");

	// Draw water surfaces of current frame
	DrawWaterSurfaces();

	// Draw light-shafts
	DrawMeshInfoListAlphablended(FrameTransparencyMeshes);

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawFog &&
		Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
		zBSP_MODE_OUTDOOR)
		PfxRenderer->RenderHeightfog();

	// Draw rain
	if (Engine::GAPI->GetRainFXWeight() > 0.0f) Effects->DrawRain();

	SetActivePixelShader("PS_Simple");
	SetActiveVertexShader("VS_Ex");

	SetDefaultStates();

	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());

	// Draw unlit decals 
	// TODO: Only get them once!
	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawParticleEffects) {
		std::vector<zCVob*> decals;
		Engine::GAPI->GetVisibleDecalList(decals);

		// Draw stuff like candle-flames
		DrawDecalList(decals, false);
	}

	// TODO: TODO: GodRays need the GBuffer1 from the scene, but Particles need to
	// clear it!
	if (Engine::GAPI->GetRendererState()->RendererSettings.EnableGodRays)
		PfxRenderer->RenderGodRays();

	// DrawParticleEffects();
	Engine::GAPI->DrawParticlesSimple();

#if (defined BUILD_GOTHIC_2_6_fix || defined BUILD_GOTHIC_1_08k)
	// Calc weapon/effect trail mesh data
	Engine::GAPI->CalcPolyStripMeshes();
	// Draw those
	DrawPolyStrips();
#endif

	// Draw debug lines
	LineRenderer->Flush();

	if (Engine::GAPI->GetRendererState()->RendererSettings.EnableHDR)
		PfxRenderer->RenderHDR();

	if (Engine::GAPI->GetRendererState()->RendererSettings.EnableSMAA)
		PfxRenderer->RenderSMAA();

	// Disable the depth-buffer
	Engine::GAPI->GetRendererState()->DepthState.DepthBufferEnabled = false;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	PresentPending = true;

	// Set viewport for gothics rendering
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	vp.Width = static_cast<float>(GetBackbufferResolution().x);
	vp.Height = static_cast<float>(GetBackbufferResolution().y);

	GetContext()->RSSetViewports(1, &vp);

	// If we currently are underwater, then draw underwater effects
	if (Engine::GAPI->IsUnderWater()) DrawUnderwaterEffects();

	// Clear here to get a working depthbuffer but no interferences with world
	// geometry for gothic UI-Rendering
	GetContext()->ClearDepthStencilView(DepthStencilBuffer->GetDepthStencilView(),
		D3D11_CLEAR_DEPTH, 1.0f, 0.0f);
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		nullptr);

	SetDefaultStates();

	// Save screenshot if wanted
	if (SaveScreenshotNextFrame) {
		SaveScreenshot();
		SaveScreenshotNextFrame = false;
	}

	return XR_SUCCESS;
}

void D3D11GraphicsEngine::SetupVS_ExMeshDrawCall() {
	UpdateRenderStates();

	if (ActiveVS) {
		ActiveVS->Apply();
	}
	if (ActivePS) {
		ActivePS->Apply();
	}

	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void D3D11GraphicsEngine::SetupVS_ExConstantBuffer() {
	auto& world =
		Engine::GAPI->GetRendererState()->TransformState.TransformWorld;
	auto& view =
		Engine::GAPI->GetRendererState()->TransformState.TransformView;
	auto& proj = Engine::GAPI->GetProjectionMatrixDX();

	VS_ExConstantBuffer_PerFrame cb = {};
	cb.View = view;
	cb.Projection = proj;
	XMStoreFloat4x4(&cb.ViewProj, XMMatrixMultiply(XMLoadFloat4x4(&proj), XMLoadFloat4x4(&view)));

	ActiveVS->GetConstantBuffer()[0]->UpdateBuffer(&cb);
	ActiveVS->GetConstantBuffer()[0]->BindToVertexShader(0);
	ActiveVS->GetConstantBuffer()[0]->BindToDomainShader(0);
	ActiveVS->GetConstantBuffer()[0]->BindToHullShader(0);
	ActiveVS->GetConstantBuffer()[0]->BindToGeometryShader(0);
}

void D3D11GraphicsEngine::SetupVS_ExPerInstanceConstantBuffer() {
	auto world = Engine::GAPI->GetRendererState()->TransformState.TransformWorld;

	VS_ExConstantBuffer_PerInstance cb = {};
	cb.World = world;


	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&cb);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);
}

/** Puts the current world matrix into a CB and binds it to the given slot */
void D3D11GraphicsEngine::SetupPerInstanceConstantBuffer(int slot) {
	auto world = Engine::GAPI->GetRendererState()->TransformState.TransformWorld;

	VS_ExConstantBuffer_PerInstance cb = {};
	cb.World = world;

	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&cb);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(slot);
}

bool SectionRenderlistSortCmp(std::pair<float, WorldMeshSectionInfo*>& a,
	std::pair<float, WorldMeshSectionInfo*>& b) {
	return a.first < b.first;
}

/** Test draw world */
void D3D11GraphicsEngine::TestDrawWorldMesh() {
	std::list<WorldMeshSectionInfo*> renderList;

	Engine::GAPI->CollectVisibleSections(renderList);

	DistortionTexture->BindToPixelShader(0);

	DrawVertexBufferIndexedUINT(
		Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
		Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0);

	for (auto const& renderItem : renderList) {
		for (auto const& mesh : renderItem->WorldMeshesByCustomTexture) {
			if (mesh.first) {
				mesh.first->BindToPixelShader(0);
			}

			for (unsigned int i = 0; i < mesh.second.size(); i++) {
				// Draw from wrapped mesh
				DrawVertexBufferIndexedUINT(nullptr, nullptr,
					mesh.second[i]->Indices.size(),
					mesh.second[i]->BaseIndexLocation);
			}
		}

		for (auto const& mesh : renderItem->WorldMeshesByCustomTextureOriginal) {
			if (mesh.first && mesh.first->GetTexture()) {
				if (mesh.first->GetTexture()->CacheIn(0.6f) == zRES_CACHED_IN)
					mesh.first->GetTexture()->Bind(0);
				else
					continue;
			}

			for (unsigned int i = 0; i < mesh.second.size(); i++) {
				// Draw from wrapped mesh
				DrawVertexBufferIndexedUINT(nullptr, nullptr,
					mesh.second[i]->Indices.size(),
					mesh.second[i]->BaseIndexLocation);
			}
		}
	}
}

/** Draws a list of mesh infos */
XRESULT D3D11GraphicsEngine::DrawMeshInfoListAlphablended(
	const std::vector<std::pair<MeshKey, MeshInfo*>>& list) {
	SetDefaultStates();

	// Setup renderstates

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);
	Engine::GAPI->ResetWorldTransform();

	SetActivePixelShader("PS_Diffuse");
	SetActiveVertexShader("VS_Ex");

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	XMFLOAT4X4 id; XMStoreFloat4x4(&id, XMMatrixIdentity());
	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	InfiniteRangeConstantBuffer->BindToPixelShader(3);

	// Bind wrapped mesh vertex buffers
	DrawVertexBufferIndexedUINT(
		Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
		Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0);

	int lastAlphaFunc = 0;

	// Draw the list
	for (auto const& it : list) {
		int indicesNumMod = 1;
		if (it.first.Material->GetAniTexture() != nullptr) {
			MyDirectDrawSurface7* surface =
				it.first.Material->GetAniTexture()->GetSurface();
			ID3D11ShaderResourceView* srv[3];

			// Get diffuse and normalmap
			srv[0] = ((D3D11Texture*)surface->GetEngineTexture())
				->GetShaderResourceView();
			srv[1] = surface->GetNormalmap()
				? ((D3D11Texture*)surface->GetNormalmap())
				->GetShaderResourceView()
				: nullptr;
			srv[2] =
				surface->GetFxMap()
				? ((D3D11Texture*)surface->GetFxMap())->GetShaderResourceView()
				: nullptr;

			// Bind both
			GetContext()->PSSetShaderResources(0, 3, srv);

			// Get the right shader for it

			BindShaderForTexture(it.first.Material->GetAniTexture(), false,
				it.first.Material->GetAlphaFunc());

			// Check for alphablending on world mesh
			if (lastAlphaFunc != it.first.Material->GetAlphaFunc()) {
				if (it.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND)
					Engine::GAPI->GetRendererState()->BlendState.SetAlphaBlending();

				if (it.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD)
					Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();

				Engine::GAPI->GetRendererState()->BlendState.SetDirty();

				Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;
				Engine::GAPI->GetRendererState()->DepthState.SetDirty();

				UpdateRenderStates();
				lastAlphaFunc = it.first.Material->GetAlphaFunc();
			}

			MaterialInfo* info = it.first.Info;
			if (!info->Constantbuffer) info->UpdateConstantbuffer();

			info->Constantbuffer->BindToPixelShader(2);

			// Don't let the game unload the texture after some time
			it.first.Material->GetAniTexture()->CacheIn(0.6f);

			// Draw the section-part
			DrawVertexBufferIndexedUINT(nullptr, nullptr, it.second->Indices.size(),
				it.second->BaseIndexLocation);
		}
	}

	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = true;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();
	Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled = false;
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	UpdateRenderStates();

	// Draw again, but only to depthbuffer this time to make them work with
	// fogging
	for (auto const& it : list) {
		if (it.first.Material->GetAniTexture() != nullptr) {
			// Draw the section-part
			DrawVertexBufferIndexedUINT(nullptr, nullptr, it.second->Indices.size(),
				it.second->BaseIndexLocation);
		}
	}

	SetDefaultStates();

	return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawWorldMesh(bool noTextures) {
	if (!Engine::GAPI->GetRendererState()->RendererSettings.DrawWorldMesh)
		return XR_SUCCESS;

	SetDefaultStates();

	// Setup renderstates
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_BACK;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);
	Engine::GAPI->ResetWorldTransform();

	SetActivePixelShader("PS_Diffuse");
	SetActiveVertexShader("VS_Ex");

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	// Bind reflection-cube to slot 4
	GetContext()->PSSetShaderResources(4, 1, ReflectionCube.GetAddressOf());

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	XMFLOAT4X4 id; XMStoreFloat4x4(&id, XMMatrixIdentity());
	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	InfiniteRangeConstantBuffer->BindToPixelShader(3);

	std::list<WorldMeshSectionInfo*> renderList;
	Engine::GAPI->CollectVisibleSections(renderList);

	MeshInfo* meshInfo = Engine::GAPI->GetWrappedWorldMesh();
	DrawVertexBufferIndexedUINT(meshInfo->MeshVertexBuffer,
		meshInfo->MeshIndexBuffer, 0, 0);

	std::list<std::pair<MeshKey, WorldMeshInfo*>> meshList;

	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	GetContext()->DSSetShader(nullptr, nullptr, 0);
	GetContext()->HSSetShader(nullptr, nullptr, 0);

	int numUncachedTextures = 0;
	for (int i = 0; i < 2; i++) {
		for (auto const& renderItem : renderList) {
			for (auto const& worldMesh : renderItem->WorldMeshes) {
				if (worldMesh.first.Material) {
					zCTexture* aniTex = worldMesh.first.Material->GetTexture();

					if (!aniTex) continue;

					if (i == 1) {  // Second try, this is only true if we have many
								   // unloaded textures
						aniTex->CacheIn(-1);
					}
					else {
						if (aniTex->CacheIn(0.6f) != zRES_CACHED_IN) {
							numUncachedTextures++;
							continue;
						}
					}

					// Check surface type
					if (worldMesh.first.Info->MaterialType == MaterialInfo::MT_Water) {
						FrameWaterSurfaces[aniTex].push_back(worldMesh.second);
						continue;
					}

					// Check if the animated texture and the registered textures are the
					// same
					if (worldMesh.first.Texture != aniTex) {
						MeshKey key = worldMesh.first;
						key.Texture = aniTex;

						// Check for alphablending
						if (worldMesh.first.Material->GetAlphaFunc() >
							zMAT_ALPHA_FUNC_FUNC_NONE &&
							worldMesh.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST) {
							FrameTransparencyMeshes.push_back(worldMesh);
						}
						else {
							// Create a new pair using the animated texture
							meshList.push_back(std::make_pair(key, worldMesh.second));
						}

					}
					else {
						// Check for alphablending
						if (worldMesh.first.Material->GetAlphaFunc() >
							zMAT_ALPHA_FUNC_FUNC_NONE &&
							worldMesh.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST) {
							FrameTransparencyMeshes.push_back(worldMesh);
						}
						else {
							// Push this texture/mesh combination
							meshList.push_back(worldMesh);
						}
					}
				}
			}
		}

		// if (numUncachedTextures < NUM_UNLOADEDTEXCOUNT_FORCE_LOAD_TEXTURES)
		break;

		// If we get here, there are many unloaded textures.
		// Clear the list and try again, with forcing the textures to load
		meshList.clear();
	}

	struct cmpstruct {
		static bool cmp(const std::pair<MeshKey, MeshInfo*>& a,
			const std::pair<MeshKey, MeshInfo*>& b) {
			return a.first.Texture < b.first.Texture;
		}
	};

	// Sort by texture
	meshList.sort(cmpstruct::cmp);

	// Draw depth only
	if (Engine::GAPI->GetRendererState()->RendererSettings.DoZPrepass) {
		INT2 camSection =
			WorldConverter::GetSectionOfPos(Engine::GAPI->GetCameraPosition());
		GetContext()->PSSetShader(nullptr, nullptr, 0);

		for (auto const& mesh : meshList) {
			if (!mesh.first.Material->GetAniTexture()) continue;

			if (mesh.first.Material->GetAniTexture()->HasAlphaChannel())
				continue;  // Don't pre-render stuff with alpha channel

			if (mesh.first.Info->MaterialType == MaterialInfo::MT_Water)
				continue;  // Don't pre-render water

			if (mesh.second->TesselationSettings.buffer.VT_TesselationFactor > 0.0f)
				continue;  // Don't pre-render tesselated surfaces

			DrawVertexBufferIndexedUINT(nullptr, nullptr, mesh.second->Indices.size(),
				mesh.second->BaseIndexLocation);
		}
	}

	SetActivePixelShader("PS_Diffuse");
	ActivePS->Apply();

	bool tesselationEnabled =
		Engine::GAPI->GetRendererState()->RendererSettings.EnableTesselation &&
		Engine::GAPI->GetRendererState()
		->RendererSettings.AllowWorldMeshTesselation;

	// Now draw the actual pixels
	zCTexture* bound = nullptr;
	MaterialInfo* boundInfo = nullptr;
	ID3D11ShaderResourceView* boundNormalmap = nullptr;
	for (auto const& mesh : meshList) {
		int indicesNumMod = 1;
		if (mesh.first.Texture != bound &&
			Engine::GAPI->GetRendererState()->RendererSettings.DrawWorldMesh > 1) {
			MyDirectDrawSurface7* surface = mesh.first.Texture->GetSurface();
			ID3D11ShaderResourceView* srv[3];
			MaterialInfo* info = mesh.first.Info;

			// Get diffuse and normalmap
			srv[0] = ((D3D11Texture*)surface->GetEngineTexture())
				->GetShaderResourceView();
			srv[1] = surface->GetNormalmap()
				? ((D3D11Texture*)surface->GetNormalmap())
				->GetShaderResourceView()
				: nullptr;
			srv[2] =
				surface->GetFxMap()
				? ((D3D11Texture*)surface->GetFxMap())->GetShaderResourceView()
				: nullptr;

			// Bind a default normalmap in case the scene is wet and we currently have
			// none
			if (!srv[1]) {
				// Modify the strength of that default normalmap for the material info
				if (info &&
					info->buffer
					.NormalmapStrength /* * Engine::GAPI->GetSceneWetness()*/
					!= DEFAULT_NORMALMAP_STRENGTH) {
					info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
					info->UpdateConstantbuffer();
				}
				srv[1] = DistortionTexture->GetShaderResourceView();
			}

			boundNormalmap = srv[1];

			// Bind both
			GetContext()->PSSetShaderResources(0, 3, srv);

			// Get the right shader for it

			BindShaderForTexture(mesh.first.Material->GetAniTexture(), false,
				mesh.first.Material->GetAlphaFunc());

			// Check for alphablending on world mesh
			if ((mesh.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND ||
				mesh.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD) &&
				!Engine::GAPI->GetRendererState()->BlendState.BlendEnabled) {
				if (mesh.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND)
					Engine::GAPI->GetRendererState()->BlendState.SetAlphaBlending();

				if (mesh.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD)
					Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();

				Engine::GAPI->GetRendererState()->BlendState.SetDirty();

				Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;
				Engine::GAPI->GetRendererState()->DepthState.SetDirty();

				UpdateRenderStates();
			}
			else if (Engine::GAPI->GetRendererState()->BlendState.BlendEnabled &&
				mesh.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_BLEND) {
				Engine::GAPI->GetRendererState()->BlendState.SetDefault();
				Engine::GAPI->GetRendererState()->BlendState.SetDirty();

				Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = true;
				Engine::GAPI->GetRendererState()->DepthState.SetDirty();

				UpdateRenderStates();
			}

			if (info) {
				if (!info->Constantbuffer) info->UpdateConstantbuffer();

				info->Constantbuffer->BindToPixelShader(2);

				// Don't let the game unload the texture after some timep
				mesh.first.Material->GetAniTexture()->CacheIn(0.6f);
				boundInfo = info;
			}
			bound = mesh.first.Material->GetAniTexture();
			// Bind normalmap to HDS
			if (!mesh.second->IndicesPNAEN.empty()) {
				GetContext()->DSSetShaderResources(0, 1, &boundNormalmap);
				GetContext()->HSSetShaderResources(0, 1, &boundNormalmap);
			}
		}

		// Check for tesselated mesh
		if (tesselationEnabled && !ActiveHDS &&
			boundInfo->TextureTesselationSettings.buffer.VT_TesselationFactor >
			0.0f) {
			// Set normal/displacement map
			GetContext()->DSSetShaderResources(0, 1, &boundNormalmap);
			GetContext()->HSSetShaderResources(0, 1, &boundNormalmap);
			Setup_PNAEN(PNAEN_Default);
		}

		// Bind infos for this mesh
		if (boundInfo &&
			boundInfo->TextureTesselationSettings.buffer.VT_TesselationFactor >
			0.0f &&
			!mesh.second->IndicesPNAEN.empty() &&
			mesh.first.Material->GetAlphaFunc() <= zMAT_ALPHA_FUNC_FUNC_NONE &&
			!bound->HasAlphaChannel())  // Only allow tesselation for materials
										// without alphablending
		{
			boundInfo->TextureTesselationSettings.Constantbuffer->BindToDomainShader(
				1);
			boundInfo->TextureTesselationSettings.Constantbuffer->BindToHullShader(1);
		}
		else if (ActiveHDS)  // Unbind tesselation-shaders if the mesh doesn't
							// support it
		{
			GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			GetContext()->DSSetShader(nullptr, nullptr, 0);
			GetContext()->HSSetShader(nullptr, nullptr, 0);
			ActiveHDS = nullptr;
			SetActiveVertexShader("VS_Ex");
			ActiveVS->Apply();

			// Bind wrapped mesh again
			DrawVertexBufferIndexedUINT(
				Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
				Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0);
		}

		if (Engine::GAPI->GetRendererState()->RendererSettings.DrawWorldMesh > 2) {
			if (!ActiveHDS) {
				DrawVertexBufferIndexed(mesh.second->MeshVertexBuffer,
					mesh.second->MeshIndexBuffer,
					mesh.second->Indices.size());
			}
			else {
				// Draw from mesh info
				DrawVertexBufferIndexed(mesh.second->MeshVertexBuffer,
					mesh.second->MeshIndexBufferPNAEN,
					mesh.second->IndicesPNAEN.size());
			}
		}
	}

	SetDefaultStates();

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_FRONT;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = true;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	// TODO: TODO: Remove DrawWorldMeshNaive finally and put this into a proper
	// location!
	UpdateOcclusion();

	return XR_SUCCESS;
}

/** Draws the world mesh */
XRESULT D3D11GraphicsEngine::DrawWorldMeshW(bool noTextures) {
	if (!Engine::GAPI->GetRendererState()->RendererSettings.DrawWorldMesh)
		return XR_SUCCESS;

	float3 camPos = Engine::GAPI->GetCameraPosition();

	// Engine::GAPI->SetFarPlane(DEFAULT_FAR_PLANE);

	INT2 camSection = WorldConverter::GetSectionOfPos(camPos);

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();

	// Setup renderstates
	Engine::GAPI->GetRendererState()->RasterizerState.SetDefault();
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_BACK;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.SetDefault();
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	Engine::GAPI->ResetWorldTransform();
	Engine::GAPI->SetViewTransformXM(view);

	// Set shader
	SetActivePixelShader("PS_AtmosphereGround");
	D3D11PShader* nrmPS = ActivePS;
	SetActivePixelShader("PS_World");
	D3D11PShader* defaultPS = ActivePS;
	SetActiveVertexShader("VS_Ex");
	D3D11VShader* vsEx = ActiveVS;

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	if (Engine::GAPI->GetRendererState()->RendererSettings.WireframeWorld) {
		Engine::GAPI->GetRendererState()->RasterizerState.Wireframe = true;
	}

	// Init drawcalls
	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	XMFLOAT4X4 id; XMStoreFloat4x4(&id, XMMatrixIdentity());
	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	InfiniteRangeConstantBuffer->BindToPixelShader(3);

	int numSections = 0;
	int sectionViewDist =
		Engine::GAPI->GetRendererState()->RendererSettings.SectionDrawRadius;

	std::list<WorldMeshSectionInfo*> renderList;

	Engine::GAPI->CollectVisibleSections(renderList);

	// Static, so we can clear the lists but leave the hashmap intact
	static std::unordered_map<
		zCTexture*, std::pair<MaterialInfo*, std::vector<WorldMeshInfo*>>>
		meshesByMaterial;
	static zCMesh* startMesh =
		Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetMesh();

	if (startMesh != Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetMesh()) {
		meshesByMaterial.clear();  // Happens on world change
		startMesh = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetMesh();
	}

	std::vector<MeshInfo*> WaterSurfaces;

	for (std::list<WorldMeshSectionInfo*>::iterator itr = renderList.begin();
		itr != renderList.end(); itr++) {
		numSections++;
		for (std::map<MeshKey, WorldMeshInfo*>::iterator it =
			(*itr)->WorldMeshes.begin();
			it != (*itr)->WorldMeshes.end(); ++it) {
			if (it->first.Material) {
				auto p = meshesByMaterial[it->first.Material->GetTexture()];
				p.second.push_back(it->second);

				if (!p.first) {
					p.first = Engine::GAPI->GetMaterialInfoFrom(
						it->first.Material->GetTextureSingle());
				}
			}
			else {
				meshesByMaterial[nullptr].second.push_back(it->second);
				meshesByMaterial[nullptr].first =
					Engine::GAPI->GetMaterialInfoFrom(nullptr);
			}
		}
	}

	// Bind wrapped mesh vertex buffers
	DrawVertexBufferIndexedUINT(
		Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
		Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0);

	for (auto&& textureInfo : meshesByMaterial) {
		if (textureInfo.second.second.empty()) continue;

		if (!textureInfo.first) {
			DistortionTexture->BindToPixelShader(0);
		}
		else {
			// FrameTextures.insert(it->first);

			MaterialInfo* info = textureInfo.second.first;
			if (!info->Constantbuffer) info->UpdateConstantbuffer();

			// Check surface type
			if (info->MaterialType == MaterialInfo::MT_Water) {
				FrameWaterSurfaces[textureInfo.first] = textureInfo.second.second;
				textureInfo.second.second.resize(0);
				continue;
			}

			info->Constantbuffer->BindToPixelShader(2);

			// Bind texture
			if (textureInfo.first->CacheIn(0.6f) == zRES_CACHED_IN)
				textureInfo.first->Bind(0);
			else
				continue;

			// Querry the second texture slot to see if there is a normalmap bound
			{
				wrl::ComPtr<ID3D11ShaderResourceView> nrmmap;
				GetContext()->PSGetShaderResources(1, 1, &nrmmap);
				if (!nrmmap) {
					if (ActivePS != defaultPS) {
						ActivePS = defaultPS;
						ActivePS->Apply();
					}
				}
				else {
					if (ActivePS != nrmPS) {
						ActivePS = nrmPS;
						ActivePS->Apply();
					}
				}
			}
			// Check for overwrites
			// TODO: This is slow, sort this!
			if (!info->VertexShader.empty()) {
				SetActiveVertexShader(info->VertexShader);
				if (ActiveVS) ActiveVS->Apply();
			}
			else if (ActiveVS != vsEx) {
				ActiveVS = vsEx;
				ActiveVS->Apply();
			}

			if (!info->TesselationShaderPair.empty()) {
				info->Constantbuffer->BindToDomainShader(2);

				GetContext()->IASetPrimitiveTopology(
					D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

				D3D11HDShader* hd =
					ShaderManager->GetHDShader(info->TesselationShaderPair);
				if (hd) hd->Apply();

				ActiveHDS = hd;

				DefaultHullShaderConstantBuffer hscb = {};

				// convert to EdgesPerScreenHeight
				hscb.H_EdgesPerScreenHeight =
					GetResolution().y / Engine::GAPI->GetRendererState()
					->RendererSettings.TesselationFactor;
				hscb.H_Proj11 =
					Engine::GAPI->GetRendererState()->TransformState.TransformProj._22;
				hscb.H_GlobalTessFactor = Engine::GAPI->GetRendererState()
					->RendererSettings.TesselationFactor;
				hscb.H_ScreenResolution = float2(GetResolution().x, GetResolution().y);
				hscb.H_FarPlane = Engine::GAPI->GetFarPlane();
				hd->GetConstantBuffer()[0]->UpdateBuffer(&hscb);
				hd->GetConstantBuffer()[0]->BindToHullShader(1);

			}
			else if (ActiveHDS) {
				ActiveHDS = nullptr;

				// Bind wrapped mesh vertex buffers
				DrawVertexBufferIndexedUINT(
					Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
					Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0);
				GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				D3D11HDShader::Unbind();
			}

			if (!info->PixelShader.empty()) {
				SetActivePixelShader(info->PixelShader);
				if (ActivePS) ActivePS->Apply();

			}
			else if (ActivePS != defaultPS && ActivePS != nrmPS) {
				// Querry the second texture slot to see if there is a normalmap bound
				ID3D11ShaderResourceView* nrmmap;
				GetContext()->PSGetShaderResources(1, 1, &nrmmap);
				if (!nrmmap) {
					if (ActivePS != defaultPS) {
						ActivePS = defaultPS;
						ActivePS->Apply();
					}
				}
				else {
					if (ActivePS != nrmPS) {
						ActivePS = nrmPS;
						ActivePS->Apply();
					}
					nrmmap->Release();
				}
			}
		}

		if (ActiveHDS) {
			for (auto&& itr = textureInfo.second.second.begin();
				itr != textureInfo.second.second.end(); itr++) {
				DrawVertexBufferIndexed((*itr)->MeshVertexBuffer,
					(*itr)->MeshIndexBuffer,
					(*itr)->Indices.size());
			}
		}
		else {
			for (auto&& itr = textureInfo.second.second.begin();
				itr != textureInfo.second.second.end(); itr++) {
				// Draw from wrapped mesh
				DrawVertexBufferIndexedUINT(nullptr, nullptr, (*itr)->Indices.size(),
					(*itr)->BaseIndexLocation);
			}
		}

		Engine::GAPI->GetRendererState()->RendererInfo.WorldMeshDrawCalls +=
			textureInfo.second.second.size();

		// Clear the list, leaving the memory allocated
		textureInfo.second.second.resize(0);
	}

	if (Engine::GAPI->GetRendererState()->RendererSettings.WireframeWorld) {
		Engine::GAPI->GetRendererState()->RasterizerState.Wireframe = false;
	}

	Engine::GAPI->GetRendererState()->RendererInfo.FrameNumSectionsDrawn =
		numSections;
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_FRONT;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	return XR_SUCCESS;
}

/** Draws the given mesh infos as water */
void D3D11GraphicsEngine::DrawWaterSurfaces() {
	SetDefaultStates();

	// Copy backbuffer
	PfxRenderer->CopyTextureToRTV(
		HDRBackBuffer->GetShaderResView(),
		PfxRenderer->GetTempBuffer()->GetRenderTargetView());
	CopyDepthStencil();

	// Pre-Draw the surfaces to fix overlaying polygons causing a huge performance
	// drop Unbind pixelshader
	GetContext()->PSSetShader(nullptr, nullptr, 0);
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());
	for (auto const& it : FrameWaterSurfaces) {
		// Draw surfaces
		for (auto const& mesh : it.second) {
			DrawVertexBufferIndexed(
				mesh->MeshVertexBuffer,
				mesh->MeshIndexBuffer,
				mesh->Indices.size());
		}
	}

	// Setup depth state so we can't have multiple layers of water
	Engine::GAPI->GetRendererState()->DepthState.DepthBufferCompareFunc =
		GothicDepthBufferStateInfo::CF_COMPARISON_LESS;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);  // Update view transform

	// Bind water shader
	SetActiveVertexShader("VS_ExWater");
	SetActivePixelShader("PS_Water");
	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	XMFLOAT4X4 id; XMStoreFloat4x4(&id, XMMatrixIdentity());
	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	// Bind distortion texture
	DistortionTexture->BindToPixelShader(4);

	// Bind copied backbuffer
	GetContext()->PSSetShaderResources(
		5, 1, PfxRenderer->GetTempBuffer()->GetShaderResViewPtr());

	// Bind depth to the shader
	DepthStencilBufferCopy->BindToPixelShader(GetContext(), 2);

	// Fill refraction info CB and bind it
	RefractionInfoConstantBuffer ricb = {};
	ricb.RI_Projection = Engine::GAPI->GetProjectionMatrixDX();
	ricb.RI_ViewportSize = float2(Resolution.x, Resolution.y);
	ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
	ricb.RI_CameraPosition = float3(Engine::GAPI->GetCameraPosition());

	ActivePS->GetConstantBuffer()[2]->UpdateBuffer(&ricb);
	ActivePS->GetConstantBuffer()[2]->BindToPixelShader(2);

	// Bind reflection cube
	GetContext()->PSSetShaderResources(3, 1, ReflectionCube.GetAddressOf());

	for (int i = 0; i < 1; i++)
	// Draw twice, but second time only to depth buffer to fix the fog
	{
		for (auto const& it : FrameWaterSurfaces) {
			if (it.first) {
				// Bind diffuse
				if (it.first->CacheIn(-1) ==
					zRES_CACHED_IN)  // Force immediate cache in, because water is
									 // important!
					it.first->Bind(0);
			}
			// Draw surfaces
			for (auto const& mesh :it.second) {
				DrawVertexBufferIndexed(
					mesh->MeshVertexBuffer,
					mesh->MeshIndexBuffer,
					mesh->Indices.size());
			}
		}
	}

	// Draw Ocean
	if (Engine::GAPI->GetOcean()) Engine::GAPI->GetOcean()->Draw();

	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());

	Engine::GAPI->GetRendererState()->DepthState.DepthBufferCompareFunc =
		GothicDepthBufferStateInfo::CF_COMPARISON_LESS_EQUAL;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();
}

/** Draws everything around the given position */
void D3D11GraphicsEngine::DrawWorldAround(
	FXMVECTOR position, float range, bool cullFront, bool indoor,
	bool noNPCs, std::list<VobInfo*>* renderedVobs,
	std::list<SkeletalVobInfo*>* renderedMobs,
	std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache) {

	// Setup renderstates
	Engine::GAPI->GetRendererState()->RasterizerState.SetDefault();
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
		: GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.DepthClipEnable = true;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.SetDefault();
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	bool linearDepth =
		(Engine::GAPI->GetRendererState()->GraphicsState.FF_GSwitches &
			GSWITCH_LINEAR_DEPTH) != 0;
	if (linearDepth) {
		SetActivePixelShader("PS_LinDepth");
	}

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	// Init drawcalls
	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	XMFLOAT4X4 id; XMStoreFloat4x4(&id, XMMatrixIdentity());
	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	// Update and bind buffer of PS
	PerObjectState ocb = {};
	ocb.OS_AmbientColor = float3(1, 1, 1);
	ActivePS->GetConstantBuffer()[3]->UpdateBuffer(&ocb);
	ActivePS->GetConstantBuffer()[3]->BindToPixelShader(3);

	float3 pos; XMStoreFloat3(pos.toXMFLOAT3(), position);
	INT2 s = WorldConverter::GetSectionOfPos(pos);

	float vobOutdoorDist =
		Engine::GAPI->GetRendererState()->RendererSettings.OutdoorVobDrawRadius;
	float vobOutdoorSmallDist = Engine::GAPI->GetRendererState()
		->RendererSettings.OutdoorSmallVobDrawRadius;
	float vobSmallSize =
		Engine::GAPI->GetRendererState()->RendererSettings.SmallVobSize;

	DistortionTexture->BindToPixelShader(0);

	InfiniteRangeConstantBuffer->BindToPixelShader(3);

	UpdateRenderStates();

	bool colorWritesEnabled =
		Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled;
	float alphaRef = Engine::GAPI->GetRendererState()->GraphicsState.FF_AlphaRef;

	std::vector<WorldMeshSectionInfo*> drawnSections;

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawWorldMesh) {
		// Bind wrapped mesh vertex buffers
		DrawVertexBufferIndexedUINT(
			Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
			Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0);

		XMFLOAT4X4 id; XMStoreFloat4x4(&id, XMMatrixIdentity());
		ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
		ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

		// Only use cache if we haven't already collected the vobs
		// TODO: Collect vobs in a different way than using the drawn sections!
		//		 The current solution won't use the cache at all when there are
		// no vobs near!
		if (worldMeshCache && renderedVobs && !renderedVobs->empty()) {
			for (auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey) {
				// Bind texture
				if (meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture()) {
					// Check surface type
					if (meshInfoByKey->first.Info->MaterialType == MaterialInfo::MT_Water) {
						continue;
					}

					if (meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ||
						colorWritesEnabled) {
						if (alphaRef > 0.0f && meshInfoByKey->first.Material->GetTexture()->CacheIn(
							0.6f) == zRES_CACHED_IN) {
							meshInfoByKey->first.Material->GetTexture()->Bind(0);
							ActivePS->Apply();
						}
						else
							continue;  // Don't render if not loaded
					}
					else {
						if (!linearDepth)  // Only unbind when not rendering linear depth
						{
							// Unbind PS
							GetContext()->PSSetShader(nullptr, nullptr, 0);
						}
					}
				}

				// Draw from wrapped mesh
				DrawVertexBufferIndexedUINT(
					Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
					Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer,
					meshInfoByKey->second->Indices.size(), meshInfoByKey->second->BaseIndexLocation);
			}

		}
		else {
			for (auto&& itx : Engine::GAPI->GetWorldSections()) {
				for (auto&& ity : itx.second) {
					float vLen; XMStoreFloat(&vLen, XMVector3Length(XMVectorSet(itx.first - s.x, ity.first - s.y, 0, 0)));
					
					if (vLen < 2) {
						WorldMeshSectionInfo& section = ity.second;
						drawnSections.push_back(&section);

						if (Engine::GAPI->GetRendererState()
							->RendererSettings.FastShadows) {
							// Draw world mesh
							if (section.FullStaticMesh)
								Engine::GAPI->DrawMeshInfo(nullptr, section.FullStaticMesh);
						}
						else {
							for (auto&& meshInfoByKey = section.WorldMeshes.begin(); 
								meshInfoByKey != section.WorldMeshes.end(); ++meshInfoByKey) {
								// Check surface type
								if (meshInfoByKey->first.Info->MaterialType == MaterialInfo::MT_Water) {
									continue;
								}

								// Bind texture
								if (meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture()) {
									if (meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ||
										colorWritesEnabled) {
										if (alphaRef > 0.0f &&
											meshInfoByKey->first.Material->GetTexture()->CacheIn(0.6f) ==
											zRES_CACHED_IN) {
											meshInfoByKey->first.Material->GetTexture()->Bind(0);
											ActivePS->Apply();
										}
										else
											continue;  // Don't render if not loaded
									}
									else {
										if (!linearDepth)  // Only unbind when not rendering linear
														   // depth
										{
											// Unbind PS
											GetContext()->PSSetShader(nullptr, nullptr, 0);
										}
									}
								}

								// Draw from wrapped mesh
								DrawVertexBufferIndexedUINT(
									Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
									Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer,
									meshInfoByKey->second->Indices.size(), meshInfoByKey->second->BaseIndexLocation);
							}
						}
					}
				}
			}
		}
	}

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawVOBs) {
		// Draw visible vobs here
		std::list<VobInfo*> rndVob;
		XMVECTOR xmLastRenderPos;
		// construct new renderedvob list or fake one
		if (!renderedVobs || renderedVobs->empty()) {
			for (size_t i = 0; i < drawnSections.size(); i++) {
				for (auto it : drawnSections[i]->Vobs) {
					if (!it->VisualInfo) {
						continue;  // Seems to happen in Gothic 1
					}

					if (!it->Vob->GetShowVisual()) {
						continue;
					}

					xmLastRenderPos = XMLoadFloat3(&it->LastRenderPosition);
					// Check vob range
					
					float dist;
					XMStoreFloat(&dist, XMVector3Length(position - xmLastRenderPos));
					if (dist > range) {
						continue;
					}

					// Check for inside vob. Don't render inside-vobs when the light is
					// outside and vice-versa.
					if (!it->IsIndoorVob && indoor || it->IsIndoorVob && !indoor) {
						continue;
					}
					rndVob.push_back(it);
				}
			}

			if (renderedVobs)* renderedVobs = rndVob;
		}

		// At this point eiter renderedVobs or rndVob is filled with something
		std::list<VobInfo*>& rl = renderedVobs != nullptr ? *renderedVobs : rndVob;
		for (auto const& vobInfo : rl) {
			// Bind per-instance buffer
			((D3D11ConstantBuffer*)vobInfo->VobConstantBuffer)->BindToVertexShader(1);

			// Draw the vob
			for (auto const& materialMesh : vobInfo->VisualInfo->Meshes) {
				if (materialMesh.first && materialMesh.first->GetTexture()) {
					if (materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_FUNC_NONE ||
						materialMesh.first->GetAlphaFunc() !=
						zMAT_ALPHA_FUNC_FUNC_MAT_DEFAULT) {
						if (materialMesh.first->GetTexture()->CacheIn(0.6f) == zRES_CACHED_IN) {
							materialMesh.first->GetTexture()->Bind(0);
						}
					}
					else {
						DistortionTexture->BindToPixelShader(0);
					}
				}
				for (auto const& meshInfo : materialMesh.second) {
					Engine::GraphicsEngine->DrawVertexBufferIndexed(
						meshInfo->MeshVertexBuffer,
						meshInfo->MeshIndexBuffer,
						meshInfo->Indices.size());
				}
			}
		}

		// Vobs have this differently
		Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
			!Engine::GAPI->GetRendererState()
			->RasterizerState.FrontCounterClockwise;
		Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();
		UpdateRenderStates();
	}

	bool renderNPCs = !noNPCs;

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_FRONT;
	Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
		true;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawMobs) {
		// Draw visible vobs here
		std::list<SkeletalVobInfo*> rndVob;

		// construct new renderedvob list or fake one
		if (!renderedMobs || renderedMobs->empty()) {
			for (auto it : Engine::GAPI->GetSkeletalMeshVobs()) {
				if (!it->VisualInfo) {
					continue;  // Seems to happen in Gothic 1
				}

				if (!it->Vob->GetShowVisual()) {
					continue;
				}

				// Check vob range
				float dist;
				XMStoreFloat(&dist, XMVector3Length(position - it->Vob->GetPositionWorldXM()));

				if (dist > range) {
					continue;
				}

				// Check for inside vob. Don't render inside-vobs when the light is
				// outside and vice-versa.
				if (!it->Vob->IsIndoorVob() && indoor ||
					it->Vob->IsIndoorVob() && !indoor) {
					continue;
				}

				// Assume everything that doesn't have a skeletal-mesh won't move very
				// much This applies to usable things like chests, chairs, beds, etc
				if (!((SkeletalMeshVisualInfo*)it->VisualInfo)
					->SkeletalMeshes.empty()) {
					continue;
				}

				rndVob.push_back(it);
			}

			if (renderedMobs) {
				*renderedMobs = rndVob;
			}
		}

		// At this point eiter renderedMobs or rndVob is filled with something
		std::list<SkeletalVobInfo*>& rl = renderedMobs != nullptr ? *renderedMobs : rndVob;
		for (auto it : rl) {
			Engine::GAPI->DrawSkeletalMeshVob(it, FLT_MAX);
		}
	}
	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawSkeletalMeshes) {
		// Draw animated skeletal meshes if wanted
		if (renderNPCs) {
			for (auto const& skeletalMeshVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs()) {
				if (!skeletalMeshVob->VisualInfo) {
					// Seems to happen in Gothic 1
					continue;
				}
				// Check vob range
				float dist;
				XMStoreFloat(&dist, XMVector3Length(position - skeletalMeshVob->Vob->GetPositionWorldXM()));

				if (dist > range) {
					// Not in range
					continue;
				}
				// Check for inside vob. Don't render inside-vobs when the light is
				// outside and vice-versa.
				if (!skeletalMeshVob->Vob->IsIndoorVob() && indoor ||
					skeletalMeshVob->Vob->IsIndoorVob() && !indoor) {
					continue;
				}

				Engine::GAPI->DrawSkeletalMeshVob(skeletalMeshVob, FLT_MAX);
			}
		}
	}
}

/** Draws everything around the given position */
void D3D11GraphicsEngine::DrawWorldAround(FXMVECTOR position,
	int sectionRange, float vobXZRange,
	bool cullFront, bool dontCull) {
	// Setup renderstates
	Engine::GAPI->GetRendererState()->RasterizerState.SetDefault();
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
		: GothicRasterizerStateInfo::CM_CULL_BACK;
	if (dontCull)
		Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_NONE;

	Engine::GAPI->GetRendererState()->RasterizerState.DepthClipEnable = true;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.SetDefault();
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();

	Engine::GAPI->ResetWorldTransform();
	Engine::GAPI->SetViewTransformXM(view);

	// Set shader
	SetActivePixelShader("PS_AtmosphereGround");
	D3D11PShader* nrmPS = ActivePS;
	SetActivePixelShader("PS_DiffuseAlphaTest");
	D3D11PShader* defaultPS = ActivePS;
	SetActiveVertexShader("VS_Ex");

	bool linearDepth =
		(Engine::GAPI->GetRendererState()->GraphicsState.FF_GSwitches &
			GSWITCH_LINEAR_DEPTH) != 0;
	if (linearDepth) {
		SetActivePixelShader("PS_LinDepth");
	}

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	// Init drawcalls
	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	XMMATRIX id = XMMatrixIdentity();
	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	// Update and bind buffer of PS
	PerObjectState ocb = {};
	ocb.OS_AmbientColor = float3(1, 1, 1);
	ActivePS->GetConstantBuffer()[3]->UpdateBuffer(&ocb);
	ActivePS->GetConstantBuffer()[3]->BindToPixelShader(3);

	float3 fPosition; XMStoreFloat3(fPosition.toXMFLOAT3(), position);
	INT2 s = WorldConverter::GetSectionOfPos(fPosition);

	float vobOutdoorDist =
		Engine::GAPI->GetRendererState()->RendererSettings.OutdoorVobDrawRadius;
	float vobOutdoorSmallDist = Engine::GAPI->GetRendererState()
		->RendererSettings.OutdoorSmallVobDrawRadius;
	float vobSmallSize =
		Engine::GAPI->GetRendererState()->RendererSettings.SmallVobSize;

	DistortionTexture->BindToPixelShader(0);

	InfiniteRangeConstantBuffer->BindToPixelShader(3);

	UpdateRenderStates();

	bool colorWritesEnabled =
		Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled;
	float alphaRef = Engine::GAPI->GetRendererState()->GraphicsState.FF_AlphaRef;

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawWorldMesh) {
		// Bind wrapped mesh vertex buffers
		DrawVertexBufferIndexedUINT(
			Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
			Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0);

		XMMATRIX id = XMMatrixIdentity();
		ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
		ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

		for (const auto& itx : Engine::GAPI->GetWorldSections()) {
			for (const auto& ity : itx.second) {

				XMVECTOR a = XMVectorSet(
					(float)(itx.first - s.x),
					static_cast<float>((ity.first - s.y)), 0, 0);
				float len;
				XMStoreFloat(&len, XMVector2Length(a));
				if (len < sectionRange) {
					const WorldMeshSectionInfo& section = ity.second;

					if (Engine::GAPI->GetRendererState()->RendererSettings.FastShadows) {
						// Draw world mesh
						if (section.FullStaticMesh)
							Engine::GAPI->DrawMeshInfo(nullptr, section.FullStaticMesh);
					}
					else {
						for (const auto& it : section.WorldMeshes) {
							// Check surface type
							if (it.first.Info->MaterialType == MaterialInfo::MT_Water) {
								continue;
							}

							// Bind texture
							if (it.first.Material && it.first.Material->GetTexture()) {
								if (it.first.Material->GetTexture()->HasAlphaChannel() ||
									colorWritesEnabled) {
									if (alphaRef > 0.0f &&
										it.first.Material->GetTexture()->CacheIn(0.6f) ==
										zRES_CACHED_IN) {
										it.first.Material->GetTexture()->Bind(0);
										ActivePS->Apply();
									}
									else
										continue;  // Don't render if not loaded
								}
								else {
									if (!linearDepth)  // Only unbind when not rendering linear
													   // depth
									{
										// Unbind PS
										GetContext()->PSSetShader(nullptr, nullptr, 0);
									}
								}
							}

							// Draw from wrapped mesh
							DrawVertexBufferIndexedUINT(
								Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
								Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer,
								it.second->Indices.size(), it.second->BaseIndexLocation);
						}
					}
				}
			}
		}
	}

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawVOBs) {
		// Vobs have this differently
		Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
			!Engine::GAPI->GetRendererState()
			->RasterizerState.FrontCounterClockwise;
		Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();
		UpdateRenderStates();

		// Reset instances
		const std::unordered_map<zCProgMeshProto*, MeshVisualInfo*>& staticMeshVisuals =
			Engine::GAPI->GetStaticMeshVisuals();

		for (auto const& it : RenderedVobs) {
			VobInstanceInfo vii;
			vii.world = it->WorldMatrix;

			if (!it->IsIndoorVob) 
				((MeshVisualInfo*)it->VisualInfo)->Instances.push_back(vii);
		}

		// Apply instancing shader
		SetActiveVertexShader("VS_ExInstancedObj");
		// SetActivePixelShader("PS_DiffuseAlphaTest");
		ActiveVS->Apply();

		if (!linearDepth)  // Only unbind when not rendering linear depth
		{
			// Unbind PS
			GetContext()->PSSetShader(nullptr, nullptr, 0);
		}

		D3D11_BUFFER_DESC desc;
		DynamicInstancingBuffer->GetVertexBuffer()->GetDesc(&desc);

		byte* data;
		UINT size;
		UINT loc = 0;
		DynamicInstancingBuffer->Map(D3D11VertexBuffer::M_WRITE_DISCARD,
			(void**)& data, &size);
		static std::vector<VobInstanceInfo, AlignmentAllocator<VobInstanceInfo, 16>> s_InstanceData;
		for (auto const& staticMeshVisual : staticMeshVisuals) {
			if (staticMeshVisual.second->Instances.empty()) continue;

			if ((loc + staticMeshVisual.second->Instances.size()) * sizeof(VobInstanceInfo) >= desc.ByteWidth)
				break;  // Should never happen

			staticMeshVisual.second->StartInstanceNum = loc;
			memcpy(data + loc * sizeof(VobInstanceInfo), &staticMeshVisual.second->Instances[0],
				sizeof(VobInstanceInfo) * staticMeshVisual.second->Instances.size());
			loc += staticMeshVisual.second->Instances.size();
		}
		DynamicInstancingBuffer->Unmap();

		// Draw all vobs the player currently sees
		for (auto const& staticMeshVisual : staticMeshVisuals) {
			if (staticMeshVisual.second->Instances.empty()) continue;

			bool doReset = true;
			for (auto const& itt : staticMeshVisual.second->MeshesByTexture) {
				std::vector<MeshInfo*>& mlist = staticMeshVisual.second->MeshesByTexture[itt.first];
				if (mlist.empty()) continue;

				for (unsigned int i = 0; i < mlist.size(); i++) {
					zCTexture* tx = itt.first.Texture;

					// Check for alphablend
					bool blendAdd =
						itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
					bool blendBlend =
						itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;
					// TODO: TODO: if one part of the mesh uses blending, all do.
					if (!doReset || blendAdd || blendBlend)
					{
						doReset = false;
						continue;
					}

					// Bind texture
					if (tx && (tx->HasAlphaChannel() || colorWritesEnabled)) {
						if (alphaRef > 0.0f && tx->CacheIn(0.6f) == zRES_CACHED_IN) {
							tx->Bind(0);
							ActivePS->Apply();
						}
						else
							continue;
					}
					else {
						if (!linearDepth)  // Only unbind when not rendering linear depth
						{
							// Unbind PS
							GetContext()->PSSetShader(nullptr, nullptr, 0);
						}
					}

					MeshInfo* mi = mlist[i];

					// Draw batch
					DrawInstanced(mi->MeshVertexBuffer, mi->MeshIndexBuffer,
						mi->Indices.size(), DynamicInstancingBuffer.get(),
						sizeof(VobInstanceInfo), staticMeshVisual.second->Instances.size(),
						sizeof(ExVertexStruct), staticMeshVisual.second->StartInstanceNum);

					Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnVobs +=
						staticMeshVisual.second->Instances.size();
				}
			}

			// Reset visual
			if (doReset) staticMeshVisual.second->StartNewFrame();
		}
	}

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawSkeletalMeshes) {
		// Draw skeletal meshes
		for (auto const& skeletalMeshVob : Engine::GAPI->GetSkeletalMeshVobs()) {
			if (!skeletalMeshVob->VisualInfo) continue;

			INT2 s = WorldConverter::GetSectionOfPos(skeletalMeshVob->Vob->GetPositionWorld());

			float dist; XMStoreFloat(&dist, XMVector3Length(skeletalMeshVob->Vob->GetPositionWorldXM() - position));
			if (dist > Engine::GAPI->GetRendererState()
				->RendererSettings.IndoorVobDrawRadius)
				continue;  // Skip out of range

			Engine::GAPI->DrawSkeletalMeshVob(skeletalMeshVob, FLT_MAX);
		}
	}

	Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled = true;
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();
}

/** Draws the static vobs instanced */
XRESULT D3D11GraphicsEngine::DrawVOBsInstanced() {
	START_TIMING();

	const std::unordered_map<zCProgMeshProto*, MeshVisualInfo*>& staticMeshVisuals =
		Engine::GAPI->GetStaticMeshVisuals();

	SetDefaultStates();

	SetActivePixelShader("PS_AtmosphereGround");
	D3D11PShader* nrmPS = ActivePS;
	SetActivePixelShader("PS_Diffuse");
	D3D11PShader* defaultPS = ActivePS;
	SetActiveVertexShader("VS_ExInstancedObj");

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	// Use default material info for now
	MaterialInfo defInfo;
	ActivePS->GetConstantBuffer()[2]->UpdateBuffer(&defInfo);
	ActivePS->GetConstantBuffer()[2]->BindToPixelShader(2);

	float3 camPos = Engine::GAPI->GetCameraPosition();
	INT2 camSection = WorldConverter::GetSectionOfPos(camPos);

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);

	if (Engine::GAPI->GetRendererState()->RendererSettings.WireframeVobs) {
		Engine::GAPI->GetRendererState()->RasterizerState.Wireframe = true;
	}

	// Vobs need this
	Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
		false;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	// Init drawcalls
	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	bool tesselationEnabled =
		Engine::GAPI->GetRendererState()->RendererSettings.EnableTesselation;

	static std::vector<VobInfo*> vobs;
	static std::vector<VobLightInfo*> lights;
	static std::vector<SkeletalVobInfo*> mobs;

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawVOBs ||
		Engine::GAPI->GetRendererState()
		->RendererSettings.EnableDynamicLighting) {
		if (!Engine::GAPI->GetRendererState()->RendererSettings.FixViewFrustum ||
			(Engine::GAPI->GetRendererState()->RendererSettings.FixViewFrustum &&
				vobs.empty())) {
			Engine::GAPI->CollectVisibleVobs(vobs, lights, mobs);
		}
	}

	// Need to collect alpha-meshes to render them laterdy
	std::list<std::pair<MeshKey, std::pair<MeshVisualInfo*, MeshInfo*>>>
		AlphaMeshes;

	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawVOBs) {
		// Create instancebuffer for this frame
		D3D11_BUFFER_DESC desc;
		DynamicInstancingBuffer->GetVertexBuffer()->GetDesc(&desc);

		if (desc.ByteWidth < sizeof(VobInstanceInfo) * vobs.size()) {
			if (Engine::GAPI->GetRendererState()->RendererSettings.EnableDebugLog)
				LogInfo() << "Instancing buffer too small (" << desc.ByteWidth
				<< "), need " << sizeof(VobInstanceInfo) * vobs.size()
				<< " bytes. Recreating buffer.";

			// Buffer too small, recreate it
			DynamicInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
			DynamicInstancingBuffer->Init(
				nullptr, sizeof(VobInstanceInfo) * vobs.size(),
				D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
				D3D11VertexBuffer::CA_WRITE);
		}

		byte* data;
		UINT size;
		UINT loc = 0;
		DynamicInstancingBuffer->Map(D3D11VertexBuffer::M_WRITE_DISCARD,
			(void**)& data, &size);
		static std::vector<VobInstanceInfo, AlignmentAllocator<VobInstanceInfo, 16>>
			s_InstanceData;
		for (auto const& staticMeshVisual : staticMeshVisuals) {
			staticMeshVisual.second->StartInstanceNum = loc;
			memcpy(data + loc * sizeof(VobInstanceInfo), &staticMeshVisual.second->Instances[0],
				sizeof(VobInstanceInfo) * staticMeshVisual.second->Instances.size());
			loc += staticMeshVisual.second->Instances.size();
		}
		DynamicInstancingBuffer->Unmap();

		for (unsigned int i = 0; i < vobs.size(); i++) {
			vobs[i]->VisibleInRenderPass = false;  // Reset this for the next frame
			RenderedVobs.push_back(vobs[i]);
		}

		// Reset buffer
		s_InstanceData.resize(0);

		for (auto const& staticMeshVisual : staticMeshVisuals) {
			if (staticMeshVisual.second->Instances.empty()) continue;

			if (staticMeshVisual.second->MeshSize <
				Engine::GAPI->GetRendererState()->RendererSettings.SmallVobSize) {
				OutdoorSmallVobsConstantBuffer->UpdateBuffer(
					float4(Engine::GAPI->GetRendererState()
						->RendererSettings.OutdoorSmallVobDrawRadius -
						staticMeshVisual.second->MeshSize,
						0, 0, 0).toPtr());
				OutdoorSmallVobsConstantBuffer->BindToPixelShader(3);
			}
			else {
				OutdoorVobsConstantBuffer->UpdateBuffer(
					float4(Engine::GAPI->GetRendererState()
						->RendererSettings.OutdoorVobDrawRadius -
						staticMeshVisual.second->MeshSize,
						0, 0, 0).toPtr());
				OutdoorVobsConstantBuffer->BindToPixelShader(3);
			}

			bool doReset = true;  // Don't reset alpha-vobs here
			for (auto const& itt : staticMeshVisual.second->MeshesByTexture) {
				std::vector<MeshInfo*>& mlist =
					staticMeshVisual.second->MeshesByTexture[itt.first];
				if (mlist.empty()) continue;

				// Bind buffers
				DrawVertexBufferIndexed(staticMeshVisual.second->FullMesh->MeshVertexBuffer,
					staticMeshVisual.second->FullMesh->MeshIndexBuffer, 0);

				for (unsigned int i = 0; i < mlist.size(); i++) {
					zCTexture* tx = itt.first.Material->GetAniTexture();
					MeshInfo* mi = mlist[i];

					if (!tx) {
#ifndef BUILD_SPACER
						continue;  // Don't render meshes without texture if not in spacer
#else
						// This is most likely some spacer helper-vob
						WhiteTexture->BindToPixelShader(0);
						PS_Diffuse->Apply();

						/*// Apply colors for these meshes
						MaterialInfo::Buffer b;
						ZeroMemory(&b, sizeof(b));
						b.Color = itt->first.Material->GetColor();
						PS_Diffuse->GetConstantBuffer()[2]->UpdateBuffer(&b);
						PS_Diffuse->GetConstantBuffer()[2]->BindToPixelShader(2);*/
#endif
				}
					else {
						// Check for alphablending on world mesh
						bool blendAdd = itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
						bool blendBlend = itt.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;
						// TODO: TODO: if one part of the mesh uses blending, all do.
						if (!doReset || blendAdd || blendBlend) {
							MeshVisualInfo* info = staticMeshVisual.second;
							MeshInfo* mesh = mlist[i];

							AlphaMeshes.push_back(
								std::make_pair(itt.first, std::make_pair(info, mesh)));

							doReset = false;
							continue;
						}

						// Bind texture

						if (tx->CacheIn(0.6f) == zRES_CACHED_IN) {
							MyDirectDrawSurface7* surface = tx->GetSurface();
							ID3D11ShaderResourceView* srv[3];
							MaterialInfo* info = itt.first.Info;

							// Get diffuse and normalmap
							srv[0] = ((D3D11Texture*)surface->GetEngineTexture())
								->GetShaderResourceView();
							srv[1] = surface->GetNormalmap()
								? ((D3D11Texture*)surface->GetNormalmap())
								->GetShaderResourceView()
								: nullptr;
							srv[2] = surface->GetFxMap()
								? ((D3D11Texture*)surface->GetFxMap())
								->GetShaderResourceView()
								: nullptr;

							// Bind a default normalmap in case the scene is wet and we
							// currently have none
							if (!srv[1]) {
								// Modify the strength of that default normalmap for the
								// material info
								if (info->buffer
									.NormalmapStrength /* *
														  Engine::GAPI->GetSceneWetness()*/
									!= DEFAULT_NORMALMAP_STRENGTH) {
									info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
									info->UpdateConstantbuffer();
								}
								srv[1] = DistortionTexture->GetShaderResourceView();
							}
							// Bind both
							GetContext()->PSSetShaderResources(0, 3, srv);

							// Set normal/displacement map
							GetContext()->DSSetShaderResources(0, 1, &srv[1]);
							GetContext()->HSSetShaderResources(0, 1, &srv[1]);

							// Force alphatest on vobs for now
							BindShaderForTexture(tx, true, 0);

							if (!info->Constantbuffer) info->UpdateConstantbuffer();

							info->Constantbuffer->BindToPixelShader(2);
						}
					
			}

					if (tesselationEnabled && !mi->IndicesPNAEN.empty() &&
						RenderingStage == DES_MAIN &&
						staticMeshVisual.second->TesselationInfo.buffer.VT_TesselationFactor > 0.0f) {
						Setup_PNAEN(PNAEN_Instanced);
						staticMeshVisual.second->TesselationInfo.Constantbuffer->BindToDomainShader(1);
						staticMeshVisual.second->TesselationInfo.Constantbuffer->BindToHullShader(1);
					}
					else if (ActiveHDS) {
						GetContext()->IASetPrimitiveTopology(
							D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
						GetContext()->DSSetShader(nullptr, nullptr, 0);
						GetContext()->HSSetShader(nullptr, nullptr, 0);
						ActiveHDS = nullptr;
						SetActiveVertexShader("VS_ExInstancedObj");
						ActiveVS->Apply();
					}

					if (!ActiveHDS) {
						// Draw batch
						DrawInstanced(mi->MeshVertexBuffer, mi->MeshIndexBuffer,
							mi->Indices.size(), DynamicInstancingBuffer.get(),
							sizeof(VobInstanceInfo), staticMeshVisual.second->Instances.size(),
							sizeof(ExVertexStruct), staticMeshVisual.second->StartInstanceNum);
					}
					else {
						// Draw batch tesselated
						DrawInstanced(mi->MeshVertexBuffer, mi->MeshIndexBufferPNAEN,
							mi->IndicesPNAEN.size(), DynamicInstancingBuffer.get(),
							sizeof(VobInstanceInfo), staticMeshVisual.second->Instances.size(),
							sizeof(ExVertexStruct), staticMeshVisual.second->StartInstanceNum);
					}
		}
	}

			// Reset visual
			if (doReset &&
				!Engine::GAPI->GetRendererState()->RendererSettings.FixViewFrustum) {
				staticMeshVisual.second->StartNewFrame();
			}
}
	}

	// Draw mobs
	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawMobs) {
		for (unsigned int i = 0; i < mobs.size(); i++) {
			Engine::GAPI->DrawSkeletalMeshVob(mobs[i], FLT_MAX);
			mobs[i]->VisibleInRenderPass = false;  // Reset this for the next frame
		}
	}

	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	GetContext()->DSSetShader(nullptr, nullptr, 0);
	GetContext()->HSSetShader(nullptr, nullptr, 0);
	ActiveHDS = nullptr;

	if (Engine::GAPI->GetRendererState()->RendererSettings.WireframeVobs) {
		Engine::GAPI->GetRendererState()->RasterizerState.Wireframe = false;
	}

	STOP_TIMING(GothicRendererTiming::TT_Vobs);

	if (RenderingStage == DES_MAIN) {
		if (Engine::GAPI->GetRendererState()->RendererSettings.DrawParticleEffects) {
			std::vector<zCVob*> decals;
			Engine::GAPI->GetVisibleDecalList(decals);
			DrawDecalList(decals, true);

			DrawQuadMarks();
		}

		START_TIMING();
		// Draw lighting, since everything is drawn by now and we have the lights
		// here
		DrawLighting(lights);

		STOP_TIMING(GothicRendererTiming::TT_Lighting);
	}

	// Make sure lighting doesn't mess up our state
	SetDefaultStates();

	SetActivePixelShader("PS_Simple");
	SetActiveVertexShader("VS_ExInstancedObj");

	Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
		true;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());

	for (auto const& alphaMesh : AlphaMeshes) {
		zCTexture* tx = alphaMesh.first.Material->GetAniTexture();

		if (!tx) continue;

		// Check for alphablending on world mesh
		bool blendAdd = alphaMesh.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
		bool blendBlend =
			alphaMesh.first.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;

		// Bind texture

		MeshInfo* mi = alphaMesh.second.second;
		MeshVisualInfo* vi = alphaMesh.second.first;

		if (tx->CacheIn(0.6f) == zRES_CACHED_IN) {
			MyDirectDrawSurface7* surface = tx->GetSurface();
			ID3D11ShaderResourceView* srv[3];

			// Get diffuse and normalmap
			srv[0] = ((D3D11Texture*)surface->GetEngineTexture())
				->GetShaderResourceView();
			srv[1] = surface->GetNormalmap()
				? ((D3D11Texture*)surface->GetNormalmap())
				->GetShaderResourceView()
				: nullptr;
			srv[2] =
				surface->GetFxMap()
				? ((D3D11Texture*)surface->GetFxMap())->GetShaderResourceView()
				: nullptr;

			// Bind both
			GetContext()->PSSetShaderResources(0, 3, srv);

			if ((blendAdd || blendBlend) &&
				!Engine::GAPI->GetRendererState()->BlendState.BlendEnabled) {
				if (blendAdd)
					Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();
				else if (blendBlend)
					Engine::GAPI->GetRendererState()->BlendState.SetAlphaBlending();

				Engine::GAPI->GetRendererState()->BlendState.SetDirty();

				Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;
				Engine::GAPI->GetRendererState()->DepthState.SetDirty();

				UpdateRenderStates();
			}

			MaterialInfo* info = alphaMesh.first.Info;
			if (!info->Constantbuffer) info->UpdateConstantbuffer();

			info->Constantbuffer->BindToPixelShader(2);
		}

		// Draw batch
		DrawInstanced(mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size(),
			DynamicInstancingBuffer.get(), sizeof(VobInstanceInfo),
			vi->Instances.size(), sizeof(ExVertexStruct),
			vi->StartInstanceNum);
	}

	// Loop again, now that all alpha-meshes have been rendered
	// so we can reset their visuals, too.
	for (auto const& alphaMesh : AlphaMeshes) {
		MeshVisualInfo* vi = alphaMesh.second.first;

		// Reset visual
		vi->StartNewFrame();
	}

	if (!Engine::GAPI->GetRendererState()->RendererSettings.FixViewFrustum) {
		lights.resize(0);
		vobs.resize(0);
		mobs.resize(0);
	}

	SetDefaultStates();

	return XR_SUCCESS;
}

/** Draws the static VOBs */
XRESULT D3D11GraphicsEngine::DrawVOBs(bool noTextures) {
	return DrawVOBsInstanced();
}

XRESULT D3D11GraphicsEngine::DrawPolyStrips(bool noTextures) {
	//DrawMeshInfoListAlphablended was mostly used as an example to write everything below
	std::map<zCTexture*, PolyStripInfo> polyStripInfos = Engine::GAPI->GetPolyStripInfos();

	// No need to do a bunch of work for nothing!
	if (polyStripInfos.size() == 0) {
		return XR_SUCCESS;
	}

	SetDefaultStates();

	// Setup renderstates
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();


	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);
	
	SetActivePixelShader("PS_Diffuse");//seems like "PS_Simple" is used anyway thanks to BindShaderForTexture function used below
	SetActiveVertexShader("VS_Ex");

	//No idea what these do
	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	// Not sure what this does, adds some kind of sky tint?
	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	// Use default material info for now
	MaterialInfo defInfo;
	ActivePS->GetConstantBuffer()[2]->UpdateBuffer(&defInfo);
	ActivePS->GetConstantBuffer()[2]->BindToPixelShader(2);

	for (auto it = polyStripInfos.begin(); it != polyStripInfos.end(); it++) {

		zCMaterial* mat = it->second.material;
		zCTexture* tx = mat->GetAniTexture();

		std::vector<ExVertexStruct> vertices = it->second.vertices;

		if (!vertices.size()) continue;

		//Setting world transform matrix/////////////
		XMFLOAT4X4 id; XMStoreFloat4x4(&id, XMMatrixIdentity());
		
		//vob->GetWorldMatrix(&id);
		ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&id);
		ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

		// Check for alphablending on world mesh
		bool blendAdd = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
		bool blendBlend = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;


		if (tx->CacheIn(0.6f) == zRES_CACHED_IN)
		{
			MyDirectDrawSurface7* surface = tx->GetSurface();
			ID3D11ShaderResourceView* srv[3];

			BindShaderForTexture(mat->GetAniTexture(), false, mat->GetAlphaFunc());

			// Get diffuse and normalmap
			srv[0] = ((D3D11Texture*)surface->GetEngineTexture())->GetShaderResourceView();
			srv[1] = surface->GetNormalmap() ? ((D3D11Texture*)surface->GetNormalmap())->GetShaderResourceView() : NULL;
			srv[2] = surface->GetFxMap() ? ((D3D11Texture*)surface->GetFxMap())->GetShaderResourceView() : NULL;

			// Bind both
			Context->PSSetShaderResources(0, 3, srv);

			if ((blendAdd || blendBlend) && !Engine::GAPI->GetRendererState()->BlendState.BlendEnabled)
			{
				if (blendAdd)
					Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();
				else if (blendBlend)
					Engine::GAPI->GetRendererState()->BlendState.SetAlphaBlending();

				Engine::GAPI->GetRendererState()->BlendState.SetDirty();

				Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;
				Engine::GAPI->GetRendererState()->DepthState.SetDirty();

				UpdateRenderStates();
			}

			MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(tx);
			if (!info->Constantbuffer)
				info->UpdateConstantbuffer();

			info->Constantbuffer->BindToPixelShader(2);

		} else {
			//Don't draw if texture is not yet cached (I have no idea how can I preload it in advance)
			continue;
		}

		//Populate TempVertexBuffer and draw it
		D3D11_BUFFER_DESC desc;
		TempVertexBuffer->GetVertexBuffer()->GetDesc(&desc);
		if (desc.ByteWidth < sizeof(ExVertexStruct) * vertices.size())
		{
			if (Engine::GAPI->GetRendererState()->RendererSettings.EnableDebugLog)
			LogInfo() << "(PolyStrip) TempVertexBuffer too small (" << desc.ByteWidth << "), need " << sizeof(ExVertexStruct) * vertices.size() << " bytes. Recreating buffer.";

			// Buffer too small, recreate it
			TempVertexBuffer.reset(new D3D11VertexBuffer());
			// Reinit with a bit of a margin, so it will not be reinit each time new vertex is added
			TempVertexBuffer->Init(NULL, sizeof(ExVertexStruct) * vertices.size() * 1.1, D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE);
		}

		TempVertexBuffer->UpdateBuffer(&vertices[0], sizeof(ExVertexStruct) * vertices.size());
		DrawVertexBuffer(TempVertexBuffer.get(), vertices.size(), sizeof(ExVertexStruct));
	}

	SetDefaultStates();

	return XR_SUCCESS;
}

/** Returns the current size of the backbuffer */
INT2 D3D11GraphicsEngine::GetResolution() { return Resolution; }

/** Returns the actual resolution of the backbuffer (not supersampled) */
INT2 D3D11GraphicsEngine::GetBackbufferResolution() {
	return Resolution;

	// TODO: TODO: Oversampling
	/*
	// Get desktop rect
	RECT desktop;
	GetClientRect(GetDesktopWindow(), &desktop);

	if (Resolution.x > desktop.right || Resolution.y > desktop.bottom)
			return INT2(desktop.right, desktop.bottom);

	return Resolution;*/
}

/** Sets up the default rendering state */
void D3D11GraphicsEngine::SetDefaultStates(bool force) {
	Engine::GAPI->GetRendererState()->RasterizerState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->DepthState.SetDefault();
	Engine::GAPI->GetRendererState()->SamplerState.SetDefault();

	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();
	Engine::GAPI->GetRendererState()->SamplerState.SetDirty();

	GetContext()->PSSetSamplers(0, 1, DefaultSamplerState.GetAddressOf());

	if (force) {
		FFRasterizerStateHash = 0;
		FFBlendStateHash = 0;
		FFDepthStencilStateHash = 0;
	}

	UpdateRenderStates();
}

/** Draws the sky using the GSky-Object */
XRESULT D3D11GraphicsEngine::DrawSky() {
	GSky* sky = Engine::GAPI->GetSky();
	sky->RenderSky();

	if (!Engine::GAPI->GetRendererState()
		->RendererSettings.AtmosphericScattering) {
		Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;
		Engine::GAPI->GetRendererState()->BlendState.SetDirty();
		UpdateRenderStates();

		Engine::GAPI->GetLoadedWorldInfo()
			->MainWorld->GetSkyControllerOutdoor()
			->RenderSkyPre();
		Engine::GAPI->SetFarPlane(
			Engine::GAPI->GetRendererState()->RendererSettings.SectionDrawRadius *
			WORLD_SECTION_SIZE);
		return XR_SUCCESS;
	}
	// Create a rotaion only view-matrix
	XMFLOAT4X4 invView; Engine::GAPI->GetInverseViewMatrixDX(&invView);

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();

	XMMATRIX scale = XMMatrixScaling(
		sky->GetAtmoshpereSettings().OuterRadius,
		sky->GetAtmoshpereSettings().OuterRadius,
		sky->GetAtmoshpereSettings()
		.OuterRadius);  // Upscale it a huge amount. Gothics world is big.

	XMMATRIX world = XMMatrixTranslation(
		Engine::GAPI->GetCameraPosition().x,
		Engine::GAPI->GetCameraPosition().y +
		sky->GetAtmoshpereSettings().SphereOffsetY,
		Engine::GAPI->GetCameraPosition().z);

	world = XMMatrixTranspose(scale * world);

	// Apply world matrix
	Engine::GAPI->SetWorldTransformXM(world);
	Engine::GAPI->SetViewTransformXM(view);

	if (sky->GetAtmosphereCB().AC_CameraHeight >
		sky->GetAtmosphereCB().AC_OuterRadius) {
		SetActivePixelShader("PS_AtmosphereOuter");
	}
	else {
		SetActivePixelShader("PS_Atmosphere");
	}

	SetActiveVertexShader("VS_ExWS");

	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(1);

	VS_ExConstantBuffer_PerInstance cbi;
	XMStoreFloat4x4(&cbi.World, world);
	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&cbi);
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.BlendEnabled = true;

	Engine::GAPI->GetRendererState()->DepthState.SetDefault();

	// Allow z-testing
	Engine::GAPI->GetRendererState()->DepthState.DepthBufferEnabled = true;

	// Disable depth-writes so the sky always stays at max distance in the
	// DepthBuffer
	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;

	Engine::GAPI->GetRendererState()->RasterizerState.SetDefault();
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_BACK;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	// Apply sky texture
	D3D11Texture* cloudsTex = Engine::GAPI->GetSky()->GetCloudTexture();
	if (cloudsTex) {
		cloudsTex->BindToPixelShader(0);
	}

	D3D11Texture* nightTex = Engine::GAPI->GetSky()->GetNightTexture();
	if (nightTex) {
		nightTex->BindToPixelShader(1);
	}

	if (sky->GetSkyDome()) sky->GetSkyDome()->DrawMesh();

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_FRONT;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.DepthBufferEnabled = true;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	Engine::GAPI->GetRendererState()->BlendState.BlendEnabled = false;
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	return XR_SUCCESS;
}

/** Called when a key got pressed */
XRESULT D3D11GraphicsEngine::OnKeyDown(unsigned int key) {
	switch (key) {
#ifndef PUBLIC_RELEASE
		case VK_NUMPAD0:
			Engine::GAPI->PrintMessageTimed(INT2(30, 30), "Reloading shaders...");
			ReloadShaders();
			break;
#endif

		case VK_NUMPAD7:
			if (Engine::GAPI->GetRendererState()->RendererSettings.AllowNumpadKeys) {
				SaveScreenshotNextFrame = true;
			}
			break;
		case VK_F1:
			if (!UIView && !Engine::GAPI->GetRendererState()
				->RendererSettings.EnableEditorPanel) {
				// If the ui-view hasn't been created yet and the editorpanel is
				// disabled, enable it here
				Engine::GAPI->GetRendererState()->RendererSettings.EnableEditorPanel =
					true;
				CreateMainUIView();
			}
			break;
		default:
			break;
	}

	return XR_SUCCESS;
}

/** Reloads shaders */
XRESULT D3D11GraphicsEngine::ReloadShaders() {
	XRESULT xr = ShaderManager->ReloadShaders();

	return xr;
}

/** Returns the line renderer object */
BaseLineRenderer* D3D11GraphicsEngine::GetLineRenderer() {
	return LineRenderer.get();
}

/** Applys the lighting to the scene */
XRESULT D3D11GraphicsEngine::DrawLighting(std::vector<VobLightInfo*>& lights) {
	static XMVECTOR xmFltMax = XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX);
	SetDefaultStates();

	// ********************************
	// Draw world shadows
	// ********************************
	CameraReplacement cr;
	XMFLOAT3 cameraPosition; 
	XMStoreFloat3(&cameraPosition, Engine::GAPI->GetCameraPositionXM());
	XMVECTOR vPlayerPosition =
		Engine::GAPI->GetPlayerVob() != nullptr
		? Engine::GAPI->GetPlayerVob()->GetPositionWorldXM()
		: xmFltMax;

	bool partialShadowUpdate = Engine::GAPI->GetRendererState()->RendererSettings.PartialDynamicShadowUpdates;

	// Draw pointlight shadows
	if (Engine::GAPI->GetRendererState()->RendererSettings.EnablePointlightShadows > 0) {
		std::list<VobLightInfo*> importantUpdates;

		for (auto const& light : lights) {
			// Create shadowmap in case we should have one but haven't got it yet
			if (!light->LightShadowBuffers && light->UpdateShadows) {
				Engine::GraphicsEngine->CreateShadowedPointLight(&light->LightShadowBuffers, light);
			}

			if (light->LightShadowBuffers) {
				// Check if this lights even needs an update
				bool needsUpdate = ((D3D11PointLight*)light->LightShadowBuffers)->NeedsUpdate();
				bool wantsUpdate = ((D3D11PointLight*)light->LightShadowBuffers)->WantsUpdate();

				// Add to the updatequeue if it does
				if (needsUpdate || light->UpdateShadows) {
					// Always update the light if the light itself moved
					if (partialShadowUpdate && !needsUpdate) {
						// Only add once. This list should never be very big, so it should
						// be ok to search it like this This needs to be done to make sure a
						// light will get updated only once and won't block the queue
						if (std::find(FrameShadowUpdateLights.begin(),
							FrameShadowUpdateLights.end(),
							light) == FrameShadowUpdateLights.end()) {
							// Always render the closest light to the playervob, so the player
							// doesn't flicker when moving
							float d;
							XMStoreFloat(&d, XMVector3LengthSq(light->Vob->GetPositionWorldXM() - vPlayerPosition));

							float range = light->Vob->GetLightRange();
							if (d < range * range &&
								importantUpdates.size() < MAX_IMPORTANT_LIGHT_UPDATES) {
								importantUpdates.push_back(light);
							}
							else {
								FrameShadowUpdateLights.push_back(light);
							}
						}
					}
					else {
						// Always render the closest light to the playervob, so the player
						// doesn't flicker when moving
						float d;
						XMStoreFloat(&d, XMVector3LengthSq(light->Vob->GetPositionWorldXM() - vPlayerPosition));
						
						float range = light->Vob->GetLightRange() * 1.5f;

						// If the engine said this light should be updated, then do so. If
						// the light said this
						if (needsUpdate || d < range * range)
							importantUpdates.push_back(light);
					}
				}
			}
		}

		// Render the closest light
		for (auto const& importantUpdate : importantUpdates) {
			((D3D11PointLight*)importantUpdate->LightShadowBuffers)->RenderCubemap(importantUpdate->UpdateShadows);
			importantUpdate->UpdateShadows = false;
		}

		// Update only a fraction of lights, but at least some
		int n = std::max(
			(UINT)NUM_MIN_FRAME_SHADOW_UPDATES,
			(UINT)(FrameShadowUpdateLights.size() / NUM_FRAME_SHADOW_UPDATES));
		while (!FrameShadowUpdateLights.empty()) {
			D3D11PointLight* l = (D3D11PointLight*)FrameShadowUpdateLights.front()
				->LightShadowBuffers;

			// Check if we have to force this light to update itself (NPCs moving
			// around, for example)
			bool force = FrameShadowUpdateLights.front()->UpdateShadows;
			FrameShadowUpdateLights.front()->UpdateShadows = false;

			l->RenderCubemap(force);
			DebugPointlight = l;

			FrameShadowUpdateLights.pop_front();

			// Only update n lights
			n--;
			if (n <= 0) break;
		}
	}

	// Get shadow direction, but don't update every frame, to get around
	// flickering
	XMVECTOR dir = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos;
	static XMVECTOR oldDir = dir;
	static XMVECTOR smoothDir = dir;

	static XMVECTOR oldP = XMVectorSet(0, 0, 0, 0);


	// Update dir
	XMVECTOR target = dir;

	float dotDir;
	XMStoreFloat(&dotDir, XMVector3Dot(smoothDir, dir));
	// Smoothly transition to the next state and wait there
	if (fabs(dotDir) < 0.99995f)  // but cut it off somewhere or the pixels will flicker
	{
		dir = XMVectorLerp(smoothDir, target, Engine::GAPI->GetFrameTimeSec() * 2.0f);
	}
	else
		oldDir = dir;

	smoothDir = dir;

	XMVECTOR WorldShadowCP;
	// Update position
	// Try to update only if the camera went 500 units away from
	// the last position
	float len;
	XMStoreFloat(&len, XMVector3Length(oldP - XMLoadFloat3(&cameraPosition)));
	if ((len < 64 &&
		// And is on even space
		(cameraPosition.x - (int)cameraPosition.x) < 0.1f &&
		// but don't let it go too far
		(cameraPosition.y - (int)cameraPosition.y) < 0.1f) || len < 600.0f)
	{
		WorldShadowCP = oldP;
	}
	else {
		oldP = XMLoadFloat3(&cameraPosition);
		WorldShadowCP = oldP;
	}

	XMVECTOR p                 = WorldShadowCP;
	// Set the camera height to the highest point in this section
	p += dir * 6000.0f;

	XMVECTOR lookAt = p;
	lookAt -= dir;

	// Create shadowmap view-matrix
	XMMATRIX crViewRepl = XMMatrixLookAtLH(p, lookAt, XMVectorSet(0, 1, 0, 0));
	crViewRepl = XMMatrixTranspose(crViewRepl);

	XMMATRIX crProjRepl =
	XMMatrixOrthographicLH(
		WorldShadowmap1->GetSizeX()* Engine::GAPI->GetRendererState()->RendererSettings.WorldShadowRangeScale,
		WorldShadowmap1->GetSizeX()* Engine::GAPI->GetRendererState()->RendererSettings.WorldShadowRangeScale,
		1,
		20000.0f);

	crProjRepl = XMMatrixTranspose(crProjRepl);

	XMStoreFloat4x4(&cr.ViewReplacement, crViewRepl);
	XMStoreFloat4x4(&cr.ProjectionReplacement, crProjRepl);
	XMStoreFloat3(&cr.PositionReplacement, p);
	XMStoreFloat3(&cr.LookAtReplacement, lookAt);

	// Replace gothics camera
	Engine::GAPI->SetCameraReplacementPtr(&cr);

	// Indoor worlds don't need shadowmaps for the world
	if (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR)
	{
		RenderShadowmaps(WorldShadowCP, nullptr, true);
	}

	SetDefaultStates();

	// Restore gothics camera
	Engine::GAPI->SetCameraReplacementPtr(nullptr);

	// Draw rainmap, if raining
	if (Engine::GAPI->GetSceneWetness() > 0.00001f) {
		Effects->DrawRainShadowmap();
	}

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);

	// ********************************
	// Draw direct lighting
	// ********************************
	SetActiveVertexShader("VS_ExPointLight");
	SetActivePixelShader("PS_DS_PointLight");

	D3D11PShader* psPointLight          = ShaderManager->GetPShader("PS_DS_PointLight");
	D3D11PShader* psPointLightDynShadow = ShaderManager->GetPShader("PS_DS_PointLightDynShadow");

	Engine::GAPI->SetFarPlane(
		Engine::GAPI->GetRendererState()->RendererSettings.SectionDrawRadius *
		WORLD_SECTION_SIZE);

	Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	// Copy this, so we can access depth in the pixelshader and still use the
	// buffer for culling
	CopyDepthStencil();

	// Set the main rendertarget
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(), DepthStencilBuffer->GetDepthStencilView());

	view = XMMatrixTranspose(view);

	DS_PointLightConstantBuffer plcb = {};

	XMStoreFloat4x4(&plcb.PL_InvProj, XMMatrixInverse(nullptr, XMLoadFloat4x4(&Engine::GAPI->GetProjectionMatrixDX())));
	XMStoreFloat4x4(&plcb.PL_InvView, XMMatrixInverse(nullptr, XMLoadFloat4x4(&Engine::GAPI->GetRendererState()->TransformState.TransformView)));

	plcb.PL_ViewportSize = float2(static_cast<float>(Resolution.x), static_cast<float>(Resolution.y));

	GBuffer0_Diffuse->BindToPixelShader(GetContext(), 0);
	GBuffer1_Normals_SpecIntens_SpecPower->BindToPixelShader(GetContext(), 1);
	DepthStencilBufferCopy->BindToPixelShader(GetContext(), 2);

	bool lastOutside = true;

	// Draw all lights
	for (auto const& light : lights) {
		zCVobLight* vob = light->Vob;

		// Reset state from CollectVisibleVobs
		light->VisibleInRenderPass = false;

		if (!vob->IsEnabled()) continue;

		// Set right shader
		if (Engine::GAPI->GetRendererState()
			->RendererSettings.EnablePointlightShadows > 0) {
			if (light->LightShadowBuffers && ActivePS != psPointLightDynShadow) {
				// Need to update shader for shadowed pointlight
				ActivePS = psPointLightDynShadow;
				ActivePS->Apply();
			}
			else if (!light->LightShadowBuffers && ActivePS != psPointLight) {
				// Need to update shader for usual pointlight
				ActivePS = psPointLight;
				ActivePS->Apply();
			}
		}

		// Animate the light
		vob->DoAnimation();

		plcb.PL_Color = float4(vob->GetLightColor());
		plcb.PL_Range = vob->GetLightRange();
		plcb.Pl_PositionWorld = vob->GetPositionWorld();
		plcb.PL_Outdoor = light->IsIndoorVob ? 0.0f : 1.0f;

		float dist;
		XMStoreFloat(&dist, 
			XMVector3Length(
				XMLoadFloat3(plcb.Pl_PositionWorld.toXMFLOAT3()) - Engine::GAPI->GetCameraPositionXM()));

		// Gradually fade in the lights
		if (dist + plcb.PL_Range <
			Engine::GAPI->GetRendererState()->RendererSettings.VisualFXDrawRadius) {
			// float fadeStart =
			// Engine::GAPI->GetRendererState()->RendererSettings.VisualFXDrawRadius -
			// plcb.PL_Range;
			float fadeEnd =
				Engine::GAPI->GetRendererState()->RendererSettings.VisualFXDrawRadius;

			float fadeFactor = std::min(
				1.0f,
				std::max(0.0f, ((fadeEnd - (dist + plcb.PL_Range)) / plcb.PL_Range)));
			plcb.PL_Color.x *= fadeFactor;
			plcb.PL_Color.y *= fadeFactor;
			plcb.PL_Color.z *= fadeFactor;
			// plcb.PL_Color.w *= fadeFactor;
		}

		// Make the lights a little bit brighter
		float lightFactor = 1.2f;

		plcb.PL_Color.x *= lightFactor;
		plcb.PL_Color.y *= lightFactor;
		plcb.PL_Color.z *= lightFactor;

		// Need that in view space
		XMVECTOR Pl_PositionWorld = XMLoadFloat3(plcb.Pl_PositionWorld.toXMFLOAT3());
		XMStoreFloat3(plcb.Pl_PositionView.toXMFLOAT3(),
			XMVector3TransformCoord(Pl_PositionWorld, view));

		XMStoreFloat3(plcb.PL_LightScreenPos.toXMFLOAT3(),
			XMVector3TransformCoord(Pl_PositionWorld,
				XMLoadFloat4x4(&Engine::GAPI->GetProjectionMatrixDX())));

		if (dist < plcb.PL_Range) {
			if (Engine::GAPI->GetRendererState()->DepthState.DepthBufferEnabled) {
				Engine::GAPI->GetRendererState()->DepthState.DepthBufferEnabled = false;
				Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
					GothicRasterizerStateInfo::CM_CULL_FRONT;
				Engine::GAPI->GetRendererState()->DepthState.SetDirty();
				Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();
				UpdateRenderStates();
			}
		}
		else {
			if (!Engine::GAPI->GetRendererState()->DepthState.DepthBufferEnabled) {
				Engine::GAPI->GetRendererState()->DepthState.DepthBufferEnabled = true;
				Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
					GothicRasterizerStateInfo::CM_CULL_BACK;
				Engine::GAPI->GetRendererState()->DepthState.SetDirty();
				Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();
				UpdateRenderStates();
			}
		}

		plcb.PL_LightScreenPos.x = plcb.PL_LightScreenPos.x / 2.0f + 0.5f;
		plcb.PL_LightScreenPos.y = plcb.PL_LightScreenPos.y / -2.0f + 0.5f;

		// Apply the constantbuffer to vs and PS
		ActivePS->GetConstantBuffer()[0]->UpdateBuffer(&plcb);
		ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);
		ActivePS->GetConstantBuffer()[0]->BindToVertexShader(
			1);  // Bind this instead of the usual per-instance buffer

		if (Engine::GAPI->GetRendererState()
			->RendererSettings.EnablePointlightShadows > 0) {
			// Bind shadowmap, if possible
			if (light->LightShadowBuffers)
				((D3D11PointLight*)light->LightShadowBuffers)->OnRenderLight();
		}

		// Draw the mesh
		InverseUnitSphereMesh->DrawMesh();

		Engine::GAPI->GetRendererState()->RendererInfo.FrameDrawnLights++;
	}

	// *cough cough* somehow this makes the worldmesh disappear in indoor
	// locations
	// TODO: Fixme
	bool oldState =
		Engine::GAPI->GetRendererState()->RendererSettings.EnableSMAA;
	Engine::GAPI->GetRendererState()->RendererSettings.EnableSMAA = false;

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_BACK;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	// Modify light when raining
	float rain = Engine::GAPI->GetRainFXWeight();
	float wetness = Engine::GAPI->GetSceneWetness();

	// Switch global light shader when raining
	if (wetness > 0.0f) {
		SetActivePixelShader("PS_DS_AtmosphericScattering_Rain");
	}
	else {
		SetActivePixelShader("PS_DS_AtmosphericScattering");
	}

	SetActiveVertexShader("VS_PFX");

	SetupVS_ExMeshDrawCall();

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	DS_ScreenQuadConstantBuffer scb = {};
	scb.SQ_InvProj = plcb.PL_InvProj;
	scb.SQ_InvView = plcb.PL_InvView;
	scb.SQ_View = Engine::GAPI->GetRendererState()->TransformState.TransformView;

	XMStoreFloat3(scb.SQ_LightDirectionVS.toXMFLOAT3(), 
		XMVector3TransformNormal(sky->GetAtmosphereCB().AC_LightPos, view));

	float3 sunColor =
		Engine::GAPI->GetRendererState()->RendererSettings.SunLightColor;

	float sunStrength = Toolbox::lerp(
		Engine::GAPI->GetRendererState()->RendererSettings.SunLightStrength,
		Engine::GAPI->GetRendererState()->RendererSettings.RainSunLightStrength,
		std::min(
			1.0f,
			rain * 2.0f));  // Scale the darkening-factor faster here, so it
							// matches more with the increasing fog-density

	scb.SQ_LightColor = float4(sunColor.x, sunColor.y, sunColor.z, sunStrength);

	scb.SQ_ShadowView = cr.ViewReplacement;
	scb.SQ_ShadowProj = cr.ProjectionReplacement;
	scb.SQ_ShadowmapSize = static_cast<float>(WorldShadowmap1->GetSizeX());

	// Get rain matrix
	scb.SQ_RainView = Effects->GetRainShadowmapCameraRepl().ViewReplacement;
	scb.SQ_RainProj = Effects->GetRainShadowmapCameraRepl().ProjectionReplacement;

	scb.SQ_ShadowStrength =
		Engine::GAPI->GetRendererState()->RendererSettings.ShadowStrength;
	scb.SQ_ShadowAOStrength =
		Engine::GAPI->GetRendererState()->RendererSettings.ShadowAOStrength;
	scb.SQ_WorldAOStrength =
		Engine::GAPI->GetRendererState()->RendererSettings.WorldAOStrength;

	// Modify lightsettings when indoor
	if (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
		zBSP_MODE_INDOOR) {
		// TODO: fix caves in Gothic 1 being way too dark. Remove this workaround.
#if BUILD_GOTHIC_1_08k
		// Kirides: Nah, just make it dark enough.
		scb.SQ_ShadowStrength = 0.085f;
#else
		// Turn off shadows
		scb.SQ_ShadowStrength = 0.0f;
#endif

		// Only use world AO
		scb.SQ_WorldAOStrength = 1.0f;
		// Darken the lights
		scb.SQ_LightColor = float4(1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x);
	}

	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(&scb);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	PFXVS_ConstantBuffer vscb = {};
	vscb.PFXVS_InvProj = scb.SQ_InvProj;
	ActiveVS->GetConstantBuffer()[0]->UpdateBuffer(&vscb);
	ActiveVS->GetConstantBuffer()[0]->BindToVertexShader(0);

	WorldShadowmap1->BindToPixelShader(GetContext(), 3);

	if (Effects->GetRainShadowmap())
		Effects->GetRainShadowmap()->BindToPixelShader(GetContext(), 4);

	GetContext()->PSSetSamplers(2, 1, ShadowmapSamplerState.GetAddressOf());

	GetContext()->PSSetShaderResources(5, 1, ReflectionCube2.GetAddressOf());

	DistortionTexture->BindToPixelShader(6);

	PfxRenderer->DrawFullScreenQuad();

	Engine::GAPI->GetRendererState()->RendererSettings.EnableSMAA = oldState;

	// Reset state
	ID3D11ShaderResourceView* srv = nullptr;
	GetContext()->PSSetShaderResources(2, 1, &srv);
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());

	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = true;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_FRONT;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	UpdateRenderStates();

	return XR_SUCCESS;
}

/** Renders the shadowmaps for a pointlight */
void D3D11GraphicsEngine::RenderShadowCube(
	DirectX::FXMVECTOR position, float range,
	RenderToDepthStencilBuffer* targetCube, ID3D11DepthStencilView* face,
	ID3D11RenderTargetView* debugRTV, bool cullFront, bool indoor, bool noNPCs,
	std::list<VobInfo*>* renderedVobs,
	std::list<SkeletalVobInfo*>* renderedMobs,
	std::map<MeshKey, WorldMeshInfo*, cmpMeshKey>* worldMeshCache) {
	D3D11_VIEWPORT oldVP;
	UINT n = 1;
	GetContext()->RSGetViewports(&n, &oldVP);

	// Apply new viewport
	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	vp.Width = static_cast<float>(targetCube->GetSizeX());
	vp.Height = static_cast<float>(targetCube->GetSizeX());

	GetContext()->RSSetViewports(1, &vp);

	if (!face) {
		// Set cubemap shader
		SetActiveGShader("GS_Cubemap");
		ActiveGS->Apply();
		face = targetCube->GetDepthStencilView();

		SetActiveVertexShader("VS_ExCube");
	}

	// Set the rendering stage
	D3D11ENGINE_RENDER_STAGE oldStage = RenderingStage;
	SetRenderingStage(DES_SHADOWMAP_CUBE);

	ID3D11ShaderResourceView* srv = nullptr;
	GetContext()->PSSetShaderResources(3, 1, &srv);

	if (!debugRTV) {
		GetContext()->OMSetRenderTargets(0, nullptr, face);

		Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled =
			true;  // Should be false, but needs to be true for SV_Depth to work
		Engine::GAPI->GetRendererState()->BlendState.SetDirty();
	}
	else {
		GetContext()->OMSetRenderTargets(1, &debugRTV, face);

		Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled = true;
		Engine::GAPI->GetRendererState()->BlendState.SetDirty();
	}

	// Dont render shadows from the sun when it isn't on the sky
	if (Engine::GAPI->GetRendererState()->RendererSettings.DrawShadowGeometry &&
		Engine::GAPI->GetRendererState()->RendererSettings.EnableShadows) {
		GetContext()->ClearDepthStencilView(face, D3D11_CLEAR_DEPTH, 1.0f, 0);

		// Draw the world mesh without textures
		DrawWorldAround(position, range, cullFront, indoor, noNPCs, renderedVobs,
			renderedMobs, worldMeshCache);
	}
	else {
		if (Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0) {
			GetContext()->ClearDepthStencilView(face, D3D11_CLEAR_DEPTH, 0.0f,
				0);  // Always shadow in the night
		}
		else {
			GetContext()->ClearDepthStencilView(
				face, D3D11_CLEAR_DEPTH, 1.0f,
				0);  // Clear shadowmap when shadows not enabled
		}
	}

	// Restore state
	SetRenderingStage(oldStage);
	GetContext()->RSSetViewports(1, &oldVP);
	GetContext()->GSSetShader(nullptr, nullptr, 0);
	SetActiveVertexShader("VS_Ex");

	Engine::GAPI->SetFarPlane(
		Engine::GAPI->GetRendererState()->RendererSettings.SectionDrawRadius *
		WORLD_SECTION_SIZE);

	SetRenderingStage(DES_MAIN);
}

/** Renders the shadowmaps for the sun */
void D3D11GraphicsEngine::RenderShadowmaps(FXMVECTOR cameraPosition,
	RenderToDepthStencilBuffer* target,
	bool cullFront, bool dontCull,
	ID3D11DepthStencilView* dsvOverwrite,
	ID3D11RenderTargetView* debugRTV) {
	if (!target) {
		target = WorldShadowmap1.get();
	}

	if (!dsvOverwrite) dsvOverwrite = target->GetDepthStencilView();

	D3D11_VIEWPORT oldVP;
	UINT n = 1;
	GetContext()->RSGetViewports(&n, &oldVP);

	// Apply new viewport
	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	vp.Width = static_cast<float>(target->GetSizeX());
	vp.Height = vp.Width;
	GetContext()->RSSetViewports(1, &vp);

	// Set the rendering stage
	D3D11ENGINE_RENDER_STAGE oldStage = RenderingStage;
	SetRenderingStage(DES_SHADOWMAP);

	// Clear and Bind the shadowmap

	ID3D11ShaderResourceView* srv = nullptr;
	GetContext()->PSSetShaderResources(3, 1, &srv);

	if (!debugRTV) {
		GetContext()->OMSetRenderTargets(0, nullptr, dsvOverwrite);
		Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled = false;
	}
	else {
		GetContext()->OMSetRenderTargets(1, &debugRTV, dsvOverwrite);
		Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled = true;
	}
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	// Dont render shadows from the sun when it isn't on the sky
	if ((target != WorldShadowmap1.get() ||
		Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y >
		0) &&  // Only stop rendering if the sun is down on main-shadowmap
			   // TODO: Take this out of here!
		Engine::GAPI->GetRendererState()->RendererSettings.DrawShadowGeometry &&
		Engine::GAPI->GetRendererState()->RendererSettings.EnableShadows) {
		GetContext()->ClearDepthStencilView(dsvOverwrite, D3D11_CLEAR_DEPTH, 1.0f, 0);

		// Draw the world mesh without textures
		DrawWorldAround(cameraPosition, 2, 10000.0f, cullFront, dontCull);
	}
	else {
		if (Engine::GAPI->GetSky()->GetAtmoshpereSettings().LightDirection.y <= 0) {
			GetContext()->ClearDepthStencilView(dsvOverwrite, D3D11_CLEAR_DEPTH, 0.0f,
				0);  // Always shadow in the night
		}
		else {
			GetContext()->ClearDepthStencilView(
				dsvOverwrite, D3D11_CLEAR_DEPTH, 1.0f,
				0);  // Clear shadowmap when shadows not enabled
		}
	}

	// Restore state
	SetRenderingStage(oldStage);
	GetContext()->RSSetViewports(1, &oldVP);

	Engine::GAPI->SetFarPlane(
		Engine::GAPI->GetRendererState()->RendererSettings.SectionDrawRadius *
		WORLD_SECTION_SIZE);
}

/** Draws a fullscreenquad, copying the given texture to the viewport */
void D3D11GraphicsEngine::DrawQuad(INT2 position, INT2 size) {
	wrl::ComPtr<ID3D11ShaderResourceView> srv;
	GetContext()->PSGetShaderResources(0, 1, &srv);

	wrl::ComPtr<ID3D11RenderTargetView> rtv;
	GetContext()->OMGetRenderTargets(1, &rtv, nullptr);

	if (srv.Get()) {
		if (rtv.Get()) {
			PfxRenderer->CopyTextureToRTV(srv.Get(), rtv.Get(), size, false, position);
		}
	}
}

/** Sets the current rendering stage */
void D3D11GraphicsEngine::SetRenderingStage(D3D11ENGINE_RENDER_STAGE stage) {
	RenderingStage = stage;
}

/** Returns the current rendering stage */
D3D11ENGINE_RENDER_STAGE D3D11GraphicsEngine::GetRenderingStage() {
	return RenderingStage;
}

/** Draws a single VOB */
void D3D11GraphicsEngine::DrawVobSingle(VobInfo* vob) {
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);

	SetActivePixelShader("PS_Preview_Textured");
	SetActiveVertexShader("VS_Ex");

	SetDefaultStates();
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.DepthClipEnable = false;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();
	// SetupVS_ExPerInstanceConstantBuffer();
	// vob->VobConstantBuffer->BindToVertexShader(1);

	ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(vob->Vob->GetWorldMatrixPtr());
	ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

	InfiniteRangeConstantBuffer->BindToPixelShader(3);

	for (auto const& itm : vob->VisualInfo->Meshes) {
		for (size_t i = 0; i < itm.second.size(); i++) {
			// Cache texture
			if (itm.first) {
				if (itm.first->GetTexture()) {
					if (itm.first->GetTexture()->CacheIn(0.6f) == zRES_CACHED_IN) {
						itm.first->GetTexture()->Bind(0);

						MaterialInfo* info =
							Engine::GAPI->GetMaterialInfoFrom(itm.first->GetTexture());
						if (!info->Constantbuffer) info->UpdateConstantbuffer();

						info->Constantbuffer->BindToPixelShader(2);
					}
					else
						continue;
				}
			}
			// Draw instances
			Engine::GraphicsEngine->DrawVertexBufferIndexed(
				itm.second[i]->MeshVertexBuffer, itm.second[i]->MeshIndexBuffer,
				itm.second[i]->Indices.size());
		}
	}

	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		nullptr);
}

/** Draws a multiple VOBs (used for inventory) */
void D3D11GraphicsEngine::DrawVobsList(const std::list<VobInfo*>& vobs, zCCamera& camera) {
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);

	SetActivePixelShader("PS_Preview_Textured");
	SetActiveVertexShader("VS_Ex");

	SetDefaultStates();
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.DepthClipEnable = false;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	view = XMLoadFloat4x4(&camera.GetTransformDX(zCCamera::TT_VIEW));
	XMMATRIX worldMatrix;

	for (auto const& vob : vobs)
	{
		worldMatrix = vob->Vob->GetWorldMatrixXM();
		Engine::GAPI->SetWorldViewTransform(worldMatrix, view);
		
		ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(vob->Vob->GetWorldMatrixPtr());
		ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

		InfiniteRangeConstantBuffer->BindToPixelShader(3);

		for (auto const& itm : vob->VisualInfo->Meshes) {
			for (auto const& itm2nd :itm.second) {
				// Cache texture
				if (itm.first) {
					if (itm.first->GetTexture()) {
						if (itm.first->GetTexture()->CacheIn(0.6f) == zRES_CACHED_IN) {
							itm.first->GetTexture()->Bind(0);

							MaterialInfo* info =
								Engine::GAPI->GetMaterialInfoFrom(itm.first->GetTexture());
							if (!info->Constantbuffer) info->UpdateConstantbuffer();

							info->Constantbuffer->BindToPixelShader(2);
						}
						else
							continue;
					}
				}
				// Draw instances
				Engine::GraphicsEngine->DrawVertexBufferIndexed(
					itm2nd->MeshVertexBuffer, itm2nd->MeshIndexBuffer,
					itm2nd->Indices.size());
			}
		}
	}

	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		nullptr);
}

/** Message-Callback for the main window */
LRESULT D3D11GraphicsEngine::OnWindowMessage(HWND hWnd, UINT msg, WPARAM wParam,
	LPARAM lParam) {
	if (UIView) {
		UIView->OnWindowMessage(hWnd, msg, wParam, lParam);
	}
	return 0;
}

/** Draws the ocean */
XRESULT D3D11GraphicsEngine::DrawOcean(GOcean* ocean) {
	SetDefaultStates();

	// Then draw the ocean
	SetActivePixelShader("PS_Ocean");
	SetActiveVertexShader("VS_ExDisplace");

	// Set constant buffer
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GSky* sky = Engine::GAPI->GetSky();
	ActivePS->GetConstantBuffer()[1]->UpdateBuffer(&sky->GetAtmosphereCB());
	ActivePS->GetConstantBuffer()[1]->BindToPixelShader(1);

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
		!Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise;
	if (Engine::GAPI->GetRendererState()->RendererSettings.WireframeWorld) {
		Engine::GAPI->GetRendererState()->RasterizerState.Wireframe = true;
	}
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	// Init drawcalls
	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	GetContext()->IASetPrimitiveTopology(
		D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

	D3D11HDShader* hd = ShaderManager->GetHDShader("OceanTess");
	if (hd) hd->Apply();

	DefaultHullShaderConstantBuffer hscb = {};

	// convert to EdgesPerScreenHeight
	hscb.H_EdgesPerScreenHeight =
		GetResolution().y /
		Engine::GAPI->GetRendererState()->RendererSettings.TesselationFactor;
	hscb.H_Proj11 =
		Engine::GAPI->GetRendererState()->TransformState.TransformProj._22;
	hscb.H_GlobalTessFactor =
		Engine::GAPI->GetRendererState()->RendererSettings.TesselationFactor;
	hscb.H_ScreenResolution = float2(GetResolution().x, GetResolution().y);
	hscb.H_FarPlane = Engine::GAPI->GetFarPlane();
	hd->GetConstantBuffer()[0]->UpdateBuffer(&hscb);
	hd->GetConstantBuffer()[0]->BindToHullShader(1);

	ID3D11ShaderResourceView* tex_displacement;
	ID3D11ShaderResourceView* tex_gradient;
	ID3D11ShaderResourceView* tex_fresnel;
	ID3D11ShaderResourceView* cube_reflect = ReflectionCube.Get();
	OceanSettingsConstantBuffer ocb = {};
	ocean->GetFFTResources(&tex_displacement, &tex_gradient, &tex_fresnel, &ocb);
	ocb.OS_SunColor = Engine::GAPI->GetSky()->GetSunColor();

	if (tex_gradient) GetContext()->PSSetShaderResources(0, 1, &tex_gradient);

	if (tex_displacement) {
		GetContext()->DSSetShaderResources(0, 1, &tex_displacement);
	}

	GetContext()->PSSetShaderResources(1, 1, &tex_fresnel);
	GetContext()->PSSetShaderResources(3, 1, &cube_reflect);

	// Scene information is still bound from rendering water surfaces

	GetContext()->PSSetSamplers(1, 1, ClampSamplerState.GetAddressOf());
	GetContext()->PSSetSamplers(2, 1, CubeSamplerState.GetAddressOf());

	// Update constantbuffer
	ActivePS->GetConstantBuffer()[2]->UpdateBuffer(&ocb);
	ActivePS->GetConstantBuffer()[2]->BindToPixelShader(4);

	// DistortionTexture->BindToPixelShader(0);

	RefractionInfoConstantBuffer ricb = {};
	ricb.RI_Projection = Engine::GAPI->GetProjectionMatrixDX();
	ricb.RI_ViewportSize = float2(Resolution.x, Resolution.y);
	ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
	ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();

	ActivePS->GetConstantBuffer()[4]->UpdateBuffer(&ricb);
	ActivePS->GetConstantBuffer()[4]->BindToPixelShader(2);

	// Bind distortion texture
	DistortionTexture->BindToPixelShader(4);

	// Bind copied backbuffer
	GetContext()->PSSetShaderResources(
		5, 1, PfxRenderer->GetTempBuffer()->GetShaderResViewPtr());

	// Bind depth to the shader
	DepthStencilBufferCopy->BindToPixelShader(GetContext(), 2);

	std::vector<XMFLOAT3> patches;
	ocean->GetPatchLocations(patches);

	XMMATRIX viewMatrix = Engine::GAPI->GetViewMatrixXM();
	viewMatrix = XMMatrixTranspose(viewMatrix);

	for (auto const& patch : patches) {
		XMMATRIX scale, world;
		scale = XMMatrixIdentity();
		scale = XMMatrixScaling(OCEAN_PATCH_SIZE,OCEAN_PATCH_SIZE,OCEAN_PATCH_SIZE);

		world = XMMatrixTranslation(patch.x, patch.y, patch.z);
		world = scale * world;
		world = XMMatrixTranspose(world);
		ActiveVS->GetConstantBuffer()[1]->UpdateBuffer(&world);
		ActiveVS->GetConstantBuffer()[1]->BindToVertexShader(1);

		XMVECTOR localEye = XMVectorSet(0, 0, 0, 0);
		world = XMMatrixTranspose(world);

		localEye = XMVector3TransformCoord(localEye, XMMatrixInverse(nullptr, world * viewMatrix));

		OceanPerPatchConstantBuffer opp;
		XMFLOAT3 localEye3; XMStoreFloat3(&localEye3, localEye);
		opp.OPP_LocalEye = localEye3;
		opp.OPP_PatchPosition = patch;
		ActivePS->GetConstantBuffer()[3]->UpdateBuffer(&opp);
		ActivePS->GetConstantBuffer()[3]->BindToPixelShader(3);

		ocean->GetPlaneMesh()->DrawMesh();
	}

	if (Engine::GAPI->GetRendererState()->RendererSettings.WireframeWorld) {
		Engine::GAPI->GetRendererState()->RasterizerState.Wireframe = false;
	}

	Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise =
		!Engine::GAPI->GetRendererState()->RasterizerState.FrontCounterClockwise;
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_BACK;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	GetContext()->PSSetSamplers(2, 1, ShadowmapSamplerState.GetAddressOf());

	SetActivePixelShader("PS_World");
	SetActiveVertexShader("VS_Ex");

	GetContext()->HSSetShader(nullptr, nullptr, 0);
	GetContext()->DSSetShader(nullptr, nullptr, 0);
	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	return XR_SUCCESS;
}

/** Constructs the makro list for shader compilation */
void D3D11GraphicsEngine::ConstructShaderMakroList(
	std::vector<D3D_SHADER_MACRO >& list) {
	GothicRendererSettings& s =
		Engine::GAPI->GetRendererState()->RendererSettings;
	D3D_SHADER_MACRO m;

	m.Name = "SHD_ENABLE";
	m.Definition = s.EnableShadows ? "1" : "0";
	list.push_back(m);

	m.Name = "SHD_FILTER_16TAP_PCF";
	m.Definition = s.EnableSoftShadows ? "1" : "0";
	list.push_back(m);

	m.Name = nullptr;
	m.Definition = nullptr;
	list.push_back(m);
}

/** Handles an UI-Event */
void D3D11GraphicsEngine::OnUIEvent(EUIEvent uiEvent) {
	if (!UIView) {
		CreateMainUIView();
	}

	if (uiEvent == UI_OpenSettings) {
		if (UIView) {
			// Show settings
			UIView->GetSettingsDialog()->SetHidden(
				!UIView->GetSettingsDialog()->IsHidden());

			// Free mouse
			Engine::GAPI->SetEnableGothicInput(
				UIView->GetSettingsDialog()->IsHidden());
		}
	}
	else if (uiEvent == UI_OpenEditor) {
		if (UIView) {
			// Show settings
			Engine::GAPI->GetRendererState()->RendererSettings.EnableEditorPanel =
				true;

			// Free mouse
			Engine::GAPI->SetEnableGothicInput(
				UIView->GetSettingsDialog()->IsHidden());
		}
	}
}

/** Returns the data of the backbuffer */
void D3D11GraphicsEngine::GetBackbufferData(byte** data, int& pixelsize) {
	constexpr int width = 256;
	byte* d = new byte[width * width * 4];

	// Copy HDR scene to backbuffer
	SetDefaultStates();

	SetActivePixelShader("PS_PFX_GammaCorrectInv");
	ActivePS->Apply();

	GammaCorrectConstantBuffer gcb = {};
	gcb.G_Gamma = Engine::GAPI->GetGammaValue();
	gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();
	gcb.G_TextureSize = GetResolution();
	gcb.G_SharpenStrength =
		Engine::GAPI->GetRendererState()->RendererSettings.SharpenFactor;

	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(&gcb);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	HRESULT hr;

	// Buffer for scaling down the image
	auto rt = std::make_unique<RenderToTextureBuffer>(
		GetDevice(), width, width, DXGI_FORMAT_R8G8B8A8_UNORM);
	
	// Downscale to 256x256
	PfxRenderer->CopyTextureToRTV(HDRBackBuffer->GetShaderResView(),
		rt->GetRenderTargetView(), INT2(width, width),
		true);

	D3D11_TEXTURE2D_DESC texDesc;
	texDesc.ArraySize = 1;
	texDesc.BindFlags = 0;
	texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.Width = width;  // Gothic transforms the backbufferdata for
							// savegamethumbs to 256x256-pictures anyways
	texDesc.Height = width;
	texDesc.MipLevels = 1;
	texDesc.MiscFlags = 0;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_STAGING;

	wrl::ComPtr<ID3D11Texture2D> texture;
	LE(GetDevice()->CreateTexture2D(&texDesc, 0, &texture));
	if (!texture.Get())
	{
		LogInfo() << "Thumbnail failed. Texture could not be created";
		return;
	}
	GetContext()->CopyResource(texture.Get(), rt->GetTexture());

	// Get data
	D3D11_MAPPED_SUBRESOURCE res;
	if (SUCCEEDED(GetContext()->Map(texture.Get(), 0, D3D11_MAP_READ, 0, &res))) {
		memcpy(d, res.pData, width * width * 4);
		GetContext()->Unmap(texture.Get(), 0);
	}
	else {
		LogInfo() << "Thumbnail failed";
	}

	pixelsize = 4;
	*data = d;
}

/** Binds the right shader for the given texture */
void D3D11GraphicsEngine::BindShaderForTexture(zCTexture* texture,
	bool forceAlphaTest,
	int zMatAlphaFunc) {
	D3D11PShader* active = ActivePS;
	D3D11PShader* newShader = ActivePS;

	bool blendAdd = zMatAlphaFunc == zMAT_ALPHA_FUNC_ADD;
	bool blendBlend = zMatAlphaFunc == zMAT_ALPHA_FUNC_BLEND;
	bool linZ = (Engine::GAPI->GetRendererState()->GraphicsState.FF_GSwitches &
		GSWITCH_LINEAR_DEPTH) != 0;

	if (linZ) {
		newShader = PS_LinDepth;
	}
	else if (blendAdd || blendBlend) {
		newShader = PS_Simple;
	}
	else if (texture->HasAlphaChannel() || forceAlphaTest) {
		if (texture->GetSurface()->GetFxMap()) {
			newShader = PS_DiffuseNormalmappedAlphatestFxMap;
		}
		else {
			newShader = PS_DiffuseNormalmappedAlphatest;
		}
	}
	else {
		if (texture->GetSurface()->GetFxMap()) {
			newShader = PS_DiffuseNormalmappedFxMap;
		}
		else {
			newShader = PS_DiffuseNormalmapped;
		}
	}

	// Bind, if changed
	if (active != newShader) {
		ActivePS = newShader;
		ActivePS->Apply();
	}
}

/** Draws the given list of decals */
void D3D11GraphicsEngine::DrawDecalList(const std::vector<zCVob*>& decals,
	bool lighting) {
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = false;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();

	// Set up alpha
	if (!lighting) {
		SetActivePixelShader("PS_Simple");
		Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();
	}
	else {
		SetActivePixelShader("PS_DiffuseAlphaTest");
	}

	SetActiveVertexShader("VS_Decal");

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	int lastAlphaFunc = -1;
	for (unsigned int i = 0; i < decals.size(); i++) {
		zCDecal* d = (zCDecal*)decals[i]->GetVisual();

		if (!d) {
			continue;
		}

		if (lighting && !d->GetAlphaTestEnabled())
			continue;  // Only allow no alpha or alpha test

		if (!lighting) {
			switch (d->GetDecalSettings()->DecalMaterial->GetAlphaFunc()) {
				case zMAT_ALPHA_FUNC_BLEND:
					Engine::GAPI->GetRendererState()->BlendState.SetDefault();
					Engine::GAPI->GetRendererState()->BlendState.BlendEnabled = true;
					break;

				case zMAT_ALPHA_FUNC_ADD:
					Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();
					break;

					/*case zRND_ALPHA_FUNC_MUL:
							Engine::GAPI->GetRendererState()->BlendState.SetModulateBlending();
							break;*/
							// TODO: TODO: Implement modulate

				default:
					continue;
			}

			if (lastAlphaFunc !=
				d->GetDecalSettings()->DecalMaterial->GetAlphaFunc()) {
				Engine::GAPI->GetRendererState()->BlendState.SetDirty();
				UpdateRenderStates();
				lastAlphaFunc = d->GetDecalSettings()->DecalMaterial->GetAlphaFunc();
			}
		}

		int alignment = decals[i]->GetAlignment();

		XMMATRIX world = decals[i]->GetWorldMatrixXM();

		XMMATRIX offset =
			XMMatrixTranslation(d->GetDecalSettings()->DecalOffset.x, -d->GetDecalSettings()->DecalOffset.y, 0);

		XMMATRIX scale =
			XMMatrixScaling(d->GetDecalSettings()->DecalSize.x * 2,
				-d->GetDecalSettings()->DecalSize.y * 2,
				1);

		scale = XMMatrixTranspose(scale);

		XMMATRIX mat = view * world;
		
		if (alignment == zVISUAL_CAM_ALIGN_FULL ||
			alignment == zVISUAL_CAM_ALIGN_YAW) {
			for (int x = 0; x < 3; x++) {
				for (int y = 0; y < 3; y++) {
					if (x == y)
						mat.r[x].m128_f32[y] = 1.0f;
					else
						mat.r[x].m128_f32[y] = 0.0f;
				}
			}
		}

		mat = mat * offset * scale;

		ParticleInstanceInfo ii;
		ii.scale = float2(50, 50);
		ii.color = 0xFFFFFFFF;

		Engine::GAPI->SetWorldTransformXM(mat);
		SetupVS_ExPerInstanceConstantBuffer();

		if (d->GetDecalSettings()->DecalMaterial) {
			if (d->GetDecalSettings()->DecalMaterial->GetTexture()) {
				if (d->GetDecalSettings()->DecalMaterial->GetTexture()->CacheIn(0.6f) !=
					zRES_CACHED_IN) {
					continue;  // Don't render not cached surfaces
				}

				d->GetDecalSettings()->DecalMaterial->BindTexture(0);
			}
		}

		DrawVertexBufferIndexed(QuadVertexBuffer, QuadIndexBuffer, 6);
	}

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_BACK;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled = true;
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.BlendEnabled = true;
}

/** Draws quadmarks in a simple way */
void D3D11GraphicsEngine::DrawQuadMarks() {
	XMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
	const stdext::unordered_map<zCQuadMark*, QuadMarkInfo>& quadMarks =
		Engine::GAPI->GetQuadMarks();

	SetActiveVertexShader("VS_Ex");
	SetActivePixelShader("PS_World");

	SetDefaultStates();

	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(
		&Engine::GAPI->GetRendererState()->GraphicsState);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(0);

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	Engine::GAPI->SetViewTransformXM(view);

	SetupVS_ExMeshDrawCall();
	SetupVS_ExConstantBuffer();

	int alphaFunc = 0;
	for (auto const& it : quadMarks) {
		if (!it.first->GetConnectedVob()) continue;

		float len; XMStoreFloat(&len, XMVector3Length(camPos - XMLoadFloat3(it.second.Position.toXMFLOAT3())));
		if (len > Engine::GAPI->GetRendererState()->RendererSettings.VisualFXDrawRadius)
			continue;

		zCMaterial* mat = it.first->GetMaterial();
		if (mat) mat->BindTexture(0);

		if (alphaFunc != mat->GetAlphaFunc()) {
			// Change alpha-func
			switch (mat->GetAlphaFunc()) {
				case zMAT_ALPHA_FUNC_ADD:
					Engine::GAPI->GetRendererState()->BlendState.SetAdditiveBlending();
					break;

				case zMAT_ALPHA_FUNC_BLEND:
					Engine::GAPI->GetRendererState()->BlendState.SetAlphaBlending();
					break;

				case zMAT_ALPHA_FUNC_FUNC_NONE:
				case zMAT_ALPHA_FUNC_TEST:
					Engine::GAPI->GetRendererState()->BlendState.SetDefault();
					break;

					/*case zRND_ALPHA_FUNC_MUL:
							Engine::GAPI->GetRendererState()->BlendState.SetModulateBlending();
							break;*/
							// TODO: TODO: Implement modulate

				default:
					continue;
			}

			alphaFunc = mat->GetAlphaFunc();

			Engine::GAPI->GetRendererState()->BlendState.SetDirty();
			UpdateRenderStates();
		}

		Engine::GAPI->SetWorldTransformXM(it.first->GetConnectedVob()->GetWorldMatrixXM());
		SetupVS_ExPerInstanceConstantBuffer();

		Engine::GraphicsEngine->DrawVertexBuffer(it.second.Mesh,
			it.second.NumVertices);
	}
}

/** Copies the depth stencil buffer to DepthStencilBufferCopy */
void D3D11GraphicsEngine::CopyDepthStencil() {
	GetContext()->CopyResource(DepthStencilBufferCopy->GetTexture(), DepthStencilBuffer->GetTexture());
}

/** Draws underwater effects */
void D3D11GraphicsEngine::DrawUnderwaterEffects() {
	SetDefaultStates();

	RefractionInfoConstantBuffer ricb = {};
	ricb.RI_Projection = Engine::GAPI->GetProjectionMatrixDX();
	ricb.RI_ViewportSize = float2(Resolution.x, Resolution.y);
	ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
	ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();

	// Set up water final copy
	SetActivePixelShader("PS_PFX_UnderwaterFinal");
	ActivePS->GetConstantBuffer()[0]->UpdateBuffer(&ricb);
	ActivePS->GetConstantBuffer()[0]->BindToPixelShader(3);

	DistortionTexture->BindToPixelShader(2);
	DepthStencilBufferCopy->BindToPixelShader(GetContext(), 3);

	PfxRenderer->BlurTexture(HDRBackBuffer.get(), false, 0.10f, UNDERWATER_COLOR_MOD,
		"PS_PFX_UnderwaterFinal");
}

/** Creates the main UI-View */
void D3D11GraphicsEngine::CreateMainUIView() {
	if (!UIView) {
		UIView = std::make_unique<D2DView>();

		wrl::ComPtr<ID3D11Texture2D> tex;
		BackbufferRTV->GetResource((ID3D11Resource * *)tex.ReleaseAndGetAddressOf());
		if (XR_SUCCESS != UIView->Init(Resolution, tex.Get())) {
			UIView.reset();
			return;
		}
	}
}

/** Sets up everything for a PNAEN-Mesh */
void D3D11GraphicsEngine::Setup_PNAEN(EPNAENRenderMode mode) {
	D3D11HDShader* pnaen = ShaderManager->GetHDShader("PNAEN_Tesselation");

	if (mode == PNAEN_Instanced)
		SetActiveVertexShader("VS_PNAEN_Instanced");
	else if (mode == PNAEN_Default)
		SetActiveVertexShader("VS_PNAEN");
	else if (mode == PNAEN_Skeletal)
		SetActiveVertexShader("VS_PNAEN_Skeletal");

	ActiveVS->Apply();

	ActiveHDS = pnaen;
	pnaen->Apply();

	PNAENConstantBuffer cb = {};
	cb.f4Eye = Engine::GAPI->GetCameraPosition();
	cb.adaptive = INT4(1, 0, 0, 0);
	cb.clipping = INT4(Engine::GAPI->GetRendererState()
		->RendererSettings.TesselationFrustumCulling
		? 1
		: 0,
		0, 0, 0);

	float f =
		Engine::GAPI->GetRendererState()->RendererSettings.TesselationFactor;
	cb.f4TessFactors = float4(
		f, f, Engine::GAPI->GetRendererState()->RendererSettings.TesselationRange,
		Engine::GAPI->GetRendererState()->RendererSettings.TesselationFactor);
	cb.f4ViewportScale.x = static_cast<float>(GetResolution().x / 2);
	cb.f4ViewportScale.y = static_cast<float>(GetResolution().y / 2);
	cb.f4x4Projection = Engine::GAPI->GetProjectionMatrixDX();

	pnaen->GetConstantBuffer()[0]->UpdateBuffer(&cb);

	pnaen->GetConstantBuffer()[0]->BindToDomainShader(0);
	pnaen->GetConstantBuffer()[0]->BindToHullShader(0);

	GetContext()->IASetPrimitiveTopology(
		D3D11_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST);
}

/** Draws particle effects */
void D3D11GraphicsEngine::DrawFrameParticles(
	std::map<zCTexture*, std::vector<ParticleInstanceInfo>>& particles,
	std::map<zCTexture*, ParticleRenderInfo>& info) {
	SetDefaultStates();

	// TODO: Maybe make particles draw at a lower res and bilinear upsample the result.

	// Clear GBuffer0 to hold the refraction vectors since it's not needed anymore
	GetContext()->ClearRenderTargetView(GBuffer0_Diffuse->GetRenderTargetView(),
		(float*)& float4(0.0f, 0.0f, 0.0f, 0.0f));
	GetContext()->ClearRenderTargetView(
		GBuffer1_Normals_SpecIntens_SpecPower->GetRenderTargetView(),
		(float*)& float4(0.0f, 0.0f, 0.0f, 0.0f));

	D3D11PShader* distPS = ShaderManager->GetPShader("PS_ParticleDistortion");

	RefractionInfoConstantBuffer ricb = {};
	ricb.RI_Projection = Engine::GAPI->GetProjectionMatrixDX();
	ricb.RI_ViewportSize = float2(Resolution.x, Resolution.y);
	ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
	ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();
	ricb.RI_Far = Engine::GAPI->GetFarPlane();

	distPS->GetConstantBuffer()[0]->UpdateBuffer(&ricb);
	distPS->GetConstantBuffer()[0]->BindToPixelShader(0);

	GothicBlendStateInfo bsi = Engine::GAPI->GetRendererState()->BlendState;
	GothicRendererState& state = *Engine::GAPI->GetRendererState();

	state.BlendState.SetAdditiveBlending();
	state.BlendState.SetDirty();

	state.DepthState.DepthWriteEnabled = false;
	state.DepthState.SetDirty();
	state.DepthState.DepthBufferCompareFunc =
		GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;

	state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
	state.RasterizerState.SetDirty();

	std::vector<std::tuple<zCTexture*, ParticleRenderInfo*,
		std::vector<ParticleInstanceInfo>* >>
		pvec;
	for (auto&& textureParticle : particles) {
		if (textureParticle.second.empty()) continue;

		pvec.push_back(std::make_tuple(textureParticle.first, &info[textureParticle.first], &textureParticle.second));
	}

	struct cmp {
		static bool cmppt(
			const std::tuple<zCTexture*, ParticleRenderInfo*,
			std::vector<ParticleInstanceInfo>*>& a,
			const std::tuple<zCTexture*, ParticleRenderInfo*,
			std::vector<ParticleInstanceInfo>*>& b) {
			// Sort additive before blend
			return std::get<1>(a)->BlendMode > std::get<1>(b)->BlendMode;
		}
	};

	// Sort additive before blend
	std::sort(pvec.begin(), pvec.end(), cmp::cmppt);

	SetActivePixelShader("PS_ParticleDistortion");
	ActivePS->Apply();

	ID3D11RenderTargetView* rtv[] = {
		GBuffer0_Diffuse->GetRenderTargetView(),
		GBuffer1_Normals_SpecIntens_SpecPower->GetRenderTargetView()};
	GetContext()->OMSetRenderTargets(2, rtv,
		DepthStencilBuffer->GetDepthStencilView());

	int lastBlendMode = -1;

	// Bind view/proj
	SetupVS_ExConstantBuffer();

	// Setup GS
	GS_Billboard->Apply();

	ParticleGSInfoConstantBuffer gcb = {};
	gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
	GS_Billboard->GetConstantBuffer()[0]->UpdateBuffer(&gcb);
	GS_Billboard->GetConstantBuffer()[0]->BindToGeometryShader(2);

	SetActiveVertexShader("VS_ParticlePoint");
	ActiveVS->Apply();

	// Rendering points only
	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

	UpdateRenderStates();

	for (auto const& textureParticleRenderInfo : pvec) {
		zCTexture* tx = std::get<0>(textureParticleRenderInfo);
		ParticleRenderInfo& info = *std::get<1>(textureParticleRenderInfo);
		std::vector<ParticleInstanceInfo>& instances = *std::get<2>(textureParticleRenderInfo);

		if (instances.empty()) continue;

		if (tx) {
			// Bind it
			if (tx->CacheIn(0.6f) == zRES_CACHED_IN)
				tx->Bind(0);
			else
				continue;
		}

		GothicBlendStateInfo& blendState = info.BlendState;

		// This only happens once or twice, since the input list is sorted
		if (info.BlendMode != lastBlendMode) {
			// Setup blend state
			state.BlendState = blendState;
			state.BlendState.SetDirty();

			lastBlendMode = info.BlendMode;

			if (info.BlendMode == zRND_ALPHA_FUNC_ADD) {
				// Set Distortion-Rendering for additive blending
				SetActivePixelShader("PS_ParticleDistortion");
				ActivePS->Apply();

				ID3D11RenderTargetView* rtv[] = {
					GBuffer0_Diffuse->GetRenderTargetView(),
					GBuffer1_Normals_SpecIntens_SpecPower->GetRenderTargetView()};
				GetContext()->OMSetRenderTargets(2, rtv,
					DepthStencilBuffer->GetDepthStencilView());
			}
			else {
				// Set usual rendering for everything else. Alphablending mostly.
				SetActivePixelShader("PS_Simple");
				PS_Simple->Apply();

				GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
					DepthStencilBuffer->GetDepthStencilView());
			}
		}

		// Push data for the particles to the GPU
		D3D11_BUFFER_DESC desc;
		TempVertexBuffer->GetVertexBuffer()->GetDesc(&desc);

		if (desc.ByteWidth < sizeof(ParticleInstanceInfo) * instances.size()) {
			if (Engine::GAPI->GetRendererState()->RendererSettings.EnableDebugLog)
				LogInfo() << "(PARTICLE) TempVertexBuffer too small (" << desc.ByteWidth
				<< "), need " << sizeof(ParticleInstanceInfo) * instances.size()
				<< " bytes. Recreating buffer.";

			// Buffer too small, recreate it
			TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();

			TempVertexBuffer->Init(
				nullptr, sizeof(ParticleInstanceInfo) * instances.size(),
				D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
				D3D11VertexBuffer::CA_WRITE);
		}

		TempVertexBuffer->UpdateBuffer(
			&instances[0], sizeof(ParticleInstanceInfo) * instances.size());

		DrawVertexBuffer(TempVertexBuffer.get(), instances.size(),
			sizeof(ParticleInstanceInfo));
	}

	GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	GetContext()->GSSetShader(nullptr, 0, 0);

	// Set usual rendertarget again
	GetContext()->OMSetRenderTargets(1, HDRBackBuffer->GetRenderTargetViewPtr(),
		DepthStencilBuffer->GetDepthStencilView());

	state.BlendState.SetDefault();
	state.BlendState.SetDirty();

	GBuffer0_Diffuse->BindToPixelShader(GetContext(), 1);
	GBuffer1_Normals_SpecIntens_SpecPower->BindToPixelShader(GetContext(), 2);

	// Copy scene behind the particle systems
	PfxRenderer->CopyTextureToRTV(
		HDRBackBuffer->GetShaderResView(),
		PfxRenderer->GetTempBuffer()->GetRenderTargetView());

	SetActivePixelShader("PS_PFX_ApplyParticleDistortion");
	ActivePS->Apply();

	// Copy it back, putting distortion behind it
	PfxRenderer->CopyTextureToRTV(
		PfxRenderer->GetTempBuffer()->GetShaderResView(),
		HDRBackBuffer->GetRenderTargetView(), INT2(0, 0), true);

	SetDefaultStates();
}

/** Called when a vob was removed from the world */
XRESULT D3D11GraphicsEngine::OnVobRemovedFromWorld(zCVob* vob) {
	if (UIView) UIView->GetEditorPanel()->OnVobRemovedFromWorld(vob);

	// Take out of shadowupdate queue
	for (auto it = FrameShadowUpdateLights.begin();
		it != FrameShadowUpdateLights.end(); ++it) {
		if ((*it)->Vob == vob) {
			FrameShadowUpdateLights.erase(it);
			break;
		}
	}

	DebugPointlight = nullptr;

	return XR_SUCCESS;
}

/** Updates the occlusion for the bsp-tree */
void D3D11GraphicsEngine::UpdateOcclusion() {
	if (!Engine::GAPI->GetRendererState()
		->RendererSettings.EnableOcclusionCulling)
		return;

	// Set up states
	Engine::GAPI->GetRendererState()->RasterizerState.CullMode =
		GothicRasterizerStateInfo::CM_CULL_NONE;
	Engine::GAPI->GetRendererState()->RasterizerState.SetDirty();

	Engine::GAPI->GetRendererState()->BlendState.SetDefault();
	Engine::GAPI->GetRendererState()->BlendState.ColorWritesEnabled =
		false;  // Rasterization is faster without writes
	Engine::GAPI->GetRendererState()->BlendState.SetDirty();

	Engine::GAPI->GetRendererState()->DepthState.DepthWriteEnabled =
		false;  // Don't write the bsp-nodes to the depth buffer, also quicker
	Engine::GAPI->GetRendererState()->DepthState.SetDirty();

	UpdateRenderStates();

	// Set up occlusion pass
	Occlusion->AdvanceFrameCounter();
	Occlusion->BeginOcclusionPass();

	// Do occlusiontests for the BSP-Tree
	Occlusion->DoOcclusionForBSP(Engine::GAPI->GetNewRootNode());

	Occlusion->EndOcclusionPass();

	SetDefaultStates();
}

/** Saves a screenshot */
void D3D11GraphicsEngine::SaveScreenshot() {
	HRESULT hr;

	// Buffer for scaling down the image
	auto rt = std::make_unique<RenderToTextureBuffer>(
		GetDevice(), Resolution.x, Resolution.y, DXGI_FORMAT_R8G8B8A8_UNORM);

	// Downscale to 256x256
	PfxRenderer->CopyTextureToRTV(HDRBackBuffer->GetShaderResView(),
		rt->GetRenderTargetView());

	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.ArraySize = 1;
	texDesc.BindFlags = 0;
	texDesc.CPUAccessFlags = 0;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.Width = Resolution.x;   // must be same as backbuffer
	texDesc.Height = Resolution.y;  // must be same as backbuffer
	texDesc.MipLevels = 1;
	texDesc.MiscFlags = 0;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_IMMUTABLE;

	wrl::ComPtr<ID3D11Texture2D> texture;
	LE(GetDevice()->CreateTexture2D(&texDesc, 0, &texture));
	if (!texture) {
		LogError() << "Could not create texture for screenshot!";
		return;
	}
	GetContext()->CopyResource(texture.Get(), rt->GetTexture());

	char date[50];
	char time[50];

	// Format the filename
	GetDateFormat(LOCALE_SYSTEM_DEFAULT, 0, nullptr, "yyyy-MM-dd", date, 50);
	GetTimeFormat(LOCALE_SYSTEM_DEFAULT, 0, nullptr, "hh-mm-ss", time, 50);

	// Create new folder if needed
	CreateDirectory("system\\Screenshots", nullptr);

	std::string name = "system\\screenshots\\GD3D11_" + std::string(date) +
		"__" + std::string(time) + ".jpg";

	LogInfo() << "Saving screenshot to: " << name;

	// Save the Texture as jpeg using Windows Imaging Component (WIC) with 95% quality.

	LE(SaveWICTextureToFile(GetContext(), texture.Get(), GUID_ContainerFormatJpeg, Toolbox::ToWideChar(name).c_str(), nullptr, [](IPropertyBag2* props)
		{
			PROPBAG2 options[1] = { 0 };
			options[0].pstrName = const_cast<wchar_t*>(L"ImageQuality");

			VARIANT varValues[1];
			varValues[0].vt = VT_R4;
			varValues[0].fltVal = 0.95f;

			(void)props->Write(1, options, varValues);
		}, false));

	// Inform the user that a screenshot has been taken
	Engine::GAPI->PrintMessageTimed(INT2(30, 30), "Screenshot taken: " + name);
}

void D3D11GraphicsEngine::DrawString(std::string str, float x, float y, float4 color = float4(1.f, 1.f, 1.f, 1.f), zTRnd_AlphaBlendFunc blendState = zTRnd_AlphaBlendFunc::zRND_ALPHA_FUNC_NONE) {

	simpleTextBuffer tb = {};
	tb.color = color;
	tb.x = x;
	tb.y = y;
	tb.str = str;
	tb.blendState = blendState;

	textToDraw.push_back(tb);
}

