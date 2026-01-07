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
	QueryPerformanceFrequency(&qpf);
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
	// FSR 4.0 SDK internally requires BufferCount = 3 (see FrameInterpolationSwapchainDX12.cpp:1159)
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

	auto fidelityFX = FSR4SkyrimHandler::GetSingleton();

	logger::info("[FSR4SkyrimHandler] Attempting to create swap chain context...");
	logger::info("[FSR4SkyrimHandler] HWND: {:p}, Width: {}, Height: {}, Format: {}", (void*)a_swapChainDesc.OutputWindow, swapChainDesc.Width, swapChainDesc.Height, (int)swapChainDesc.Format);
	logger::info("[FSR4SkyrimHandler] BEFORE ffxCreateContext: swapChain ptr = {:p}", (void*)swapChain);

	// Call the C API directly
	auto ret = ffxCreateContext(&fidelityFX->swapChainContext, &ffxSwapChainDesc.header, nullptr);
	
	logger::info("[FSR4SkyrimHandler] AFTER ffxCreateContext: swapChain ptr = {:p}, ret = 0x{:X}", (void*)swapChain, (uint32_t)ret);
	
	if (ret != FFX_API_RETURN_OK) {
		logger::critical("[FSR4SkyrimHandler] Failed to create swap chain context! Error code: 0x{:X}", (uint32_t)ret);
	} else {
		logger::info("[FSR4SkyrimHandler] Successfully created swap chain context.");
		fidelityFX->swapChainContextInitialized = true;
	}

	if (swapChain) {
		logger::info("[DX12SwapChain] SwapChain created successfully at {:p}", (void*)swapChain);
		
		// DIAGNOSTIC: Verify SwapChain is FSR Proxy
		// SDK uses this IID to detect if SwapChain is an FSR interpolation proxy
		static const GUID IID_IFfxFrameInterpolationSwapChain = { 0xbeed74b2, 0x282e, 0x4aa3, {0xbb, 0xf7, 0x53, 0x45, 0x60, 0x50, 0x7a, 0x45} };
		// Also check IFrameInterpolationSwapChainDX12 (used by FfxAbiFrameGenSwapChainCall)
		static const GUID IID_IFrameInterpolationSwapChainDX12 = { 0x5f5fa2f5, 0x3bc5, 0x48d8, {0xa6, 0x3b, 0xe6, 0x03, 0x18, 0xe3, 0x80, 0x00} };
		
		void* testPtr = nullptr;
		HRESULT qiResult1 = swapChain->QueryInterface(IID_IFfxFrameInterpolationSwapChain, &testPtr);
		logger::info("[DX12SwapChain] QueryInterface IID_IFfxFrameInterpolationSwapChain: 0x{:X}", (uint32_t)qiResult1);
		if (testPtr) { ((IUnknown*)testPtr)->Release(); testPtr = nullptr; }
		
		HRESULT qiResult2 = swapChain->QueryInterface(IID_IFrameInterpolationSwapChainDX12, &testPtr);
		logger::info("[DX12SwapChain] QueryInterface IID_IFrameInterpolationSwapChainDX12: 0x{:X}", (uint32_t)qiResult2);
		if (testPtr) { ((IUnknown*)testPtr)->Release(); testPtr = nullptr; }
		
		// Also check standard DXGI interfaces to see what the object actually is
		HRESULT qiResult3 = swapChain->QueryInterface(__uuidof(IDXGISwapChain4), &testPtr);
		logger::info("[DX12SwapChain] QueryInterface IDXGISwapChain4: 0x{:X}", (uint32_t)qiResult3);
		if (testPtr) { ((IUnknown*)testPtr)->Release(); testPtr = nullptr; }
		
		if (SUCCEEDED(qiResult1) || SUCCEEDED(qiResult2)) {
			logger::info("[DX12SwapChain] GOOD: SwapChain IS an FSR Frame Interpolation Proxy!");
		} else {
			logger::error("[DX12SwapChain] BAD: SwapChain is NOT an FSR Proxy!");
			logger::error("[DX12SwapChain] This means Configure will NOT enable interpolation on the SwapChain!");
		}
		
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
		if (!d3d12Fence) {
			HANDLE sharedFenceHandle;
			DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence)));
			DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
		
			HRESULT hr = d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&d3d11Fence));
			CloseHandle(sharedFenceHandle);
		
			if (FAILED(hr)) {
				logger::error("[DX12SwapChain] CreateInterop: Failed to OpenSharedFence! HRESULT: 0x{:X}", (uint32_t)hr);
				return;
			}
			fenceValue = 1; // Start from 1 for safety
			for (int i = 0; i < 3; i++) frameFenceValues[i] = 0;
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
	// Following ENBFrameGeneration: Force SyncInterval to 0
	SyncInterval = 0;
	
	// Following ENBFrameGeneration: Handle ALLOW_TEARING flag based on fullscreen state
	BOOL fullscreen = FALSE;
	swapChain->GetFullscreenState(&fullscreen, nullptr);
	if (fullscreen || SyncInterval) {
		Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
	} else if (SyncInterval == 0) {
		Flags |= DXGI_PRESENT_ALLOW_TEARING;
	}

	auto upscaling_ptr = Upscaling::GetSingleton();
	(void)upscaling_ptr; // May be unused
	frameCounter++;

	// Core interop check - this is a hard requirement
	if (!d3d11Fence || !d3d12Fence || !swapChainBufferWrapped || !d3d11Context) {
		logger::warn("[DX12SwapChain] Missing core interop resources");
		return swapChain->Present(SyncInterval, Flags);
	}

	// NOTE: CopyBuffersToSharedResources is now called from TAA hooks (ReplaceTAA or CopyBuffersToSharedResources)
	// DO NOT call it here again - it would overwrite the pre-TAA data with post-TAA data!
	// The TAA hook ensures data is collected at the correct moment in the rendering pipeline.

	// D3D11 Signal -> D3D12 Wait
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	fenceValue++;

	// Reset command list
	DX::ThrowIfFailed(commandAllocators[frameIndex]->Reset());
	DX::ThrowIfFailed(commandLists[frameIndex]->Reset(commandAllocators[frameIndex].get(), nullptr));

	// Copy shared texture to swap chain buffer
	{
		auto fakeSwapChain = swapChainBufferWrapped->resource.get();
		auto realSwapChain = swapChainBuffers[frameIndex].get();
		
		if (fakeSwapChain && realSwapChain) {
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

			commandLists[frameIndex]->CopyResource(realSwapChain, fakeSwapChain);

			barriers.clear();
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}
	}

	// Following ENBFrameGeneration: NO barriers needed for shared resources
	// Our resources are created with D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
	// which allows D3D11 and D3D12 to access them without explicit state transitions

	// Call FSR Present
	auto handler = FSR4SkyrimHandler::GetSingleton();
	if (handler) {
		handler->Present(upscaling_ptr->settings.frameGenerationMode, false);
	}

	DX::ThrowIfFailed(commandLists[frameIndex]->Close());

	ID3D12CommandList* commandListsToExecute[] = { commandLists[frameIndex].get() };
	commandQueue->ExecuteCommandLists(1, commandListsToExecute);

	// Present the frame
	HRESULT hr = swapChain->Present(0, Flags);

	// D3D12 Signal -> D3D11 Wait
	DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
	DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));
	fenceValue++;

	// Update the frame index
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	return hr;
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
	if (ppvObj == nullptr) return E_POINTER;

	// Standard Proxy pattern: If the game asks for any DXGI SwapChain interface, return OURSELVES.
	if (riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDXGIObject) ||
		riid == __uuidof(IDXGIDeviceSubObject) ||
		riid == __uuidof(IDXGISwapChain) ||
		riid == __uuidof(IDXGISwapChain1) ||
		riid == __uuidof(IDXGISwapChain2) ||
		riid == __uuidof(IDXGISwapChain3) ||
		riid == __uuidof(IDXGISwapChain4)) {
		AddRef();
		*ppvObj = this;
		return S_OK;
	}

	// For anything else (like undocumented ENB interfaces or DXGI 1.6+ features we don't proxy yet), 
	// let the real swapchain handle it. Note: This might return a pointer that bypasses our proxy!
	return swapChain->QueryInterface(riid, ppvObj);
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
	logger::info("[DXGISwapChainProxy] ResizeBuffers called: {}x{}, BufferCount: {}, Format: {}", Width, Height, BufferCount, (int)NewFormat);
	LOG_FLUSH();

	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	auto upscaling_ptr = Upscaling::GetSingleton();

	// 1. Flush D3D11 and D3D12 to ensure GPU is done with resources
	if (dx12SwapChain->d3d11Context) {
		dx12SwapChain->d3d11Context->Flush();
		// Wait for all GPU work to finish
		if (dx12SwapChain->d3d11Fence) {
			dx12SwapChain->d3d11Context->Signal(dx12SwapChain->d3d11Fence.get(), dx12SwapChain->fenceValue);
			while (dx12SwapChain->d3d12Fence->GetCompletedValue() < dx12SwapChain->fenceValue) {
				SwitchToThread();
			}
			dx12SwapChain->fenceValue++;
		}
	}

	// 2. Release internal buffers that depend on SwapChain size
	for (int i = 0; i < 3; i++) {
		dx12SwapChain->swapChainBuffers[i] = nullptr;
	}
	if (dx12SwapChain->swapChainBufferWrapped) {
		delete dx12SwapChain->swapChainBufferWrapped;
		dx12SwapChain->swapChainBufferWrapped = nullptr;
	}

	// 3. Inform Upscaling to release its resources
	// CRITICAL: We MUST null out the shared resources so FSR doesn't try to use them with new resolution
	upscaling_ptr->setupBuffers = false;
	if (upscaling_ptr->HUDLessBufferShared) { delete upscaling_ptr->HUDLessBufferShared; upscaling_ptr->HUDLessBufferShared = nullptr; }
	if (upscaling_ptr->upscaledBufferShared) { delete upscaling_ptr->upscaledBufferShared; upscaling_ptr->upscaledBufferShared = nullptr; }
	if (upscaling_ptr->depthBufferShared) { delete upscaling_ptr->depthBufferShared; upscaling_ptr->depthBufferShared = nullptr; }
	if (upscaling_ptr->motionVectorBufferShared) { delete upscaling_ptr->motionVectorBufferShared; upscaling_ptr->motionVectorBufferShared = nullptr; }

	// 4. Call original ResizeBuffers
	HRESULT hr = swapChain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
	
	if (SUCCEEDED(hr)) {
		// 5. Update internal desc
		swapChain->GetDesc1(&dx12SwapChain->swapChainDesc);
		
		// 6. Re-acquire backbuffers
		// BufferCount can be 0 (meaning no change), so we should use the actual count from the swapchain
		UINT actualBufferCount = dx12SwapChain->swapChainDesc.BufferCount;
		for (UINT i = 0; i < actualBufferCount && i < 3; i++) {
			DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&dx12SwapChain->swapChainBuffers[i])));
			logger::info("[DXGISwapChainProxy] Re-acquired Buffer {}", i);
		}
		dx12SwapChain->frameIndex = swapChain->GetCurrentBackBufferIndex();

		// 7. Re-create Interop
		dx12SwapChain->CreateInterop();

		logger::info("[DXGISwapChainProxy] ResizeBuffers successfully handled.");
		LOG_FLUSH();
	} else {
		logger::error("[DXGISwapChainProxy] ResizeBuffers failed! HRESULT: 0x{:X}", (uint32_t)hr);
		LOG_FLUSH();
	}

	return hr;
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

