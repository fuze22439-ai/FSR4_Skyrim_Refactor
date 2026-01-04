#include "PCH.h"
#include "DX12SwapChain.h"

#include <dx12/ffx_api_dx12.hpp>
#include <dx12/ffx_api_framegeneration_dx12.hpp>
#include <ffx_framegeneration.hpp>
#include <dxgi1_6.h>

#include "FidelityFX.h"
#include "Upscaling.h"

DX12SwapChain::DX12SwapChain()
{
	frameCounter = 0;
	enbReady = false;
}

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter)
{
	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device)));

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.NodeMask = 0;

	DX::ThrowIfFailed(d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));

	for (int i = 0; i < 3; i++) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i].get(), nullptr, IID_PPV_ARGS(&commandLists[i])));
		commandLists[i]->Close();
	}

	fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void DX12SwapChain::CreateSwapChain(IDXGIFactory4* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	logger::info("[DX12SwapChain] Creating D3D12 SwapChain...");
	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = a_swapChainDesc.BufferDesc.Format;
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 3;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Force high-performance flip model
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; // Enable tearing for better latency

	logger::info("[DX12SwapChain] SwapChain Dimensions: {}x{}, Format: {}", swapChainDesc.Width, swapChainDesc.Height, (int)swapChainDesc.Format);

	// Use raw C API and explicit descriptors to bypass template issues
	ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 ffxSwapChainDesc{};
	ffxSwapChainDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
	ffxSwapChainDesc.desc = &swapChainDesc;
	ffxSwapChainDesc.dxgiFactory = a_dxgiFactory;
	ffxSwapChainDesc.fullscreenDesc = nullptr;
	ffxSwapChainDesc.gameQueue = commandQueue.get();
	ffxSwapChainDesc.hwnd = a_swapChainDesc.OutputWindow;
	ffxSwapChainDesc.swapchain = &swapChain;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc{};
	versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
	
	ffxCreateBackendDX12Desc backendDesc{};
	backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backendDesc.device = d3d12Device.get();

	// Link headers manually: ffxSwapChainDesc -> versionDesc -> backendDesc
	ffxSwapChainDesc.header.pNext = &versionDesc.header;
	versionDesc.header.pNext = &backendDesc.header;
	backendDesc.header.pNext = nullptr;

	auto fidelityFX = FidelityFX::GetSingleton();

	logger::info("[FidelityFX] Attempting to create swap chain context...");
	logger::info("[FidelityFX] HWND: {:p}, Width: {}, Height: {}, Format: {}", (void*)a_swapChainDesc.OutputWindow, swapChainDesc.Width, swapChainDesc.Height, (int)swapChainDesc.Format);

	// Call the C API directly
	auto ret = ffxCreateContext(&fidelityFX->swapChainContext, &ffxSwapChainDesc.header, nullptr);
	if (ret != FFX_API_RETURN_OK) {
		logger::critical("[FidelityFX] Failed to create swap chain context! Error code: 0x{:X}", (uint32_t)ret);
	} else {
		logger::info("[FidelityFX] Successfully created swap chain context.");
		fidelityFX->swapChainContextInitialized = true;
	}

	if (swapChain) {
		logger::info("[DX12SwapChain] SwapChain created successfully at {:p}", (void*)swapChain);
		
		DXGI_SWAP_CHAIN_DESC1 actualDesc;
		swapChain->GetDesc1(&actualDesc);
		UINT bufferCount = actualDesc.BufferCount;
		if (bufferCount > 3) bufferCount = 3;

		for (UINT i = 0; i < bufferCount; i++) {
			DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&swapChainBuffers[i])));
			logger::info("[DX12SwapChain] Acquired SwapChain Buffer {} at {:p}", i, (void*)swapChainBuffers[i].get());
		}

		frameIndex = swapChain->GetCurrentBackBufferIndex();

		fidelityFX->SetupFrameGeneration();

		swapChainProxy = new DXGISwapChainProxy(swapChain);
	} else {
		logger::error("[DX12SwapChain] SwapChain pointer is NULL after ffxCreateContext!");
	}
}

