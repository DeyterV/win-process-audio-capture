#include "process-audio-capture.hpp"

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <audiopolicy.h>
#include <objbase.h>

#include <set>

#define OPT_PROCESS "process"
#define OBS_KSAUDIO_SPEAKER_4POINT1 (KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY)
#define BUFFER_TIME_100NS (5 * 10000000)

static DWORD GetSpeakerChannelMask(speaker_layout layout)
{
	switch (layout) {
	case SPEAKERS_STEREO:
		return KSAUDIO_SPEAKER_STEREO;
	case SPEAKERS_2POINT1:
		return KSAUDIO_SPEAKER_2POINT1;
	case SPEAKERS_4POINT0:
		return KSAUDIO_SPEAKER_SURROUND;
	case SPEAKERS_4POINT1:
		return OBS_KSAUDIO_SPEAKER_4POINT1;
	case SPEAKERS_5POINT1:
		return KSAUDIO_SPEAKER_5POINT1_SURROUND;
	case SPEAKERS_7POINT1:
		return KSAUDIO_SPEAKER_7POINT1_SURROUND;
	default:
		return (DWORD)layout;
	}
}

/* ------------------------------------------------------------------ */
/* ActivationCompletionHandler                                          */

ActivationCompletionHandler::ActivationCompletionHandler()
{
	activationSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!activationSignal.Valid())
		throw "Could not create activation signal";
}

HRESULT ActivationCompletionHandler::GetActivateResult(IAudioClient **client)
{
	WaitForSingleObject(activationSignal, INFINITE);
	*client = static_cast<IAudioClient *>(unknown);
	return activationResult;
}

HRESULT ActivationCompletionHandler::ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op)
{
	HRESULT hr, hr_activate;
	hr = op->GetActivateResult(&hr_activate, &unknown);
	hr = SUCCEEDED(hr) ? hr_activate : hr;
	activationResult = hr;
	SetEvent(activationSignal);
	return hr;
}

/* ------------------------------------------------------------------ */
/* ProcessAudioCapture                                                  */

ProcessAudioCapture::ProcessAudioCapture(obs_data_t *settings, obs_source_t *source)
	: source(source)
{
	HMODULE mmdevapi = GetModuleHandleA("mmdevapi.dll");
	if (mmdevapi) {
		activate_audio_interface_async = (PFN_ActivateAudioInterfaceAsync)GetProcAddress(
			mmdevapi, "ActivateAudioInterfaceAsync");
	}

	captureEvent = CreateEvent(nullptr, false, false, nullptr);
	stopSignal = CreateEvent(nullptr, true, false, nullptr);

	ParseSettings(settings);
}

ProcessAudioCapture::~ProcessAudioCapture()
{
	Deactivate();
}

void ProcessAudioCapture::ParseSettings(obs_data_t *settings)
{
	const char *val = obs_data_get_string(settings, OPT_PROCESS);
	if (!val || !*val) {
		executable.clear();
		preferred_pid = 0;
		return;
	}

	/* stored as "game.exe|1234" */
	const char *sep = strchr(val, '|');
	if (sep) {
		executable = std::string(val, sep - val);
		preferred_pid = (DWORD)atol(sep + 1);
	} else {
		executable = val;
		preferred_pid = 0;
	}
}

void ProcessAudioCapture::Update(obs_data_t *settings)
{
	std::string old_exe = executable;
	DWORD old_pid = preferred_pid;

	ParseSettings(settings);

	if ((old_exe != executable || old_pid != preferred_pid) && backgroundThread.Valid()) {
		Deactivate();
		Activate();
	}
}

void ProcessAudioCapture::Activate()
{
	if (!backgroundThread.Valid()) {
		ResetEvent(stopSignal);
		backgroundThread = CreateThread(nullptr, 0, BackgroundThread, this, 0, nullptr);
	}
}

void ProcessAudioCapture::Deactivate()
{
	if (backgroundThread.Valid()) {
		SetEvent(stopSignal);
		WaitForSingleObject(backgroundThread, INFINITE);
		backgroundThread = NULL;
	}
	StopCapture();
}

