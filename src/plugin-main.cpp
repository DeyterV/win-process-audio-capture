#include <obs-module.h>
#include <util/windows/win-version.h>

#include "process-audio-capture.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-process-audio-capture", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Windows process audio capture source (selects by process, not window)";
}

bool obs_module_load(void)
{
	struct win_version_info ver;
	get_win_ver(&ver);

	struct win_version_info minimum = {};
	minimum.major = 10;
	minimum.minor = 0;
	minimum.build = 19041; /* Windows 10 2004 — earliest build where process loopback works */

	if (win_version_compare(&ver, &minimum) < 0) {
		blog(LOG_WARNING,
		     "[win-process-audio-capture] Process loopback requires Windows 10 2004 "
		     "(build 19041) or later — source not registered");
		return true;
	}

	RegisterProcessAudioCapture();
	return true;
}

void obs_module_unload(void) {}
