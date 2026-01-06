#include "PCH.h"
#include "Upscaling.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <cmath>

#include "DX12SwapChain.h"
#include "Hooks.h"

#include <ClibUtil/simpleINI.hpp>

static void SetDirtyStates(bool a_computeShader)
{
	using func_t = void (*)(bool);
	static REL::Relocation<func_t> func{ REL::RelocationID(75580, 77386) };
	func(a_computeShader);
}

void Upscaling::LoadINI()
{
	std::lock_guard<std::shared_mutex> lk(fileLock);
	CSimpleIniA ini;
	if (!std::filesystem::exists("enbseries")) {
		std::filesystem::create_directory("enbseries");
	}
	ini.LoadFile("enbseries/enbframegeneration.ini");
	settings.frameGenerationMode = clib_util::ini::get_value<uint32_t>(ini, settings.frameGenerationMode, "FRAME GENERATION", "FrameGenerationMode", "# Default: 1");
	settings.frameLimitMode = clib_util::ini::get_value<uint32_t>(ini, settings.frameLimitMode, "FRAME GENERATION", "FrameLimitMode", "# Default: 0 (Disabled by default for smoothness)");
	settings.frameGenerationForceEnable = clib_util::ini::get_value<uint32_t>(ini, settings.frameGenerationForceEnable, "FRAME GENERATION", "ForceEnable", "# Default: 0");
	settings.sharpness = clib_util::ini::get_value<float>(ini, settings.sharpness, "FRAME GENERATION", "Sharpness", "# RCAS sharpening, range of 0.0 to 1.0\n# Default: 0.5");
	settings.allowAsyncWorkloads = clib_util::ini::get_value<uint32_t>(ini, settings.allowAsyncWorkloads, "FRAME GENERATION", "AllowAsyncWorkloads", "# Default: 1 (Enabled for performance)");
}

void Upscaling::SaveINI()
{
	std::lock_guard<std::shared_mutex> lk(fileLock);
	CSimpleIniA ini;
	if (!std::filesystem::exists("enbseries")) {
		std::filesystem::create_directory("enbseries");
	}
	ini.SetValue("FRAME GENERATION", "FrameGenerationMode", std::to_string(settings.frameGenerationMode).c_str(), "# Default: 1");
	ini.SetValue("FRAME GENERATION", "FrameLimitMode", std::to_string(settings.frameLimitMode).c_str(), "# Default: 0");
	ini.SetValue("FRAME GENERATION", "ForceEnable", std::to_string(settings.frameGenerationForceEnable).c_str(), "# Default: 0");
	ini.SetValue("FRAME GENERATION", "Sharpness", std::to_string(settings.sharpness).c_str(), "# RCAS sharpening, range of 0.0 to 1.0\n# Default: 0.5");
	ini.SetValue("FRAME GENERATION", "AllowAsyncWorkloads", std::to_string(settings.allowAsyncWorkloads).c_str(), "# Default: 1");
	ini.SaveFile("enbseries/enbframegeneration.ini");
}

