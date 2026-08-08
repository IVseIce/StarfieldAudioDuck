#include "pch.h"

#include "GalacticRadioCompatibility.h"

#include <mmsystem.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cwctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

namespace
{
	using mci_send_string_w_t = MCIERROR(WINAPI*)(LPCWSTR, LPWSTR, UINT, HWND);

	constexpr wchar_t kRadioModuleName[] = L"StarfieldGalacticRadio.dll";
	constexpr std::wstring_view kRadioAlias = L"sfradio";

	std::atomic<StarfieldAudioDuck::GalacticRadioCompatibility*> g_compatibilityInstance{ nullptr };
	std::atomic<mci_send_string_w_t>                            g_originalMciSendStringW{ nullptr };
	std::mutex                                                   g_installMutex{};

	[[nodiscard]] bool EqualInsensitive(const std::wstring_view a_left, const std::wstring_view a_right) noexcept
	{
		if (a_left.size() != a_right.size()) {
			return false;
		}

		return std::equal(a_left.begin(), a_left.end(), a_right.begin(), [](const wchar_t a_leftCharacter, const wchar_t a_rightCharacter) {
			return towlower(a_leftCharacter) == towlower(a_rightCharacter);
		});
	}

	[[nodiscard]] std::wstring_view ConsumeToken(std::wstring_view& a_text) noexcept
	{
		while (!a_text.empty() && iswspace(a_text.front())) {
			a_text.remove_prefix(1);
		}

		const auto separator = a_text.find_first_of(L" \t\r\n");
		if (separator == std::wstring_view::npos) {
			const auto token = a_text;
			a_text = {};
			return token;
		}

		const auto token = a_text.substr(0, separator);
		a_text.remove_prefix(separator);
		return token;
	}

	[[nodiscard]] bool IsRadioCommand(std::wstring_view& a_arguments) noexcept
	{
		return EqualInsensitive(ConsumeToken(a_arguments), kRadioAlias);
	}

	[[nodiscard]] bool HasRadioAlias(std::wstring_view a_arguments) noexcept
	{
		while (!a_arguments.empty()) {
			const auto token = ConsumeToken(a_arguments);
			if (EqualInsensitive(token, kRadioAlias)) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] bool ParseVolume(std::wstring_view a_text, long& a_volume) noexcept
	{
		while (!a_text.empty() && iswspace(a_text.front())) {
			a_text.remove_prefix(1);
		}

		if (a_text.empty()) {
			return false;
		}

		long sign = 1;
		if (a_text.front() == L'-') {
			sign = -1;
			a_text.remove_prefix(1);
		}

		if (a_text.empty() || !iswdigit(a_text.front())) {
			return false;
		}

		long value = 0;
		while (!a_text.empty() && iswdigit(a_text.front())) {
			const auto digit = static_cast<long>(a_text.front() - L'0');
			if (value > (LONG_MAX - digit) / 10) {
				return false;
			}
			value = value * 10 + digit;
			a_text.remove_prefix(1);
		}

		a_volume = sign * value;
		return true;
	}

	[[nodiscard]] bool PatchMciImport(
		HMODULE a_module,
		void*   a_replacement,
		void*&  a_original)
	{
		if (!a_module) {
			return false;
		}

		const auto moduleBase = reinterpret_cast<std::uintptr_t>(a_module);
		const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0) {
			return false;
		}

		const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + static_cast<std::uintptr_t>(dosHeader->e_lfanew));
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
			ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
			ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
			return false;
		}

		const auto imageSize = static_cast<std::uintptr_t>(ntHeaders->OptionalHeader.SizeOfImage);
		const auto importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (importDirectory.VirtualAddress == 0 || importDirectory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
			return false;
		}

		const auto isValidRVA = [moduleBase, imageSize](const DWORD a_rva, const std::size_t a_size = 1) {
			return static_cast<std::uintptr_t>(a_rva) < imageSize &&
				static_cast<std::uintptr_t>(a_rva) + a_size <= imageSize;
		};

		if (!isValidRVA(importDirectory.VirtualAddress, importDirectory.Size)) {
			return false;
		}

		const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(moduleBase + importDirectory.VirtualAddress);
		const auto descriptorCount = importDirectory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
		for (std::size_t descriptorIndex = 0; descriptorIndex < descriptorCount; ++descriptorIndex) {
			const auto& descriptor = descriptors[descriptorIndex];
			if (descriptor.Name == 0) {
				break;
			}
			if (!isValidRVA(descriptor.Name)) {
				return false;
			}

			const auto* moduleName = reinterpret_cast<const char*>(moduleBase + descriptor.Name);
			if (_stricmp(moduleName, "WINMM.dll") != 0) {
				continue;
			}

			if (descriptor.OriginalFirstThunk == 0 || descriptor.FirstThunk == 0 ||
				!isValidRVA(descriptor.OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA64)) ||
				!isValidRVA(descriptor.FirstThunk, sizeof(IMAGE_THUNK_DATA64))) {
				return false;
			}

			const auto* originalThunks = reinterpret_cast<const IMAGE_THUNK_DATA64*>(moduleBase + descriptor.OriginalFirstThunk);
			auto* firstThunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(moduleBase + descriptor.FirstThunk);
			for (std::size_t thunkIndex = 0;; ++thunkIndex) {
				const auto thunkOffset = thunkIndex * sizeof(IMAGE_THUNK_DATA64);
				if (!isValidRVA(descriptor.OriginalFirstThunk + static_cast<DWORD>(thunkOffset), sizeof(IMAGE_THUNK_DATA64)) ||
					!isValidRVA(descriptor.FirstThunk + static_cast<DWORD>(thunkOffset), sizeof(IMAGE_THUNK_DATA64))) {
					return false;
				}

				const auto importData = originalThunks[thunkIndex].u1.AddressOfData;
				if (importData == 0) {
					break;
				}
				if ((importData & IMAGE_ORDINAL_FLAG64) != 0 || !isValidRVA(static_cast<DWORD>(importData), sizeof(WORD) + 1)) {
					continue;
				}

				const auto* importByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(moduleBase + importData);
				if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), "mciSendStringW") != 0) {
					continue;
				}

				auto* slot = std::addressof(firstThunks[thunkIndex].u1.Function);
				const auto oldValue = reinterpret_cast<void*>(*slot);
				if (oldValue == a_replacement) {
					a_original = g_originalMciSendStringW.load(std::memory_order_acquire);
					return a_original != nullptr;
				}

				DWORD oldProtection = 0;
				if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
					return false;
				}

				*slot = reinterpret_cast<ULONGLONG>(a_replacement);
				FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));

				DWORD ignoredProtection = 0;
				VirtualProtect(slot, sizeof(*slot), oldProtection, &ignoredProtection);
				a_original = oldValue;
				return a_original != nullptr;
			}
		}

		return false;
	}

	MCIERROR WINAPI GalacticRadioMciSendStringHook(
		LPCWSTR a_command,
		LPWSTR  a_returnString,
		UINT    a_returnLength,
		HWND    a_callbackWindow)
	{
		const auto original = g_originalMciSendStringW.load(std::memory_order_acquire);
		if (!original) {
			return MCIERR_DRIVER_INTERNAL;
		}

		const auto result = original(a_command, a_returnString, a_returnLength, a_callbackWindow);
		if (auto* instance = g_compatibilityInstance.load(std::memory_order_acquire)) {
			instance->ObserveCommand(a_command, result);
		}
		return result;
	}
}