DWORD ProcessAudioCapture::FindTargetProcess()
{
	if (executable.empty())
		return 0;

	wchar_t wexe[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, executable.c_str(), -1, wexe, MAX_PATH);

	/* Try the preferred PID first — fast path when the process hasn't restarted */
	if (preferred_pid != 0) {
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, preferred_pid);
		if (h) {
			wchar_t path[MAX_PATH];
			DWORD size = MAX_PATH;
			bool match = false;
			if (QueryFullProcessImageNameW(h, 0, path, &size)) {
				wchar_t *fn = wcsrchr(path, L'\\');
				match = fn && _wcsicmp(fn + 1, wexe) == 0;
			}
			CloseHandle(h);
			if (match)
				return preferred_pid;
		}
	}

	/* Fall back: first process matching the exe name */
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	DWORD found = 0;

	if (Process32FirstW(snapshot, &pe)) {
		do {
			if (_wcsicmp(pe.szExeFile, wexe) == 0) {
				found = pe.th32ProcessID;
				break;
			}
		} while (Process32NextW(snapshot, &pe));
	}

	CloseHandle(snapshot);
	return found;
}

bool ProcessAudioCapture::IsTargetAlive()
{
	if (!processHandle.Valid())
		return false;
	/* WAIT_TIMEOUT means the process is still running */
	return WaitForSingleObject(processHandle, 0) == WAIT_TIMEOUT;
}

bool ProcessAudioCapture::TryInitialize()
{
	if (!activate_audio_interface_async) {
		if (!previouslyFailed)
			blog(LOG_WARNING, "[ProcessAudioCapture] ActivateAudioInterfaceAsync not available "
					  "(requires Windows 10 2004 or later)");
		previouslyFailed = true;
		return false;
	}

	DWORD pid = FindTargetProcess();
	if (pid == 0) {
		if (!previouslyFailed)
			blog(LOG_INFO, "[ProcessAudioCapture] Waiting for process '%s'",
			     executable.c_str());
		previouslyFailed = true;
		return false;
	}

	processHandle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!processHandle.Valid()) {
		previouslyFailed = true;
		return false;
	}

	struct obs_audio_info oai;
	obs_get_audio_info(&oai);

	const WORD nChannels = (WORD)get_audio_channels(oai.speakers);
	const DWORD nSamplesPerSec = oai.samples_per_sec;
	constexpr WORD wBitsPerSample = 32;
	const WORD nBlockAlign = nChannels * wBitsPerSample / 8;

	WAVEFORMATEXTENSIBLE wfx = {};
	wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wfx.Format.nChannels = nChannels;
	wfx.Format.nSamplesPerSec = nSamplesPerSec;
	wfx.Format.nAvgBytesPerSec = nSamplesPerSec * nBlockAlign;
	wfx.Format.nBlockAlign = nBlockAlign;
	wfx.Format.wBitsPerSample = wBitsPerSample;
	wfx.Format.cbSize = sizeof(wfx) - sizeof(wfx.Format);
	wfx.Samples.wValidBitsPerSample = wBitsPerSample;
	wfx.dwChannelMask = GetSpeakerChannelMask(oai.speakers);
	wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

	AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
	activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	activationParams.ProcessLoopbackParams.TargetProcessId = pid;
	activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
		PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

	PROPVARIANT propvar = {};
	propvar.vt = VT_BLOB;
	propvar.blob.cbSize = sizeof(activationParams);
	propvar.blob.pBlobData = (BYTE *)&activationParams;

	ComPtr<IAudioClient> newClient;
	{
		Microsoft::WRL::ComPtr<ActivationCompletionHandler> handler =
			Microsoft::WRL::Make<ActivationCompletionHandler>();

		ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
		HRESULT hr = activate_audio_interface_async(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
							     __uuidof(IAudioClient), &propvar,
							     handler.Get(), &asyncOp);
		if (FAILED(hr)) {
			if (!previouslyFailed)
				blog(LOG_WARNING,
				     "[ProcessAudioCapture] ActivateAudioInterfaceAsync failed: %lX", hr);
			previouslyFailed = true;
			return false;
		}

		hr = handler->GetActivateResult(newClient.Assign());
		if (FAILED(hr)) {
			if (!previouslyFailed)
				blog(LOG_WARNING,
				     "[ProcessAudioCapture] Async activation failed: %lX", hr);
			previouslyFailed = true;
			return false;
		}
	}

	HRESULT hr = newClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
					   AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
						   AUDCLNT_STREAMFLAGS_LOOPBACK,
					   BUFFER_TIME_100NS, 0, &wfx.Format, nullptr);
	if (FAILED(hr)) {
		if (!previouslyFailed)
			blog(LOG_WARNING, "[ProcessAudioCapture] IAudioClient::Initialize failed: %lX",
			     hr);
		previouslyFailed = true;
		return false;
	}

	hr = newClient->SetEventHandle(captureEvent);
	if (FAILED(hr)) {
		previouslyFailed = true;
		return false;
	}

	ComPtr<IAudioCaptureClient> newCapture;
	hr = newClient->GetService(IID_PPV_ARGS(newCapture.Assign()));
	if (FAILED(hr)) {
		previouslyFailed = true;
		return false;
	}

	hr = newClient->Start();
	if (FAILED(hr)) {
		previouslyFailed = true;
		return false;
	}

	client = std::move(newClient);
	capture = std::move(newCapture);
	speakers = oai.speakers;
	sampleRate = oai.samples_per_sec;
	format = AUDIO_FORMAT_FLOAT;
	previouslyFailed = false;

	blog(LOG_INFO, "[ProcessAudioCapture] Capturing '%s' (PID %lu)", executable.c_str(),
	     (unsigned long)pid);
	return true;
}