void Upscaling::RefreshUI()
{
	if (!g_ENB)
		return;

	auto generalBar = g_ENB->TwGetBarByEnum(ENB_API::ENBWindowType::EditorBarButtons);

	if (d3d12Interop)
		g_ENB->TwAddButton(generalBar, "Frame Generation is ENABLED", NULL, NULL, "group='PERFORMANCE'");
	else
		g_ENB->TwAddButton(generalBar, "Frame Generation is DISABLED", NULL, NULL, "group='PERFORMANCE'");

	g_ENB->TwAddButton(generalBar, "Uses AMD FSR 4.0 Frame Generation", NULL, NULL, "group='PERFORMANCE'");
	g_ENB->TwAddButton(generalBar, "Requires a D3D11 to D3D12 proxy", NULL, NULL, "group='PERFORMANCE'");
	g_ENB->TwAddButton(generalBar, "Toggling FG requires a restart", NULL, NULL, "group='PERFORMANCE'");

	if (!isWindowed)
		g_ENB->TwAddButton(generalBar, "Warning: Requires windowed mode", NULL, NULL, "group='PERFORMANCE'");

	if (lowRefreshRate && !settings.frameGenerationForceEnable)
		g_ENB->TwAddButton(generalBar, "Warning: Requires a high refresh rate monitor or Force Enable Frame Generation", NULL, NULL, "group='PERFORMANCE'");

	if (fidelityFXMissing)
		g_ENB->TwAddButton(generalBar, "Warning: FSR 4.0 DLLs are not loaded", NULL, NULL, "group='PERFORMANCE'");


	g_ENB->TwAddVarRW(generalBar, "Frame Generation", TW_TYPE_BOOL32, &settings.frameGenerationMode, "group='PERFORMANCE'");

	if (d3d12Interop) {
		g_ENB->TwAddVarRW(generalBar, "Frame Limit (Variable Refresh Rate)", TW_TYPE_BOOL32, &settings.frameLimitMode, "group='PERFORMANCE'");
		g_ENB->TwAddVarRW(generalBar, "Allow Async Workloads", TW_TYPE_BOOL32, &settings.allowAsyncWorkloads, "group='PERFORMANCE'");
	}

	g_ENB->TwAddVarRW(generalBar, "Sharpness", TW_TYPE_FLOAT, &settings.sharpness, "group='PERFORMANCE' min=0.0 max=1.0 step=0.05");

	g_ENB->TwAddVarRW(generalBar, "Force Enable Frame Generation", TW_TYPE_BOOL32, &settings.frameGenerationForceEnable, "group='PERFORMANCE'");
}

ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const char* ProgramType, const char* Program = "main")
{
	auto device = DX::GetDevice();
	if (!device)
		return nullptr;

	// Compiler setup
	uint32_t flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

	ID3DBlob* shaderBlob;
	ID3DBlob* shaderErrors;

	std::string str;
	std::wstring path{ FilePath };
	std::transform(path.begin(), path.end(), std::back_inserter(str), [](wchar_t c) {
		return (char)c;
	});
	if (!std::filesystem::exists(FilePath)) {
		logger::error("Failed to compile shader; {} does not exist", str);
		return nullptr;
	}
	if (FAILED(D3DCompileFromFile(FilePath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, Program, ProgramType, flags, 0, &shaderBlob, &shaderErrors))) {
		logger::warn("Shader compilation failed:\n\n{}", shaderErrors ? static_cast<char*>(shaderErrors->GetBufferPointer()) : "Unknown error");
		if (shaderErrors) shaderErrors->Release();
		return nullptr;
	}
	if (shaderErrors) {
		logger::debug("Shader logs:\n{}", static_cast<char*>(shaderErrors->GetBufferPointer()));
		shaderErrors->Release();
	}

	if (!shaderBlob) {
		return nullptr;
	}

	ID3D11ComputeShader* regShader = nullptr;
	HRESULT hr = device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
	shaderBlob->Release();

	if (FAILED(hr)) {
		logger::error("Failed to create compute shader (HRESULT: {:08X})", (uint32_t)hr);
		return nullptr;
	}

	return regShader;
}

void Upscaling::UpdateJitter()
{
	if (!d3d12Interop) {
		return;
	}

	// Reset earlyCopy flag at the start of jitter update (frame start)
	earlyCopy = false;

	try {
		auto state = RE::BSGraphics::State::GetSingleton();
		if (!state) {
			return;
		}

		auto gameViewport = reinterpret_cast<StateEx*>(state);

		auto ffx = FSR4SkyrimHandler::GetSingleton();
		if (!ffx->upscaleInitialized) {
			return;
		}

		// Log every 300 frames to confirm jitter is updating
		bool shouldLogJitter = (gameViewport->frameCount % 300 == 0);

		ffxQueryDescUpscaleGetJitterOffset queryDesc = {};
		queryDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
		// Now using CORRECT frameCount offset (0x4C) from ArranzCNL/CommonLibSSE-NG
		queryDesc.index = gameViewport->frameCount; 
		queryDesc.phaseCount = 8;
		queryDesc.pOutX = &jitter.x;
		queryDesc.pOutY = &jitter.y;

		auto queryResult = ffx::Query(ffx->upscaleContext, queryDesc);
		if (queryResult != ffx::ReturnCode::Ok) {
			if (shouldLogJitter) {
				logger::warn("[Upscaling] UpdateJitter: Query failed! Result: {}", (int)queryResult);
			}
			return;
		}

		auto dx12SwapChain = DX12SwapChain::GetSingleton();
		uint32_t screenWidth = dx12SwapChain->swapChainDesc.Width;
		uint32_t screenHeight = dx12SwapChain->swapChainDesc.Height;

		if (screenWidth == 0 || screenHeight == 0)
			return;

		// Now writing to CORRECT offsets (0x44, 0x48)
		gameViewport->projectionPosScaleX = -2.0f * jitter.x / (float)screenWidth;
		gameViewport->projectionPosScaleY = 2.0f * jitter.y / (float)screenHeight;
		
		if (shouldLogJitter) {
			logger::info("[Upscaling] UpdateJitter: frameCount={}, jitter=({:.4f}, {:.4f}), projScale=({:.6f}, {:.6f})", 
				gameViewport->frameCount, jitter.x, jitter.y, gameViewport->projectionPosScaleX, gameViewport->projectionPosScaleY);
		}
	} catch (const std::exception& e) {
		logger::critical("[Upscaling] UpdateJitter Exception: {}", e.what());
		LOG_FLUSH();
	} catch (...) {
		logger::critical("[Upscaling] UpdateJitter Unknown Exception");
		LOG_FLUSH();
	}
}

