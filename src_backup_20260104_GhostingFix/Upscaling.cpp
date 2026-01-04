#include "PCH.h"
#include "Upscaling.h"

#include <d3dcompiler.h>
#include <filesystem>

#include "DX12SwapChain.h"
#include "Hooks.h"

#include <ClibUtil/simpleINI.hpp>

#include <ENB/ENBSeriesAPI.h>
extern ENB_API::ENBSDKALT1001* g_ENB;

void Upscaling::LoadINI()
{
	std::lock_guard<std::shared_mutex> lk(fileLock);
	CSimpleIniA ini;
	if (!std::filesystem::exists("enbseries")) {
		std::filesystem::create_directory("enbseries");
	}
	ini.LoadFile("enbseries/enbframegeneration.ini");
	settings.frameGenerationMode = clib_util::ini::get_value<uint32_t>(ini, settings.frameGenerationMode, "FRAME GENERATION", "FrameGenerationMode", "# Default: 1");
	settings.frameLimitMode = clib_util::ini::get_value<uint32_t>(ini, settings.frameLimitMode, "FRAME GENERATION", "FrameLimitMode", "# Default: 0 (Disabled by default for smoothness)");
	settings.frameGenerationForceEnable = clib_util::ini::get_value<uint32_t>(ini, settings.frameGenerationForceEnable, "FRAME GENERATION", "ForceEnable", "# Default: 0");
	settings.sharpness = clib_util::ini::get_value<float>(ini, settings.sharpness, "FRAME GENERATION", "Sharpness", "# RCAS sharpening, range of 0.0 to 1.0\n# Default: 0.5");
	settings.allowAsyncWorkloads = clib_util::ini::get_value<uint32_t>(ini, settings.allowAsyncWorkloads, "FRAME GENERATION", "AllowAsyncWorkloads", "# Default: 1 (Enabled for performance)");
}

void Upscaling::SaveINI()
{
	std::lock_guard<std::shared_mutex> lk(fileLock);
	CSimpleIniA ini;
	if (!std::filesystem::exists("enbseries")) {
		std::filesystem::create_directory("enbseries");
	}
	ini.SetValue("FRAME GENERATION", "FrameGenerationMode", std::to_string(settings.frameGenerationMode).c_str(), "# Default: 1");
	ini.SetValue("FRAME GENERATION", "FrameLimitMode", std::to_string(settings.frameLimitMode).c_str(), "# Default: 0");
	ini.SetValue("FRAME GENERATION", "ForceEnable", std::to_string(settings.frameGenerationForceEnable).c_str(), "# Default: 0");
	ini.SetValue("FRAME GENERATION", "Sharpness", std::to_string(settings.sharpness).c_str(), "# RCAS sharpening, range of 0.0 to 1.0\n# Default: 0.5");
	ini.SetValue("FRAME GENERATION", "AllowAsyncWorkloads", std::to_string(settings.allowAsyncWorkloads).c_str(), "# Default: 1");
	ini.SaveFile("enbseries/enbframegeneration.ini");
}

