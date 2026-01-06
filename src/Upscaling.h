#pragma once

#include <shared_mutex>
#include <atomic>
#include "FidelityFX.h"
#include "WrappedResource.h"

// Memory layout fix for Skyrim SE (1.5.97)
// Correct offsets from ArranzCNL/CommonLibSSE-NG
struct StateEx
{
	uint8_t  pad00[0x34];          // 0x00
	uint32_t unk34;                // 0x34
	uint32_t unk38;                // 0x38
	float    unknown[2];           // 0x3C
	float    projectionPosScaleX;  // 0x44
	float    projectionPosScaleY;  // 0x48
	uint32_t frameCount;           // 0x4C (uiFrameCount)
	bool     insideFrame;          // 0x50
};

namespace RE
{
	class BSImagespaceShaderISTemporalAA;
}

class Upscaling
{
public:
	static Upscaling* GetSingleton()
	{
		static Upscaling singleton;
		return &singleton;
	}

	std::shared_mutex fileLock;
	void LoadINI();
	void SaveINI();
	void RefreshUI();

	struct Settings
	{
		uint32_t frameGenerationMode = 1;
		uint32_t frameLimitMode = 0;
		uint32_t frameGenerationForceEnable = 0;
		float sharpness = 0.5f;
		uint32_t allowAsyncWorkloads = 1;
	};

	Settings settings;
	bool isWindowed = false;
	bool lowRefreshRate = false;

	bool streamlineMissing = false;
	bool fidelityFXMissing = false;

	bool d3d12Interop = false;
	double refreshRate = 0.0f;

	WrappedResource* HUDLessBufferShared = nullptr;
	WrappedResource* upscaledBufferShared = nullptr;
	WrappedResource* depthBufferShared = nullptr;
	WrappedResource* motionVectorBufferShared = nullptr;

	ID3D11ComputeShader* copyDepthToSharedBufferCS;

	bool useHUDLess = false;
	bool earlyCopy = false;

	bool setupBuffers = false;
	
	// Thread-safe flag for resource invalidation during game state changes (Load/New/DataLoaded)
	std::atomic<bool> resourcesInvalidated{ false };

	struct Jitter
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	Jitter jitter;

	void UpdateJitter();
	void CreateFrameGenerationResources();
	void InvalidateResources();
	void EarlyCopyBuffersToSharedResources();
	void CopyBuffersToSharedResources();
	void PostDisplay();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter();

	static double GetRefreshRate(HWND a_window);

	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state)
		{
			func(a_state);
			GetSingleton()->UpdateJitter();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TAA_BeginTechnique
	{
		static void thunk(RE::BSImagespaceShaderISTemporalAA* a_shader, RE::BSTriShape* a_null)
		{
			func(a_shader, a_null);
			GetSingleton()->validTaaPass = true;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TAA_EndTechnique
	{
		static void thunk(RE::BSImagespaceShaderISTemporalAA* a_shader, RE::BSTriShape* a_null)
		{
			auto singleton = GetSingleton();
			if (singleton->validTaaPass) {
				singleton->CopyBuffersToSharedResources();
			}
			func(a_shader, a_null);
			singleton->validTaaPass = false;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool validTaaPass = false;

	static void InstallHooks()
	{
		logger::info("[Upscaling] Installing hooks...");
		LOG_FLUSH();
		auto& trampoline = SKSE::GetTrampoline();
		
		Main_UpdateJitter::func = trampoline.write_call<5>(REL::RelocationID(75460, 77245).address() + REL::Relocate(0xE5, 0xE2, 0x104), reinterpret_cast<uintptr_t>(Main_UpdateJitter::thunk));
		TAA_BeginTechnique::func = trampoline.write_call<5>(REL::RelocationID(100540, 107270).address() + REL::Relocate(0x3E9, 0x3EA, 0x448), reinterpret_cast<uintptr_t>(TAA_BeginTechnique::thunk));
		TAA_EndTechnique::func = trampoline.write_call<5>(REL::RelocationID(100540, 107270).address() + REL::Relocate(0x3F3, 0x3F4, 0x452), reinterpret_cast<uintptr_t>(TAA_EndTechnique::thunk));
		
		logger::info("[Upscaling] All hooks installed successfully.");
		LOG_FLUSH();
	}
};
