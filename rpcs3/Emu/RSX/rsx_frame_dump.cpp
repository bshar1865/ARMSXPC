#include "stdafx.h"
#include "rsx_frame_dump.h"

#include "Emu/system_config.h"

namespace rsx::frame_dump
{
	static atomic_t<u32> g_request{0};

	bool request(dump_mode mode)
	{
		if (mode == dump_mode::none ||
			(g_cfg.video.renderer != video_renderer::vulkan && g_cfg.video.renderer != video_renderer::opengl))
		{
			return false;
		}

		g_request = static_cast<u32>(mode);
		return true;
	}

	dump_mode take_request()
	{
		return static_cast<dump_mode>(g_request.exchange(0));
	}
}
