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
	if (frameGenInitialized || upscaleInitialized)
		return;

	auto swapChain = DX12SwapChain::GetSingleton();

	ffx::CreateContextDescFrameGeneration createFg{};
	createFg.displaySize = { swapChain->swapChainDesc.Width, swapChain->swapChainDesc.Height };
	createFg.maxRenderSize = createFg.displaySize;
	createFg.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT | 
                     FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
                     FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
                     FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE |
                     FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
                     FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
	createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(swapChain->swapChainDesc.Format);

    // FSR 4.0 Version Descriptor
    ffxCreateContextDescFrameGenerationVersion versionDesc{};
	versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    versionDesc.version = FFX_FRAMEGENERATION_VERSION;

	ffxCreateBackendDX12Desc backendDesc{};
	backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backendDesc.device = swapChain->d3d12Device.get();

	// Link headers manually
	createFg.header.pNext = &versionDesc.header;
	versionDesc.header.pNext = &backendDesc.header;
	backendDesc.header.pNext = nullptr;

	logger::info("[FSR4SkyrimHandler] Attempting to create frame generation context...");
	auto ret = ffxCreateContext(&frameGenContext, &createFg.header, nullptr);
	if (ret != FFX_API_RETURN_OK) {
		logger::critical("[FSR4SkyrimHandler] Failed to create frame generation context! Error code: 0x{:X}", (uint32_t)ret);
	} else {
		logger::info("[FSR4SkyrimHandler] Successfully created frame generation context.");
		frameGenInitialized = true;
	}

	// Setup Upscale Context for Native AA
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
	ret = ffxCreateContext(&upscaleContext, &createUpscale.header, nullptr);
	if (ret != FFX_API_RETURN_OK) {
		logger::critical("[FSR4SkyrimHandler] Failed to create upscale context! Error code: 0x{:X}", (uint32_t)ret);
	} else {
		logger::info("[FSR4SkyrimHandler] Successfully created upscale context.");
		upscaleInitialized = true;
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
	// FSR 4.0: The callback is responsible for the actual interpolation dispatch.
	if (!pUserCtx) return FFX_API_RETURN_ERROR_PARAMETER;

	ffxContext context = reinterpret_cast<ffxContext>(pUserCtx);

	// FORCE Frame Generation: 1 generated frame means 2x FPS
	params->numGeneratedFrames = 1;
	params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;

	static uint32_t callCount = 0;
	static auto lastTime = std::chrono::high_resolution_clock::now();
	
	callCount++;

	auto currentTime = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastTime).count();

	if (duration >= 1000) { // Log every second
		float avgFrameTime = (float)duration / (float)callCount;
		logger::info("[FSR4_Telemetry] FG Callback Stats: Calls/sec: {}, Avg Interval: {:.2f}ms, Last SchedulerID: {}, Generated: {}", 
			callCount, avgFrameTime, params->frameID, params->numGeneratedFrames);
		lastTime = currentTime;
		callCount = 0;
	}

	// The presentColor passed here is the backbuffer containing UI.
	// The HUDLessColor was already provided in ffx::Configure.
	auto ret = ffxDispatch(&context, &params->header);
	if (ret != FFX_API_RETURN_OK) {
		static uint32_t lastErr = 0;
		if ((uint32_t)ret != lastErr) {
			logger::error("[FSR4SkyrimHandler] FrameGenerationCallback: ffxDispatch failed! Error: 0x{:X}, frameID: {}", (uint32_t)ret, params->frameID);
			lastErr = (uint32_t)ret;
		}
	}
	return ret;
}