namespace StarfieldAudioDuck
{
	GalacticRadioCompatibility::GalacticRadioCompatibility(StateCallback a_callback) :
		stateCallback(std::move(a_callback))
	{}

	bool GalacticRadioCompatibility::Install()
	{
		std::scoped_lock lock(g_installMutex);
		if (installed.load(std::memory_order_acquire)) {
			return true;
		}

		const auto radioModule = GetModuleHandleW(kRadioModuleName);
		if (!radioModule) {
			REX::INFO("StarfieldAudioDuck: Galactic Radio DLL not found; compatibility hook is inactive");
			return false;
		}

		void* original = nullptr;
		if (!PatchMciImport(radioModule, reinterpret_cast<void*>(&GalacticRadioMciSendStringHook), original)) {
			REX::WARN("StarfieldAudioDuck: Galactic Radio mciSendStringW import was not found; compatibility hook is inactive");
			return false;
		}

		g_originalMciSendStringW.store(reinterpret_cast<mci_send_string_w_t>(original), std::memory_order_release);
		g_compatibilityInstance.store(this, std::memory_order_release);
		installed.store(true, std::memory_order_release);
		REX::INFO("StarfieldAudioDuck: Galactic Radio compatibility hook installed");
		return true;
	}

	void GalacticRadioCompatibility::ObserveCommand(
		const wchar_t*  a_command,
		const std::uint32_t a_result) noexcept
	{
		if (a_result != 0 || !a_command) {
			return;
		}

		try {
			std::wstring_view remaining{ a_command };
			const auto command = ConsumeToken(remaining);
			if (EqualInsensitive(command, L"play") || EqualInsensitive(command, L"resume")) {
				if (IsRadioCommand(remaining)) {
					radioPlaying.store(true, std::memory_order_release);
					UpdateAudioState();
				}
				return;
			}

			if (EqualInsensitive(command, L"stop") || EqualInsensitive(command, L"pause") || EqualInsensitive(command, L"close")) {
				if (IsRadioCommand(remaining)) {
					radioPlaying.store(false, std::memory_order_release);
					radioVolume.store(0, std::memory_order_release);
					UpdateAudioState();
				}
				return;
			}

			if (EqualInsensitive(command, L"open")) {
				if (HasRadioAlias(remaining)) {
					radioPlaying.store(false, std::memory_order_release);
					radioVolume.store(0, std::memory_order_release);
					UpdateAudioState();
				}
				return;
			}

			if (!EqualInsensitive(command, L"setaudio") || !IsRadioCommand(remaining)) {
				return;
			}

			if (!EqualInsensitive(ConsumeToken(remaining), L"volume") || !EqualInsensitive(ConsumeToken(remaining), L"to")) {
				return;
			}

			long volume = 0;
			if (ParseVolume(remaining, volume)) {
				radioVolume.store(volume, std::memory_order_release);
				UpdateAudioState();
			}
		} catch (...) {
			REX::ERROR("StarfieldAudioDuck: Galactic Radio compatibility command handling failed");
		}
	}

	void GalacticRadioCompatibility::UpdateAudioState() noexcept
	{
		const bool active = radioPlaying.load(std::memory_order_acquire) && radioVolume.load(std::memory_order_acquire) > 0;
		const bool previous = radioAudioActive.exchange(active, std::memory_order_acq_rel);
		if (previous == active || !stateCallback) {
			return;
		}

		try {
			REX::INFO("StarfieldAudioDuck: Galactic Radio audio state changed: {}", active ? "active" : "inactive");
			stateCallback(active);
		} catch (...) {
			REX::ERROR("StarfieldAudioDuck: Galactic Radio compatibility state callback failed");
		}
	}
}
