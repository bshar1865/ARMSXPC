#include "stdafx.h"
#include "remote_play_capture.h"

#include "Emu/IdManager.h"
#include "Emu/System.h"
#include "util/video_provider.h"

extern atomic_t<recording_mode> g_recording_mode;

LOG_CHANNEL(remote_play_log, "RemotePlay");

namespace utils
{
	std::mutex remote_play_capture_sink::s_mutex;
	std::shared_ptr<remote_play_capture_sink> remote_play_capture_sink::s_active;

	remote_play_capture_sink::remote_play_capture_sink()
	{
		use_internal_video = true;
		use_internal_audio = true;
		accept_all_samples = true;
		drop_old_video_frames = true;
		max_audio_queue_ms = 250;
		m_framerate = 60;
		m_sample_rate = 48000;
	}

	std::shared_ptr<remote_play_capture_sink> remote_play_capture_sink::start(std::string& error)
	{
		std::lock_guard lock(s_mutex);
		if (s_active && !s_active->stopped())
			return s_active;

		if (Emu.IsStopped() || !g_fxo->is_init<video_provider>())
		{
			error = "Start a game before starting Remote Play.";
			return {};
		}

		if (g_recording_mode != recording_mode::stopped)
		{
			error = "The game's recording hook is already in use.";
			return {};
		}

		auto sink = std::shared_ptr<remote_play_capture_sink>(new remote_play_capture_sink());
		if (!g_fxo->get<video_provider>().set_video_sink(sink, recording_mode::rpcs3))
		{
			error = "Could not attach Remote Play to the game's capture hook.";
			return {};
		}

		g_recording_mode = recording_mode::rpcs3;
		s_active = sink;
		remote_play_log.notice("desktop capture started");
		return sink;
	}

	void remote_play_capture_sink::stop_capture()
	{
		std::shared_ptr<remote_play_capture_sink> sink;
		{
			std::lock_guard lock(s_mutex);
			sink.swap(s_active);
		}

		if (!sink)
			return;

		sink->m_stopped = true;
		if (!Emu.IsStopped() && g_fxo->is_init<video_provider>())
		{
			g_recording_mode = recording_mode::stopped;
			g_fxo->get<video_provider>().set_video_sink(nullptr, recording_mode::rpcs3);
		}
		remote_play_log.notice("desktop capture stopped");
	}

	std::shared_ptr<remote_play_capture_sink> remote_play_capture_sink::current()
	{
		std::lock_guard lock(s_mutex);
		return s_active;
	}

	void remote_play_capture_sink::stop(bool)
	{
		m_stopped = true;
	}

	void remote_play_capture_sink::pause(bool)
	{
		m_paused = true;
	}

	void remote_play_capture_sink::resume()
	{
		m_paused = false;
	}

	bool remote_play_capture_sink::take_latest_frame(encoder_frame& frame)
	{
		std::lock_guard lock(m_video_mtx);
		if (m_frames_to_encode.empty())
			return false;

		frame = std::move(m_frames_to_encode.back());
		m_frames_to_encode.clear();
		return true;
	}

	bool remote_play_capture_sink::take_audio(std::deque<encoder_sample>& samples)
	{
		std::lock_guard lock(m_audio_mtx);
		if (m_samples_to_encode.empty())
			return false;

		samples.swap(m_samples_to_encode);
		return true;
	}
}
