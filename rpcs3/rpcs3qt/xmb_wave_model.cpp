#include "stdafx.h"
#include "xmb_wave_model.h"
#include "xmb_firmware.h"

#include <QFile>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
	constexpr float pi = 3.14159265358979323846f;
	constexpr int lx = 11, ly = 7, lz = 7;
	float clamp01(float x) { return std::clamp(x, 0.f, 1.f); }
	float noise(quint32 n0)
	{
		const quint32 n = (n0 << 13) ^ n0;
		const quint32 t = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
		return 1.f - static_cast<float>(t) / 1073741824.f;
	}
}

xmb_wave_model::xmb_wave_model(xmb_firmware& fw)
{
	auto f = [&](const char* name, float d) { return fw.wave_value(QString::fromLatin1(name), d); };
	m_line.damping = f("DAMPING", m_line.damping); m_line.length = f("LENGTH", m_line.length);
	m_line.tension = f("TENSION", m_line.tension); m_line.timestep = f("TIMESTEP", m_line.timestep);
	m_line.perturbation = f("PERTURBATION", m_line.perturbation);
	m_line.pos = {f("POS X", m_line.pos[0]), f("POS Y", m_line.pos[1]), f("POS Z", m_line.pos[2])};
	m_line.axis = {f("ANG X", m_line.axis[0]), f("ANG Y", m_line.axis[1]), f("ANG Z", m_line.axis[2])};
	m_line.rotation = f("ANG ROT", m_line.rotation);
	m_line.end = {f("END X", m_line.end[0]), f("END Y", m_line.end[1]), f("END Z", m_line.end[2])};
	m_line.scale1 = {f("FFD SCALE1 X", m_line.scale1[0]), f("FFD SCALE1 Y", m_line.scale1[1]), f("FFD SCALE1 Z", m_line.scale1[2])};
	m_line.scale2 = {f("FFD SCALE2 X", m_line.scale2[0]), f("FFD SCALE2 Y", m_line.scale2[1]), f("FFD SCALE2 Z", m_line.scale2[2])};
	m_line.offset = {f("FFD OFFSET X", m_line.offset[0]), f("FFD OFFSET Y", m_line.offset[1]), f("FFD OFFSET Z", m_line.offset[2])};
	m_line.param1 = f("FFD PARAM 1", m_line.param1);

	m_position.fill(0.f, count * 3); m_velocity.fill(0.f, count * 3); m_previous.fill(0.f, count * 3);
	QFile state(QStringLiteral(":/xmb/wave_state.bin"));
	if (state.open(QIODevice::ReadOnly) && state.size() >= count * 6 * qsizetype(sizeof(float)))
	{
		const QByteArray bytes = state.read(count * 6 * qsizetype(sizeof(float)));
		std::memcpy(m_position.data(), bytes.constData(), count * 3 * sizeof(float));
		std::memcpy(m_velocity.data(), bytes.constData() + count * 3 * sizeof(float), count * 3 * sizeof(float));
	}
	else
	{
		// Port of XmbWaveModel's own initial-state path: straight lines from the ring, then
		// 12,000 spring steps. This is ARMSX3 behavior for a missing bundled state asset.
		m_counter = 0;
		for (int r = 0; r < side; ++r) for (int c = 0; c < side; ++c)
		{
			const int i = (r * side + c) * 3;
			const float phase = static_cast<float>(r) / side;
			m_position[i] = (side - 1 - c) * .33f;
			m_position[i + 1] = (std::sin(11.f * phase) * .5f + .5f) * m_line.end[1];
			m_position[i + 2] = (std::cos(15.f * phase) + 1.f) * .5f * m_line.end[2];
		}
		for (int i = 0; i < 12000; ++i) step();
	}
	m_previous = m_position;
	m_counter = static_cast<quint32>(std::chrono::steady_clock::now().time_since_epoch().count()) & 0xffffff;
	m_lattice_time = m_clock * 10.f;
	float ax = m_line.axis[0], ay = m_line.axis[1], az = m_line.axis[2];
	const float n = std::sqrt(ax * ax + ay * ay + az * az); ax /= n; ay /= n; az /= n;
	const float a = m_line.rotation * pi / 180.f, c = std::cos(a), s = std::sin(a), ic = 1.f - c;
	m_rotation = {c + ax * ax * ic, ax * ay * ic - az * s, ax * az * ic + ay * s,
		ay * ax * ic + az * s, c + ay * ay * ic, ay * az * ic - ax * s,
		az * ax * ic - ay * s, az * ay * ic + ax * s, c + az * az * ic};
}