void DX12SwapChain::CreateInterop()
{
	logger::info("[DX12SwapChain] Creating Interop resources...");

	if (!d3d11Device || !d3d12Device) {
		logger::error("[DX12SwapChain] CreateInterop: D3D11 or D3D12 device is NULL!");
		return;
	}

	try {
		HANDLE sharedFenceHandle;
		DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence)));
		DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
		
		HRESULT hr = d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&d3d11Fence));
		CloseHandle(sharedFenceHandle);
		
		if (FAILED(hr)) {
			logger::error("[DX12SwapChain] CreateInterop: Failed to OpenSharedFence! HRESULT: 0x{:X}", (uint32_t)hr);
			return;
		}

		D3D11_TEXTURE2D_DESC texDesc11{};
		texDesc11.Width = swapChainDesc.Width;
		texDesc11.Height = swapChainDesc.Height;
		texDesc11.MipLevels = 1;
		texDesc11.ArraySize = 1;
		texDesc11.Format = swapChainDesc.Format;
		texDesc11.SampleDesc.Count = 1;
		texDesc11.SampleDesc.Quality = 0;
		texDesc11.Usage = D3D11_USAGE_DEFAULT;
		texDesc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		texDesc11.CPUAccessFlags = 0;
		texDesc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

		logger::info("[DX12SwapChain] Creating Wrapped Backbuffer...");
		swapChainBufferWrapped = new WrappedResource(texDesc11, d3d11Device.get(), d3d12Device.get());
		logger::info("[DX12SwapChain] Interop resources created successfully.");
	} catch (const std::exception& e) {
		logger::critical("[DX12SwapChain] CreateInterop: Exception occurred: {}", e.what());
	} catch (...) {
		logger::critical("[DX12SwapChain] CreateInterop: Unknown exception occurred!");
	}
}

DXGISwapChainProxy* DX12SwapChain::GetSwapChainProxy()
{
	return swapChainProxy;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	if (!a_d3d11Device) {
		logger::error("[DX12SwapChain] SetD3D11Device: Received NULL device!");
		return;
	}
	HRESULT hr = a_d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device));
	if (FAILED(hr)) {
		logger::error("[DX12SwapChain] SetD3D11Device: Failed to QI ID3D11Device5! HRESULT: 0x{:X}", (uint32_t)hr);
	} else {
		logger::info("[DX12SwapChain] SetD3D11Device: ID3D11Device5 acquired.");
	}
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	if (!a_d3d11Context) {
		logger::error("[DX12SwapChain] SetD3D11DeviceContext: Received NULL context!");
		return;
	}
	HRESULT hr = a_d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context));
	if (FAILED(hr)) {
		logger::error("[DX12SwapChain] SetD3D11DeviceContext: Failed to QI ID3D11DeviceContext4! HRESULT: 0x{:X}", (uint32_t)hr);
	} else {
		logger::info("[DX12SwapChain] SetD3D11DeviceContext: ID3D11DeviceContext4 acquired.");
	}
}