bool ProcessAudioCapture::ProcessCaptureData()
{
	UINT captureSize = 0;
	HRESULT hr = capture->GetNextPacketSize(&captureSize);
	if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(hr))
		return false;

	while (captureSize > 0) {
		LPBYTE buffer;
		UINT32 frames;
		DWORD flags;
		UINT64 pos, ts;

		hr = capture->GetBuffer(&buffer, &frames, &flags, &pos, &ts);
		if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(hr))
			return false;

		if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
			uint32_t needed = get_audio_channels(speakers) * frames * sizeof(float);
			if (silence.size() < needed)
				silence.assign(needed, 0);
			buffer = silence.data();
		}

		obs_source_audio data = {};
		data.data[0] = buffer;
		data.frames = frames;
		data.speakers = speakers;
		data.samples_per_sec = sampleRate;
		data.format = format;
		data.timestamp = ts * 100;

		obs_source_output_audio(source, &data);

		capture->ReleaseBuffer(frames);

		hr = capture->GetNextPacketSize(&captureSize);
		if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(hr))
			return false;
	}

	return true;
}

void ProcessAudioCapture::StopCapture()
{
	if (client)
		client->Stop();
	capture.Clear();
	client.Clear();
	processHandle = NULL;
}

DWORD WINAPI ProcessAudioCapture::BackgroundThread(LPVOID param)
{
	ProcessAudioCapture *self = (ProcessAudioCapture *)param;

	DWORD taskIndex = 0;
	HANDLE mmTask = AvSetMmThreadCharacteristics(L"Audio", &taskIndex);

	const HANDLE sigs[2] = {self->stopSignal, self->captureEvent};

	while (WaitForSingleObject(self->stopSignal, 0) == WAIT_TIMEOUT) {
		if (!self->TryInitialize()) {
			WaitForSingleObject(self->stopSignal, 3000);
			continue;
		}

		bool running = true;
		while (running) {
			DWORD ret = WaitForMultipleObjects(2, sigs, false, 200);
			if (ret == WAIT_OBJECT_0) {
				/* stop signal */
				running = false;
			} else if (ret == WAIT_OBJECT_0 + 1) {
				if (!self->ProcessCaptureData() || !self->IsTargetAlive())
					running = false;
			} else {
				/* timeout: check process still alive */
				if (!self->IsTargetAlive())
					running = false;
			}
		}

		self->StopCapture();
		self->previouslyFailed = false;

		if (WaitForSingleObject(self->stopSignal, 0) == WAIT_TIMEOUT) {
			blog(LOG_INFO, "[ProcessAudioCapture] Lost '%s', retrying in 3s...",
			     self->executable.c_str());
			WaitForSingleObject(self->stopSignal, 3000);
		}
	}

	if (mmTask)
		AvRevertMmThreadCharacteristics(mmTask);
	return 0;
}

/* ------------------------------------------------------------------ */
/* OBS source registration                                              */

