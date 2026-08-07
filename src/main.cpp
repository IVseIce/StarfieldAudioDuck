#include "pch.h"

#include "AudioSessionMonitor.h"

#include "RE/S/Setting.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace
{
	constexpr std::string_view kPluginName = "StarfieldAudioDuck";
	constexpr char             kPluginDLLName[] = "StarfieldAudioDuck.dll";

	// Starfield 1.16.244.0, image base 0x140000000.
	// The public setting is a normal RE::Setting.  The adjacent $Music object
	// is Starfield's runtime audio-bus record and is not an RE::Setting.
	constexpr std::size_t kMusicSettingOffset = 0x5F07A20;
	constexpr std::size_t kMusicBusOffset = 0x5F07A40;
	constexpr std::size_t kSettingValueOffset = 0x08;
	constexpr std::string_view kMusicSettingKey = "fAudioVolumeMusic:Audio";
	constexpr std::string_view kMusicBusKey = "$Music";
	// These are the two side-effecting calls made by Starfield's settings
	// slider handler for the music setting.  The slider handler itself must not
	// be called here: its final UI-event dispatch requires a live Scaleform
	// event context that an SFSE plugin does not own.
	constexpr std::size_t kSettingFloatUpdateOffset = 0x684B30;
	constexpr std::size_t kAudioVolumeEventOffset = 0x2CD6D60;

	struct MusicBusLike
	{
		const char* key{ nullptr };
		float       value{ 0.0f };
	};
	static_assert(offsetof(MusicBusLike, value) == kSettingValueOffset);

	struct Config
	{
		bool          enabled{ true };
		float         mutedMusicVolume{ 0.0f };
		std::uint32_t restoreDelayMilliseconds{ 500 };
		bool          includeSystemSessions{ true };
	};

	std::string GetPluginDirectory()
	{
		char path[MAX_PATH]{};
		const auto module = GetModuleHandleA(kPluginDLLName);
		if (!module || GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path))) == 0) {
			return {};
		}

		std::string fullPath{ path };
		if (const auto separator = fullPath.find_last_of("\\/"); separator != std::string::npos) {
			fullPath.resize(separator);
		}
		return fullPath;
	}

	std::string ReadINIValue(const char* a_section, const char* a_key, const char* a_fallback)
	{
		const auto directory = GetPluginDirectory();
		const auto path = directory.empty() ? std::string{} : directory + "\\StarfieldAudioDuck.ini";

		char value[128]{};
		GetPrivateProfileStringA(
			a_section,
			a_key,
			a_fallback,
			value,
			static_cast<DWORD>(std::size(value)),
			path.c_str());
		return value;
	}

	bool ParseBool(std::string value, const bool a_fallback)
	{
		std::ranges::transform(value, value.begin(), [](const unsigned char a_character) {
			return static_cast<char>(std::tolower(a_character));
		});

		if (value == "1" || value == "true" || value == "yes" || value == "on") {
			return true;
		}
		if (value == "0" || value == "false" || value == "no" || value == "off") {
			return false;
		}
		return a_fallback;
	}

	float ParseFloat(
		std::string_view a_value,
		const float       a_fallback,
		const float       a_minimum,
		const float       a_maximum)
	{
		std::string text{ a_value };
		char*       end = nullptr;
		const float parsed = std::strtof(text.c_str(), &end);
		if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed) || parsed < a_minimum || parsed > a_maximum) {
			return a_fallback;
		}
		return parsed;
	}

	Config LoadConfig()
	{
		Config config{};
		config.enabled = ParseBool(ReadINIValue("Settings", "bEnable", "1"), true);
		config.mutedMusicVolume = ParseFloat(
			ReadINIValue("Settings", "fMutedMusicVolume", "0.0"),
			0.0f,
			0.0f,
			1.0f);

		const float restoreDelaySeconds = ParseFloat(
			ReadINIValue("Settings", "fRestoreDelaySeconds", "0.5"),
			0.5f,
			0.0f,
			30.0f);
		config.restoreDelayMilliseconds = static_cast<std::uint32_t>(std::lround(restoreDelaySeconds * 1000.0f));
		config.includeSystemSessions = ParseBool(
			ReadINIValue("Settings", "bIncludeSystemSessions", "1"),
			true);
		return config;
	}

	class MusicVolumeController final
	{
	public:
		explicit MusicVolumeController(const float a_mutedVolume) :
			mutedVolume(a_mutedVolume)
		{}

		[[nodiscard]] bool Apply(const bool a_muted)
		{
			auto* setting = GetMusicSetting();
			if (!setting) {
				LogSettingUnavailable("fAudioVolumeMusic:Audio setting is unavailable");
				return false;
			}
			if (!GetMusicBus()) {
				LogSettingUnavailable("$Music runtime bus object or key is unavailable");
				return false;
			}

			const float current = ReadMusicValue(setting);
			if (!std::isfinite(current) || current < 0.0f || current > 1.0f) {
				LogSettingUnavailable("music setting has an invalid value");
				return false;
			}
			settingUnavailableLogged = false;

			if (!initialized || !currentlyMuted) {
				normalVolume = current;
			}
			if (!initialized) {
				initialized = true;
				REX::INFO("StarfieldAudioDuck: captured initial Starfield music volume {:.3f}", normalVolume);
			}

			if (a_muted == currentlyMuted) {
				return true;
			}

			const float target = a_muted ? mutedVolume : normalVolume;
			const auto audioBusID = ReadMusicAudioBusID(setting);
			if (audioBusID == 0) {
				LogSettingUnavailable("music audio-bus ID is unavailable");
				return false;
			}

			if (!ApplyThroughStarfield(setting, audioBusID, target)) {
				return false;
			}

			currentlyMuted = a_muted;
			REX::INFO(
				"StarfieldAudioDuck: {} music volume to {:.3f}",
				a_muted ? "muted" : "restored",
				target);
			return true;
		}

	private:
		static RE::Setting* GetMusicSetting()
		{
			auto* setting = reinterpret_cast<RE::Setting*>(REL::Offset(kMusicSettingOffset).address());
			if (!setting || setting->GetKey() != kMusicSettingKey) {
				return nullptr;
			}
			return setting;
		}

		static MusicBusLike* GetMusicBus()
		{
			auto* bus = reinterpret_cast<MusicBusLike*>(REL::Offset(kMusicBusOffset).address());
			if (!bus || !bus->key || bus->key != kMusicBusKey) {
				return nullptr;
			}
			return bus;
		}

		static float ReadMusicValue(const RE::Setting* a_setting) noexcept
		{
			float value = std::numeric_limits<float>::quiet_NaN();
			std::memcpy(
				std::addressof(value),
				reinterpret_cast<const std::byte*>(a_setting) + kSettingValueOffset,
				sizeof(value));
			return value;
		}

		static std::uint32_t ReadMusicAudioBusID(const RE::Setting* a_setting) noexcept
		{
			std::uint32_t id = 0;
			std::memcpy(
				std::addressof(id),
				reinterpret_cast<const std::byte*>(a_setting) - sizeof(std::uint64_t),
				sizeof(id));
			return id;
		}

		void LogSettingUnavailable(const char* a_reason)
		{
			if (!settingUnavailableLogged) {
				REX::WARN("StarfieldAudioDuck: cannot access Starfield $Music setting: {}", a_reason);
				settingUnavailableLogged = true;
			}
		}

		[[nodiscard]] bool ApplyThroughStarfield(
			RE::Setting* const a_setting,
			const std::uint32_t a_audioBusID,
			const float a_value)
		{
			using setting_update_t = RE::Setting* (*)(RE::Setting*, float);
			using audio_event_t = std::uint32_t (*)(
				std::uint32_t,
				float,
				std::uint64_t,
				std::uint32_t,
				std::uint32_t,
				std::uint8_t);

			static REL::Relocation<setting_update_t> updateSetting{ REL::Offset(kSettingFloatUpdateOffset) };
			static REL::Relocation<audio_event_t>    postAudioEvent{ REL::Offset(kAudioVolumeEventOffset) };
			if (!updateSetting || !postAudioEvent) {
				LogSettingUnavailable("Starfield audio update relocation is unavailable");
				return false;
			}

			const float value = std::clamp(a_value, 0.0f, 1.0f);

			// This is Starfield's Setting::SetValue(float) implementation.  It
			// writes the public setting and emits the normal setting notification.
			updateSetting(a_setting, value);

			// This argument layout matches the native call immediately after the
			// music setting update at Starfield RVA 0x15128E8:
			//   ecx = the music bus/event ID, xmm1 = volume, r8 = UINT64_MAX,
			//   r9 = 0, stack args = 4, 0.
			// The callee queues the audio update; the runtime $Music value can
			// therefore still contain the old value when this returns.
			const auto result = postAudioEvent(
				a_audioBusID,
				value,
				std::numeric_limits<std::uint64_t>::max(),
				0,
				4,
				0);
			if (result != 1) {
				LogSettingUnavailable("Starfield rejected the music audio event");
				return false;
			}

			const float applied = a_setting ? ReadMusicValue(a_setting) : std::numeric_limits<float>::quiet_NaN();
			if (!std::isfinite(applied) || std::fabs(applied - value) > 0.001f) {
				LogSettingUnavailable("Starfield did not accept the requested music volume");
				return false;
			}
			return true;
		}

		float normalVolume{ 1.0f };
		float mutedVolume{ 0.0f };
		bool  initialized{ false };
		bool  currentlyMuted{ false };
		bool  settingUnavailableLogged{ false };
	};

	Config                                                   g_config{};
	std::unique_ptr<MusicVolumeController>                  g_musicController{};
	std::unique_ptr<StarfieldAudioDuck::AudioSessionMonitor> g_audioMonitor{};
	const SFSE::TaskInterface*                               g_taskInterface{ nullptr };
	std::atomic<bool>                                        g_desiredMuted{ false };
	std::atomic<bool>                                        g_taskPending{ false };
	std::atomic<std::uint32_t>                               g_applyRetries{ 0 };

	void RequestMusicState(bool a_muted);

	void RunMusicTask()
	{
		if (!g_musicController) {
			g_taskPending.store(false, std::memory_order_release);
			return;
		}

		constexpr std::uint32_t kMaximumApplyRetries = 120;
		bool                  lastProcessedState = g_desiredMuted.load(std::memory_order_acquire);
		bool                  retry = false;

		for (;;) {
			const bool target = g_desiredMuted.load(std::memory_order_acquire);
			lastProcessedState = target;
			if (!g_musicController->Apply(target)) {
				const auto retryCount = g_applyRetries.fetch_add(1, std::memory_order_acq_rel) + 1;
				retry = retryCount <= kMaximumApplyRetries;
				break;
			}

			g_applyRetries.store(0, std::memory_order_release);
			if (g_desiredMuted.load(std::memory_order_acquire) == target) {
				break;
			}
		}

		g_taskPending.store(false, std::memory_order_release);
		const bool latestState = g_desiredMuted.load(std::memory_order_acquire);
		if (retry || latestState != lastProcessedState) {
			RequestMusicState(latestState);
		}
	}

	void RequestMusicState(const bool a_muted)
	{
		g_desiredMuted.store(a_muted, std::memory_order_release);
		if (!g_taskInterface) {
			return;
		}

		bool expected = false;
		if (!g_taskPending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			return;
		}

		try {
			g_taskInterface->AddTask([]() {
				RunMusicTask();
			});
		} catch (const std::exception& e) {
			g_taskPending.store(false, std::memory_order_release);
			REX::ERROR("StarfieldAudioDuck: failed to enqueue game task: {}", e.what());
		} catch (...) {
			g_taskPending.store(false, std::memory_order_release);
			REX::ERROR("StarfieldAudioDuck: failed to enqueue game task");
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	if (!a_sfse) {
		return false;
	}

	SFSE::InitInfo initInfo{};
	initInfo.logName = "StarfieldAudioDuck";
	SFSE::Init(a_sfse, initInfo);

	const auto runtime = a_sfse->RuntimeVersion();
	if (runtime != SFSE::RUNTIME_SF_1_16_244) {
		REX::ERROR(
			"StarfieldAudioDuck: unsupported Starfield runtime {}; this build only supports {}",
			runtime,
			SFSE::RUNTIME_SF_1_16_244);
		return false;
	}

	g_config = LoadConfig();
	REX::INFO(
		"StarfieldAudioDuck: enabled={}, mutedMusicVolume={:.3f}, restoreDelay={} ms, includeSystemSessions={}",
		g_config.enabled,
		g_config.mutedMusicVolume,
		g_config.restoreDelayMilliseconds,
		g_config.includeSystemSessions);

	if (!g_config.enabled) {
		return true;
	}

	g_taskInterface = SFSE::GetTaskInterface();
	if (!g_taskInterface) {
		REX::ERROR("StarfieldAudioDuck: SFSE task interface is unavailable");
		return true;
	}

	g_musicController = std::make_unique<MusicVolumeController>(g_config.mutedMusicVolume);
	RequestMusicState(false);

	StarfieldAudioDuck::AudioSessionMonitorConfig monitorConfig{};
	monitorConfig.restoreDelayMilliseconds = g_config.restoreDelayMilliseconds;
	monitorConfig.includeSystemSessions = g_config.includeSystemSessions;
	g_audioMonitor = std::make_unique<StarfieldAudioDuck::AudioSessionMonitor>(
		monitorConfig,
		[](const bool a_externalAudioActive) {
			REX::INFO(
				"StarfieldAudioDuck: external audio state changed: {}",
				a_externalAudioActive ? "active" : "inactive");
			RequestMusicState(a_externalAudioActive);
		});

	if (!g_audioMonitor->Start()) {
		REX::ERROR("StarfieldAudioDuck: failed to start Windows Core Audio monitor");
		g_audioMonitor.reset();
	}

	return true;
}
