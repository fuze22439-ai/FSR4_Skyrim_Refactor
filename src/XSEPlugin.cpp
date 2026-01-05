#include "PCH.h"
#include "Hooks.h"
#include "Upscaling.h"
#include "DX12SwapChain.h"
#include "FidelityFX.h"

#include <ENB/ENBSeriesAPI.h>
ENB_API::ENBSDKALT1001* g_ENB = nullptr;

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kPostLoadGame:
	case SKSE::MessagingInterface::kNewGame:
	case SKSE::MessagingInterface::kDataLoaded:
		logger::info("Game state change detected (Load/New/DataLoaded), resetting FSR state...");
		// FSR4SkyrimHandler::Present will handle the reset via lastBypass=true
		// but we can explicitly set it here if we want to be extra safe.
		FSR4SkyrimHandler::GetSingleton()->lastBypass = true;
		break;
	}
}

bool Load()
{
	if (REL::Module::IsVR()) {
		logger::info("Skyrim VR detected, disabling all hooks and features");
		return true;
	}

	auto messaging = SKSE::GetMessagingInterface();
	if (messaging) {
		messaging->RegisterListener(MessageHandler);
		logger::info("Registered SKSE message listener");
	}

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
				break;
			case ENBCallbackType::ENBCallback_PostReset:
				Upscaling::GetSingleton()->RefreshUI();
				break;
			case ENBCallbackType::ENBCallback_PreSave:
				Upscaling::GetSingleton()->SaveINI();
				break;
			}
		});
	} else {
		logger::info("ENB API not found, running in standalone mode");
	}

	SKSE::AllocTrampoline(512);

	Hooks::Install();
	Upscaling::InstallHooks();
	Upscaling::GetSingleton()->LoadINI();

	return true;
}
