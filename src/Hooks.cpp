#include "Hooks.h"

#include <detours/Detours.h>

#include "Upscaling.h"
#include "DX12SwapChain.h"

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChain;
decltype(&IDXGIFactory::CreateSwapChain) ptrCreateSwapChain;

HRESULT WINAPI hk_IDXGIFactory_CreateSwapChain(IDXGIFactory2* This, _In_ ID3D11Device* a_device, _In_ DXGI_SWAP_CHAIN_DESC* pDesc, _COM_Outptr_ IDXGISwapChain** ppSwapChain)
{
	logger::info("[Hooks] hk_IDXGIFactory_CreateSwapChain called");
	pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	IDXGIFactory5* dxgiFactory = nullptr;
	if (SUCCEEDED(This->QueryInterface(IID_PPV_ARGS(&dxgiFactory)))) {
		BOOL allowTearing = false;
		if (SUCCEEDED(dxgiFactory->CheckFeatureSupport(
				DXGI_FEATURE_PRESENT_ALLOW_TEARING,
				&allowTearing,
				sizeof(allowTearing)))) {
			if (allowTearing) {
				pDesc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
			} else {
				pDesc->Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
			}
		}
		dxgiFactory->Release();
	}

	IDXGIDevice* dxgiDevice = nullptr;
	DX::ThrowIfFailed(a_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice));
	
	IDXGIAdapter* adapter = nullptr;
	DX::ThrowIfFailed(dxgiDevice->GetAdapter(&adapter));
	
	auto proxy = DX12SwapChain::GetSingleton();

	proxy->SetD3D11Device(a_device);

	ID3D11DeviceContext* context;
	a_device->GetImmediateContext(&context);
	proxy->SetD3D11DeviceContext(context);

	proxy->CreateD3D12Device(adapter);
	
	IDXGIFactory4* factory4 = nullptr;
	if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory4)))) {
		proxy->CreateSwapChain(factory4, *pDesc);
		factory4->Release();
	} else {
		logger::error("[Frame Generation] Failed to get IDXGIFactory4 from adapter");
		adapter->Release();
		dxgiDevice->Release();
		return E_FAIL;
	}

	adapter->Release();
	dxgiDevice->Release();

	proxy->CreateInterop();

	*ppSwapChain = proxy->GetSwapChainProxy();

	return S_OK;
}

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	const D3D_FEATURE_LEVEL* pFeatureLevels,
	UINT FeatureLevels,
	UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	auto upscaling = Upscaling::GetSingleton();

	bool shouldProxy = false;

	if (pSwapChainDesc) {
		auto refreshRate = Upscaling::GetRefreshRate(pSwapChainDesc->OutputWindow);
		upscaling->refreshRate = refreshRate;

		shouldProxy = pSwapChainDesc->Windowed;

		if (shouldProxy) {
			if (upscaling->settings.frameGenerationMode)
				if (refreshRate >= 119)
					shouldProxy = true;
				else if (upscaling->settings.frameGenerationForceEnable)
					shouldProxy = true;
				else
					shouldProxy = false;
			else
				shouldProxy = false;
		}

		upscaling->lowRefreshRate = refreshRate < 119;
		upscaling->isWindowed = pSwapChainDesc->Windowed;
	}

	if (shouldProxy && pSwapChainDesc) {
		logger::info("[Frame Generation] Frame Generation enabled, using D3D12 proxy");
		
		auto fidelityFX = FSR4SkyrimHandler::GetSingleton();

		if (fidelityFX->isAvailable) {
			IDXGIFactory4* dxgiFactory = nullptr;
			HRESULT hr = E_FAIL;

			if (pAdapter) {
				logger::info("[Frame Generation] Getting factory from adapter");
				hr = pAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
			} else {
				logger::info("[Frame Generation] Creating new DXGI factory");
				IDXGIFactory1* tempFactory = nullptr;
				hr = CreateDXGIFactory1(IID_PPV_ARGS(&tempFactory));
				if (SUCCEEDED(hr) && tempFactory) {
					hr = tempFactory->QueryInterface(IID_PPV_ARGS(&dxgiFactory));
					tempFactory->Release();
				}
			}

			if (SUCCEEDED(hr) && dxgiFactory) {
				if (!ptrCreateSwapChain) {
					logger::info("[Frame Generation] Hooking IDXGIFactory::CreateSwapChain");
					*(uintptr_t*)&ptrCreateSwapChain = Detours::X64::DetourClassVTable(*(uintptr_t*)dxgiFactory, &hk_IDXGIFactory_CreateSwapChain, 10);
					logger::info("[Frame Generation] Hook installed successfully");
				}
				dxgiFactory->Release();
				upscaling->d3d12Interop = true;
			} else {
				logger::error("[Frame Generation] Failed to obtain DXGI Factory (HRESULT: {:08X})", (uint32_t)hr);
			}
		} else {
			logger::warn("[Frame Generation] FSR 4.0 loader is not available, skipping proxy");
			upscaling->fidelityFXMissing = true;
		}
	}

	return ptrD3D11CreateDeviceAndSwapChain(
		pAdapter,
		DriverType,
		Software,
		Flags,
		pFeatureLevels,
		FeatureLevels,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);
}

struct Main_RenderWorld
{
	static void thunk(bool a1);
	static inline REL::Relocation<decltype(thunk)> func;
};

void Main_RenderWorld::thunk(bool a1)
{
	func(a1);
	Upscaling::GetSingleton()->EarlyCopyBuffersToSharedResources();
};

namespace Hooks
{
	void Install()
	{
		auto fidelityFX = FSR4SkyrimHandler::GetSingleton();
		fidelityFX->LoadFFX();

		*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChain = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChain, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
		
		// Ensure we hook after ENB if it's present
		if (GetModuleHandleA("enbseries.dll")) {
			logger::info("[Hooks] ENB detected, ensuring IAT hook is applied to enbseries.dll as well");
			SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChain, "enbseries.dll", "D3D11CreateDeviceAndSwapChain");
		}
	}
}
