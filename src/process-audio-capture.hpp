#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <wrl/implements.h>

#include <obs-module.h>
#include <util/windows/ComPtr.hpp>
#include <util/windows/WinHandle.hpp>

#include <algorithm>
#include <string>
#include <vector>

typedef HRESULT(STDAPICALLTYPE *PFN_ActivateAudioInterfaceAsync)(
	LPCWSTR, REFIID, PROPVARIANT *,
	IActivateAudioInterfaceCompletionHandler *,
	IActivateAudioInterfaceAsyncOperation **);

/* COM completion handler for async process loopback activation */
class ActivationCompletionHandler
	: public Microsoft::WRL::RuntimeClass<
		  Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
		  Microsoft::WRL::FtmBase,
		  IActivateAudioInterfaceCompletionHandler> {

	IUnknown *unknown = nullptr;
	HRESULT activationResult = E_FAIL;
	WinHandle activationSignal;

public:
	ActivationCompletionHandler();
	HRESULT GetActivateResult(IAudioClient **client);

private:
	HRESULT STDMETHODCALLTYPE
	ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override final;
};

class ProcessAudioCapture {
public:
	ProcessAudioCapture(obs_data_t *settings, obs_source_t *source);
	~ProcessAudioCapture();

	void Update(obs_data_t *settings);
	void Activate();
	void Deactivate();
	DWORD GetSavedPid() const { return preferred_pid; }

private:
	obs_source_t *source = nullptr;

	std::string executable;
	DWORD preferred_pid = 0;

	ComPtr<IAudioClient> client;
	ComPtr<IAudioCaptureClient> capture;
	WinHandle captureEvent;
	WinHandle processHandle;

	WinHandle backgroundThread;
	WinHandle stopSignal;

	speaker_layout speakers = SPEAKERS_STEREO;
	audio_format format = AUDIO_FORMAT_FLOAT;
	uint32_t sampleRate = 48000;

	std::vector<uint8_t> silence;
	bool previouslyFailed = false;

	PFN_ActivateAudioInterfaceAsync activate_audio_interface_async = nullptr;

	void ParseSettings(obs_data_t *settings);
	DWORD FindTargetProcess();
	bool IsTargetAlive();
	bool TryInitialize();
	bool ProcessCaptureData();
	void StopCapture();

	static DWORD WINAPI BackgroundThread(LPVOID param);
};

void RegisterProcessAudioCapture();