void Upscaling::RefreshUI()
{
	if (!g_ENB)
		return;

	auto generalBar = g_ENB->TwGetBarByEnum(ENB_API::ENBWindowType::EditorBarButtons);

	if (d3d12Interop)
		g_ENB->TwAddButton(generalBar, "Frame Generation is ENABLED", NULL, NULL, "group='PERFORMANCE'");
	else
		g_ENB->TwAddButton(generalBar, "Frame Generation is DISABLED", NULL, NULL, "group='PERFORMANCE'");

	g_ENB->TwAddButton(generalBar, "Uses AMD FSR 4.0 Frame Generation", NULL, NULL, "group='PERFORMANCE'");
	g_ENB->TwAddButton(generalBar, "Requires a D3D11 to D3D12 proxy", NULL, NULL, "group='PERFORMANCE'");
	g_ENB->TwAddButton(generalBar, "Toggling FG requires a restart", NULL, NULL, "group='PERFORMANCE'");

	if (!isWindowed)
		g_ENB->TwAddButton(generalBar, "Warning: Requires windowed mode", NULL, NULL, "group='PERFORMANCE'");

	if (lowRefreshRate && !settings.frameGenerationForceEnable)
		g_ENB->TwAddButton(generalBar, "Warning: Requires a high refresh rate monitor or Force Enable Frame Generation", NULL, NULL, "group='PERFORMANCE'");

	if (fidelityFXMissing)
		g_ENB->TwAddButton(generalBar, "Warning: FSR 4.0 DLLs are not loaded", NULL, NULL, "group='PERFORMANCE'");


	g_ENB->TwAddVarRW(generalBar, "Frame Generation", TW_TYPE_BOOL32, &settings.frameGenerationMode, "group='PERFORMANCE'");

	if (d3d12Interop) {
		g_ENB->TwAddVarRW(generalBar, "Frame Limit (Variable Refresh Rate)", TW_TYPE_BOOL32, &settings.frameLimitMode, "group='PERFORMANCE'");
		g_ENB->TwAddVarRW(generalBar, "Allow Async Workloads", TW_TYPE_BOOL32, &settings.allowAsyncWorkloads, "group='PERFORMANCE'");
	}

	g_ENB->TwAddVarRW(generalBar, "Sharpness", TW_TYPE_FLOAT, &settings.sharpness, "group='PERFORMANCE' min=0.0 max=1.0 step=0.05");

	g_ENB->TwAddVarRW(generalBar, "Force Enable Frame Generation", TW_TYPE_BOOL32, &settings.frameGenerationForceEnable, "group='PERFORMANCE'");
}

ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const char* ProgramType, const char* Program = "main")
{
	auto device = DX::GetDevice();
	if (!device)
		return nullptr;

	// Compiler setup
	uint32_t flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

	ID3DBlob* shaderBlob;
	ID3DBlob* shaderErrors;

	std::string str;
	std::wstring path{ FilePath };
	std::transform(path.begin(), path.end(), std::back_inserter(str), [](wchar_t c) {
		return (char)c;
	});
	if (!std::filesystem::exists(FilePath)) {
		logger::error("Failed to compile shader; {} does not exist", str);
		return nullptr;
	}
	if (FAILED(D3DCompileFromFile(FilePath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, Program, ProgramType, flags, 0, &shaderBlob, &shaderErrors))) {
		logger::warn("Shader compilation failed:\n\n{}", shaderErrors ? static_cast<char*>(shaderErrors->GetBufferPointer()) : "Unknown error");
		if (shaderErrors) shaderErrors->Release();
		return nullptr;
	}
	if (shaderErrors) {
		logger::debug("Shader logs:\n{}", static_cast<char*>(shaderErrors->GetBufferPointer()));
		shaderErrors->Release();
	}

	if (!shaderBlob) {
		return nullptr;
	}

	ID3D11ComputeShader* regShader = nullptr;
	HRESULT hr = device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader);
	shaderBlob->Release();

	if (FAILED(hr)) {
		logger::error("Failed to create compute shader (HRESULT: {:08X})", (uint32_t)hr);
		return nullptr;
	}

	return regShader;
}

void Upscaling::UpdateJitter()
{
	if (!d3d12Interop)
		return;

	try {
		auto state = RE::BSGraphics::State::GetSingleton();
		if (!state)
			return;

		auto gameViewport = reinterpret_cast<StateEx*>(state);

		auto ffx = FidelityFX::GetSingleton();
		if (!ffx->upscaleInitialized)
			return;

		static bool loggedOnce = false;
		if (!loggedOnce) {
			auto dx12SwapChain = DX12SwapChain::GetSingleton();
			logger::info("[Upscaling] UpdateJitter: First call. FrameCount: {}, Res: {}x{}", 
				gameViewport->frameCount, dx12SwapChain->swapChainDesc.Width, dx12SwapChain->swapChainDesc.Height);
			LOG_FLUSH();
			loggedOnce = true;
		}

		ffxQueryDescUpscaleGetJitterOffset queryDesc = {};
		queryDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
		queryDesc.index = gameViewport->frameCount;
		queryDesc.phaseCount = 8;
		queryDesc.pOutX = &jitter.x;
		queryDesc.pOutY = &jitter.y;

		if (ffx::Query(ffx->upscaleContext, queryDesc) != ffx::ReturnCode::Ok)
			return;

		auto dx12SwapChain = DX12SwapChain::GetSingleton();
		uint32_t screenWidth = dx12SwapChain->swapChainDesc.Width;
		uint32_t screenHeight = dx12SwapChain->swapChainDesc.Height;

		if (screenWidth == 0 || screenHeight == 0)
			return;

		gameViewport->projectionPosScaleX = -2.0f * jitter.x / (float)screenWidth;
		gameViewport->projectionPosScaleY = 2.0f * jitter.y / (float)screenHeight;
		
		static bool loggedJitter = false;
		if (!loggedJitter) {
			logger::info("[Upscaling] Jitter calculated and applied: {}, {}", jitter.x, jitter.y);
			LOG_FLUSH();
			loggedJitter = true;
		}
	} catch (const std::exception& e) {
		logger::critical("[Upscaling] UpdateJitter Exception: {}", e.what());
		LOG_FLUSH();
	} catch (...) {
		logger::critical("[Upscaling] UpdateJitter Unknown Exception");
		LOG_FLUSH();
	}
}

