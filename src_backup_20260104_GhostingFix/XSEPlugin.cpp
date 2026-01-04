#include "PCH.h"
#include "Hooks.h"
#include "Upscaling.h"
#include "DX12SwapChain.h"

#include <ENB/ENBSeriesAPI.h>
ENB_API::ENBSDKALT1001* g_ENB = nullptr;

bool Load()
{
	if (REL::Module::IsVR()) {
		logger::info("Skyrim VR detected, disabling all hooks and features");
		return true;
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
