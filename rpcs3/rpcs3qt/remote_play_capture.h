#pragma once

#include "util/video_sink.h"

#include <memory>
#include <mutex>
#include <string>

namespace utils
{
	// Desktop side of ARMSXPC Remote Play's capture path. The Android implementation owns a
	// Vulkan staging-buffer ring; this sink consumes the renderer-independent recording hook,
	// which provides CPU-readable frames on both Vulkan and OpenGL.
	class remote_play_capture_sink final : public video_sink
	{
	public:
		static std::shared_ptr<remote_play_capture_sink> start(std::string& error);
		static void stop_capture();
		static std::shared_ptr<remote_play_capture_sink> current();

		void stop(bool flush = true) override;
		void pause(bool flush = true) override;
		void resume() override;

		bool take_latest_frame(encoder_frame& frame);
		bool take_audio(std::deque<encoder_sample>& samples);
		bool stopped() const { return m_stopped; }

	private:
		remote_play_capture_sink();
		atomic_t<bool> m_stopped{false};
		static std::mutex s_mutex;
		static std::shared_ptr<remote_play_capture_sink> s_active;
	};
}
