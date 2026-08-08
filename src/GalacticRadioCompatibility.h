#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace StarfieldAudioDuck
{
	class GalacticRadioCompatibility final
	{
	public:
		using StateCallback = std::function<void(bool a_audioActive)>;

		explicit GalacticRadioCompatibility(StateCallback a_callback);
		~GalacticRadioCompatibility() = default;

		GalacticRadioCompatibility(const GalacticRadioCompatibility&) = delete;
		GalacticRadioCompatibility(GalacticRadioCompatibility&&) = delete;
		GalacticRadioCompatibility& operator=(const GalacticRadioCompatibility&) = delete;
		GalacticRadioCompatibility& operator=(GalacticRadioCompatibility&&) = delete;

		[[nodiscard]] bool Install();
		void ObserveCommand(const wchar_t* a_command, std::uint32_t a_result) noexcept;

	private:
		void UpdateAudioState() noexcept;

		StateCallback      stateCallback{};
		std::atomic<bool>  radioPlaying{ false };
		std::atomic<long>  radioVolume{ 0 };
		std::atomic<bool>  radioAudioActive{ false };
		std::atomic<bool>  installed{ false };
	};
}