static bool CollectPidsWithAudioSessions(std::set<DWORD> &pids)
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool we_initialized = (hr == S_OK || hr == S_FALSE);
	bool com_usable = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

	bool ok = false;

	if (com_usable) {
		ComPtr<IMMDeviceEnumerator> enumerator;
		hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				      IID_PPV_ARGS(enumerator.Assign()));

		if (SUCCEEDED(hr)) {
			ComPtr<IMMDeviceCollection> devices;
			hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
							     devices.Assign());

			if (SUCCEEDED(hr)) {
				UINT count = 0;
				devices->GetCount(&count);

				for (UINT i = 0; i < count; i++) {
					ComPtr<IMMDevice> device;
					if (FAILED(devices->Item(i, device.Assign())))
						continue;

					ComPtr<IAudioSessionManager2> sessionManager;
					if (FAILED(device->Activate(__uuidof(IAudioSessionManager2),
								     CLSCTX_ALL, nullptr,
								     (void **)sessionManager.Assign())))
						continue;

					ComPtr<IAudioSessionEnumerator> sessionEnum;
					if (FAILED(sessionManager->GetSessionEnumerator(
						    sessionEnum.Assign())))
						continue;

					int sessionCount = 0;
					sessionEnum->GetCount(&sessionCount);

					for (int j = 0; j < sessionCount; j++) {
						ComPtr<IAudioSessionControl> sessionControl;
						if (FAILED(sessionEnum->GetSession(
							    j, sessionControl.Assign())))
							continue;

						ComPtr<IAudioSessionControl2> sessionControl2;
						if (FAILED(sessionControl->QueryInterface(
							    IID_PPV_ARGS(sessionControl2.Assign()))))
							continue;

						if (sessionControl2->IsSystemSoundsSession() == S_OK)
							continue;

						AudioSessionState state;
						if (FAILED(sessionControl2->GetState(&state)) ||
						    state == AudioSessionStateExpired)
							continue;

						DWORD pid = 0;
						if (SUCCEEDED(sessionControl2->GetProcessId(&pid)) &&
						    pid != 0)
							pids.insert(pid);
					}
				}

				ok = true;
			}
		}
	}

	if (we_initialized)
		CoUninitialize();

	return ok;
}

static void fill_process_list(obs_property_t *list, DWORD saved_pid)
{
	std::set<DWORD> pids_with_audio;
	bool have_audio_info = CollectPidsWithAudioSessions(pids_with_audio);

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return;

	struct ProcessEntry {
		DWORD pid;
		std::string exe;
	};

	std::vector<ProcessEntry> entries;

	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);

	if (Process32FirstW(snapshot, &pe)) {
		do {
			if (pe.th32ProcessID == GetCurrentProcessId())
				continue;

			bool keep = !have_audio_info || pids_with_audio.count(pe.th32ProcessID) > 0 ||
				    (saved_pid != 0 && pe.th32ProcessID == saved_pid);
			if (!keep)
				continue;

			char exe[MAX_PATH];
			WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exe, sizeof(exe), NULL, NULL);
			entries.push_back({pe.th32ProcessID, exe});

		} while (Process32NextW(snapshot, &pe));
	}

	CloseHandle(snapshot);

	std::sort(entries.begin(), entries.end(), [](const ProcessEntry &a, const ProcessEntry &b) {
		return a.pid < b.pid;
	});

	for (const auto &entry : entries) {
		char label[MAX_PATH + 32];
		snprintf(label, sizeof(label), "%s (PID %lu)", entry.exe.c_str(),
			 (unsigned long)entry.pid);

		char value[MAX_PATH + 32];
		snprintf(value, sizeof(value), "%s|%lu", entry.exe.c_str(),
			 (unsigned long)entry.pid);

		obs_property_list_add_string(list, label, value);
	}
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	DWORD saved_pid = data ? ((ProcessAudioCapture *)data)->GetSavedPid() : 0;

	obs_property_t *list = obs_properties_get(props, OPT_PROCESS);
	obs_property_list_clear(list);
	fill_process_list(list, saved_pid);
	return true;
}

static obs_properties_t *get_properties(void *data)
{
	DWORD saved_pid = data ? ((ProcessAudioCapture *)data)->GetSavedPid() : 0;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, OPT_PROCESS,
						       obs_module_text("Process"),
						       OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	fill_process_list(list, saved_pid);

	obs_properties_add_button(props, "refresh", obs_module_text("RefreshProcessList"),
				  refresh_clicked);

	return props;
}

static const char *get_name(void *)
{
	return obs_module_text("ProcessAudioCapture");
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	try {
		return new ProcessAudioCapture(settings, source);
	} catch (...) {
		return nullptr;
	}
}

static void source_destroy(void *data)
{
	delete (ProcessAudioCapture *)data;
}

static void source_update(void *data, obs_data_t *settings)
{
	((ProcessAudioCapture *)data)->Update(settings);
}

static void source_activate(void *data)
{
	((ProcessAudioCapture *)data)->Activate();
}

static void source_deactivate(void *data)
{
	((ProcessAudioCapture *)data)->Deactivate();
}

void RegisterProcessAudioCapture()
{
	obs_source_info info = {};
	info.id = "wasapi_process_capture_v2";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.get_name = get_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.activate = source_activate;
	info.deactivate = source_deactivate;
	info.get_properties = get_properties;
	info.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT;

	obs_register_source(&info);
}
