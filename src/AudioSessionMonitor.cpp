#include "pch.h"

#include "AudioSessionMonitor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace StarfieldAudioDuck
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		constexpr DWORD kDisconnectedSessionWaitMilliseconds = 5000;

		void LogHRESULT(const char* a_operation, const HRESULT a_result)
		{
			REX::WARN(
				"StarfieldAudioDuck: {} failed with HRESULT 0x{:08X}",
				a_operation,
				static_cast<unsigned long>(a_result));
		}
	}

	struct AudioSessionMonitor::Impl
	{
		struct SessionEvents;
		struct SessionNotification;
		struct DeviceNotification;

		struct PendingSession
		{
			ComPtr<IAudioSessionControl> control;
			std::uint64_t                generation{ 0 };
		};

		struct SessionRecord
		{
			ComPtr<IAudioSessionControl>  control;
			ComPtr<IAudioSessionControl2> control2;
			SessionEvents*                events{ nullptr };
			DWORD                         processID{ 0 };
			bool                          external{ false };
		};

		Impl(AudioSessionMonitorConfig a_config, AudioSessionMonitor::StateCallback a_callback) :
			config(a_config),
			stateCallback(std::move(a_callback))
		{
			stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		}

		~Impl();

		[[nodiscard]] bool Start();
		void              Stop();
		void              SetCompatibilityAudioActive(bool a_active);

		void ThreadMain();
		[[nodiscard]] bool InitializeCOMObjects();
		void               RebuildDefaultDevice();
		void               TeardownSessionManager();

		void ProcessQueuedCallbacks();
		void ProcessAggregateTransitions();
		void ProcessActivationTimer();
		void ProcessRestoreTimer();
		[[nodiscard]] DWORD CalculateWaitTimeout() const;
		void               NotifyState(bool a_externalAudioActive);
		void               ClearCallbackQueues();

		void TrackSession(IAudioSessionControl* a_control);
		void RemoveSession(SessionEvents* a_events);
		void DeactivateSession(SessionEvents* a_events);
		void HandleSessionState(SessionEvents* a_events, AudioSessionState a_state) noexcept;
		void HandleAggregateTransition(bool a_externalAudioActive);

		void QueueNewSession(IAudioSessionControl* a_control, std::uint64_t a_generation);
		void QueueSessionRemoval(SessionEvents* a_events);
		void QueueAggregateTransition(bool a_externalAudioActive);
		void SignalWake() noexcept
		{
			if (wakeEvent) {
				SetEvent(wakeEvent);
			}
		}

		AudioSessionMonitorConfig        config{};
		AudioSessionMonitor::StateCallback stateCallback{};
		HANDLE                           stopEvent{ nullptr };
		HANDLE                           wakeEvent{ nullptr };
		std::thread                      thread{};
		std::atomic<bool>                stopRequested{ false };
		std::atomic<bool>                deviceDirty{ true };
		std::uint64_t                    deviceGeneration{ 0 };
		const DWORD                      gameProcessID{ GetCurrentProcessId() };

		std::mutex                    callbackQueueMutex{};
		std::mutex                    aggregateStateMutex{};
		std::deque<PendingSession>    pendingSessions{};
		std::deque<SessionEvents*>    pendingRemovals{};
		std::deque<bool>              pendingAggregateTransitions{};

		std::atomic<std::int32_t> externalActiveSessions{ 0 };
		std::atomic<bool>           externalAudioActive{ false };
		bool                        compatibilityAudioActive{ false };

		ComPtr<IMMDeviceEnumerator> deviceEnumerator{};
		ComPtr<IMMDevice>           defaultDevice{};
		ComPtr<IAudioSessionManager2> sessionManager{};
		ComPtr<IAudioSessionEnumerator> sessionEnumerator{};
		SessionNotification*          sessionNotification{ nullptr };
		DeviceNotification*           deviceNotification{ nullptr };
		std::unordered_map<SessionEvents*, SessionRecord> sessions{};

		bool                                  activationPending{ false };
		std::chrono::steady_clock::time_point activationDue{};
		bool                                  restorePending{ false };
		std::chrono::steady_clock::time_point restoreDue{};
		bool                                  activeStateNotified{ false };
	};

	struct AudioSessionMonitor::Impl::SessionEvents final : IAudioSessionEvents
	{
		explicit SessionEvents(Impl* a_owner, const bool a_external) :
			owner(a_owner),
			external(a_external)
		{}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_object) override
		{
			if (!a_object) {
				return E_POINTER;
			}

			*a_object = nullptr;
			if (a_iid == IID_IUnknown || a_iid == __uuidof(IAudioSessionEvents)) {
				*a_object = static_cast<IAudioSessionEvents*>(this);
				AddRef();
				return S_OK;
			}

			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return referenceCount.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			const auto count = referenceCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
			if (count == 0) {
				delete this;
			}
			return count;
		}

		HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }

		HRESULT STDMETHODCALLTYPE OnStateChanged(const AudioSessionState a_newState) override
		{
			if (auto* currentOwner = owner.load(std::memory_order_acquire)) {
				currentOwner->HandleSessionState(this, a_newState);
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override
		{
			if (auto* currentOwner = owner.load(std::memory_order_acquire)) {
				currentOwner->QueueSessionRemoval(this);
			}
			return S_OK;
		}

		void Detach() noexcept
		{
			owner.store(nullptr, std::memory_order_release);
		}

		[[nodiscard]] bool SetActive(const bool a_active) noexcept
		{
			return active.exchange(a_active, std::memory_order_acq_rel);
		}

		[[nodiscard]] bool IsExternal() const noexcept { return external; }

	private:
		std::atomic<ULONG>        referenceCount{ 1 };
		std::atomic<Impl*>        owner{ nullptr };
		std::atomic<bool>         active{ false };
		const bool                external;
	};

	struct AudioSessionMonitor::Impl::SessionNotification final : IAudioSessionNotification
	{
		explicit SessionNotification(Impl* a_owner, const std::uint64_t a_generation) :
			owner(a_owner),
			generation(a_generation)
		{}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_object) override
		{
			if (!a_object) {
				return E_POINTER;
			}

			*a_object = nullptr;
			if (a_iid == IID_IUnknown || a_iid == __uuidof(IAudioSessionNotification)) {
				*a_object = static_cast<IAudioSessionNotification*>(this);
				AddRef();
				return S_OK;
			}

			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return referenceCount.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			const auto count = referenceCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
			if (count == 0) {
				delete this;
			}
			return count;
		}

		HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* a_newSession) override
		{
			if (auto* currentOwner = owner.load(std::memory_order_acquire)) {
				currentOwner->QueueNewSession(a_newSession, generation);
			}
			return S_OK;
		}

		void Detach() noexcept
		{
			owner.store(nullptr, std::memory_order_release);
		}

	private:
		std::atomic<ULONG>        referenceCount{ 1 };
		std::atomic<Impl*>        owner{ nullptr };
		const std::uint64_t       generation;
	};

	struct AudioSessionMonitor::Impl::DeviceNotification final : IMMNotificationClient
	{
		explicit DeviceNotification(Impl* a_owner) :
			owner(a_owner)
		{}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_object) override
		{
			if (!a_object) {
				return E_POINTER;
			}

			*a_object = nullptr;
			if (a_iid == IID_IUnknown || a_iid == __uuidof(IMMNotificationClient)) {
				*a_object = static_cast<IMMNotificationClient*>(this);
				AddRef();
				return S_OK;
			}

			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return referenceCount.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			const auto count = referenceCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
			if (count == 0) {
				delete this;
			}
			return count;
		}

		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
			const EDataFlow a_flow,
			const ERole a_role,
			LPCWSTR) override
		{
			if (a_flow == eRender && a_role == eConsole) {
				if (auto* currentOwner = owner.load(std::memory_order_acquire)) {
					currentOwner->deviceDirty.store(true, std::memory_order_release);
					currentOwner->SignalWake();
				}
			}
			return S_OK;
		}

		void Detach() noexcept
		{
			owner.store(nullptr, std::memory_order_release);
		}

	private:
		std::atomic<ULONG> referenceCount{ 1 };
		std::atomic<Impl*> owner{ nullptr };
	};

	AudioSessionMonitor::Impl::~Impl()
	{
		Stop();

		if (stopEvent) {
			CloseHandle(stopEvent);
			stopEvent = nullptr;
		}
		if (wakeEvent) {
			CloseHandle(wakeEvent);
			wakeEvent = nullptr;
		}
	}

	bool AudioSessionMonitor::Impl::Start()
	{
		if (!stopEvent || !wakeEvent || thread.joinable()) {
			return false;
		}

		stopRequested.store(false, std::memory_order_release);
		thread = std::thread([this]() {
			try {
				ThreadMain();
			} catch (const std::exception& e) {
				REX::ERROR("StarfieldAudioDuck: audio monitor terminated with exception: {}", e.what());
			} catch (...) {
				REX::ERROR("StarfieldAudioDuck: audio monitor terminated with an unknown exception");
			}
		});
		return true;
	}

	void AudioSessionMonitor::Impl::Stop()
	{
		if (!thread.joinable()) {
			return;
		}

		stopRequested.store(true, std::memory_order_release);
		if (stopEvent) {
			SetEvent(stopEvent);
		}
		SignalWake();
		thread.join();
	}

	void AudioSessionMonitor::Impl::SetCompatibilityAudioActive(const bool a_active)
	{
		std::scoped_lock lock(aggregateStateMutex);
		if (compatibilityAudioActive == a_active) {
			return;
		}

		compatibilityAudioActive = a_active;
		const bool aggregateActive = externalActiveSessions.load(std::memory_order_acquire) > 0 || compatibilityAudioActive;
		const bool previousAggregate = externalAudioActive.exchange(aggregateActive, std::memory_order_acq_rel);
		if (previousAggregate != aggregateActive) {
			QueueAggregateTransition(aggregateActive);
		}
	}

	void AudioSessionMonitor::Impl::ThreadMain()
	{
		const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(comResult)) {
			LogHRESULT("CoInitializeEx(COINIT_MULTITHREADED)", comResult);
			return;
		}

		const bool comInitialized = SUCCEEDED(comResult);
		if (!InitializeCOMObjects()) {
			if (comInitialized) {
				CoUninitialize();
			}
			return;
		}

		while (!stopRequested.load(std::memory_order_acquire)) {
			if (deviceDirty.exchange(false, std::memory_order_acq_rel)) {
				RebuildDefaultDevice();
			}

			ProcessQueuedCallbacks();
			ProcessAggregateTransitions();
			ProcessActivationTimer();
			ProcessRestoreTimer();

			if (stopRequested.load(std::memory_order_acquire)) {
				break;
			}

			const HANDLE waitHandles[] = { stopEvent, wakeEvent };
			const DWORD waitResult = WaitForMultipleObjects(
				static_cast<DWORD>(std::size(waitHandles)),
				waitHandles,
				FALSE,
				CalculateWaitTimeout());
			if (waitResult == WAIT_FAILED) {
				LogHRESULT("WaitForMultipleObjects", HRESULT_FROM_WIN32(GetLastError()));
				break;
			}
		}

		TeardownSessionManager();
		ClearCallbackQueues();
		if (deviceEnumerator && deviceNotification) {
			deviceEnumerator->UnregisterEndpointNotificationCallback(deviceNotification);
			deviceNotification->Detach();
			deviceNotification->Release();
			deviceNotification = nullptr;
		}
		deviceEnumerator.Reset();

		if (comInitialized) {
			CoUninitialize();
		}
	}

	bool AudioSessionMonitor::Impl::InitializeCOMObjects()
	{
		HRESULT result = CoCreateInstance(
			__uuidof(MMDeviceEnumerator),
			nullptr,
			CLSCTX_ALL,
			IID_PPV_ARGS(deviceEnumerator.ReleaseAndGetAddressOf()));
		if (FAILED(result)) {
			LogHRESULT("CoCreateInstance(MMDeviceEnumerator)", result);
			return false;
		}

		deviceNotification = new DeviceNotification(this);
		result = deviceEnumerator->RegisterEndpointNotificationCallback(deviceNotification);
		if (FAILED(result)) {
			LogHRESULT("RegisterEndpointNotificationCallback", result);
			deviceNotification->Detach();
			deviceNotification->Release();
			deviceNotification = nullptr;
			return false;
		}

		return true;
	}

	void AudioSessionMonitor::Impl::RebuildDefaultDevice()
	{
		++deviceGeneration;
		TeardownSessionManager();

		HRESULT result = deviceEnumerator->GetDefaultAudioEndpoint(
			eRender,
			eConsole,
			defaultDevice.ReleaseAndGetAddressOf());
		if (FAILED(result)) {
			LogHRESULT("GetDefaultAudioEndpoint(eRender, eConsole)", result);
			return;
		}

		result = defaultDevice->Activate(
			__uuidof(IAudioSessionManager2),
			CLSCTX_ALL,
			nullptr,
			reinterpret_cast<void**>(sessionManager.ReleaseAndGetAddressOf()));
		if (FAILED(result)) {
			LogHRESULT("IMMDevice::Activate(IAudioSessionManager2)", result);
			defaultDevice.Reset();
			return;
		}

		sessionNotification = new SessionNotification(this, deviceGeneration);
		result = sessionManager->RegisterSessionNotification(sessionNotification);
		if (FAILED(result)) {
			LogHRESULT("RegisterSessionNotification", result);
			sessionNotification->Detach();
			sessionNotification->Release();
			sessionNotification = nullptr;
			sessionManager.Reset();
			defaultDevice.Reset();
			return;
		}

		result = sessionManager->GetSessionEnumerator(sessionEnumerator.ReleaseAndGetAddressOf());
		if (FAILED(result)) {
			LogHRESULT("GetSessionEnumerator", result);
			return;
		}

		int sessionCount = 0;
		result = sessionEnumerator->GetCount(&sessionCount);
		if (FAILED(result)) {
			LogHRESULT("IAudioSessionEnumerator::GetCount", result);
			return;
		}

		for (int index = 0; index < sessionCount; ++index) {
			ComPtr<IAudioSessionControl> control;
			result = sessionEnumerator->GetSession(index, control.ReleaseAndGetAddressOf());
			if (SUCCEEDED(result)) {
				TrackSession(control.Get());
			} else {
				LogHRESULT("IAudioSessionEnumerator::GetSession", result);
			}
		}

		REX::INFO(
			"StarfieldAudioDuck: attached to default render device; sessions={}, externalActive={}",
			sessionCount,
			externalActiveSessions.load(std::memory_order_acquire));
	}

	void AudioSessionMonitor::Impl::TeardownSessionManager()
	{
		if (sessionManager && sessionNotification) {
			sessionManager->UnregisterSessionNotification(sessionNotification);
		}

		for (auto& [events, record] : sessions) {
			if (record.control && events) {
				record.control->UnregisterAudioSessionNotification(events);
			}
			if (events) {
				events->Detach();
				DeactivateSession(events);
				events->Release();
			}
		}
		sessions.clear();
		sessionEnumerator.Reset();
		sessionManager.Reset();
		defaultDevice.Reset();

		if (sessionNotification) {
			sessionNotification->Detach();
			sessionNotification->Release();
			sessionNotification = nullptr;
		}
	}

	void AudioSessionMonitor::Impl::ProcessQueuedCallbacks()
	{
		std::deque<PendingSession> pendingSessionsLocal;
		std::deque<SessionEvents*> pendingRemovalsLocal;
		{
			std::scoped_lock lock(callbackQueueMutex);
			pendingSessionsLocal.swap(pendingSessions);
			pendingRemovalsLocal.swap(pendingRemovals);
		}

		for (auto* events : pendingRemovalsLocal) {
			RemoveSession(events);
			events->Release();
		}

		for (auto& pending : pendingSessionsLocal) {
			if (pending.generation == deviceGeneration && pending.control) {
				TrackSession(pending.control.Get());
			}
		}
	}

	void AudioSessionMonitor::Impl::ProcessAggregateTransitions()
	{
		std::deque<bool> transitions;
		{
			std::scoped_lock lock(callbackQueueMutex);
			transitions.swap(pendingAggregateTransitions);
		}

		for (const bool externalState : transitions) {
			if (externalState && this->externalAudioActive.load(std::memory_order_acquire)) {
				restorePending = false;
				HandleAggregateTransition(true);
			} else if (!this->externalAudioActive.load(std::memory_order_acquire)) {
				HandleAggregateTransition(false);
			}
		}
	}

	void AudioSessionMonitor::Impl::ProcessActivationTimer()
	{
		if (!activationPending) {
			return;
		}

		if (!externalAudioActive.load(std::memory_order_acquire)) {
			activationPending = false;
			return;
		}

		if (std::chrono::steady_clock::now() >= activationDue) {
			activationPending = false;
			if (!activeStateNotified) {
				activeStateNotified = true;
				NotifyState(true);
			}
		}
	}

	void AudioSessionMonitor::Impl::ProcessRestoreTimer()
	{
		if (!restorePending) {
			return;
		}

		if (externalAudioActive.load(std::memory_order_acquire)) {
			restorePending = false;
			return;
		}

		if (std::chrono::steady_clock::now() >= restoreDue) {
			restorePending = false;
			if (activeStateNotified) {
				activeStateNotified = false;
				NotifyState(false);
			}
		}
	}

	DWORD AudioSessionMonitor::Impl::CalculateWaitTimeout() const
	{
		DWORD timeout = sessionManager ? INFINITE : kDisconnectedSessionWaitMilliseconds;
		const auto now = std::chrono::steady_clock::now();

		if (activationPending) {
			if (now >= activationDue) {
				return 0;
			}

			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(activationDue - now).count();
			const auto activationTimeout = static_cast<DWORD>(std::clamp<std::int64_t>(remaining + 1, 1, MAXDWORD));
			timeout = std::min(timeout, activationTimeout);
		}

		if (restorePending) {
			if (now >= restoreDue) {
				return 0;
			}

			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(restoreDue - now).count();
			const auto restoreTimeout = static_cast<DWORD>(std::clamp<std::int64_t>(remaining + 1, 1, MAXDWORD));
			timeout = std::min(timeout, restoreTimeout);
		}

		return timeout;
	}

	void AudioSessionMonitor::Impl::TrackSession(IAudioSessionControl* a_control)
	{
		if (!a_control) {
			return;
		}

		for (const auto& [events, record] : sessions) {
			if (record.control.Get() == a_control) {
				return;
			}
		}

		ComPtr<IAudioSessionControl2> control2;
		const HRESULT queryResult = a_control->QueryInterface(IID_PPV_ARGS(control2.ReleaseAndGetAddressOf()));
		DWORD processID = 0;
		bool external = true;
		if (SUCCEEDED(queryResult)) {
			const HRESULT processResult = control2->GetProcessId(&processID);
			if (SUCCEEDED(processResult)) {
				external = processID != gameProcessID && (processID != 0 || config.includeSystemSessions);
			} else {
				LogHRESULT("IAudioSessionControl2::GetProcessId", processResult);
			}
		} else {
			LogHRESULT("IAudioSessionControl::QueryInterface(IAudioSessionControl2)", queryResult);
		}

		auto* events = new SessionEvents(this, external);
		const HRESULT registerResult = a_control->RegisterAudioSessionNotification(events);
		if (FAILED(registerResult)) {
			LogHRESULT("RegisterAudioSessionNotification", registerResult);
			events->Detach();
			events->Release();
			return;
		}

		SessionRecord record{};
		record.control = a_control;
		record.control2 = std::move(control2);
		record.events = events;
		record.processID = processID;
		record.external = external;
		sessions.emplace(events, std::move(record));

		AudioSessionState state = AudioSessionStateInactive;
		if (SUCCEEDED(a_control->GetState(&state))) {
			HandleSessionState(events, state);
		}
	}

	void AudioSessionMonitor::Impl::RemoveSession(SessionEvents* a_events)
	{
		if (!a_events) {
			return;
		}

		const auto it = sessions.find(a_events);
		if (it == sessions.end()) {
			return;
		}

		a_events->Detach();
		if (it->second.control) {
			it->second.control->UnregisterAudioSessionNotification(a_events);
		}
		DeactivateSession(a_events);
		a_events->Release();
		sessions.erase(it);
	}

	void AudioSessionMonitor::Impl::DeactivateSession(SessionEvents* a_events)
	{
		if (!a_events || !a_events->IsExternal() || !a_events->SetActive(false)) {
			return;
		}

		std::scoped_lock lock(aggregateStateMutex);
		const auto remaining = externalActiveSessions.fetch_sub(1, std::memory_order_acq_rel) - 1;
		const bool aggregateActive = remaining > 0 || compatibilityAudioActive;
		const bool previousAggregate = externalAudioActive.exchange(aggregateActive, std::memory_order_acq_rel);
		if (previousAggregate != aggregateActive) {
			QueueAggregateTransition(aggregateActive);
		}
	}

	void AudioSessionMonitor::Impl::HandleSessionState(
		SessionEvents* const         a_events,
		const AudioSessionState      a_state) noexcept
	{
		if (!a_events || !a_events->IsExternal()) {
			return;
		}

		const bool active = a_state == AudioSessionStateActive;
		const bool wasActive = a_events->SetActive(active);
		if (wasActive == active) {
			return;
		}

		std::scoped_lock lock(aggregateStateMutex);
		const auto count = active ?
				externalActiveSessions.fetch_add(1, std::memory_order_acq_rel) + 1 :
				externalActiveSessions.fetch_sub(1, std::memory_order_acq_rel) - 1;
		const bool aggregateActive = count > 0 || compatibilityAudioActive;
		const bool previousAggregate = externalAudioActive.exchange(aggregateActive, std::memory_order_acq_rel);
		if (previousAggregate != aggregateActive) {
			QueueAggregateTransition(aggregateActive);
		}
	}

	void AudioSessionMonitor::Impl::HandleAggregateTransition(const bool a_externalAudioActive)
	{
		if (a_externalAudioActive) {
			restorePending = false;
			if (activeStateNotified) {
				activationPending = false;
				return;
			}

			if (config.activationDelayMilliseconds != 0) {
				activationPending = true;
				activationDue = std::chrono::steady_clock::now() +
					std::chrono::milliseconds(config.activationDelayMilliseconds);
				return;
			}

			activationPending = false;
			activeStateNotified = true;
			NotifyState(true);
			return;
		} else {
			activationPending = false;
			if (!activeStateNotified) {
				restorePending = false;
				return;
			}
		}

		if (config.restoreDelayMilliseconds != 0) {
			restorePending = true;
			restoreDue = std::chrono::steady_clock::now() +
				std::chrono::milliseconds(config.restoreDelayMilliseconds);
			return;
		}

		activeStateNotified = false;
		NotifyState(a_externalAudioActive);
	}

	void AudioSessionMonitor::Impl::NotifyState(const bool a_externalAudioActive)
	{
		if (stateCallback) {
			try {
				stateCallback(a_externalAudioActive);
			} catch (const std::exception& e) {
				REX::ERROR("StarfieldAudioDuck: state callback failed: {}", e.what());
			} catch (...) {
				REX::ERROR("StarfieldAudioDuck: state callback failed with an unknown exception");
			}
		}
	}

	void AudioSessionMonitor::Impl::ClearCallbackQueues()
	{
		std::deque<SessionEvents*> removals;
		{
			std::scoped_lock lock(callbackQueueMutex);
			pendingSessions.clear();
			removals.swap(pendingRemovals);
			pendingAggregateTransitions.clear();
		}

		for (auto* events : removals) {
			if (events) {
				events->Release();
			}
		}
	}

	void AudioSessionMonitor::Impl::QueueNewSession(
		IAudioSessionControl* const a_control,
		const std::uint64_t          a_generation)
	{
		if (!a_control) {
			return;
		}

		PendingSession pending{};
		pending.control = a_control;
		pending.generation = a_generation;
		{
			std::scoped_lock lock(callbackQueueMutex);
			pendingSessions.emplace_back(std::move(pending));
		}
		SignalWake();
	}

	void AudioSessionMonitor::Impl::QueueSessionRemoval(SessionEvents* const a_events)
	{
		if (!a_events) {
			return;
		}

		a_events->AddRef();
		{
			std::scoped_lock lock(callbackQueueMutex);
			pendingRemovals.push_back(a_events);
		}
		SignalWake();
	}

	void AudioSessionMonitor::Impl::QueueAggregateTransition(const bool a_externalAudioActive)
	{
		{
			std::scoped_lock lock(callbackQueueMutex);
			if (pendingAggregateTransitions.empty() || pendingAggregateTransitions.back() != a_externalAudioActive) {
				pendingAggregateTransitions.push_back(a_externalAudioActive);
			}
		}
		SignalWake();
	}

	AudioSessionMonitor::AudioSessionMonitor(AudioSessionMonitorConfig a_config, StateCallback a_callback) :
		_impl(std::make_unique<Impl>(a_config, std::move(a_callback)))
	{}

	AudioSessionMonitor::~AudioSessionMonitor() = default;

	bool AudioSessionMonitor::Start()
	{
		return _impl && _impl->Start();
	}

	void AudioSessionMonitor::Stop()
	{
		if (_impl) {
			_impl->Stop();
		}
	}

	void AudioSessionMonitor::SetCompatibilityAudioActive(const bool a_active)
	{
		if (_impl) {
			_impl->SetCompatibilityAudioActive(a_active);
		}
	}
}
