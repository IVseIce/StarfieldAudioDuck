#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace StarfieldAudioDuck
{
	struct AudioSessionMonitorConfig
	{
		std::uint32_t activationDelayMilliseconds{ 2500 };
		std::uint32_t restoreDelayMilliseconds{ 500 };
		bool          includeSystemSessions{ true };
	};

	class AudioSessionMonitor final
	{
	public:
		using StateCallback = std::function<void(bool a_externalAudioActive)>;

		AudioSessionMonitor(AudioSessionMonitorConfig a_config, StateCallback a_callback);
		~AudioSessionMonitor();

		AudioSessionMonitor(const AudioSessionMonitor&) = delete;
		AudioSessionMonitor(AudioSessionMonitor&&) = delete;
		AudioSessionMonitor& operator=(const AudioSessionMonitor&) = delete;
		AudioSessionMonitor& operator=(AudioSessionMonitor&&) = delete;

		[[nodiscard]] bool Start();
		void Stop();
		void SetCompatibilityAudioActive(bool a_active);

	private:
		struct Impl;
		std::unique_ptr<Impl> _impl;
	};
}
