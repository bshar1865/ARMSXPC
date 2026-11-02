#pragma once

#include <atomic>
#include <cstdint>

namespace remote_play_input
{
	inline std::atomic<bool> active{false};
	inline std::atomic<std::uint32_t> buttons{0};
	inline std::atomic<std::uint8_t> lx{128}, ly{128}, rx{128}, ry{128}, l2{0}, r2{0};
	inline std::atomic<std::uint8_t> rumble_large{0}, rumble_small{0};
	inline void set(std::uint32_t b, std::uint8_t x, std::uint8_t y, std::uint8_t ax, std::uint8_t ay, std::uint8_t lt, std::uint8_t rt)
	{
		buttons.store(b, std::memory_order_relaxed); lx.store(x, std::memory_order_relaxed); ly.store(y, std::memory_order_relaxed);
		rx.store(ax, std::memory_order_relaxed); ry.store(ay, std::memory_order_relaxed); l2.store(lt, std::memory_order_relaxed); r2.store(rt, std::memory_order_relaxed);
		active.store(true, std::memory_order_release);
	}
	inline void clear() { active.store(false, std::memory_order_release); buttons.store(0, std::memory_order_relaxed); rumble_large.store(0, std::memory_order_relaxed); rumble_small.store(0, std::memory_order_relaxed); }
}