// ============================================================================
// D3D11 <-> D3D12 Synchronization Methods
// Used by ReplaceTAA() for synchronous AA execution
// ============================================================================

void DX12SwapChain::SignalD3D11ToD3D12()
{
	// D3D11 signals fence, then D3D12 waits on same fence
	// This notifies D3D12 that D3D11 data (HUDLess, MV, Depth) is ready
	if (!d3d11Fence || !d3d12Fence || !d3d11Context || !commandQueue) {
		logger::warn("[DX12SwapChain] SignalD3D11ToD3D12: Missing fence or queue");
		return;
	}
	
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	fenceValue++;
}

void DX12SwapChain::SignalD3D12ToD3D11()
{
	// D3D12 signals fence (for D3D11 wait)
	// Call this AFTER D3D12 command list execution
	if (!d3d12Fence || !commandQueue) {
		logger::warn("[DX12SwapChain] SignalD3D12ToD3D11: Missing fence or queue");
		return;
	}
	
	DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
}

void DX12SwapChain::WaitForD3D12Completion()
{
	// D3D11 waits for D3D12 fence signal
	// Call this AFTER SignalD3D12ToD3D11() to block until D3D12 work completes
	if (!d3d11Fence || !d3d12Fence || !d3d11Context) {
		logger::warn("[DX12SwapChain] WaitForD3D12Completion: Missing fence or context");
		return;
	}
	
	DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));
	fenceValue++;
}
