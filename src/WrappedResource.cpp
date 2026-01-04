#include "PCH.h"
#include "WrappedResource.h"

WrappedResource::WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device)
{
	// FSR 4.0 requires UAV for most buffers. Ensure format compatibility.
	// D3D12 does not allow UAV on SRGB formats.
	if (a_texDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
		logger::warn("[WrappedResource] SRGB format detected with UAV request. Converting to UNORM for D3D12 compatibility.");
		a_texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	logger::info("[WrappedResource] Creating resource: {}x{}, Format: {}, BindFlags: 0x{:X}", a_texDesc.Width, a_texDesc.Height, (int)a_texDesc.Format, a_texDesc.BindFlags);
	LOG_FLUSH();

	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
	if (a_texDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	
	D3D12_RESOURCE_DESC desc12{ D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, a_texDesc.Width, a_texDesc.Height, (UINT16)a_texDesc.ArraySize, (UINT16)a_texDesc.MipLevels, a_texDesc.Format, { a_texDesc.SampleDesc.Count, a_texDesc.SampleDesc.Quality }, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
	D3D12_HEAP_PROPERTIES heapProp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1 };

	logger::info("[WrappedResource] Calling CreateCommittedResource...");
	LOG_FLUSH();
	DX::ThrowIfFailed(a_d3d12Device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_SHARED, &desc12, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));

	logger::info("[WrappedResource] Creating Shared Handle...");
	LOG_FLUSH();
	HANDLE sharedHandle = nullptr;
	DX::ThrowIfFailed(a_d3d12Device->CreateSharedHandle(resource.get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle));

	logger::info("[WrappedResource] Opening Shared Resource in D3D11...");
	LOG_FLUSH();
	DX::ThrowIfFailed(a_d3d11Device->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&resource11)));
	CloseHandle(sharedHandle);

	if (a_texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = a_texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		DX::ThrowIfFailed(a_d3d11Device->CreateShaderResourceView(resource11, &srvDesc, &srv));
	}

	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = a_texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		DX::ThrowIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav));
	}

	if (a_texDesc.BindFlags & D3D11_BIND_RENDER_TARGET) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = a_texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		DX::ThrowIfFailed(a_d3d11Device->CreateRenderTargetView(resource11, &rtvDesc, &rtv));
	}
	logger::info("[WrappedResource] Resource created successfully.");
	LOG_FLUSH();
}