void Upscaling::CreateFrameGenerationResources()
{
	logger::info("[Upscaling] CreateFrameGenerationResources Entry");
	LOG_FLUSH();
	try {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return;

		auto& main = renderer->data.renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

		if (!main.texture || !main.SRV || !main.RTV || !motionVector.texture) {
			static bool loggedOnce = false;
			if (!loggedOnce) {
				logger::warn("[Upscaling] One or more game textures not available for resource creation.");
				LOG_FLUSH();
				loggedOnce = true;
			}
			return;
		}

		auto dx12SwapChain = DX12SwapChain::GetSingleton();
		if (!dx12SwapChain->d3d12Device || !dx12SwapChain->swapChain) {
			return;
		}

		logger::info("[Frame Generation] Creating resources (Reverse Interop)...");
		LOG_FLUSH();
		
		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);

		// Common flags for shared resources
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.CPUAccessFlags = 0;

		// HUDLess & Upscaled (R8G8B8A8_UNORM)
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		logger::info("[Upscaling] Creating HUDLessBufferShared...");
		LOG_FLUSH();
		HUDLessBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());
		
		logger::info("[Upscaling] Creating upscaledBufferShared...");
		LOG_FLUSH();
		upscaledBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Depth (R32_FLOAT)
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		logger::info("[Upscaling] Creating depthBufferShared...");
		LOG_FLUSH();
		depthBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Motion Vectors (Original Format)
		D3D11_TEXTURE2D_DESC texDescMV{};
		motionVector.texture->GetDesc(&texDescMV);
		texDesc.Format = texDescMV.Format;
		logger::info("[Upscaling] Creating motionVectorBufferShared...");
		LOG_FLUSH();
		motionVectorBufferShared = new WrappedResource(texDesc, dx12SwapChain->d3d11Device.get(), dx12SwapChain->d3d12Device.get());

		// Find shader...
		logger::info("[Upscaling] Compiling depth copy shader...");
		LOG_FLUSH();
		HMODULE hModule = NULL;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)&Upscaling::GetSingleton, &hModule);
		wchar_t pluginPath[MAX_PATH];
		GetModuleFileNameW(hModule, pluginPath, MAX_PATH);
		std::wstring path(pluginPath);
		size_t lastSlash = path.find_last_of(L"\\/");
		std::wstring dir = (lastSlash != std::wstring::npos) ? path.substr(0, lastSlash) : L".";
		
		std::wstring dllName = path.substr(lastSlash + 1);
		size_t dot = dllName.find_last_of(L".");
		std::wstring folderName = (dot != std::wstring::npos) ? dllName.substr(0, dot) : dllName;
		
		std::wstring shaderPath = dir + L"\\" + folderName + L"\\CopyDepthToSharedBufferCS.hlsl";
		if (!std::filesystem::exists(shaderPath)) {
			shaderPath = dir + L"\\CopyDepthToSharedBufferCS.hlsl";
		}

		copyDepthToSharedBufferCS = (ID3D11ComputeShader*)CompileShader(shaderPath.c_str(), "cs_5_0");

		if (copyDepthToSharedBufferCS) {
			logger::info("[Upscaling] Frame generation resources initialized successfully.");
			LOG_FLUSH();
			setupBuffers = true;
		} else {
			logger::error("[Upscaling] Failed to compile depth copy shader. Frame generation will be disabled.");
			LOG_FLUSH();
		}
	} catch (const std::exception& e) {
		logger::critical("[Upscaling] CreateFrameGenerationResources: Exception: {}", e.what());
		LOG_FLUSH();
	} catch (...) {
		logger::critical("[Upscaling] CreateFrameGenerationResources: Unknown exception!");
		LOG_FLUSH();
	}
}