void Upscaling::InvalidateResources()
{
	logger::info("[Upscaling] InvalidateResources: Releasing shared resources...");
	LOG_FLUSH();
	
	// Mark buffers as not setup - this will trigger recreation on next frame
	setupBuffers = false;
	
	// Release all shared resources to prevent stale pointer access
	// These will be recreated in CreateFrameGenerationResources on next valid frame
	if (HUDLessBufferShared) {
		delete HUDLessBufferShared;
		HUDLessBufferShared = nullptr;
	}
	if (upscaledBufferShared) {
		delete upscaledBufferShared;
		upscaledBufferShared = nullptr;
	}
	if (depthBufferShared) {
		delete depthBufferShared;
		depthBufferShared = nullptr;
	}
	if (motionVectorBufferShared) {
		delete motionVectorBufferShared;
		motionVectorBufferShared = nullptr;
	}
	
	// Reset early copy flag
	earlyCopy = false;
	
	logger::info("[Upscaling] InvalidateResources: All shared resources released.");
	LOG_FLUSH();
}

void Upscaling::CreateFrameGenerationResources()
{
	logger::info("[Upscaling] CreateFrameGenerationResources Entry");
	LOG_FLUSH();
	try {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;

		auto& main = renderer->data.renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

		if (!main.texture || !main.SRV || !main.RTV || !motionVector.texture) {
			static bool loggedOnce = false;
			if (!loggedOnce) {
				logger::warn("[Upscaling] One or more game textures not available for resource creation.");
				LOG_FLUSH();
				loggedOnce = true;
			}
			return;
		}

		auto dx12SwapChain = DX12SwapChain::GetSingleton();
		if (!dx12SwapChain->d3d12Device || !dx12SwapChain->swapChain) {
			return;
		}

		logger::info("[Frame Generation] Creating resources (Reverse Interop)...");
		LOG_FLUSH();
		
		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);

		// Common flags for shared resources
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.CPUAccessFlags = 0;

		// HUDLess & Upscaled (R8G8B8A8_UNORM)
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		logger::info("[Upscaling] Creating HUDLessBufferShared...");
		LOG_FLUSH();
		HUDLessBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());
		
		logger::info("[Upscaling] Creating upscaledBufferShared...");
		LOG_FLUSH();
		upscaledBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Depth (R32_FLOAT)
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		logger::info("[Upscaling] Creating depthBufferShared...");
		LOG_FLUSH();
		depthBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Motion Vectors (Original Format)
		auto& motionVectorRT = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		if (!motionVectorRT.texture) {
			logger::error("[Upscaling] Motion vector texture is NULL during resource creation!");
			setupBuffers = false;
			return;
		}
		D3D11_TEXTURE2D_DESC texDescMV{};
		motionVectorRT.texture->GetDesc(&texDescMV);
		texDesc.Format = texDescMV.Format;
		logger::info("[Upscaling] Creating motionVectorBufferShared (Format: {})...", (int)texDesc.Format);
		LOG_FLUSH();
		motionVectorBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Find shader...
		logger::info("[Upscaling] Compiling depth copy shader...");
		LOG_FLUSH();
		
		// Use a more robust way to find the shader file
		std::wstring shaderPath = L"Data/SKSE/Plugins/CopyDepthToSharedBufferCS.hlsl";
		if (!std::filesystem::exists(shaderPath)) {
			shaderPath = L"Data/SKSE/Plugins/FSR4_Skyrim/CopyDepthToSharedBufferCS.hlsl";
		}
		if (!std::filesystem::exists(shaderPath)) {
			// Fallback to current directory
			shaderPath = L"CopyDepthToSharedBufferCS.hlsl";
		}

		if (!std::filesystem::exists(shaderPath)) {
			logger::error("[Upscaling] Could not find CopyDepthToSharedBufferCS.hlsl in any expected location!");
			LOG_FLUSH();
		}

		copyDepthToSharedBufferCS = (ID3D11ComputeShader*)CompileShader(shaderPath.c_str(), "cs_5_0");

		if (copyDepthToSharedBufferCS) {
			logger::info("[Upscaling] Frame generation resources initialized successfully.");
			LOG_FLUSH();
			setupBuffers = true;
		} else {
			logger::error("[Upscaling] Failed to compile depth copy shader. Frame generation will be disabled.");
			LOG_FLUSH();
		}
	} catch (const std::exception& e) {
		logger::critical("[Upscaling] CreateFrameGenerationResources: Exception: {}", e.what());
		LOG_FLUSH();
	} catch (...) {
		logger::critical("[Upscaling] CreateFrameGenerationResources: Unknown exception!");
		LOG_FLUSH();
	}
}