void xmb_wave_model::spring(int a, int b, float rest, float strength)
{
	const int ia = a * 3, ib = b * 3;
	const float dx = m_position[ib] - m_position[ia], dy = m_position[ib + 1] - m_position[ia + 1], dz = m_position[ib + 2] - m_position[ia + 2];
	const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (len < 1e-12f) return;
	const float force = (len - rest) * strength / len;
	for (int k = 0; k < 3; ++k) { const float d = (k == 0 ? dx : k == 1 ? dy : dz) * force; m_velocity[ia + k] += d; m_velocity[ib + k] -= d; }
}

void xmb_wave_model::step()
{
	m_previous = m_position;
	const float ts = m_line.timestep;
	m_clock += ts * 1e-4f; if (m_clock > 10.f) m_clock = 0.f;
	for (int r = 0; r < side; ++r) for (int c = 0; c < side; ++c)
	{
		const int i = r * side + c;
		if (c > 0) spring(i, i - 1, m_line.length, m_line.tension);
		if (c > 1) spring(i, i - 2, 2.f * m_line.length, m_line.tension * 10.f);
		if (r > 0) spring(i, i - side, m_line.length, m_line.tension);
		if (r > 1) spring(i, i - 2 * side, 2.f * m_line.length, m_line.tension * 10.f);
		for (int k = 0; k < 3; ++k) m_velocity[i * 3 + k] += noise(++m_counter) * m_line.perturbation;
	}
	const float dt = ts * 1e-4f, keep = 1.f - m_line.damping * ts;
	for (int j = 0; j < count * 3; ++j) { m_position[j] += m_velocity[j] * dt; m_velocity[j] *= keep; }
	m_smooth += (m_clock - m_smooth) * .1f;
	for (int r = 0; r < side; ++r)
	{
		m_velocity[r * side * 3] += ts * .02f;
		const float phase = static_cast<float>(r) / side + m_smooth;
		const int i = (r * side + side - 1) * 3;
		m_position[i] = 0.f; m_position[i + 1] = (std::sin(phase * 11.f) * .5f + .5f) * m_line.end[1];
		m_position[i + 2] = (std::cos(phase * 15.f) + 1.f) * .5f * m_line.end[2];
		m_velocity[i] = m_velocity[i + 1] = m_velocity[i + 2] = 0.f;
	}
}

void xmb_wave_model::fill_lattice(float t, float push)
{
	const float c = (std::sin(.1f * t) + .2f) / 1.2f;
	const float amp = (std::sin(2.f * t) + 3.f) * std::exp(-1e-4f * t) * (1.f + push * .6f), bob = std::sin(.25f * t);
	m_lattice.resize(lx * ly * lz * 2);
	for (int k = 0; k < lz; ++k) for (int j = 0; j < ly; ++j) for (int i = 0; i < lx; ++i)
	{
		const float u = static_cast<float>(std::clamp(i - 1, 0, 7)) / 8.f, w = static_cast<float>(std::clamp(k - 1, 0, 3)) / 4.f;
		const float dx = .2f * u + 1.3f * u * u - .15f;
		const float dy = .24f * (std::tanh((u - .5f) / 2.f) + 1.f) * amp * std::sin(7.85f * u - 2.5f * t - 1.25f)
			+ bob + std::exp(-50.f * (u - c) * (u - c)) + .15f * std::sin(t - 2.f * pi * w);
		const int o = ((k * ly + j) * lx + i) * 2; m_lattice[o] = 6.4f * dx; m_lattice[o + 1] = dy;
	}
}

