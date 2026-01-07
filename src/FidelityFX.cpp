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
	
	// 4. Initialize Anti-Lag 2.0 (AMD only, will gracefully fail on non-AMD)
	InitAntiLag(swapChain->d3d12Device.get());

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

// ============================================================================
// AMD Anti-Lag 2.0 Implementation
// ============================================================================

void FSR4SkyrimHandler::InitAntiLag(ID3D12Device* device)
{
	if (!device) return;
	
	// Zero-initialize context
	memset(&antiLagContext, 0, sizeof(antiLagContext));
	
	HRESULT hr = AMD::AntiLag2DX12::Initialize(&antiLagContext, device);
	if (SUCCEEDED(hr)) {
		antiLagAvailable = true;
		logger::info("[FSR4] AMD Anti-Lag 2.0 initialized successfully!");
	} else {
		antiLagAvailable = false;
		// This is expected on non-AMD systems or older drivers
		if (hr == E_HANDLE) {
			logger::info("[FSR4] Anti-Lag 2.0 not available (non-AMD GPU or driver not supported)");
		} else {
			logger::info("[FSR4] Anti-Lag 2.0 initialization returned: 0x{:X}", (uint32_t)hr);
		}
	}
}

void FSR4SkyrimHandler::UpdateAntiLag()
{
	// Call this before input polling each frame
	if (antiLagAvailable) {
		// maxFPS = 0 means no frame rate limit from Anti-Lag
		AMD::AntiLag2DX12::Update(&antiLagContext, antiLagEnabled, 0);
	}
}

void FSR4SkyrimHandler::MarkEndOfRendering()
{
	// Call this after main rendering workload (after PrepareV2)
	if (antiLagAvailable && antiLagEnabled) {
		AMD::AntiLag2DX12::MarkEndOfFrameRendering(&antiLagContext);
	}
}