void Upscaling::EarlyCopyBuffersToSharedResources()
{
	if (!d3d12Interop)
		return;

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	if (!renderer)
		return;

	if (!setupBuffers) {
		auto& main = renderer->data.renderTargets[RE::RENDER_TARGETS::kMAIN];
		// Ignore before assets have not been created yet
		if (!main.texture)
			return;
		logger::info("[Upscaling] Initializing frame generation resources (Early)...");
		CreateFrameGenerationResources();
		if (!setupBuffers) return;
	}

	// Following ENBFrameGeneration: Use renderer's context
	auto context = reinterpret_cast<ID3D11DeviceContext*>(renderer->data.context);
	if (!context) return;
	
	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	auto ui = RE::UI::GetSingleton();
	if (ui && ui->GameIsPaused())
	{
		float clearColor[4] = { 0, 0, 0, 0 };
		if (motionVectorBufferShared && motionVectorBufferShared->rtv)
			context->ClearRenderTargetView(motionVectorBufferShared->rtv, clearColor);
	} else {
		auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		if (motionVector.texture && motionVectorBufferShared && motionVectorBufferShared->resource11) {
			context->CopyResource(motionVectorBufferShared->resource11, (ID3D11Resource*)motionVector.texture);
			
			// DIAGNOSTIC: Sample MV pixels to check if they contain valid data
			static uint64_t mvCopyLogCounter = 0;
			if (mvCopyLogCounter++ % 300 == 0) {
				D3D11_TEXTURE2D_DESC mvDesc;
				((ID3D11Texture2D*)motionVector.texture)->GetDesc(&mvDesc);
				logger::info("[Upscaling] MV Copy: src={:p} ({}x{}, fmt={}), dst={:p}",
					(void*)motionVector.texture, mvDesc.Width, mvDesc.Height, (int)mvDesc.Format,
					(void*)motionVectorBufferShared->resource11);
				
				// Read MV pixel values via staging texture
				ID3D11Device* device = nullptr;
				context->GetDevice(&device);
				if (device) {
					D3D11_TEXTURE2D_DESC stagingDesc = mvDesc;
					stagingDesc.Width = 4;  // Only sample 4x4 pixels
					stagingDesc.Height = 4;
					stagingDesc.MipLevels = 1;
					stagingDesc.ArraySize = 1;
					stagingDesc.Usage = D3D11_USAGE_STAGING;
					stagingDesc.BindFlags = 0;
					stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					stagingDesc.MiscFlags = 0;
					
					ID3D11Texture2D* stagingTex = nullptr;
					if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex))) {
						// Copy center region of MV texture
						D3D11_BOX srcBox;
						srcBox.left = mvDesc.Width / 2 - 2;
						srcBox.right = mvDesc.Width / 2 + 2;
						srcBox.top = mvDesc.Height / 2 - 2;
						srcBox.bottom = mvDesc.Height / 2 + 2;
						srcBox.front = 0;
						srcBox.back = 1;
						context->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, (ID3D11Texture2D*)motionVector.texture, 0, &srcBox);
						
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
							// R16G16_FLOAT: 2 half-floats per pixel (4 bytes)
							uint16_t* data = (uint16_t*)mapped.pData;
							// Sample center pixel (index 1,1 in 4x4 grid)
							uint32_t pixelOffset = (1 * (mapped.RowPitch / 4)) + 1 * 2;
							uint16_t mvX_half = data[pixelOffset];
							uint16_t mvY_half = data[pixelOffset + 1];
							
							// Convert half-float to float (approximate)
							auto halfToFloat = [](uint16_t h) -> float {
								uint32_t sign = (h >> 15) & 0x1;
								uint32_t exp = (h >> 10) & 0x1F;
								uint32_t mant = h & 0x3FF;
								if (exp == 0) return sign ? -0.0f : 0.0f;
								if (exp == 31) return sign ? -INFINITY : INFINITY;
								float f = (1.0f + mant / 1024.0f) * powf(2.0f, (float)exp - 15);
								return sign ? -f : f;
							};
							
							float mvX = halfToFloat(mvX_half);
							float mvY = halfToFloat(mvY_half);
							
							logger::info("[Upscaling] MV Center Pixel: X={:.4f}, Y={:.4f} (raw: 0x{:04X}, 0x{:04X})",
								mvX, mvY, mvX_half, mvY_half);
							
							// Also sample a few more pixels
							float sumX = 0, sumY = 0;
							int count = 0;
							for (int y = 0; y < 4; y++) {
								for (int x = 0; x < 4; x++) {
									uint32_t off = (y * (mapped.RowPitch / 4)) + x * 2;
									sumX += halfToFloat(data[off]);
									sumY += halfToFloat(data[off + 1]);
									count++;
								}
							}
							logger::info("[Upscaling] MV 4x4 Average: X={:.4f}, Y={:.4f}", sumX/count, sumY/count);
							
							context->Unmap(stagingTex, 0);
						}
						stagingTex->Release();
					}
					device->Release();
				}
			}
		} else {
			// DIAGNOSTIC: Log why MV copy was skipped
			static uint64_t mvSkipLogCounter = 0;
			if (mvSkipLogCounter++ % 300 == 0) {
				logger::warn("[Upscaling] MV Copy SKIPPED: motionVector.texture={:p}, motionVectorBufferShared={:p}, resource11={:p}",
					(void*)motionVector.texture,
					(void*)motionVectorBufferShared,
					motionVectorBufferShared ? (void*)motionVectorBufferShared->resource11 : nullptr);
			}
		}
	}

	{
		// Use kPOST_ZPREPASS_COPY (index 8) for stability as it is a dedicated copy for shader sampling
		auto& depth = renderer->data.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		if (depth.depthSRV && copyDepthToSharedBufferCS && depthBufferShared && depthBufferShared->uav) {
			uint32_t dispatchX = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Width) / 8.0f);
			uint32_t dispatchY = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Height) / 8.0f);

			ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[1] = { depthBufferShared->uav };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(copyDepthToSharedBufferCS, nullptr, 0);

			context->Dispatch(dispatchX, dispatchY, 1);

			// Explicitly clear states instead of potentially crashing SetDirtyStates during loading
			ID3D11ShaderResourceView* nullViews[1] = { nullptr };
			context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);

			ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);

			context->CSSetShader(nullptr, nullptr, 0);
		}
	}

	earlyCopy = true;
}


