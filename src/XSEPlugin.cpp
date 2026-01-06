#include "PCH.h"
#include "Hooks.h"
#include "Upscaling.h"
#include "DX12SwapChain.h"
#include "FidelityFX.h"

#include <ENB/ENBSeriesAPI.h>
ENB_API::ENBSDKALT1001* g_ENB = nullptr;

#define LOG_FLUSH() spdlog::default_logger()->flush()

// REMOVED: MessageHandler
// ENBFrameGeneration does NOT use SKSE MessageHandler at all.
// It trusts that render targets are stable throughout the game lifecycle.
// Our previous attempts to handle game state changes here caused crashes.
// FSR 4.0's internal reset mechanism (via frameID discontinuity) should handle scene transitions.

bool Load()
{
	if (REL::Module::IsVR()) {
		logger::info("Skyrim VR detected, disabling all hooks and features");
		return true;
	}

	// REMOVED: messaging->RegisterListener(MessageHandler)
	// Following ENBFrameGeneration: No SKSE message handling

	g_ENB = reinterpret_cast<ENB_API::ENBSDKALT1001*>(ENB_API::RequestENBAPI(ENB_API::SDKVersion::V1001));

	if (g_ENB) {
		logger::info("Obtained ENB API");

		g_ENB->SetCallbackFunction([](ENBCallbackType calltype) {
			switch (calltype) {
			case ENBCallbackType::ENBCallback_OnInit:
				DX12SwapChain::GetSingleton()->enbReady = true;
				logger::info("[ENB] Callback: OnInit received. ENB is ready.");
				break;
			case ENBCallbackType::ENBCallback_PostLoad:
				Upscaling::GetSingleton()->RefreshUI();
				Upscaling::GetSingleton()->LoadINI();
				// Request FSR reset on game load to handle camera discontinuity
				FSR4SkyrimHandler::GetSingleton()->RequestReset();
				logger::info("[ENB] Callback: PostLoad - FSR reset requested.");
				break;
			case ENBCallbackType::ENBCallback_PostReset:
				Upscaling::GetSingleton()->RefreshUI();
				// Request FSR reset on device reset
				FSR4SkyrimHandler::GetSingleton()->RequestReset();
				logger::info("[ENB] Callback: PostReset - FSR reset requested.");
				break;
			case ENBCallbackType::ENBCallback_PreSave:
				Upscaling::GetSingleton()->SaveINI();
				break;
			}
		});
	} else {
		logger::info("ENB API not found, running in standalone mode");
		DX12SwapChain::GetSingleton()->enbReady = true; // Set to ready immediately if no ENB
	}

	SKSE::AllocTrampoline(512);

	Hooks::Install();
	Upscaling::InstallHooks();
	Upscaling::GetSingleton()->LoadINI();

	return true;
}
