#include "PCH.h"
#include "FidelityFX.h"
#include "Upscaling.h"
#include "DX12SwapChain.h"

#include <dx12/ffx_api_framegeneration_dx12.hpp>
#include <ffx_framegeneration.hpp>

void FidelityFX::LoadFFX()
{
    // Get the path of the current DLL (FSR4_Skyrim.dll)
    wchar_t pluginPath[MAX_PATH];
    HMODULE hModule = NULL;
    
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, 
                          (LPCWSTR)&FidelityFX::GetSingleton, &hModule)) {
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
        logger::info("[FidelityFX] Successfully located and loaded AMD FidelityFX loader and effect DLLs.");
    } else {
        logger::error("[FidelityFX] amd_fidelityfx_loader_dx12.dll not found!");
        isAvailable = false;
    }
}


void FidelityFX::SetupFrameGeneration()
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
                     FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
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

	logger::info("[FidelityFX] Attempting to create frame generation context...");
	auto ret = ffxCreateContext(&frameGenContext, &createFg.header, nullptr);
	if (ret != FFX_API_RETURN_OK) {
		logger::critical("[FidelityFX] Failed to create frame generation context! Error code: 0x{:X}", (uint32_t)ret);
	} else {
		logger::info("[FidelityFX] Successfully created frame generation context.");
		frameGenInitialized = true;
	}

	// Setup Upscale Context for Native AA
	ffx::CreateContextDescUpscale createUpscale{};
	createUpscale.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | 
                          FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
                          FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
	createUpscale.maxRenderSize = { swapChain->swapChainDesc.Width, swapChain->swapChainDesc.Height };
	createUpscale.maxUpscaleSize = createUpscale.maxRenderSize;

	ffxCreateContextDescUpscaleVersion upscaleVersionDesc{};
	upscaleVersionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
	upscaleVersionDesc.version = FFX_UPSCALER_VERSION;

	createUpscale.header.pNext = &upscaleVersionDesc.header;
	upscaleVersionDesc.header.pNext = &backendDesc.header;

	logger::info("[FidelityFX] Attempting to create upscale context (Native AA)...");
	ret = ffxCreateContext(&upscaleContext, &createUpscale.header, nullptr);
	if (ret != FFX_API_RETURN_OK) {
		logger::critical("[FidelityFX] Failed to create upscale context! Error code: 0x{:X}", (uint32_t)ret);
	} else {
		logger::info("[FidelityFX] Successfully created upscale context.");
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
	// We must ensure the params are correctly configured for 1 interpolated frame.
	params->numGeneratedFrames = 1;
	params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;

	static uint64_t lastLoggedID = 0xFFFFFFFFFFFFFFFF;
	if (params->frameID > lastLoggedID + 60 || params->frameID < lastLoggedID) {
		logger::info("[FidelityFX] FrameGenerationCallback executing. Scheduler FrameID: {}, numGenerated: {}", 
			params->frameID, params->numGeneratedFrames);
		lastLoggedID = params->frameID;
	}

	// The presentColor passed here is the backbuffer containing UI.
	// The HUDLessColor was already provided in ffx::Configure.
	return ffxDispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
}