void FSR4SkyrimHandler::Present(bool a_useFrameGeneration, bool a_bypass)
{
	if (!swapChainContextInitialized)
		return;

	auto upscaling = Upscaling::GetSingleton();
	auto swapChain = DX12SwapChain::GetSingleton();
	auto commandList = swapChain->commandLists[swapChain->frameIndex].get();

	auto state = RE::BSGraphics::State::GetSingleton();
	auto stateEx = reinterpret_cast<StateEx*>(state);

	// FSR 4.0: Use a dedicated, strictly incrementing frame ID for the scheduler.
	static uint64_t internalFrameID = 0;
	uint64_t frameID = internalFrameID++;

	// Calculate manual deltaTime for better stability
	static LARGE_INTEGER lastPresentTime = { 0 };
	LARGE_INTEGER currentTime;
	QueryPerformanceCounter(&currentTime);

	float manualDeltaTime = 16.6f; // Default to 60fps
	if (lastPresentTime.QuadPart != 0 && swapChain->qpf.QuadPart > 0) {
		manualDeltaTime = (float)(currentTime.QuadPart - lastPresentTime.QuadPart) * 1000.0f / (float)swapChain->qpf.QuadPart;
	} else if (swapChain->qpf.QuadPart == 0) {
		// Fallback if QPF is not initialized
		QueryPerformanceFrequency(&swapChain->qpf);
	}
	lastPresentTime = currentTime;

	// Clamp deltaTime to reasonable values (60fps = 16.6ms, 30fps = 33.3ms)
	if (manualDeltaTime < 1.0f) manualDeltaTime = 1.0f;
	if (manualDeltaTime > 100.0f) manualDeltaTime = 100.0f; // Clamp to 10fps min to accommodate heavy areas like Whiterun exterior

	auto HUDLessColor = (upscaling->HUDLessBufferShared) ? upscaling->HUDLessBufferShared->resource.get() : nullptr;
	auto depth = (upscaling->depthBufferShared) ? upscaling->depthBufferShared->resource.get() : nullptr;
	auto motionVectors = (upscaling->motionVectorBufferShared) ? upscaling->motionVectorBufferShared->resource.get() : nullptr;
	auto upscaledColor = (upscaling->upscaledBufferShared) ? upscaling->upscaledBufferShared->resource.get() : nullptr;

	bool shouldLog = (frameID % 100 == 0);
	bool resourcesReady = HUDLessColor && depth && motionVectors && upscaledColor;

	if (!commandList) {
		return;
	}

	if (shouldLog) {
		logger::info("[FSR4SkyrimHandler] Present Dispatch Start. frameID: {}, useFG: {}, bypass: {}, resourcesReady: {}, deltaTime: {:.2f}ms", 
			frameID, a_useFrameGeneration, a_bypass, resourcesReady, manualDeltaTime);
	}

	// 1. Configure Frame Pacing & Scheduler (SwapChain Context)
	if (swapChainContextInitialized) {
		if (shouldLog) logger::info("[FSR4SkyrimHandler] Configuring SwapChain Context...");
		// FSR 4.0: Temporarily disable KeyValue configuration to isolate crash
		/*
		FfxApiSwapchainFramePacingTuning framePacingTuning{ 0.1f, 0.1f, true, 2, false }; 
		ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 framePacingTuningParameters{};
		memset(&framePacingTuningParameters, 0, sizeof(framePacingTuningParameters));
		framePacingTuningParameters.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
		framePacingTuningParameters.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
		framePacingTuningParameters.ptr = &framePacingTuning;

		auto ret = ffxConfigure(&swapChainContext, &framePacingTuningParameters.header);
		if (shouldLog) logger::info("[FSR4SkyrimHandler] SwapChain Context Configured. Result: 0x{:X}", (uint32_t)ret);
		*/
	}

	// 2. Configure Frame Generation Context
	{
		if (shouldLog) logger::info("[FSR4SkyrimHandler] Configuring FrameGen Context... (Initialized: {}, bypass: {}, resourcesReady: {})", frameGenInitialized, a_bypass, resourcesReady);
		if (frameGenInitialized) {
			ffxConfigureDescFrameGeneration configParameters{};
			memset(&configParameters, 0, sizeof(configParameters)); // Zero out to prevent SDK from reading stack garbage
			configParameters.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
			configParameters.swapChain = swapChainContext;
			
			// FSR 4.0: Only enable frame generation if NOT in bypass AND resources are ready
			configParameters.frameGenerationEnabled = a_useFrameGeneration && !a_bypass && resourcesReady;
			
			configParameters.frameGenerationCallback = FrameGenerationCallback;
			configParameters.frameGenerationCallbackUserContext = frameGenContext;
			
			bool useAA = upscaleInitialized && (upscaling->settings.sharpness > 0.0f);
			if (resourcesReady) {
				configParameters.HUDLessColor = ffxApiGetResourceDX12(useAA ? upscaledColor : HUDLessColor);
			} else {
				configParameters.HUDLessColor.resource = nullptr;
				configParameters.HUDLessColor.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
			}
			
			configParameters.frameID = frameID;
			configParameters.onlyPresentGenerated = false;
			configParameters.allowAsyncWorkloads = upscaling->settings.allowAsyncWorkloads != 0; 
			configParameters.flags = FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES;
			configParameters.generationRect.width = swapChain->swapChainDesc.Width;
			configParameters.generationRect.height = swapChain->swapChainDesc.Height;
			
			if (shouldLog) {
				logger::info("[FSR4SkyrimHandler] Debugging configParameters before ffxConfigure:");
				logger::info("  - type: {}", (uint32_t)configParameters.header.type);
				logger::info("  - swapChain: {}", (void*)configParameters.swapChain);
				logger::info("  - enabled: {}", configParameters.frameGenerationEnabled);
				logger::info("  - callback: {}", (void*)configParameters.frameGenerationCallback);
				logger::info("  - userCtx: {}", (void*)configParameters.frameGenerationCallbackUserContext);
				logger::info("  - HUDLessColor.resource: {}", (void*)configParameters.HUDLessColor.resource);
				logger::info("  - frameID: {}", configParameters.frameID);
				logger::info("  - flags: 0x{:X}", configParameters.flags);
				logger::info("  - rect: {}x{}", configParameters.generationRect.width, configParameters.generationRect.height);
				LOG_FLUSH();
			}
			
			if (shouldLog) logger::info("[FSR4SkyrimHandler] Calling ffxConfigure for FrameGen...");
			
			// FSR 4.0: Only call configure if resources are ready OR if we are explicitly enabling/disabling
			// It seems calling it with HUDLessColor.resource = 0x0 causes a crash in some SDK versions
			if (resourcesReady) {
				auto ret = ffxConfigure(&frameGenContext, &configParameters.header);
				if (shouldLog) logger::info("[FSR4SkyrimHandler] FrameGen Context Configured. Result: 0x{:X}", (uint32_t)ret);
			} else {
				if (shouldLog) logger::info("[FSR4SkyrimHandler] Skipping ffxConfigure (Resources not ready).");
			}
		} else {
			if (shouldLog) logger::warn("[FSR4SkyrimHandler] Skipping FrameGen configure (Not Initialized).");
		}
	}

	if (a_useFrameGeneration && frameGenInitialized && !a_bypass && resourcesReady) {
		// Safety check for camera pointers
		static uintptr_t bsGraphicsStateAddr = REL::RelocationID(517032, 403540).address();
		float cameraNearVal = 0.1f;
		float cameraFarVal = 100000.0f;
		if (bsGraphicsStateAddr != 0) {
			cameraNearVal = *(float*)(bsGraphicsStateAddr + 0x40);
			cameraFarVal = *(float*)(bsGraphicsStateAddr + 0x44);
		}

		if (shouldLog) logger::info("[FSR4SkyrimHandler] Dispatching Prepare & AA...");
		// Dispatch FSR AA (Native AA)
		bool useAA = upscaleInitialized && (upscaling->settings.sharpness > 0.0f);
		if (useAA) {
			if (shouldLog) logger::info("[FSR4SkyrimHandler] Dispatching Upscale (AA)...");
			ffxDispatchDescUpscale upscaleDispatch{};
			memset(&upscaleDispatch, 0, sizeof(upscaleDispatch));
			upscaleDispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
			upscaleDispatch.commandList = commandList;
			upscaleDispatch.color = ffxApiGetResourceDX12(HUDLessColor);
			upscaleDispatch.depth = ffxApiGetResourceDX12(depth);
			upscaleDispatch.motionVectors = ffxApiGetResourceDX12(motionVectors);
			upscaleDispatch.output = ffxApiGetResourceDX12(upscaledColor, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

			if (stateEx) {
				upscaleDispatch.jitterOffset.x = stateEx->projectionPosScaleX * (float)swapChain->swapChainDesc.Width / 2.0f;
				upscaleDispatch.jitterOffset.y = stateEx->projectionPosScaleY * (float)swapChain->swapChainDesc.Height / 2.0f;
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
			upscaleDispatch.reset = (frameID < 5);
			upscaleDispatch.enableSharpening = true;
			upscaleDispatch.sharpness = upscaling->settings.sharpness;
			upscaleDispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

			auto ret = ffxDispatch(&upscaleContext, &upscaleDispatch.header);
			if (shouldLog) logger::info("[FSR4SkyrimHandler] Upscale Dispatch Result: 0x{:X}", (uint32_t)ret);

			// Transition upscaledColor to NON_PIXEL_SHADER_RESOURCE for FrameGen
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barrier.Transition.pResource = upscaledColor;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &barrier);
		} else {
			// Even if AA is off, transition upscaledColor to SRV to match the back transition in DX12SwapChain
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			barrier.Transition.pResource = upscaledColor;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &barrier);
		}

		if (shouldLog) logger::info("[FSR4SkyrimHandler] Dispatching FrameGen Prepare...");
		ffxDispatchDescFrameGenerationPrepareV2 dispatchParameters{};
		memset(&dispatchParameters, 0, sizeof(dispatchParameters));
		dispatchParameters.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
		dispatchParameters.commandList = commandList;
		dispatchParameters.renderSize.width = swapChain->swapChainDesc.Width;
		dispatchParameters.renderSize.height = swapChain->swapChainDesc.Height;

		if (stateEx) {
			dispatchParameters.jitterOffset.x = stateEx->projectionPosScaleX * (float)swapChain->swapChainDesc.Width / 2.0f;
			dispatchParameters.jitterOffset.y = stateEx->projectionPosScaleY * (float)swapChain->swapChainDesc.Height / 2.0f;
		}

		dispatchParameters.frameTimeDelta = manualDeltaTime;
		dispatchParameters.frameID = frameID;
		dispatchParameters.flags = FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES;
		dispatchParameters.depth = ffxApiGetResourceDX12(depth);
		dispatchParameters.motionVectors = ffxApiGetResourceDX12(motionVectors);

		dispatchParameters.motionVectorScale.x = (float)swapChain->swapChainDesc.Width;
		dispatchParameters.motionVectorScale.y = (float)swapChain->swapChainDesc.Height;

		dispatchParameters.cameraNear = cameraNearVal;
		dispatchParameters.cameraFar = cameraFarVal;
		dispatchParameters.cameraFovAngleVertical = GetVerticalFOVRad();
		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
		
		dispatchParameters.reset = (frameID < 5) || lastBypass;

		// Fill in camera info from Skyrim's PlayerCamera
		try {
			auto camera = RE::PlayerCamera::GetSingleton();
			if (camera && camera->cameraRoot) {
				auto& world = camera->cameraRoot->world;
				dispatchParameters.cameraPosition[0] = world.translate.x;
				dispatchParameters.cameraPosition[1] = world.translate.y;
				dispatchParameters.cameraPosition[2] = world.translate.z;

				dispatchParameters.cameraRight[0] = world.rotate.entry[0][0];
				dispatchParameters.cameraRight[1] = world.rotate.entry[1][0];
				dispatchParameters.cameraRight[2] = world.rotate.entry[2][0];

				dispatchParameters.cameraForward[0] = world.rotate.entry[0][1];
				dispatchParameters.cameraForward[1] = world.rotate.entry[1][1];
				dispatchParameters.cameraForward[2] = world.rotate.entry[2][1];

				dispatchParameters.cameraUp[0] = world.rotate.entry[0][2];
				dispatchParameters.cameraUp[1] = world.rotate.entry[1][2];
				dispatchParameters.cameraUp[2] = world.rotate.entry[2][2];
			} else {
				dispatchParameters.cameraPosition[0] = 0.0f; dispatchParameters.cameraPosition[1] = 0.0f; dispatchParameters.cameraPosition[2] = 0.0f;
				dispatchParameters.cameraForward[0] = 0.0f; dispatchParameters.cameraForward[1] = 1.0f; dispatchParameters.cameraForward[2] = 0.0f;
				dispatchParameters.cameraUp[0] = 0.0f; dispatchParameters.cameraUp[1] = 0.0f; dispatchParameters.cameraUp[2] = 1.0f;
				dispatchParameters.cameraRight[0] = 1.0f; dispatchParameters.cameraRight[1] = 0.0f; dispatchParameters.cameraRight[2] = 0.0f;
			}
		} catch (...) {
			dispatchParameters.cameraPosition[0] = 0.0f; dispatchParameters.cameraPosition[1] = 0.0f; dispatchParameters.cameraPosition[2] = 0.0f;
			dispatchParameters.cameraForward[0] = 0.0f; dispatchParameters.cameraForward[1] = 1.0f; dispatchParameters.cameraForward[2] = 0.0f;
			dispatchParameters.cameraUp[0] = 0.0f; dispatchParameters.cameraUp[1] = 0.0f; dispatchParameters.cameraUp[2] = 1.0f;
			dispatchParameters.cameraRight[0] = 1.0f; dispatchParameters.cameraRight[1] = 0.0f; dispatchParameters.cameraRight[2] = 0.0f;
		}

		auto ret = ffxDispatch(&frameGenContext, &dispatchParameters.header);
		if (shouldLog) logger::info("[FSR4SkyrimHandler] FrameGen Prepare Result: 0x{:X}", (uint32_t)ret);
		if (shouldLog) logger::info("[FSR4SkyrimHandler] Dispatch Complete.");
	}

	lastBypass = a_bypass;
}
