#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <ffx_api.h>
#include <ffx_api.hpp>
#include <ffx_upscale.h>
#include <ffx_upscale.hpp>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_dx12.hpp>
#include <ffx_framegeneration.h>
#include <ffx_framegeneration.hpp>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.hpp>

class FSR4SkyrimHandler
{
public:
	static FSR4SkyrimHandler* GetSingleton()
	{
		static FSR4SkyrimHandler singleton;
		return &singleton;
	}

	HMODULE module = nullptr;

	ffxContext swapChainContext = nullptr;
	ffxContext frameGenContext = nullptr;
	ffxContext upscaleContext = nullptr;

	bool isAvailable = false;
	bool upscaleInitialized = false;
	bool frameGenInitialized = false;
	bool swapChainContextInitialized = false;
	bool lastBypass = true;

	void LoadFFX();
	void SetupFrameGeneration();
	void Present(bool a_useFrameGeneration, bool a_bypass = false);
};

