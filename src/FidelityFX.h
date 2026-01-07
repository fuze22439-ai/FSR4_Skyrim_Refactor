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

// AMD Anti-Lag 2.0 SDK
#include <amd/antilag2/ffx_antilag2_dx12.h>

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
	uint64_t currentFSRFrameID = 1;
	
	// Reset flag for scene transitions (load game, fast travel, etc.)
	bool needsReset = true;  // Start with reset to handle initial frames
	
	// Anti-Lag 2.0
	AMD::AntiLag2DX12::Context antiLagContext = {};
	bool antiLagAvailable = false;
	bool antiLagEnabled = true;  // User setting
	
	void RequestReset() { needsReset = true; }

	void LoadFFX();
	void SetupFrameGeneration();
	void InitAntiLag(ID3D12Device* device);
	void UpdateAntiLag();  // Call before input polling
	void MarkEndOfRendering();  // Call after PrepareV2
	void SetFrameType(bool isInterpolated);  // Call before Present
	void Present(bool a_useFrameGeneration, bool a_bypass = false);
	
	// Synchronous AA dispatch for TAA replacement
	// Called from ReplaceTAA() in D3D11 context, blocks until D3D12 AA completes
	// Returns true if AA was executed successfully
	bool DispatchAASync(
		ID3D12Resource* inputColor,      // HUDLessBufferShared->resource (AA input)
		ID3D12Resource* outputColor,     // upscaledBufferShared->resource (AA output)
		ID3D12Resource* depth,           // depthBufferShared->resource
		ID3D12Resource* motionVectors    // motionVectorBufferShared->resource
	);
};