void xmb_wave_model::update(float dt_seconds, float aspect)
{
	m_accumulator += std::clamp(dt_seconds, 0.f, .25f) * 60.f;
	int steps = 0; while (m_accumulator > 1.f && steps < 8) { step(); m_accumulator -= 1.f; ++steps; }
	if (m_accumulator > 1.f) m_accumulator = 1.f;
	// Match ARMSX3's music-driven firmware wave: ease its billow over 1/3 second, advance
	// the lattice continuously at any display refresh rate, and keep its framing upright.
	const float dt = std::clamp(dt_seconds, 0.f, .25f);
	m_billow += (m_drive - m_billow) * std::min(1.f, dt * 3.f);
	m_lattice_time += dt * 60.f * m_line.timestep * 1e-3f * (1.f + m_billow * 1.5f);
	if (m_lattice_time > 100.f) m_lattice_time = 0.f;
	fill_lattice(m_lattice_time, m_billow);
	std::array<float, 4> bx{}, by{}, bz{};
	auto weights = [](float t, std::array<float, 4>& o)
	{
		const float t2 = t * t, t3 = t2 * t, m = 1.f - t;
		o = {m * m * m / 6.f, (3.f * t3 - 6.f * t2 + 4.f) / 6.f, (-3.f * t3 + 3.f * t2 + 3.f * t + 1.f) / 6.f, t3 / 6.f};
	};
	const float sx = 2.0057f / std::max(aspect, 16.f / 9.f), sy = sx * aspect;
	for (int i = 0; i < count; ++i)
	{
		const float px = m_previous[i * 3] + (m_position[i * 3] - m_previous[i * 3]) * m_accumulator;
		float py = m_previous[i * 3 + 1] + (m_position[i * 3 + 1] - m_previous[i * 3 + 1]) * m_accumulator;
		float pz = m_previous[i * 3 + 2] + (m_position[i * 3 + 2] - m_previous[i * 3 + 2]) * m_accumulator;
		const float d = px - m_line.param1;
		const float mm = clamp01(d / 5.f);
		const float scale = d < 0.f ? .3f : 1.3f - std::cos(d * pi / 2.f) * (1.f - mm) * (1.f - mm) * (1.f + 2.f * mm);
		py *= scale; pz *= scale;
		const float qx = (px - m_line.offset[0]) / m_line.scale1[0], qy = (py - m_line.offset[1]) / m_line.scale1[1], qz = (pz - m_line.offset[2]) / m_line.scale1[2];
		const float sxl = clamp01(qx) * 8.f, syl = clamp01(qy) * 4.f, szl = clamp01(qz) * 4.f;
		const int cx = std::min(7, static_cast<int>(sxl)), cy = std::min(3, static_cast<int>(syl)), cz = std::min(3, static_cast<int>(szl));
		weights(sxl - cx, bx); weights(syl - cy, by); weights(szl - cz, bz);
		float ddx = 0.f, ddy = 0.f;
		for (int k = 0; k < 4; ++k) for (int j = 0; j < 4; ++j)
		{
			const float wzy = bz[k] * by[j]; const int row = (((cz + k) * ly + cy + j) * lx + cx) * 2;
			for (int a = 0; a < 4; ++a) { const float weight = wzy * bx[a]; ddx += weight * m_lattice[row + a * 2]; ddy += weight * m_lattice[row + a * 2 + 1]; }
		}
		const float ox = (qx + ddx) * m_line.scale2[0], oy = (qy + ddy) * m_line.scale2[1], oz = qz * m_line.scale2[2];
		const float wx = m_rotation[0] * ox + m_rotation[1] * oy + m_rotation[2] * oz + m_line.pos[0] - .39f;
		const float wy = m_rotation[3] * ox + m_rotation[4] * oy + m_rotation[5] * oz + m_line.pos[1] - .63f;
		const float wz = m_rotation[6] * ox + m_rotation[7] * oy + m_rotation[8] * oz + m_line.pos[2] - 1.87f;
		const float w = -wz;
		m_points[i] = QVector4D(sx * wx, sy * wy, 1.0002f * w - .2f, w);
	}
}