HRESULT DX12SwapChain::GetBuffer(void** ppSurface)
{
	if (!swapChainBufferWrapped) {
		logger::error("[DX12SwapChain] GetBuffer called but swapChainBufferWrapped is NULL!");
		return E_FAIL;
	}
	*ppSurface = swapChainBufferWrapped->resource11;
	return S_OK;
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	// Frame Generation handles its own pacing, force SyncInterval to 0
	SyncInterval = 0;

	extern ::ENB_API::ENBSDKALT1001* g_ENB;
	// Reduced wait frames for faster testing
	uint64_t waitFrames = g_ENB ? 120 : 60;

	auto upscaling_ptr = Upscaling::GetSingleton();
	bool shouldLog = (frameCounter % 100 == 0); // Log every 100 frames
	if (shouldLog) {
		logger::info("[DX12SwapChain] Present Start. Frame: {}, frameIndex: {}, fenceValue: {}, setupBuffers: {}", 
			frameCounter, frameIndex, fenceValue, upscaling_ptr->setupBuffers);
	}
	frameCounter++;

	try {
		BOOL fullscreen = FALSE;
		swapChain->GetFullscreenState(&fullscreen, nullptr);
		if (fullscreen || SyncInterval) {
			Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
		} else if (SyncInterval == 0) {
			Flags |= DXGI_PRESENT_ALLOW_TEARING;
		}

		// Wait for D3D11 to finish
		if (shouldLog) {
			logger::info("[DX12SwapChain] Step 1: CopyBuffersToSharedResources");
		}
		upscaling_ptr->CopyBuffersToSharedResources();

		// Stability Check: Wait for ENB compilation and resource initialization
		bool enbIsCompiling = false;
		if (g_ENB) {
			if (!enbReady || g_ENB->GetRenderInfo() == nullptr) {
				enbIsCompiling = true;
			}
		}

		bool bypass = enbIsCompiling || frameCounter < waitFrames;

		if (!d3d11Fence || !d3d12Fence || !swapChainBufferWrapped || !d3d11Context) {
			if (shouldLog) {
				logger::warn("[DX12SwapChain] Missing core interop resources, falling back to original Present");
			}
			return swapChain->Present(SyncInterval, Flags);
		}

		// Ensure D3D11 work is submitted before D3D12 takes over
		d3d11Context->Flush();

		// Strict Synchronization (Adopted from ENBFrameGeneration)
		if (shouldLog) {
			logger::info("[DX12SwapChain] Step 2: D3D11 Signal -> D3D12 Wait");
		}
		DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
		DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
		fenceValue++;

		// Safety check for frameIndex and resources
		if (frameIndex >= 3 || !commandAllocators[frameIndex] || !commandLists[frameIndex]) {
			logger::error("[DX12SwapChain] Invalid frameIndex or NULL resources! frameIndex: {}, allocs: {}, lists: {}", 
				frameIndex, (bool)commandAllocators[frameIndex], (bool)commandLists[frameIndex]);
			return swapChain->Present(SyncInterval, Flags);
		}

		// New frame, reset
		if (shouldLog) {
			logger::info("[DX12SwapChain] Step 3: Command List Reset (frameIndex: {})", frameIndex);
		}

		// CPU-side wait to ensure the allocator is not in use by the GPU
		if (d3d12Fence->GetCompletedValue() < frameFenceValues[frameIndex]) {
			DX::ThrowIfFailed(d3d12Fence->SetEventOnCompletion(frameFenceValues[frameIndex], fenceEvent));
			WaitForSingleObject(fenceEvent, INFINITE);
		}

		DX::ThrowIfFailed(commandAllocators[frameIndex]->Reset());
		DX::ThrowIfFailed(commandLists[frameIndex]->Reset(commandAllocators[frameIndex].get(), nullptr));

		// Copy shared texture to swap chain buffer
		// We ALWAYS copy to the backbuffer because FSR needs it for UI extraction/composition
		{
			auto fakeSwapChain = swapChainBufferWrapped->resource.get();
			// Use the ACTUAL swapchain index for the destination to avoid flickering
			UINT actualIndex = swapChain->GetCurrentBackBufferIndex();
			auto realSwapChain = swapChainBuffers[actualIndex].get();
			
			if (fakeSwapChain && realSwapChain) {
				if (shouldLog) {
					logger::info("[DX12SwapChain] Step 4: Resource Barriers & CopyResource (actualIndex: {})", actualIndex);
				}
				{
					std::vector<D3D12_RESOURCE_BARRIER> barriers;
					barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
					barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
					commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
				}

				commandLists[frameIndex]->CopyResource(realSwapChain, fakeSwapChain);

				{
					std::vector<D3D12_RESOURCE_BARRIER> barriers;
					barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
					barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
					commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
				}
			}
		}

		if (upscaling_ptr->setupBuffers) {
			if (shouldLog) {
				logger::info("[DX12SwapChain] Step 5: FidelityFX Present (bypass: {})", bypass);
			}
			FidelityFX::GetSingleton()->Present(upscaling_ptr->settings.frameGenerationMode, bypass);
		} else {
			if (shouldLog) {
				logger::info("[DX12SwapChain] Step 5 Skipped: setupBuffers is false");
			}
		}

		if (shouldLog) {
			logger::info("[DX12SwapChain] Step 6: ExecuteCommandLists");
		}
		DX::ThrowIfFailed(commandLists[frameIndex]->Close());

		ID3D12CommandList* commandListsToExecute[] = { commandLists[frameIndex].get() };
		commandQueue->ExecuteCommandLists(1, commandListsToExecute);

		// Present the frame
		if (shouldLog) {
			logger::info("[DX12SwapChain] Step 7: swapChain->Present");
		}
		HRESULT hr = swapChain->Present(0, Flags);

		if (FAILED(hr)) {
			logger::critical("[DX12SwapChain] swapChain->Present failed! HRESULT: 0x{:X}", (uint32_t)hr);
			return hr;
		}

		// Update frame fence value for this allocator
		frameFenceValues[frameIndex] = fenceValue;
		DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));

		// Wait for D3D12 to finish (GPU-side sync for D3D11)
		if (shouldLog) {
			logger::info("[DX12SwapChain] Step 8: D3D12 Signal -> D3D11 Wait");
		}
		DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));
		fenceValue++;

		if (shouldLog) {
			logger::info("[DX12SwapChain] Present End. Next frameIndex: {}", swapChain->GetCurrentBackBufferIndex());
		}

		// Update the frame index (Return to system-managed index)
		frameIndex = swapChain->GetCurrentBackBufferIndex();

		// upscaling_ptr->FrameLimiter(); // Temporarily disabled to check if it's causing the "half FPS" issue

		return hr;
	} catch (const std::exception& e) {
		logger::critical("[DX12SwapChain] Present: Exception occurred: {}", e.what());
		return E_FAIL;
	} catch (...) {
		logger::critical("[DX12SwapChain] Present: Unknown exception occurred!");
		return E_FAIL;
	}
}

