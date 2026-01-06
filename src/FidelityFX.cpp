#include "PCH.h"
#include "FidelityFX.h"
#include "Upscaling.h"
#include "DX12SwapChain.h"
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <RE/P/PlayerCamera.h>
#include <RE/N/NiNode.h>

#define LOG_FLUSH() spdlog::default_logger()->flush()

void FSR4SkyrimHandler::LoadFFX()
{
    // Get the path of the current DLL (FSR4_Skyrim.dll)
    wchar_t pluginPath[MAX_PATH];
    HMODULE hModule = NULL;
    
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, 
                          (LPCWSTR)&FSR4SkyrimHandler::GetSingleton, &hModule)) {
        GetModuleFileNameW(hModule, pluginPath, MAX_PATH);
    } else {
        GetModuleFileNameW(NULL, pluginPath, MAX_PATH);
    }
    
    std::wstring path(pluginPath);
    size_t lastSlash = path.find_last_of(L"\\/");
    std::wstring dir = (lastSlash != std::wstring::npos) ? path.substr(0, lastSlash) : L".";
    
    // Look in a subfolder named after the DLL (e.g., SKSE/Plugins/FSR4_Skyrim/)
    std::wstring dllName = path.substr(lastSlash + 1);
    size_t dot = dllName.find_last_of(L".");
    std::wstring folderName = (dot != std::wstring::npos) ? dllName.substr(0, dot) : dllName;
    
    std::wstring subDir = dir + L"\\" + folderName;
    
    // Explicitly load effect DLLs first
    LoadLibraryExW((subDir + L"\\amd_fidelityfx_upscaler_dx12.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    LoadLibraryExW((subDir + L"\\amd_fidelityfx_framegeneration_dx12.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

    std::wstring loaderPath = subDir + L"\\amd_fidelityfx_loader_dx12.dll";

    // Try to load from the subfolder first
    HMODULE loader = LoadLibraryExW(loaderPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    
    // Fallback to same directory as plugin
    if (!loader) {
        LoadLibraryExW((dir + L"\\amd_fidelityfx_upscaler_dx12.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        LoadLibraryExW((dir + L"\\amd_fidelityfx_framegeneration_dx12.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

        loaderPath = dir + L"\\amd_fidelityfx_loader_dx12.dll";
        loader = LoadLibraryExW(loaderPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    
    // Fallback to standard search path
    if (!loader) {
        LoadLibraryW(L"amd_fidelityfx_upscaler_dx12.dll");
        LoadLibraryW(L"amd_fidelityfx_framegeneration_dx12.dll");
        loader = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
    }

    if (loader) {
        isAvailable = true;
        // Keep the library loaded
        logger::info("[FSR4SkyrimHandler] Successfully located and loaded AMD FidelityFX loader and effect DLLs.");
    } else {
        logger::error("[FSR4SkyrimHandler] amd_fidelityfx_loader_dx12.dll not found!");
        isAvailable = false;
    }
}


void FSR4SkyrimHandler::SetupFrameGeneration()
{
	// Only skip if ALL contexts are already initialized
	if (frameGenInitialized && upscaleInitialized && swapChainContextInitialized) {
		logger::info("[FSR4SkyrimHandler] SetupFrameGeneration: All contexts already initialized, skipping.");
		return;
	}

	auto swapChain = DX12SwapChain::GetSingleton();

	// 1. Create Backend
	ffxCreateBackendDX12Desc backendDesc{};
	backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backendDesc.device = swapChain->d3d12Device.get();

	// 2. SwapChain Context is ALREADY created in DX12SwapChain::CreateSwapChain() using ForHwnd
	// DO NOT attempt to wrap again here - it would fail with Error 0x3
	// The swapChainContextInitialized flag is already set by DX12SwapChain
	if (!swapChainContextInitialized) {
		logger::warn("[FSR4SkyrimHandler] SwapChain context was not initialized by DX12SwapChain! This should not happen.");
	}

	// 3. Setup Frame Generation Context
	{
		ffx::CreateContextDescFrameGeneration createFg{};
		createFg.displaySize = { swapChain->swapChainDesc.Width, swapChain->swapChainDesc.Height };
		createFg.maxRenderSize = createFg.displaySize;
		// Following ENBFrameGeneration: Only use ASYNC_WORKLOAD_SUPPORT
		// Other flags may cause issues with FSR 4.0
		createFg.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
		createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(swapChain->swapChainDesc.Format);

		// FSR 4.0 Version Descriptor
		ffxCreateContextDescFrameGenerationVersion versionDesc{};
		versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
		versionDesc.version = FFX_FRAMEGENERATION_VERSION;

		// Link headers manually
		createFg.header.pNext = &versionDesc.header;
		versionDesc.header.pNext = &backendDesc.header;

		logger::info("[FSR4SkyrimHandler] Attempting to create frame generation context...");
		auto ret = ffxCreateContext(&frameGenContext, &createFg.header, nullptr);
		if (ret != FFX_API_RETURN_OK) {
			logger::critical("[FSR4SkyrimHandler] Failed to create frame generation context! Error code: 0x{:X}", (uint32_t)ret);
		} else {
			logger::info("[FSR4SkyrimHandler] Successfully created frame generation context.");
			frameGenInitialized = true;
		}
	}

	// 4. Setup Upscale Context for Native AA
	{
		ffx::CreateContextDescUpscale createUpscale{};
		createUpscale.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | 
							  FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
							  FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
							  FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
							  FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
		createUpscale.maxRenderSize = { swapChain->swapChainDesc.Width, swapChain->swapChainDesc.Height };
		createUpscale.maxUpscaleSize = createUpscale.maxRenderSize;

		ffxCreateContextDescUpscaleVersion upscaleVersionDesc{};
		upscaleVersionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		upscaleVersionDesc.version = FFX_UPSCALER_VERSION;

		createUpscale.header.pNext = &upscaleVersionDesc.header;
		upscaleVersionDesc.header.pNext = &backendDesc.header;

		logger::info("[FSR4SkyrimHandler] Attempting to create upscale context (Native AA)...");
		auto ret = ffxCreateContext(&upscaleContext, &createUpscale.header, nullptr);
		if (ret != FFX_API_RETURN_OK) {
			logger::critical("[FSR4SkyrimHandler] Failed to create upscale context! Error code: 0x{:X}", (uint32_t)ret);
		} else {
			logger::info("[FSR4SkyrimHandler] Successfully created upscale context.");
			upscaleInitialized = true;
		}
	}
}



// https://github.com/PureDark/Skyrim-Upscaler/blob/fa057bb088cf399e1112c1eaba714590c881e462/src/SkyrimUpscaler.cpp#L88
float GetVerticalFOVRad()
{
	auto swapChain = DX12SwapChain::GetSingleton();
	static float& fac = (*(float*)(REL::RelocationID(513786, 388785).address()));
	const auto base = fac;
	const auto x = base / 1.30322540f;
	const auto vFOV = 2 * atan(x / (float(swapChain->swapChainDesc.Width) / float(swapChain->swapChainDesc.Height)));
	return vFOV;
}

static ffxReturnCode_t FrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* pUserCtx)
{
	// Detailed diagnostic log for frame generation
	static uint64_t callCount = 0;
	callCount++;
	
	// Log every 60 calls AND first 10 calls
	bool shouldLog = (callCount % 60 == 0) || (callCount < 10);
	if (shouldLog) {
		logger::info("[FG_Callback] call#{}, frameID={}, numGenFrames={}, reset={}",
			callCount, params->frameID, params->numGeneratedFrames, params->reset);
	}
	
	// Check if callback context is valid
	if (!pUserCtx) {
		logger::error("[FG_Callback] pUserCtx is NULL!");
		return FFX_API_RETURN_ERROR;
	}
	
	// FSR 4.0: Match ENBFrameGeneration's minimal callback - let FSR handle pacing internally
	// DO NOT modify params->numGeneratedFrames - it breaks FSR's frame pacing!
	auto result = ffxDispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
	if (result != FFX_API_RETURN_OK && callCount % 100 == 0) {
		logger::error("[FG_Callback] ffxDispatch failed! Error: 0x{:X}", (uint32_t)result);
	}
	return result;
}

void FSR4SkyrimHandler::Present(bool a_useFrameGeneration, bool a_bypass)
{
	(void)a_bypass; // Unused - kept for API compatibility

	if (!swapChainContextInitialized)
		return;

	// Following ENBFrameGeneration: No bypass logic, FSR always runs
	uint64_t frameID = currentFSRFrameID;

	auto upscaling = Upscaling::GetSingleton();
	auto swapChain = DX12SwapChain::GetSingleton();
	auto commandList = swapChain->commandLists[swapChain->frameIndex].get();

	bool shouldLog = (frameID < 200) || (frameID % 100 == 0);

	// DIAGNOSTIC: Log every frame for first 20 frames to verify frameID increments by 1
	static uint64_t lastLoggedFrameID = 0;
	if (frameID < 20 || (frameID - lastLoggedFrameID != 1 && lastLoggedFrameID != 0)) {
		logger::info("[FidelityFX] frameID={}, lastFrameID={}, diff={}", frameID, lastLoggedFrameID, frameID - lastLoggedFrameID);
	}
	lastLoggedFrameID = frameID;

	// Get StateEx for reading projectionPosScaleX/Y (jitter)
	auto state = RE::BSGraphics::State::GetSingleton();
	auto stateEx = state ? reinterpret_cast<StateEx*>(state) : nullptr;

	// Use game's deltaTime like ENBFrameGeneration for consistency
	static auto s_deltaTime = (float*)REL::RelocationID(523660, 410199).address();
	float manualDeltaTime = s_deltaTime ? (*s_deltaTime * 1000.0f) : 16.6f;
	
	// ENBFrameGeneration does NOT clamp deltaTime - follow the same approach
	// Invalid deltaTime (<=0 or extremely large) indicates game pause/loading
	// In these states, FG should still run but FSR will handle it gracefully
	// Only apply minimal sanity check to prevent division by zero issues
	if (manualDeltaTime <= 0.0f) manualDeltaTime = 16.6f; // Default to 60fps if invalid

	auto HUDLessColor = (upscaling->HUDLessBufferShared) ? upscaling->HUDLessBufferShared->resource.get() : nullptr;
	auto depth = (upscaling->depthBufferShared) ? upscaling->depthBufferShared->resource.get() : nullptr;
	auto motionVectors = (upscaling->motionVectorBufferShared) ? upscaling->motionVectorBufferShared->resource.get() : nullptr;
	auto upscaledColor = (upscaling->upscaledBufferShared) ? upscaling->upscaledBufferShared->resource.get() : nullptr;

	bool resourcesReady = HUDLessColor && depth && motionVectors && upscaledColor;

	if (!commandList) {
		currentFSRFrameID++;
		return;
	}

	// Track resource state changes (only log when state changes to reduce spam)
	static bool lastResourcesReady = false;
	if (resourcesReady != lastResourcesReady) {
		logger::info("[FSR4SkyrimHandler] Resource state changed: {} -> {} at frame {}", 
			lastResourcesReady, resourcesReady, frameID);
		LOG_FLUSH();
		lastResourcesReady = resourcesReady;
	}

	if (shouldLog) {
		logger::info("[FSR4SkyrimHandler] Present Dispatch Start. frameID: {}, useFG: {}, resourcesReady: {}, deltaTime: {:.2f}ms", 
			frameID, a_useFrameGeneration, resourcesReady, manualDeltaTime);
		LOG_FLUSH();
	}

	// 1. Configure Pacing
	if (swapChainContextInitialized) {
		// varianceFactor: 0.1 (default) is tight, 0.3-0.5 is more forgiving for unstable frame times
		// Skyrim with mods has highly variable frame times (22-52ms observed), so use higher variance
		FfxApiSwapchainFramePacingTuning framePacingTuning{ 0.1f, 0.3f, true, 2, false };
		ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 tuning{};
		tuning.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
		tuning.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
		tuning.ptr = &framePacingTuning;
		ffxConfigure(&swapChainContext, &tuning.header);
	}

	// 2. Configure Frame Generation (MANDATORY EVERY FRAME)
	// Following ENBFrameGeneration: Enable FG based on user setting, NOT resource availability!
	// FSR will use backbuffer if HUDLessColor is not provided.
	if (frameGenInitialized) {
		ffxConfigureDescFrameGeneration configParameters{};
		memset(&configParameters, 0, sizeof(configParameters));
		configParameters.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		configParameters.swapChain = swapChain->swapChain;
		
		// Following ENBFrameGeneration: Only check user setting, NOT resourcesReady!
		if (a_useFrameGeneration) {
			configParameters.frameGenerationEnabled = true;
			configParameters.frameGenerationCallback = FrameGenerationCallback;
			configParameters.frameGenerationCallbackUserContext = &frameGenContext;
			
			// Provide HUDLessColor if available, otherwise FSR uses backbuffer
			if (resourcesReady) {
				bool useAA = upscaleInitialized && (upscaling->settings.sharpness > 0.0f);
				configParameters.HUDLessColor = ffxApiGetResourceDX12(useAA ? upscaledColor : HUDLessColor);
			} else {
				configParameters.HUDLessColor = FfxApiResource{};
			}
		} else {
			// User disabled FG
			configParameters.frameGenerationEnabled = false;
			configParameters.frameGenerationCallbackUserContext = nullptr;
			configParameters.frameGenerationCallback = nullptr;
			configParameters.HUDLessColor = FfxApiResource{};
		}
		
		configParameters.frameID = frameID;
		// CRITICAL: Following ENBFrameGeneration - must set onlyPresentGenerated = false
		// This tells FSR to present BOTH original and generated frames
		// Without this, FSR may only present generated frames or skip frames entirely
		configParameters.onlyPresentGenerated = false;
		// CRITICAL: Match ENBFrameGeneration - always true for best pacing
		configParameters.allowAsyncWorkloads = true;
		// Match ENBFrameGeneration: flags = 0 for production (no debug pacing lines)
		configParameters.flags = 0;
		configParameters.generationRect.left = 0;
		configParameters.generationRect.top = 0;
		configParameters.generationRect.width = swapChain->swapChainDesc.Width;
		configParameters.generationRect.height = swapChain->swapChainDesc.Height;
		
		auto configResult = ffxConfigure(&frameGenContext, &configParameters.header);
		if (configResult != FFX_API_RETURN_OK) {
			logger::error("[FidelityFX] Failed to configure frame generation! Error: 0x{:X}", (uint32_t)configResult);
		}
		
		// DIAGNOSTIC: Log Configure details every 300 frames
		static uint64_t lastConfigLogFrame = 0;
		if (frameID - lastConfigLogFrame >= 300) {
			lastConfigLogFrame = frameID;
			logger::info("[FidelityFX] Configure: swapChain={:p}, enabled={}, callback={:p}, onlyPresentGen={}, flags={}",
				(void*)configParameters.swapChain,
				configParameters.frameGenerationEnabled,
				(void*)configParameters.frameGenerationCallback,
				configParameters.onlyPresentGenerated,
				configParameters.flags);
		}
	}

	// Following ENBFrameGeneration: Dispatch PrepareV2 when user enables FG, regardless of resource availability
	// FSR handles null resources gracefully
	if (a_useFrameGeneration && frameGenInitialized) {
		// Use static memory addresses like ENBFrameGeneration for stability during loading
		static auto s_cameraNear = (float*)(REL::RelocationID(517032, 403540).address() + 0x40);
		static auto s_cameraFar = (float*)(REL::RelocationID(517032, 403540).address() + 0x44);
		float cameraNearVal = s_cameraNear ? *s_cameraNear : 0.1f;
		float cameraFarVal = s_cameraFar ? *s_cameraFar : 100000.0f;
		
		// Safety clamp
		if (cameraNearVal <= 0.0f) cameraNearVal = 0.1f;
		if (cameraFarVal <= cameraNearVal) cameraFarVal = 100000.0f;

		// Dispatch AA only when resources are actually ready
		bool useAA = upscaleInitialized && resourcesReady && (upscaling->settings.sharpness > 0.0f);
		
		// DIAGNOSTIC: Log AA dispatch decision every 300 frames
		static uint64_t aaLogCounter = 0;
		if (aaLogCounter++ % 300 == 0) {
			logger::info("[FidelityFX] AA Check: upscaleInit={}, resourcesReady={}, sharpness={:.2f}, useAA={}",
				upscaleInitialized, resourcesReady, upscaling->settings.sharpness, useAA);
		}
		
		if (useAA) {
			ffxDispatchDescUpscale upscaleDispatch{};
			memset(&upscaleDispatch, 0, sizeof(upscaleDispatch));
			upscaleDispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
			upscaleDispatch.commandList = commandList;
			upscaleDispatch.color = ffxApiGetResourceDX12(HUDLessColor);
			upscaleDispatch.depth = ffxApiGetResourceDX12(depth);
			upscaleDispatch.motionVectors = ffxApiGetResourceDX12(motionVectors);
			upscaleDispatch.output = ffxApiGetResourceDX12(upscaledColor, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

			// Read jitter from projectionPosScaleX/Y like ENBFrameGeneration does
			if (stateEx) {
				upscaleDispatch.jitterOffset.x = stateEx->projectionPosScaleX * (float)swapChain->swapChainDesc.Width / 2.0f;
				upscaleDispatch.jitterOffset.y = stateEx->projectionPosScaleY * (float)swapChain->swapChainDesc.Height / 2.0f;
			} else {
				upscaleDispatch.jitterOffset.x = 0.0f;
				upscaleDispatch.jitterOffset.y = 0.0f;
			}
			upscaleDispatch.motionVectorScale.x = (float)swapChain->swapChainDesc.Width;
			upscaleDispatch.motionVectorScale.y = (float)swapChain->swapChainDesc.Height;
			upscaleDispatch.renderSize.width = swapChain->swapChainDesc.Width;
			upscaleDispatch.renderSize.height = swapChain->swapChainDesc.Height;
			upscaleDispatch.upscaleSize = upscaleDispatch.renderSize;
			upscaleDispatch.frameTimeDelta = manualDeltaTime;
			upscaleDispatch.cameraNear = cameraNearVal;
			upscaleDispatch.cameraFar = cameraFarVal;
			upscaleDispatch.cameraFovAngleVertical = GetVerticalFOVRad();
			upscaleDispatch.viewSpaceToMetersFactor = 0.01428222656f;
			upscaleDispatch.preExposure = 1.0f;
			upscaleDispatch.reset = (frameID < 10);
			upscaleDispatch.enableSharpening = true;
			upscaleDispatch.sharpness = upscaling->settings.sharpness;
			upscaleDispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

			ffxDispatch(&upscaleContext, &upscaleDispatch.header);

			D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(upscaledColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			commandList->ResourceBarrier(1, &barrier);
		} else if (upscaledColor) {
			D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(upscaledColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			commandList->ResourceBarrier(1, &barrier);
		}

		// Dispatch PrepareV2
		ffxDispatchDescFrameGenerationPrepareV2 prepare{};
		memset(&prepare, 0, sizeof(prepare));
		prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		prepare.commandList = commandList;
		prepare.renderSize.width = swapChain->swapChainDesc.Width;
		prepare.renderSize.height = swapChain->swapChainDesc.Height;
		// Read jitter from projectionPosScaleX/Y like ENBFrameGeneration does
		// This is the value that was written by UpdateJitter at frame start
		if (stateEx) {
			prepare.jitterOffset.x = stateEx->projectionPosScaleX * (float)swapChain->swapChainDesc.Width / 2.0f;
			prepare.jitterOffset.y = stateEx->projectionPosScaleY * (float)swapChain->swapChainDesc.Height / 2.0f;
		} else {
			prepare.jitterOffset.x = 0.0f;
			prepare.jitterOffset.y = 0.0f;
		}
		prepare.frameTimeDelta = manualDeltaTime;
		prepare.frameID = frameID;
		// Match ENBFrameGeneration: flags = 0 for production
		prepare.flags = 0;
		prepare.depth = ffxApiGetResourceDX12(depth);
		prepare.motionVectors = ffxApiGetResourceDX12(motionVectors);
		prepare.motionVectorScale.x = (float)swapChain->swapChainDesc.Width;
		prepare.motionVectorScale.y = (float)swapChain->swapChainDesc.Height;
		prepare.cameraNear = cameraNearVal;
		prepare.cameraFar = cameraFarVal;
		prepare.cameraFovAngleVertical = GetVerticalFOVRad();
		prepare.viewSpaceToMetersFactor = 0.01428222656f;
		
		// Reset flag: true on first few frames OR when RequestReset() was called (scene transitions)
		prepare.reset = needsReset || (frameID < 5);
		if (needsReset) {
			logger::info("[FSR4SkyrimHandler] Reset triggered at frame {}", frameID);
			needsReset = false;  // Clear after use
		}

		// FSR 4.0 requires camera vectors - provide safe defaults if camera unavailable
		// Default: Identity orientation looking forward (+Y in Skyrim), positioned at origin
		prepare.cameraPosition[0] = 0.0f; prepare.cameraPosition[1] = 0.0f; prepare.cameraPosition[2] = 0.0f;
		prepare.cameraRight[0] = 1.0f; prepare.cameraRight[1] = 0.0f; prepare.cameraRight[2] = 0.0f;
		prepare.cameraForward[0] = 0.0f; prepare.cameraForward[1] = 1.0f; prepare.cameraForward[2] = 0.0f;
		prepare.cameraUp[0] = 0.0f; prepare.cameraUp[1] = 0.0f; prepare.cameraUp[2] = 1.0f;

		// Try to get actual camera data if available
		// Using multiple null checks to avoid accessing invalid memory during loading
		// Also detect large camera jumps to auto-trigger reset (fast travel, load game, etc.)
		static float lastCameraPos[3] = {0.0f, 0.0f, 0.0f};
		auto camera = RE::PlayerCamera::GetSingleton();
		if (camera) {
			auto cameraRoot = camera->cameraRoot.get();
			if (cameraRoot) {
				auto& world = cameraRoot->world;
				prepare.cameraPosition[0] = world.translate.x;
				prepare.cameraPosition[1] = world.translate.y;
				prepare.cameraPosition[2] = world.translate.z;
				prepare.cameraRight[0] = world.rotate.entry[0][0]; prepare.cameraRight[1] = world.rotate.entry[1][0]; prepare.cameraRight[2] = world.rotate.entry[2][0];
				prepare.cameraForward[0] = world.rotate.entry[0][1]; prepare.cameraForward[1] = world.rotate.entry[1][1]; prepare.cameraForward[2] = world.rotate.entry[2][1];
				prepare.cameraUp[0] = world.rotate.entry[0][2]; prepare.cameraUp[1] = world.rotate.entry[1][2]; prepare.cameraUp[2] = world.rotate.entry[2][2];
				
				// Auto-detect large camera jumps (>1000 units = ~14 meters) as scene transitions
				float dx = prepare.cameraPosition[0] - lastCameraPos[0];
				float dy = prepare.cameraPosition[1] - lastCameraPos[1];
				float dz = prepare.cameraPosition[2] - lastCameraPos[2];
				float distSq = dx*dx + dy*dy + dz*dz;
				if (distSq > 1000000.0f) { // 1000^2 = 1000000
					prepare.reset = true;
					logger::info("[FSR4SkyrimHandler] Camera jump detected (dist^2={:.0f}), reset triggered", distSq);
				}
				lastCameraPos[0] = prepare.cameraPosition[0];
				lastCameraPos[1] = prepare.cameraPosition[1];
				lastCameraPos[2] = prepare.cameraPosition[2];
			}
		}

		// DIAGNOSTIC: Log Prepare details every 300 frames
		static uint64_t lastPrepareLogFrame = 0;
		if (frameID - lastPrepareLogFrame >= 300) {
			lastPrepareLogFrame = frameID;
			// Also log raw projectionPosScale to verify offset correctness
			if (stateEx) {
				logger::info("[FidelityFX] projectionPosScale: X={:.6f}, Y={:.6f}", 
					stateEx->projectionPosScaleX, stateEx->projectionPosScaleY);
			}
			logger::info("[FidelityFX] PrepareV2: depth={:p}, motionVectors={:p}, mvScale=({:.1f},{:.1f}), jitter=({:.4f},{:.4f})",
				(void*)prepare.depth.resource,
				(void*)prepare.motionVectors.resource,
				prepare.motionVectorScale.x, prepare.motionVectorScale.y,
				prepare.jitterOffset.x, prepare.jitterOffset.y);
			logger::info("[FidelityFX] PrepareV2: deltaTime={:.2f}ms, near={:.2f}, far={:.0f}, fov={:.4f}rad",
				prepare.frameTimeDelta, prepare.cameraNear, prepare.cameraFar, prepare.cameraFovAngleVertical);
			logger::info("[FidelityFX] PrepareV2: cameraPos=({:.1f},{:.1f},{:.1f}), reset={}",
				prepare.cameraPosition[0], prepare.cameraPosition[1], prepare.cameraPosition[2], prepare.reset);
		}

		auto dispatchResult = ffxDispatch(&frameGenContext, &prepare.header);
		if (dispatchResult != FFX_API_RETURN_OK) {
			logger::error("[FidelityFX] PrepareV2 Dispatch failed! Error: 0x{:X}", (uint32_t)dispatchResult);
		}
		if (shouldLog) logger::info("[FSR4_Skyrim] Dispatch Complete. ID: {}", frameID);
	}

	currentFSRFrameID++; // Increment for next frame
}