void Upscaling::CopyBuffersToSharedResources()
{
	if (!d3d12Interop)
		return;

	// Following ENBFrameGeneration pattern: Do NOT check PlayerCamera here.
	// ENBFrameGeneration trusts that if TAA_EndTechnique is called, the resources are valid.

	auto state = RE::BSGraphics::State::GetSingleton();
	if (!state) return;

	auto gameViewport = reinterpret_cast<StateEx*>(state);
	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	// Log frequently for the first 200 frames to catch early crashes
	bool shouldLog = (dx12SwapChain->frameCounter < 200) || (gameViewport->frameCount % 600 == 0);

	try {
		// Following ENBFrameGeneration: Use renderer's context directly
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return;
		}

		if (!setupBuffers) {
			auto& main = renderer->data.renderTargets[RE::RENDER_TARGETS::kMAIN];
			if (!main.texture) {
				return;
			}
			logger::info("[Upscaling] Initializing frame generation resources... Frame: {}", gameViewport->frameCount);
			LOG_FLUSH();
			CreateFrameGenerationResources();
			if (!setupBuffers) {
				return;
			}
		}

		// Following ENBFrameGeneration: Use renderer's context, NOT dx12SwapChain's context
		ID3D11DeviceContext* context = reinterpret_cast<ID3D11DeviceContext*>(renderer->data.context);
		if (!context) return;

		// 1. Motion Vectors & Depth (Only copy if not already done by EarlyCopy/Main_RenderWorld)
		if (!earlyCopy) {
			auto ui = RE::UI::GetSingleton();
			if (ui && !ui->GameIsPaused()) {
				auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
				if (motionVector.texture && motionVectorBufferShared && motionVectorBufferShared->resource11) {
					if (shouldLog) logger::info("[Upscaling] Copying Motion Vectors (TAA_End)...");
					context->CopyResource(motionVectorBufferShared->resource11, (ID3D11Resource*)motionVector.texture);
					
					// DIAGNOSTIC: Sample MV at TAA_End to compare with EarlyCopy
					static uint64_t mvTaaLogCounter = 0;
					if (mvTaaLogCounter++ % 300 == 0) {
						D3D11_TEXTURE2D_DESC mvDesc;
						((ID3D11Texture2D*)motionVector.texture)->GetDesc(&mvDesc);
						
						ID3D11Device* device = nullptr;
						context->GetDevice(&device);
						if (device) {
							D3D11_TEXTURE2D_DESC stagingDesc = mvDesc;
							stagingDesc.Width = 4;
							stagingDesc.Height = 4;
							stagingDesc.MipLevels = 1;
							stagingDesc.ArraySize = 1;
							stagingDesc.Usage = D3D11_USAGE_STAGING;
							stagingDesc.BindFlags = 0;
							stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
							stagingDesc.MiscFlags = 0;
							
							ID3D11Texture2D* stagingTex = nullptr;
							if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex))) {
								D3D11_BOX srcBox;
								srcBox.left = mvDesc.Width / 2 - 2;
								srcBox.right = mvDesc.Width / 2 + 2;
								srcBox.top = mvDesc.Height / 2 - 2;
								srcBox.bottom = mvDesc.Height / 2 + 2;
								srcBox.front = 0;
								srcBox.back = 1;
								context->CopySubresourceRegion(stagingTex, 0, 0, 0, 0, (ID3D11Texture2D*)motionVector.texture, 0, &srcBox);
								
								D3D11_MAPPED_SUBRESOURCE mapped;
								if (SUCCEEDED(context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
									uint16_t* data = (uint16_t*)mapped.pData;
									uint32_t pixelOffset = (1 * (mapped.RowPitch / 4)) + 1 * 2;
									uint16_t mvX_half = data[pixelOffset];
									uint16_t mvY_half = data[pixelOffset + 1];
									
									auto halfToFloat = [](uint16_t h) -> float {
										uint32_t sign = (h >> 15) & 0x1;
										uint32_t exp = (h >> 10) & 0x1F;
										uint32_t mant = h & 0x3FF;
										if (exp == 0) return sign ? -0.0f : 0.0f;
										if (exp == 31) return sign ? -INFINITY : INFINITY;
										float f = (1.0f + mant / 1024.0f) * powf(2.0f, (float)exp - 15);
										return sign ? -f : f;
									};
									
									float mvX = halfToFloat(mvX_half);
									float mvY = halfToFloat(mvY_half);
									logger::info("[Upscaling] MV at TAA_End: X={:.4f}, Y={:.4f} (raw: 0x{:04X}, 0x{:04X})",
										mvX, mvY, mvX_half, mvY_half);
									context->Unmap(stagingTex, 0);
								}
								stagingTex->Release();
							}
							device->Release();
						}
					}
				}
			}

			// 2. Depth
			{
				// Use kPOST_ZPREPASS_COPY as it's the depth copy intended for shader reading
				// Index 8 in SE 1.5.97
				auto& depth = renderer->data.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
				
				// FSR 4.0: Extra safety check for depth SRV
				if (depth.depthSRV && copyDepthToSharedBufferCS && depthBufferShared && depthBufferShared->uav) {
					if (shouldLog) logger::info("[Upscaling] Copying Depth (kPOST_ZPREPASS_COPY)...");
					uint32_t dispatchX = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Width) / 8.0f);
					uint32_t dispatchY = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Height) / 8.0f);

					ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
					context->CSSetShaderResources(0, ARRAYSIZE(views), views);
					ID3D11UnorderedAccessView* uavs[1] = { depthBufferShared->uav };
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
					context->CSSetShader(copyDepthToSharedBufferCS, nullptr, 0);
					context->Dispatch(dispatchX, dispatchY, 1);

					ID3D11ShaderResourceView* nullViews[1] = { nullptr };
					context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
					ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
					context->CSSetShader(nullptr, nullptr, 0);

					// Note: ENBFrameGeneration does NOT call SetDirtyStates here
					// Removed to match reference implementation and avoid potential crashes during loading
				}
			}
		}

		// 3. HUDLess (Backbuffer before UI) - Following ENBFrameGeneration: use SRV->GetResource
		{
			// Framebuffer is Index 0 and contains the scene before UI if called at EndTechnique
			auto& framebuffer = renderer->data.renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
			if (framebuffer.SRV && HUDLessBufferShared && HUDLessBufferShared->resource11) {
				ID3D11Resource* framebufferResource = nullptr;
				framebuffer.SRV->GetResource(&framebufferResource);
				if (framebufferResource) {
					if (shouldLog) logger::info("[Upscaling] Copying HUDLess Color via SRV...");
					context->CopyResource(HUDLessBufferShared->resource11, framebufferResource);
					framebufferResource->Release(); // GetResource adds a reference
				}
			}
		}

		// Following ENBFrameGeneration: Reset earlyCopy at the END of CopyBuffersToSharedResources
		earlyCopy = false;

		if (shouldLog) {
			logger::info("[Upscaling] CopyBuffersToSharedResources Complete. Frame: {}", gameViewport->frameCount);
			LOG_FLUSH();
		}
	} catch (const std::exception& e) {
		logger::critical("[Upscaling] CopyBuffersToSharedResources: Exception: {}", e.what());
		LOG_FLUSH();
	} catch (...) {
		logger::critical("[Upscaling] CopyBuffersToSharedResources: Unknown Exception!");
		LOG_FLUSH();
	}
}