HRESULT DX12SwapChain::GetDevice(REFIID uuid, void** ppDevice)
{
	if (uuid == __uuidof(ID3D11Device) || uuid == __uuidof(ID3D11Device1) || uuid == __uuidof(ID3D11Device2) || uuid == __uuidof(ID3D11Device3) || uuid == __uuidof(ID3D11Device4) || uuid == __uuidof(ID3D11Device5)) {
		*ppDevice = d3d11Device.get();
		return S_OK;
	}

	return swapChain->GetDevice(uuid, ppDevice);
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain = a_swapChain;
}

/****IUknown****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	auto ret = swapChain->QueryInterface(riid, ppvObj);
	if (*ppvObj)
		*ppvObj = this;
	return ret;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return swapChain->AddRef();
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	return swapChain->Release();
}

/****IDXGIObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData)
{
	return swapChain->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown)
{
	return swapChain->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData)
{
	return swapChain->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent)
{
	return swapChain->GetParent(riid, ppParent);
}

/****IDXGIDeviceSubObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice)
{
	return DX12SwapChain::GetSingleton()->GetDevice(riid, ppDevice);
}

/****IDXGISwapChain****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
	return DX12SwapChain::GetSingleton()->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT, _In_ REFIID, _COM_Outptr_ void** ppSurface)
{
	return DX12SwapChain::GetSingleton()->GetBuffer(ppSurface);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget)
{
	return swapChain->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget)
{
	return swapChain->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc)
{
	return swapChain->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	return swapChain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters)
{
	return swapChain->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput)
{
	return swapChain->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats)
{
	return swapChain->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(_Out_ UINT* pLastPresentCount)
{
	return swapChain->GetLastPresentCount(pLastPresentCount);
}
