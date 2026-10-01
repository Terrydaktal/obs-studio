#pragma once

#include <memory>

// Give recordings a larger share of contested CPU time without changing the
// encoder. The Linux implementation uses an unprivileged, process-local scope.
class RecordingCpuPriority {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	RecordingCpuPriority();
	~RecordingCpuPriority();

	// Keep enabled until the output has finished draining, not just until Stop.
	void SetRecording(bool active);
};