void Upscaling::EarlyCopyBuffersToSharedResources()
{
	if (!d3d12Interop)
		return;

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	if (!renderer)
		return;

	if (!setupBuffers) {
		auto& main = renderer->data.renderTargets[RE::RENDER_TARGETS::kMAIN];
		// Ignore before assets have not been created yet
		if (!main.texture)
			return;
		logger::info("[Upscaling] Initializing frame generation resources (Early)...");
		CreateFrameGenerationResources();
	}

	auto context = DX::GetContext();
	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	if (RE::UI::GetSingleton()->GameIsPaused())
	{
		float clearColor[4] = { 0, 0, 0, 0 };
		if (motionVectorBufferShared && motionVectorBufferShared->rtv)
			context->ClearRenderTargetView(motionVectorBufferShared->rtv, clearColor);
	} else {
		auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		if (motionVector.texture && motionVectorBufferShared && motionVectorBufferShared->resource11) {
			context->CopyResource(motionVectorBufferShared->resource11, motionVector.texture);
		}
	}

	{
		auto& depth = renderer->data.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		if (depth.depthSRV && copyDepthToSharedBufferCS && depthBufferShared && depthBufferShared->uav) {
			uint32_t dispatchX = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Width) / 8.0f);
			uint32_t dispatchY = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Height) / 8.0f);

			ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[1] = { depthBufferShared->uav };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(copyDepthToSharedBufferCS, nullptr, 0);

			context->Dispatch(dispatchX, dispatchY, 1);

			ID3D11ShaderResourceView* nullViews[1] = { nullptr };
			context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);

			ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);

			context->CSSetShader(nullptr, nullptr, 0);
		}
	}

	earlyCopy = true;
}


void Upscaling::CopyBuffersToSharedResources()
{
	if (!d3d12Interop)
		return;

	try {
		auto dx12SwapChain = DX12SwapChain::GetSingleton();
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return;
		}

		if (!setupBuffers) {
			auto& main = renderer->data.renderTargets[RE::RENDER_TARGETS::kMAIN];
			if (!main.texture) {
				return;
			}
			logger::info("[Upscaling] Initializing frame generation resources...");
			LOG_FLUSH();
			CreateFrameGenerationResources();
			if (!setupBuffers) {
				return;
			}
		}

		auto context = DX::GetContext();

		// 1. Motion Vectors
		if (!RE::UI::GetSingleton()->GameIsPaused()) {
			auto& motionVector = renderer->data.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			if (motionVector.texture && motionVectorBufferShared && motionVectorBufferShared->resource11) {
				context->CopyResource(motionVectorBufferShared->resource11, motionVector.texture);
			}
		}

		// 2. Depth
		{
			auto& depth = renderer->data.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			if (depth.depthSRV && copyDepthToSharedBufferCS && depthBufferShared && depthBufferShared->uav) {
				uint32_t dispatchX = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Width) / 8.0f);
				uint32_t dispatchY = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Height) / 8.0f);

				ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
				context->CSSetShaderResources(0, ARRAYSIZE(views), views);
				ID3D11UnorderedAccessView* uavs[1] = { depthBufferShared->uav };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
				context->CSSetShader(copyDepthToSharedBufferCS, nullptr, 0);
				context->Dispatch(dispatchX, dispatchY, 1);

				ID3D11ShaderResourceView* nullViews[1] = { nullptr };
				context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
				ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
				context->CSSetShader(nullptr, nullptr, 0);
			}
		}

		// 3. HUDLess (Backbuffer before UI)
		auto& swapChain = renderer->data.renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
		if (swapChain.texture && HUDLessBufferShared && HUDLessBufferShared->resource11) {
			context->CopyResource(HUDLessBufferShared->resource11, swapChain.texture);
		}

	} catch (...) {
		logger::critical("[Upscaling] CopyBuffersToSharedResources: Unknown exception!");
		LOG_FLUSH();
	}
}

void Upscaling::PostDisplay()
{
    // No longer used, replaced by TAA_EndTechnique
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter()
{
	if (d3d12Interop && settings.frameLimitMode) {
		static uint64_t frameCount = 0;
		if (frameCount % 60 == 0) {
			logger::info("[Upscaling] FrameLimiter active. Target Refresh Rate: {}", refreshRate);
		}
		frameCount++;

		double bestRefreshRate = refreshRate - (refreshRate * refreshRate) / 3600.0;

		LARGE_INTEGER qpf;
		QueryPerformanceFrequency(&qpf);

		int64_t targetFrameTicks = int64_t(double(qpf.QuadPart) / (bestRefreshRate * (settings.frameGenerationMode ? 0.5 : 1.0)));

		static LARGE_INTEGER lastFrame = {};
		LARGE_INTEGER timeNow;
		QueryPerformanceCounter(&timeNow);
		int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
		if (delta < targetFrameTicks) {
			TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
		}
		QueryPerformanceCounter(&lastFrame);
	}
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						if (wcscmp(info.szDevice, sourceName.viewGdiDeviceName) == 0) {
							// get the refresh rate
							UINT numerator = p.targetInfo.refreshRate.Numerator;
							UINT denominator = p.targetInfo.refreshRate.Denominator;
							return (double)numerator / (double)denominator;
						}
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}