void FidelityFX::Present(bool a_useFrameGeneration, bool a_bypass)
{
	if (!swapChainContextInitialized)
		return;

	auto upscaling = Upscaling::GetSingleton();
	auto swapChain = DX12SwapChain::GetSingleton();
	auto commandList = swapChain->commandLists[swapChain->frameIndex].get();

	auto state = RE::BSGraphics::State::GetSingleton();
	auto stateEx = reinterpret_cast<StateEx*>(state);

	// Use the stable frameCounter from DX12SwapChain to ensure strict +1 increments.
	// FSR 4.0 is extremely sensitive to frameID jumps.
	uint64_t frameID = swapChain->frameCounter;

	auto HUDLessColor = (upscaling->HUDLessBufferShared) ? upscaling->HUDLessBufferShared->resource.get() : nullptr;
	auto depth = (upscaling->depthBufferShared) ? upscaling->depthBufferShared->resource.get() : nullptr;
	auto motionVectors = (upscaling->motionVectorBufferShared) ? upscaling->motionVectorBufferShared->resource.get() : nullptr;
	auto upscaledColor = (upscaling->upscaledBufferShared) ? upscaling->upscaledBufferShared->resource.get() : nullptr;

	if (!HUDLessColor || !depth || !motionVectors || !upscaledColor) {
		return;
	}

	// Resource Barriers for FSR (Reverse Interop: COMMON -> READ/UAV)
	{
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(HUDLessColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(upscaledColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	}

	bool shouldLog = (frameID % 300 == 0);
	if (shouldLog) {
		logger::info("[FidelityFX] Present Dispatch Start. frameID: {}, useFG: {}, bypass: {}, swapChainContext: {}", 
			frameID, a_useFrameGeneration, a_bypass, swapChainContextInitialized);
	}

	// Stability & Smoothness: Configure Frame Pacing
	static bool pacingConfigured = false;
	if (!pacingConfigured && swapChainContextInitialized) {
		FfxApiSwapchainFramePacingTuning framePacingTuning{ 0.1f, 0.1f, true, 2, false };
		ffx::ConfigureDescFrameGenerationSwapChainKeyValueDX12 framePacingTuningParameters{};
		framePacingTuningParameters.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
		framePacingTuningParameters.ptr = &framePacingTuning;

		if (ffx::Configure(swapChainContext, framePacingTuningParameters) == ffx::ReturnCode::Ok) {
			logger::info("[FidelityFX] Frame pacing tuning configured successfully.");
			pacingConfigured = true;
		}
	}

	ffx::ConfigureDescFrameGeneration configParameters{};

	if (frameGenInitialized) {
		// Even if a_bypass is true, we should keep FSR informed of the frameID to prevent drift
		if (a_useFrameGeneration && !a_bypass) {
			configParameters.frameGenerationEnabled = true;
			configParameters.frameGenerationCallback = FrameGenerationCallback;
			configParameters.frameGenerationCallbackUserContext = &frameGenContext;
			configParameters.HUDLessColor = ffxApiGetResourceDX12(HUDLessColor);
		} else {
			configParameters.frameGenerationEnabled = false;
			configParameters.frameGenerationCallbackUserContext = nullptr;
			configParameters.frameGenerationCallback = nullptr;
			configParameters.HUDLessColor = ffxApiGetResourceDX12(nullptr);
		}

		configParameters.frameID = frameID;
		configParameters.swapChain = swapChain->swapChain;
		configParameters.onlyPresentGenerated = false;
		configParameters.allowAsyncWorkloads = upscaling->settings.allowAsyncWorkloads != 0;
		configParameters.flags = 0;

		configParameters.generationRect.left = 0;
		configParameters.generationRect.top = 0;
		configParameters.generationRect.width = swapChain->swapChainDesc.Width;
		configParameters.generationRect.height = swapChain->swapChainDesc.Height;

		try {
			auto ret = ffx::Configure(frameGenContext, configParameters);
			if (ret != ffx::ReturnCode::Ok) {
				static uint32_t lastError = 0;
				if ((uint32_t)ret != lastError) {
					logger::error("[FidelityFX] Failed to configure frame generation! Error: 0x{:X}", (uint32_t)ret);
					lastError = (uint32_t)ret;
				}
			}
		} catch (...) {
			logger::critical("[FidelityFX] CRASH in ffx::Configure!");
		}
	}

	if (a_useFrameGeneration && frameGenInitialized && !a_bypass) {
		// Resource Barriers for FSR
		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(HUDLessColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(upscaledColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
			commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}

		// Dispatch FSR AA (Native AA)
		bool useAA = upscaleInitialized && (upscaling->settings.sharpness > 0.0f);
		if (useAA) {
			ffx::DispatchDescUpscale upscaleDispatch{};
			upscaleDispatch.commandList = commandList;
			upscaleDispatch.color = ffxApiGetResourceDX12(HUDLessColor);
			upscaleDispatch.depth = ffxApiGetResourceDX12(depth);
			upscaleDispatch.motionVectors = ffxApiGetResourceDX12(motionVectors);
			upscaleDispatch.output = ffxApiGetResourceDX12(upscaledColor);

			if (stateEx) {
				upscaleDispatch.jitterOffset.x = -upscaling->jitter.x;
				upscaleDispatch.jitterOffset.y = upscaling->jitter.y;
			}

			upscaleDispatch.motionVectorScale.x = (float)swapChain->swapChainDesc.Width;
			upscaleDispatch.motionVectorScale.y = (float)swapChain->swapChainDesc.Height;
			upscaleDispatch.renderSize.width = swapChain->swapChainDesc.Width;
			upscaleDispatch.renderSize.height = swapChain->swapChainDesc.Height;
			upscaleDispatch.upscaleSize = upscaleDispatch.renderSize;

			auto deltaTime = (float*)REL::RelocationID(523660, 410199).address();
			upscaleDispatch.frameTimeDelta = *deltaTime * 1000.f;

			static auto cameraNear = (float*)(REL::RelocationID(517032, 403540).address() + 0x40);
			static auto cameraFar = (float*)(REL::RelocationID(517032, 403540).address() + 0x44);
			upscaleDispatch.cameraNear = *cameraNear;
			upscaleDispatch.cameraFar = *cameraFar;
			upscaleDispatch.cameraFovAngleVertical = GetVerticalFOVRad();
			upscaleDispatch.viewSpaceToMetersFactor = 0.01428222656f;
			upscaleDispatch.preExposure = 1.0f;
			upscaleDispatch.reset = false;
			upscaleDispatch.enableSharpening = true;
			upscaleDispatch.sharpness = upscaling->settings.sharpness;

			try {
				if (ffx::Dispatch(upscaleContext, upscaleDispatch) != ffx::ReturnCode::Ok) {
					logger::critical("[FidelityFX] Failed to dispatch upscale!");
				}
			} catch (...) {
				logger::critical("[FidelityFX] CRASH in ffx::Dispatch (Upscale)!");
			}
		}

		auto deltaTimePtr = (float*)REL::RelocationID(523660, 410199).address();
		float currentDeltaTime = *deltaTimePtr * 1000.f;

		if (shouldLog) logger::info("[FidelityFX] Dispatching Frame Generation Prepare. frameID: {}, deltaTime: {}", frameID, currentDeltaTime);
		LOG_FLUSH();
		ffx::DispatchDescFrameGenerationPrepareV2 dispatchParameters{};

		dispatchParameters.commandList = commandList;
		dispatchParameters.motionVectorScale.x = (float)swapChain->swapChainDesc.Width;
		dispatchParameters.motionVectorScale.y = (float)swapChain->swapChainDesc.Height;
		dispatchParameters.renderSize.width = swapChain->swapChainDesc.Width;
		dispatchParameters.renderSize.height = swapChain->swapChainDesc.Height;

		if (stateEx) {
			dispatchParameters.jitterOffset.x = -upscaling->jitter.x;
			dispatchParameters.jitterOffset.y = upscaling->jitter.y;
		}

		dispatchParameters.frameTimeDelta = currentDeltaTime;
		if (dispatchParameters.frameTimeDelta < 1.0f) dispatchParameters.frameTimeDelta = 16.6f; // Fallback to 60fps if delta is too small

		static auto cameraNear = (float*)(REL::RelocationID(517032, 403540).address() + 0x40);
		static auto cameraFar = (float*)(REL::RelocationID(517032, 403540).address() + 0x44);

		dispatchParameters.cameraNear = *cameraNear;
		dispatchParameters.cameraFar = *cameraFar;
		dispatchParameters.cameraFovAngleVertical = GetVerticalFOVRad();
		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
		dispatchParameters.frameID = frameID;

		dispatchParameters.depth = ffxApiGetResourceDX12(depth);
		dispatchParameters.motionVectors = ffxApiGetResourceDX12(motionVectors);

		// Initialize camera vectors to defaults to prevent garbage data
		for (int i = 0; i < 3; ++i) {
			dispatchParameters.cameraPosition[i] = 0.0f;
			dispatchParameters.cameraUp[i] = (i == 2) ? 1.0f : 0.0f; // Z is Up
			dispatchParameters.cameraRight[i] = (i == 0) ? 1.0f : 0.0f; // X is Right
			dispatchParameters.cameraForward[i] = (i == 1) ? 1.0f : 0.0f; // Y is Forward
		}

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

				dispatchParameters.cameraUp[0] = world.rotate.entry[0][2];
				dispatchParameters.cameraUp[1] = world.rotate.entry[1][2];
				dispatchParameters.cameraUp[2] = world.rotate.entry[2][2];

				dispatchParameters.cameraForward[0] = world.rotate.entry[0][1];
				dispatchParameters.cameraForward[1] = world.rotate.entry[1][1];
				dispatchParameters.cameraForward[2] = world.rotate.entry[2][1];
			}
		} catch (...) {}

		dispatchParameters.reset = false;

		try {
			if (frameGenInitialized && ffx::Dispatch(frameGenContext, dispatchParameters) != ffx::ReturnCode::Ok) {
				logger::critical("[FidelityFX] Failed to dispatch frame generation!");
			}
		} catch (...) {
			logger::critical("[FidelityFX] CRASH in ffx::Dispatch (Frame Generation)!");
		}
	}

	// Resource Barriers for FSR (Restore to COMMON)
	{
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(HUDLessColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(motionVectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(upscaledColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	}
}
