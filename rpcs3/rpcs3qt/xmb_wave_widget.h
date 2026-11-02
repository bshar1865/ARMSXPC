#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>

class QTimer;
class xmb_firmware;
class xmb_wave_model;

// The Android XmbFirmwareWave GLES renderer, on the desktop GL context. Its spring grid and
// textures are supplied by the same xmb_wave_model and installed PS3 firmware files.
class xmb_wave_widget final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
	Q_OBJECT
public:
	explicit xmb_wave_widget(xmb_wave_model& model, xmb_firmware& firmware, QWidget* parent = nullptr);

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;

private:
	xmb_wave_model& m_model;
	xmb_firmware& m_firmware;
	QTimer* m_tick = nullptr;
	GLuint m_line_program = 0, m_comp_program = 0;
	GLuint m_grid_vao = 0, m_quad_vao = 0, m_ctrl_texture = 0, m_strand_texture = 0;
	GLuint m_fbo = 0, m_fbo_texture = 0;
	int m_fbo_width = 0, m_fbo_height = 0, m_index_count = 0;
	bool m_ready = false;
	GLuint compile_program(const char* vertex, const char* fragment);
	GLuint compile_shader(GLenum type, const char* source);
	void create_grid();
	void create_quad();
	void ensure_target(int width, int height);
	void draw_firmware_wave(int width, int height);
};
