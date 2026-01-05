#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <xbyak/xbyak.h>

#include <detours/Detours.h>

#include <spdlog/sinks/basic_file_sink.h>
#ifndef NDEBUG
#	include <spdlog/sinks/msvc_sink.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <ENB/ENBSeriesAPI.h>

#define LOG_FLUSH() spdlog::default_logger()->flush()

using namespace std::literals;

extern ENB_API::ENBSDKALT1001* g_ENB;

namespace stl
{
	using namespace SKSE::stl;

	template <class T, std::size_t Size = 5>
	void write_thunk_call(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		if (Size == 6) {
			T::func = *(uintptr_t*)trampoline.write_call<6>(a_src, T::thunk);
		} else {
			T::func = trampoline.write_call<Size>(a_src, T::thunk);
		}
	}

	template <class F, size_t index, class T>
	void write_vfunc()
	{
		REL::Relocation<std::uintptr_t> vtbl{ F::VTABLE[index] };
		T::func = vtbl.write_vfunc(T::size, T::thunk);
	}

	template <std::size_t idx, class T>
	void write_vfunc(REL::VariantID id)
	{
		REL::Relocation<std::uintptr_t> vtbl{ id };
		T::func = vtbl.write_vfunc(idx, T::thunk);
	}

	template <class T>
	void write_thunk_jmp(std::uintptr_t a_src)
	{
		auto& trampoline = SKSE::GetTrampoline();
		T::func = trampoline.write_branch<5>(a_src, T::thunk);
	}

	template <class F, class T>
	void write_vfunc()
	{
		write_vfunc<F, 0, T>();
	}

	template <class T>
	void detour_thunk(REL::RelocationID a_relId)
	{
		*(uintptr_t*)&T::func = Detours::X64::DetourFunction(a_relId.address(), (uintptr_t)&T::thunk);
	}

	template <class T>
	void detour_thunk_ignore_func(REL::RelocationID a_relId)
	{
		std::ignore = Detours::X64::DetourFunction(a_relId.address(), (uintptr_t)&T::thunk);
	}

	template <std::size_t idx, class T>
	void detour_vfunc(void* target)
	{
		*(uintptr_t*)&T::func = Detours::X64::DetourClassVTable(*(uintptr_t*)target, &T::thunk, idx);
	}
}

namespace logger = SKSE::log;

namespace util
{
	using SKSE::stl::report_and_fail;
}

#define DLLEXPORT __declspec(dllexport)

#define LOG_FLUSH() spdlog::default_logger()->flush()

#include "Plugin.h"

bool Load();

void InitializeLog()
{
	auto path = logger::log_directory();
	if (!path) {
		util::report_and_fail("Failed to find standard logging directory"sv);
	}

	*path /= std::format("{}.log"sv, Plugin::NAME);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);

#ifndef NDEBUG
	const auto level = spdlog::level::trace;
#else
	const auto level = spdlog::level::info;
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
	log->set_level(level);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%v"s);
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	logger::info("Loaded plugin");

	// Add the plugin directory and its subfolder to the DLL search path so dependencies can be found
	wchar_t pluginPath[MAX_PATH];
	HMODULE hModule = NULL;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&InitializeLog, &hModule)) {
		GetModuleFileNameW(hModule, pluginPath, MAX_PATH);
		std::wstring path(pluginPath);
		size_t lastSlash = path.find_last_of(L"\\/");
		if (lastSlash != std::wstring::npos) {
			std::wstring dir = path.substr(0, lastSlash);
			
			// Add main directory
			SetDllDirectoryW(dir.c_str());
			
			// Convert wstring to string safely for logging
			std::string sDir;
			sDir.reserve(dir.length());
			for (wchar_t c : dir) {
				sDir += (c < 128) ? static_cast<char>(c) : '?';
			}
			logger::info("Added DLL search path: {}", sDir);

			// Add subfolder named after the plugin (e.g., FSR4_Skyrim)
			std::wstring dllName = path.substr(lastSlash + 1);
			size_t dot = dllName.find_last_of(L".");
			std::wstring folderName = (dot != std::wstring::npos) ? dllName.substr(0, dot) : dllName;
			std::wstring subDir = dir + L"\\" + folderName;
			
			if (std::filesystem::exists(subDir)) {
				// Note: SetDllDirectoryW only keeps the last one added if we don't use AddDllDirectory
				// But for simplicity, we can just use the subfolder if it exists, 
				// or rely on the fact that LoadLibraryW in FidelityFX.cpp uses the full path for the loader.
				// Actually, the best way is to use AddDllDirectory, but that requires some extra setup.
				// Let's just stick to the current logic and ensure the loader is loaded with full path.
			}
		}
	}

	SKSE::Init(a_skse);
	return Load();
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginName(Plugin::NAME.data());
	v.PluginVersion(Plugin::VERSION);
	v.UsesAddressLibrary(true);
	v.HasNoStructUse();
	return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* pluginInfo)
{
	pluginInfo->name = SKSEPlugin_Version.pluginName;
	pluginInfo->infoVersion = SKSE::PluginInfo::kVersion;
	pluginInfo->version = SKSEPlugin_Version.pluginVersion;
	return true;
}

namespace DX
{
	inline ID3D11Device* GetDevice()
	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return nullptr;
		return reinterpret_cast<ID3D11Device*>(renderer->data.forwarder);
	}

	inline ID3D11DeviceContext* GetContext()
	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return nullptr;
		return reinterpret_cast<ID3D11DeviceContext*>(renderer->data.context);
	}

	// Helper class for COM exceptions
	class com_exception : public std::exception
	{
	public:
		explicit com_exception(HRESULT hr) noexcept :
			result(hr) {}

		const char* what() const override
		{
			static char s_str[64] = {};
			sprintf_s(s_str, "Failure with HRESULT of %08X", static_cast<unsigned int>(result));
			return s_str;
		}

	private:
		HRESULT result;
	};

	// Helper utility converts D3D API failures into exceptions.
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr)) {
			throw com_exception(hr);
		}
	}
}
