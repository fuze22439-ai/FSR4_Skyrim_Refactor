#include "PCH.h"
#include "Upscaling.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <cmath>

#include "DX12SwapChain.h"
#include "FidelityFX.h"
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
	settings.antiLagEnabled = clib_util::ini::get_value<uint32_t>(ini, settings.antiLagEnabled, "FRAME GENERATION", "AntiLagEnabled", "# AMD Anti-Lag 2.0 (AMD GPUs only)\n# Default: 1");
	
	// Sync Anti-Lag setting to FidelityFX handler
	auto fidelityFX = FSR4SkyrimHandler::GetSingleton();
	if (fidelityFX) {
		fidelityFX->antiLagEnabled = (settings.antiLagEnabled != 0);
	}
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
	ini.SetValue("FRAME GENERATION", "AntiLagEnabled", std::to_string(settings.antiLagEnabled).c_str(), "# AMD Anti-Lag 2.0 (AMD GPUs only)\n# Default: 1");
	ini.SaveFile("enbseries/enbframegeneration.ini");
}

void Upscaling::RefreshUI()
{
	if (!g_ENB)
		return;

	auto generalBar = g_ENB->TwGetBarByEnum(ENB_API::ENBWindowType::EditorBarButtons);
	auto fidelityFX = FSR4SkyrimHandler::GetSingleton();

	// === FSR 4.0 FRAME GENERATION ===
	g_ENB->TwAddButton(generalBar, "--- FSR 4.0 Frame Generation ---", NULL, NULL, "group='FSR4 FRAME GENERATION'");
	
	if (d3d12Interop)
		g_ENB->TwAddButton(generalBar, "Status: ENABLED", NULL, NULL, "group='FSR4 FRAME GENERATION'");
	else
		g_ENB->TwAddButton(generalBar, "Status: DISABLED", NULL, NULL, "group='FSR4 FRAME GENERATION'");

	g_ENB->TwAddButton(generalBar, "AMD FSR 4.0 MLFI (Machine Learning)", NULL, NULL, "group='FSR4 FRAME GENERATION'");

	if (!isWindowed)
		g_ENB->TwAddButton(generalBar, "[!] Requires Borderless Windowed", NULL, NULL, "group='FSR4 FRAME GENERATION'");

	if (lowRefreshRate && !settings.frameGenerationForceEnable)
		g_ENB->TwAddButton(generalBar, "[!] High Refresh Rate Recommended", NULL, NULL, "group='FSR4 FRAME GENERATION'");

	if (fidelityFXMissing)
		g_ENB->TwAddButton(generalBar, "[!] FSR 4.0 DLLs Not Loaded", NULL, NULL, "group='FSR4 FRAME GENERATION'");

	g_ENB->TwAddVarRW(generalBar, "Enable Frame Generation", TW_TYPE_BOOL32, &settings.frameGenerationMode, "group='FSR4 FRAME GENERATION'");

	if (d3d12Interop) {
		g_ENB->TwAddVarRW(generalBar, "VRR Frame Pacing", TW_TYPE_BOOL32, &settings.frameLimitMode, "group='FSR4 FRAME GENERATION'");
		g_ENB->TwAddVarRW(generalBar, "Async Compute", TW_TYPE_BOOL32, &settings.allowAsyncWorkloads, "group='FSR4 FRAME GENERATION'");
	}

	g_ENB->TwAddVarRW(generalBar, "Sharpness", TW_TYPE_FLOAT, &settings.sharpness, "group='FSR4 FRAME GENERATION' min=0.0 max=1.0 step=0.05");
	g_ENB->TwAddVarRW(generalBar, "Force Enable (Low Hz)", TW_TYPE_BOOL32, &settings.frameGenerationForceEnable, "group='FSR4 FRAME GENERATION'");

	// === ANTI-LAG 2.0 ===
	g_ENB->TwAddButton(generalBar, "--- AMD Anti-Lag 2.0 ---", NULL, NULL, "group='FSR4 FRAME GENERATION'");
	
	if (fidelityFX && fidelityFX->antiLagAvailable) {
		g_ENB->TwAddButton(generalBar, "Anti-Lag: Available", NULL, NULL, "group='FSR4 FRAME GENERATION'");
		g_ENB->TwAddVarRW(generalBar, "Enable Anti-Lag 2.0", TW_TYPE_BOOL32, &settings.antiLagEnabled, "group='FSR4 FRAME GENERATION'");
		// Sync setting to FidelityFX handler
		fidelityFX->antiLagEnabled = (settings.antiLagEnabled != 0);
	} else {
		g_ENB->TwAddButton(generalBar, "Anti-Lag: Not Available", NULL, NULL, "group='FSR4 FRAME GENERATION'");
		g_ENB->TwAddButton(generalBar, "(Requires AMD GPU + Driver)", NULL, NULL, "group='FSR4 FRAME GENERATION'");
	}

	g_ENB->TwAddButton(generalBar, "Restart game to apply changes", NULL, NULL, "group='FSR4 FRAME GENERATION'");
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

		ffxQueryDescUpscaleGetJitterOffset queryDesc = {};
		queryDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
		// Now using CORRECT frameCount offset (0x4C) from ArranzCNL/CommonLibSSE-NG
		queryDesc.index = gameViewport->frameCount; 
		queryDesc.phaseCount = 8;
		queryDesc.pOutX = &jitter.x;
		queryDesc.pOutY = &jitter.y;

		auto queryResult = ffx::Query(ffx->upscaleContext, queryDesc);
		if (queryResult != ffx::ReturnCode::Ok) {
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

		logger::info("[FSR4] Creating shared resources...");
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
		HUDLessBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());
		upscaledBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Depth (R32_FLOAT)
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		depthBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Motion Vectors (Original Format)
		auto& motionVectorRT = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		if (!motionVectorRT.texture) {
			logger::error("[FSR4] Motion vector texture NULL!");
			setupBuffers = false;
			return;
		}
		D3D11_TEXTURE2D_DESC texDescMV{};
		motionVectorRT.texture->GetDesc(&texDescMV);
		texDesc.Format = texDescMV.Format;
		motionVectorBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Find and compile depth copy shader
		std::wstring shaderPath = L"Data/SKSE/Plugins/CopyDepthToSharedBufferCS.hlsl";
		if (!std::filesystem::exists(shaderPath)) {
			shaderPath = L"Data/SKSE/Plugins/FSR4_Skyrim/CopyDepthToSharedBufferCS.hlsl";
		}
		if (!std::filesystem::exists(shaderPath)) {
			shaderPath = L"CopyDepthToSharedBufferCS.hlsl";
		}

		if (!std::filesystem::exists(shaderPath)) {
			logger::error("[FSR4] CopyDepthToSharedBufferCS.hlsl not found!");
			LOG_FLUSH();
		}

		copyDepthToSharedBufferCS = (ID3D11ComputeShader*)CompileShader(shaderPath.c_str(), "cs_5_0");

		if (copyDepthToSharedBufferCS) {
			logger::info("[FSR4] Resources initialized successfully.");
			LOG_FLUSH();
			setupBuffers = true;
		} else {
			logger::error("[FSR4] Depth shader compilation failed.");
			LOG_FLUSH();
		}
	} catch (const std::exception& e) {
		logger::critical("[FSR4] CreateResources exception: {}", e.what());
		LOG_FLUSH();
	} catch (...) {
		logger::critical("[FSR4] CreateResources unknown exception!");
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

	// NOTE: Do NOT copy MV here! MV is only valid after TAA pass renders it.
	// EarlyCopy happens BEFORE TAA, so MV would be stale/zero.
	// MV will be copied in CopyBuffersToSharedResources (TAA_EndTechnique)

	// Only copy Depth in EarlyCopy (Depth is valid at this point)
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

void Upscaling::ReplaceTAA()
{
	// This function replaces native TAA, following enb-anti-aliasing's Upscale() pattern
	// Key difference: We collect data for D3D12 FSR4, not execute D3D11 FSR3 directly
	
	if (!d3d12Interop)
		return;

	auto state = RE::BSGraphics::State::GetSingleton();
	if (!state) return;

	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	try {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) return;

		if (!setupBuffers) {
			auto& main = renderer->data.renderTargets[RE::RENDER_TARGETS::kMAIN];
			if (!main.texture) return;
			logger::info("[FSR4] Initializing resources...");
			LOG_FLUSH();
			CreateFrameGenerationResources();
			if (!setupBuffers) return;
		}

		ID3D11DeviceContext* context = reinterpret_cast<ID3D11DeviceContext*>(renderer->data.context);
		if (!context) return;

		// Following enb-anti-aliasing: Call SetDirtyStates(false) at start
		SetDirtyStates(false);

		// ========================================================================
		// CRITICAL: Get input/output from current pipeline state (like enb-anti-aliasing)
		// This captures WHAT TAA WOULD HAVE processed, not the result
		// ========================================================================
		
		// Get input texture from Pixel Shader slot 0 (what TAA would read)
		ID3D11ShaderResourceView* inputTextureSRV = nullptr;
		context->PSGetShaderResources(0, 1, &inputTextureSRV);
		
		// Get output texture from Output Merger (where TAA would write)
		ID3D11RenderTargetView* outputTextureRTV = nullptr;
		ID3D11DepthStencilView* dsv = nullptr;
		context->OMGetRenderTargets(1, &outputTextureRTV, &dsv);
		
		// Unbind render targets to avoid conflicts during copy
		context->OMSetRenderTargets(0, nullptr, nullptr);
		
		// Safely release references (GetXXX adds refcount)
		if (inputTextureSRV) inputTextureSRV->Release();
		if (outputTextureRTV) outputTextureRTV->Release();
		if (dsv) dsv->Release();
		
		if (!inputTextureSRV || !outputTextureRTV) {
			return;
		}
		
		// Get actual resource handles
		ID3D11Resource* inputTextureResource = nullptr;
		ID3D11Resource* outputTextureResource = nullptr;
		inputTextureSRV->GetResource(&inputTextureResource);
		outputTextureRTV->GetResource(&outputTextureResource);

		// ========================================================================
		// Copy resources to shared buffers for FSR4 (D3D12)
		// ========================================================================
		
		// 1. Copy pre-TAA Color (input) to HUDLessBufferShared
		// This is the KEY CHANGE: we use the TAA INPUT, not post-TAA framebuffer!
		if (inputTextureResource && HUDLessBufferShared && HUDLessBufferShared->resource11) {
			context->CopyResource(HUDLessBufferShared->resource11, inputTextureResource);
		}
		
		// 2. Copy Motion Vectors
		{
			auto ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				float clearColor[4] = { 0, 0, 0, 0 };
				if (motionVectorBufferShared && motionVectorBufferShared->rtv)
					context->ClearRenderTargetView(motionVectorBufferShared->rtv, clearColor);
			} else {
				auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
				if (motionVector.texture && motionVectorBufferShared && motionVectorBufferShared->resource11) {
					context->CopyResource(motionVectorBufferShared->resource11, (ID3D11Resource*)motionVector.texture);
				}
			}
		}
		
		// 3. Copy Depth (if not done by EarlyCopy)
		if (!earlyCopy) {
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

				// Clear compute shader state
				ID3D11ShaderResourceView* nullViews[1] = { nullptr };
				context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
				ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
				context->CSSetShader(nullptr, nullptr, 0);
			}
		}
		
		// ========================================================================
		// PHASE 2: Execute FSR4 AA on D3D12 and copy result back to D3D11
		// This is the KEY ADDITION: synchronous D3D11 -> D3D12 -> D3D11 roundtrip
		// ========================================================================
		
		auto fsr4Handler = FSR4SkyrimHandler::GetSingleton();
		bool aaExecuted = false;
		
		// Check if AA should run
		bool shouldRunAA = fsr4Handler && 
		                   fsr4Handler->upscaleInitialized && 
		                   HUDLessBufferShared && HUDLessBufferShared->resource.get() &&
		                   upscaledBufferShared && upscaledBufferShared->resource.get() &&
		                   depthBufferShared && depthBufferShared->resource.get() &&
		                   motionVectorBufferShared && motionVectorBufferShared->resource.get();
		
		if (shouldRunAA) {
			// Step 1: D3D11 signals that shared resources are ready
			dx12SwapChain->SignalD3D11ToD3D12();
			
			// Step 2: Execute AA synchronously on D3D12
			// DispatchAASync will:
			// - Reset command list
			// - Execute AA dispatch (HUDLess -> upscaled)
			// - Execute command list
			// - Signal D3D12 completion
			aaExecuted = fsr4Handler->DispatchAASync(
				HUDLessBufferShared->resource.get(),      // AA input
				upscaledBufferShared->resource.get(),     // AA output  
				depthBufferShared->resource.get(),        // Depth
				motionVectorBufferShared->resource.get()  // Motion Vectors
			);
			
			if (aaExecuted) {
				// Step 3: D3D11 waits for D3D12 AA completion
				dx12SwapChain->WaitForD3D12Completion();
				
				// Step 4: Copy AA result back to D3D11 framebuffer
				// upscaledBufferShared->resource11 contains the AA result
				if (upscaledBufferShared->resource11 && outputTextureResource) {
					context->CopyResource(outputTextureResource, upscaledBufferShared->resource11);
				}
			} else {
				logger::warn("[FSR4] AA dispatch failed");
			}
		}
		
		// Fallback: If AA didn't run, copy input to output directly (no AA)
		if (!aaExecuted) {
			if (inputTextureResource && outputTextureResource) {
				context->CopyResource(outputTextureResource, inputTextureResource);
			}
		}
		
		// Release resource references
		if (inputTextureResource) inputTextureResource->Release();
		if (outputTextureResource) outputTextureResource->Release();
		
		// Following enb-anti-aliasing: Call SetDirtyStates(true) after compute shader work
		SetDirtyStates(true);
		
		earlyCopy = false;
	} catch (const std::exception& e) {
		logger::critical("[Upscaling] ReplaceTAA: Exception: {}", e.what());
		LOG_FLUSH();
	} catch (...) {
		logger::critical("[Upscaling] ReplaceTAA: Unknown exception!");
		LOG_FLUSH();
	}
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
	(void)dx12SwapChain; // May be unused

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

		// 1. Motion Vectors - ALWAYS copy at TAA_End (MV is only valid after TAA pass renders it)
		{
			auto ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				// Clear MV when paused
				float clearColor[4] = { 0, 0, 0, 0 };
				if (motionVectorBufferShared && motionVectorBufferShared->rtv)
					context->ClearRenderTargetView(motionVectorBufferShared->rtv, clearColor);
			} else {
				auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
				if (motionVector.texture && motionVectorBufferShared && motionVectorBufferShared->resource11) {
					context->CopyResource(motionVectorBufferShared->resource11, (ID3D11Resource*)motionVector.texture);
				}
			}
		}

		// 2. Depth (Only copy if not already done by EarlyCopy)
		if (!earlyCopy) {
			// Use kPOST_ZPREPASS_COPY as it's the depth copy intended for shader reading
			// Index 8 in SE 1.5.97
			auto& depth = renderer->data.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			
			// FSR 4.0: Extra safety check for depth SRV
			if (depth.depthSRV && copyDepthToSharedBufferCS && depthBufferShared && depthBufferShared->uav) {
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
					context->CopyResource(HUDLessBufferShared->resource11, framebufferResource);
					framebufferResource->Release(); // GetResource adds a reference
				}
			}
		}

		// Following ENBFrameGeneration: Reset earlyCopy at the END of CopyBuffersToSharedResources
		earlyCopy = false;
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