void FSR4SkyrimHandler::SetFrameType(bool isInterpolated)
{
	// Call this before Present
	if (antiLagAvailable && antiLagEnabled) {
		AMD::AntiLag2DX12::SetFrameGenFrameType(&antiLagContext, isInterpolated);
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
	static uint64_t callCount = 0;
	callCount++;
	
	uint32_t numGenBefore = params->numGeneratedFrames;
	
	// Check if callback context is valid
	if (!pUserCtx) {
		logger::error("[FG_Callback] pUserCtx is NULL!");
		return FFX_API_RETURN_ERROR;
	}
	
	// Anti-Lag 2.0: Indicate if this is an interpolated frame BEFORE Present
	// numGeneratedFrames > 0 means FSR is generating interpolated frames
	bool isInterpolatedFrame = (params->numGeneratedFrames > 0);
	FSR4SkyrimHandler::GetSingleton()->SetFrameType(isInterpolatedFrame);
	
	// FSR 4.0: Match ENBFrameGeneration's minimal callback - let FSR handle pacing internally
	// DO NOT modify params->numGeneratedFrames - it breaks FSR's frame pacing!
	auto result = ffxDispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
	
	uint32_t numGenAfter = params->numGeneratedFrames;
	// Only warn if interpolation fails repeatedly
	if (numGenAfter == 0 && numGenBefore > 0) {
		static uint64_t zeroGenCount = 0;
		zeroGenCount++;
		if (zeroGenCount <= 5 || zeroGenCount % 300 == 0) {
			logger::warn("[FG_Callback] Frame NOT interpolated! (count={})", zeroGenCount);
		}
	}
	
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

	bool shouldLog = false; // Release: Only log errors
	static uint64_t lastLoggedFrameID = 0;
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

	static bool lastResourcesReady = false;
	if (resourcesReady != lastResourcesReady) {
		logger::info("[FSR4] Resources {}", resourcesReady ? "ready" : "not ready");
		lastResourcesReady = resourcesReady;
	}

	if (shouldLog) {
		logger::info("[FSR4] Frame {}: FG={}, dt={:.1f}ms", frameID, a_useFrameGeneration, manualDeltaTime);
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

	// Handle FG disabled case - must still configure to disable FG
	if (!a_useFrameGeneration && frameGenInitialized) {
		ffxConfigureDescFrameGeneration configParameters{};
		memset(&configParameters, 0, sizeof(configParameters));
		configParameters.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		configParameters.swapChain = swapChain->swapChain;
		configParameters.frameGenerationEnabled = false;
		configParameters.frameGenerationCallback = nullptr;
		configParameters.frameGenerationCallbackUserContext = nullptr;
		configParameters.HUDLessColor = FfxApiResource{};
		configParameters.frameID = frameID;
		ffxConfigure(&frameGenContext, &configParameters.header);
		currentFSRFrameID++;
		return;
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

		// NOTE: AA is now executed synchronously in ReplaceTAA() via DispatchAASync()
		// The upscaledColor buffer already contains the AA result when we reach here.
		// skipTaaEnabled controls whether AA runs in ReplaceTAA.
		bool aaWasExecutedInTAA = upscaling->skipTaaEnabled && upscaleInitialized && 
		                          upscaledColor && resourcesReady;
		
		static uint64_t aaLogCounter = 0;
		(void)aaLogCounter++; // Keep counter for potential debugging
		
		// ========================================================================
		// STEP 2: AA is now executed in ReplaceTAA() - NO AA dispatch here!
		// This ensures AA result is available BEFORE FG Configure
		// ========================================================================
		// (Removed AA dispatch from Present - it now runs synchronously in TAA hook)

		// ========================================================================
		// STEP 3: Configure FG - use AA output (upscaledColor) if AA was executed
		// ========================================================================
		{
			ffxConfigureDescFrameGeneration configParameters{};
			memset(&configParameters, 0, sizeof(configParameters));
			configParameters.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
			configParameters.swapChain = swapChain->swapChain;
			
			configParameters.frameGenerationEnabled = true;
			configParameters.frameGenerationCallback = FrameGenerationCallback;
			configParameters.frameGenerationCallbackUserContext = &frameGenContext;
			
			// Use AA result (upscaledColor) if AA was executed in ReplaceTAA, otherwise raw HUDLess
			if (resourcesReady) {
				configParameters.HUDLessColor = ffxApiGetResourceDX12(aaWasExecutedInTAA ? upscaledColor : HUDLessColor);
			} else {
				configParameters.HUDLessColor = FfxApiResource{};
			}
			
			configParameters.frameID = frameID;
			configParameters.onlyPresentGenerated = false;
			configParameters.allowAsyncWorkloads = true;
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
				logger::info("[FidelityFX] Configure: swapChain={:p}, enabled={}, HUDLess={:p}, aaWasExecutedInTAA={}",
					(void*)configParameters.swapChain,
					configParameters.frameGenerationEnabled,
					configParameters.HUDLessColor.resource,
					aaWasExecutedInTAA);
			}
		}

		// ========================================================================
		// STEP 4: Dispatch PrepareV2
		// ========================================================================
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

		// Diagnostic logging disabled in release build
		// Enable shouldLog above for debugging if needed

		auto dispatchResult = ffxDispatch(&frameGenContext, &prepare.header);
		if (dispatchResult != FFX_API_RETURN_OK) {
			logger::error("[FSR4] PrepareV2 failed! Error: 0x{:X}", (uint32_t)dispatchResult);
		}
		
		// Anti-Lag 2.0: Mark end of main rendering work (after PrepareV2)
		MarkEndOfRendering();
	}

	currentFSRFrameID++; // Increment for next frame
}

// ============================================================================
// Synchronous AA Dispatch for TAA Replacement
// Called from ReplaceTAA() in D3D11 hook context
// ============================================================================
bool FSR4SkyrimHandler::DispatchAASync(
	ID3D12Resource* inputColor,
	ID3D12Resource* outputColor,
	ID3D12Resource* depth,
	ID3D12Resource* motionVectors)
{
	if (!upscaleInitialized || !inputColor || !outputColor) {
		logger::warn("[FidelityFX] DispatchAASync: Not initialized or missing resources");
		return false;
	}
	
	auto swapChain = DX12SwapChain::GetSingleton();
	auto upscaling = Upscaling::GetSingleton();
	
	if (!swapChain || !upscaling) return false;
	
	// Get current command list
	auto commandList = swapChain->commandLists[swapChain->frameIndex].get();
	auto commandAllocator = swapChain->commandAllocators[swapChain->frameIndex].get();
	
	if (!commandList || !commandAllocator) {
		logger::warn("[FidelityFX] DispatchAASync: Missing command list or allocator");
		return false;
	}
	
	// Get state for jitter
	auto state = RE::BSGraphics::State::GetSingleton();
	auto stateEx = state ? reinterpret_cast<StateEx*>(state) : nullptr;
	
	// Use game's deltaTime
	static auto s_deltaTime = (float*)REL::RelocationID(523660, 410199).address();
	float manualDeltaTime = s_deltaTime ? (*s_deltaTime * 1000.0f) : 16.6f;
	if (manualDeltaTime <= 0.0f) manualDeltaTime = 16.6f;
	
	// Camera near/far
	static auto s_cameraNear = (float*)(REL::RelocationID(517032, 403540).address() + 0x40);
	static auto s_cameraFar = (float*)(REL::RelocationID(517032, 403540).address() + 0x44);
	float cameraNearVal = s_cameraNear ? *s_cameraNear : 0.1f;
	float cameraFarVal = s_cameraFar ? *s_cameraFar : 100000.0f;
	if (cameraNearVal <= 0.0f) cameraNearVal = 0.1f;
	if (cameraFarVal <= cameraNearVal) cameraFarVal = 100000.0f;
	
	bool shouldLog = false; // Release: Only log errors
	
	try {
		// Reset command allocator and list for AA work
		DX::ThrowIfFailed(commandAllocator->Reset());
		DX::ThrowIfFailed(commandList->Reset(commandAllocator, nullptr));
		
		// Build AA dispatch descriptor
		ffxDispatchDescUpscale upscaleDispatch{};
		memset(&upscaleDispatch, 0, sizeof(upscaleDispatch));
		upscaleDispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		upscaleDispatch.commandList = commandList;
		upscaleDispatch.color = ffxApiGetResourceDX12(inputColor);
		upscaleDispatch.depth = ffxApiGetResourceDX12(depth);
		upscaleDispatch.motionVectors = ffxApiGetResourceDX12(motionVectors);
		upscaleDispatch.output = ffxApiGetResourceDX12(outputColor, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		
		// Read jitter from game state
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
		upscaleDispatch.upscaleSize = upscaleDispatch.renderSize;  // Native res AA
		upscaleDispatch.frameTimeDelta = manualDeltaTime;
		upscaleDispatch.cameraNear = cameraNearVal;
		upscaleDispatch.cameraFar = cameraFarVal;
		upscaleDispatch.cameraFovAngleVertical = GetVerticalFOVRad();
		upscaleDispatch.viewSpaceToMetersFactor = 0.01428222656f;
		upscaleDispatch.preExposure = 1.0f;
		upscaleDispatch.reset = needsReset || (currentFSRFrameID < 10);
		upscaleDispatch.enableSharpening = true;
		upscaleDispatch.sharpness = upscaling->settings.sharpness;
		upscaleDispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
		
		if (shouldLog) {
			logger::info("[FSR4] AA: jitter=({:.4f},{:.4f})", upscaleDispatch.jitterOffset.x, upscaleDispatch.jitterOffset.y);
		}
		
		// Execute AA dispatch
		auto aaResult = ffxDispatch(&upscaleContext, &upscaleDispatch.header);
		if (aaResult != FFX_API_RETURN_OK) {
			logger::error("[FidelityFX] DispatchAASync: ffxDispatch failed! Error: 0x{:X}", (uint32_t)aaResult);
			return false;
		}
		
		// Transition output to readable state
		D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			outputColor, 
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 
			D3D12_RESOURCE_STATE_COMMON  // COMMON allows D3D11 to read via shared handle
		);
		commandList->ResourceBarrier(1, &barrier);
		
		// Close and execute command list
		DX::ThrowIfFailed(commandList->Close());
		
		ID3D12CommandList* commandListsToExecute[] = { commandList };
		swapChain->commandQueue->ExecuteCommandLists(1, commandListsToExecute);
		
		// Signal D3D12 completion (D3D11 will wait on this)
		swapChain->SignalD3D12ToD3D11();
		
		// Clear reset flag after successful dispatch
		if (needsReset) needsReset = false;
		
		return true;
		
	} catch (const std::exception& e) {
		logger::critical("[FidelityFX] DispatchAASync exception: {}", e.what());
		return false;
	} catch (...) {
		logger::critical("[FidelityFX] DispatchAASync unknown exception!");
		return false;
	}
}
