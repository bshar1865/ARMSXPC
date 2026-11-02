#pragma once

#include <QVector>
#include <QVector4D>

#include <array>

class xmb_firmware;

// CPU port of ARMSX3's XmbWaveModel. Its inputs are the installed PS3 firmware's LINE1.mnu
// values and the app's deterministic starting state; rendering consumes its clip-space grid.
class xmb_wave_model final
{
public:
	explicit xmb_wave_model(xmb_firmware& firmware);
	void update(float dt_seconds, float aspect);
	const QVector<QVector4D>& points() const { return m_points; }

private:
	struct line_parameters
	{
		float damping = 0.0001f, length = 0.306001f, tension = 0.25f;
		float timestep = 4.f, perturbation = 0.0998587f;
		std::array<float, 3> pos{-7.67933f, -1.08844f, -6.40287f};
		std::array<float, 3> axis{-0.994157f, 0.0867576f, 0.065364f};
		float rotation = 18.1208f;
		std::array<float, 3> end{3.f, 0.4f, 0.2f};
		std::array<float, 3> scale1{5.67726f, 1.00077f, 1.f};
		std::array<float, 3> scale2{2.82755f, 1.27579f, 2.88782f};
		std::array<float, 3> offset{0.f, -0.469999f, 0.f};
		float param1 = -1.33509f;
	};
	static constexpr int side = 19;
	static constexpr int count = side * side;
	line_parameters m_line;
	QVector<float> m_position, m_velocity, m_previous, m_lattice;
	QVector<QVector4D> m_points = QVector<QVector4D>(count);
	std::array<float, 9> m_rotation{};
	float m_clock = 0.f, m_smooth = 0.f, m_accumulator = 0.f;
	quint32 m_counter = 0x5eed;
	void step();
	void spring(int a, int b, float rest, float strength);
	void fill_lattice(float time);
};