void Upscaling::PostDisplay()
{
    // No longer used, replaced by TAA_EndTechnique
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter()
{
	if (d3d12Interop && settings.frameLimitMode) {
		static uint64_t frameCount = 0;
		if (frameCount % 60 == 0) {
			logger::info("[Upscaling] FrameLimiter active. Target Refresh Rate: {}", refreshRate);
		}
		frameCount++;

		double bestRefreshRate = refreshRate - (refreshRate * refreshRate) / 3600.0;

		LARGE_INTEGER qpf;
		QueryPerformanceFrequency(&qpf);

		int64_t targetFrameTicks = int64_t(double(qpf.QuadPart) / (bestRefreshRate * (settings.frameGenerationMode ? 0.5 : 1.0)));

		static LARGE_INTEGER lastFrame = {};
		LARGE_INTEGER timeNow;
		QueryPerformanceCounter(&timeNow);
		int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
		if (delta < targetFrameTicks) {
			TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
		}
		QueryPerformanceCounter(&lastFrame);
	}
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						if (wcscmp(info.szDevice, sourceName.viewGdiDeviceName) == 0) {
							// get the refresh rate
							UINT numerator = p.targetInfo.refreshRate.Numerator;
							UINT denominator = p.targetInfo.refreshRate.Denominator;
							return (double)numerator / (double)denominator;
						}
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}
