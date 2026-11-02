#include "stdafx.h"
#include "xmb_wave_widget.h"
#include "xmb_firmware.h"
#include "xmb_wave_model.h"

#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVector4D>

#include <algorithm>
#include <cmath>

namespace
{
	constexpr int grid_side = 128;
	constexpr int control_side = 19;
	constexpr float wave_gain = 1.6f;

	constexpr const char* line_vertex = R"GLSL(#version 330 core
layout(location=0) in vec2 aRC;
uniform sampler2D uCtrl;
uniform float uFresnel;
uniform float uBrightness;
uniform float uMipBias;
out vec2 vTc0;
out vec4 vTc1;
void basis(float t, out vec4 b, out vec4 d) {
  float t2=t*t, t3=t2*t, m=1.0-t;
  b=vec4(m*m*m,3.0*t3-6.0*t2+4.0,-3.0*t3+3.0*t2+3.0*t+1.0,t3)/6.0;
  d=vec4(-3.0*m*m,9.0*t2-12.0*t,-9.0*t2+6.0*t+3.0,3.0*t2)/6.0;
}
void main() {
  int r=int(aRC.x+0.5), c=int(aRC.y+0.5), pr=r/8, pc=c/8;
  vec4 br,dr,bc,dc;
  basis(float(r-pr*8)*16.0/127.0,br,dr); basis(float(c-pc*8)*16.0/127.0,bc,dc);
  vec4 P=vec4(0.0), Pr=vec4(0.0), Pc=vec4(0.0);
  for(int a=0;a<4;a++) {
    vec4 rowP=vec4(0.0), rowD=vec4(0.0);
    for(int b=0;b<4;b++) { vec4 C=texelFetch(uCtrl,ivec2(pc+b,pr+a),0); rowP+=bc[b]*C; rowD+=dc[b]*C; }
    P+=br[a]*rowP; Pr+=dr[a]*rowP; Pc+=br[a]*rowD;
  }
  vec3 N=cross(Pr.xyz,Pc.xyz); float lp=max(length(P.xyz),1e-6); vec3 v=P.xyz/lp;
  float w=abs(dot(v,N))/lp*uMipBias;
  float vn=abs(dot(v,N/max(length(N),1e-20)));
  float fr=min(1.0,10.0*min(aRC.x,127.0-aRC.x)/127.0);
  float fc=min(1.0,10.0*min(aRC.y,127.0-aRC.y)/127.0);
  vTc0=vec2(aRC.x/127.0,w); vTc1=vec4(uFresnel*w,uBrightness*w,vn*w,fr*fc);
  gl_Position=P;
})GLSL";

	constexpr const char* line_fragment = R"GLSL(#version 330 core
in vec2 vTc0; in vec4 vTc1; uniform sampler2D uStrands; out vec4 oColor;
void main() {
  float y=clamp(vTc0.y*4.0-0.5,0.0,3.0);
  float s=(y<1.0?mix(127.0,5.0,y):(y<2.0?mix(5.0,0.0,y-1.0):0.0))/255.0;
  float strand=texture(uStrands,vec2(vTc1.z,0.5)).r;
  float light=(vTc1.y*s+strand*vTc1.x)*vTc1.w;
  oColor=vec4(clamp(light,0.0,1.0),0.0,0.0,0.0);
})GLSL";

	constexpr const char* quad_vertex = R"GLSL(#version 330 core
layout(location=0) in vec2 aPos; out vec2 vUv;
void main(){vUv=aPos*0.5+0.5;gl_Position=vec4(aPos,0.0,1.0);}
)GLSL";

	constexpr const char* composite_fragment = R"GLSL(#version 330 core
in vec2 vUv; uniform sampler2D uLines; uniform vec3 uCol3; uniform vec3 uCol4; uniform float uGain; out vec4 oColor;
void main(){float light=min(texture(uLines,vUv).r,0.53);oColor=vec4(mix(uCol3,uCol4,vUv.x)*light*uGain,0.0);}
)GLSL";
}

xmb_wave_widget::xmb_wave_widget(xmb_wave_model& model, xmb_firmware& firmware, QWidget* parent)
	: QOpenGLWidget(parent), m_model(model), m_firmware(firmware)
{
	QSurfaceFormat format; format.setRenderableType(QSurfaceFormat::OpenGL); format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile); format.setAlphaBufferSize(8);
	setFormat(format);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_AlwaysStackOnTop);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAutoFillBackground(false);
	m_tick = new QTimer(this); m_tick->setInterval(33);
	connect(m_tick, &QTimer::timeout, this, [this] { update(); });
	m_tick->start();
}

GLuint xmb_wave_widget::compile_shader(GLenum type, const char* source)
{
	const GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
	GLint ok = GL_FALSE; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE)
	{
		GLint length = 0; glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		QByteArray log(std::max(1, length), Qt::Uninitialized); glGetShaderInfoLog(shader, length, nullptr, log.data());
		qWarning("ARMSX3 XMB wave shader compile failed: %s", log.constData()); glDeleteShader(shader); return 0;
	}
	return shader;
}

GLuint xmb_wave_widget::compile_program(const char* vertex, const char* fragment)
{
	const GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex), fs = compile_shader(GL_FRAGMENT_SHADER, fragment);
	if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
	const GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
	glDeleteShader(vs); glDeleteShader(fs);
	GLint ok = GL_FALSE; glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (ok == GL_FALSE)
	{
		GLint length = 0; glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
		QByteArray log(std::max(1, length), Qt::Uninitialized); glGetProgramInfoLog(program, length, nullptr, log.data());
		qWarning("ARMSX3 XMB wave shader link failed: %s", log.constData()); glDeleteProgram(program); return 0;
	}
	return program;
}

void xmb_wave_widget::create_grid()
{
	std::vector<float> vertices(grid_side * grid_side * 2);
	for (int r = 0; r < grid_side; ++r) for (int c = 0; c < grid_side; ++c)
	{
		const qsizetype at = (r * grid_side + c) * 2; vertices[at] = static_cast<float>(r); vertices[at + 1] = static_cast<float>(c);
	}
	std::vector<quint16> indices((grid_side - 1) * (grid_side * 2 + 1)); qsizetype n = 0;
	for (int r = 0; r < grid_side - 1; ++r)
	{
		for (int c = 0; c < grid_side; ++c) { indices[n++] = static_cast<quint16>(r * grid_side + c); indices[n++] = static_cast<quint16>((r + 1) * grid_side + c); }
		indices[n++] = 0xffff;
	}
	m_index_count = static_cast<int>(n);
	glGenVertexArrays(1, &m_grid_vao); glBindVertexArray(m_grid_vao);
	GLuint buffers[2]{}; glGenBuffers(2, buffers);
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]); glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]); glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(quint16)), indices.data(), GL_STATIC_DRAW);
	glBindVertexArray(0); glDeleteBuffers(2, buffers);
}

void xmb_wave_widget::create_quad()
{
	constexpr float vertices[]{-1.f,-1.f, 1.f,-1.f, -1.f,1.f, 1.f,1.f};
	glGenVertexArrays(1, &m_quad_vao); glBindVertexArray(m_quad_vao);
	GLuint buffer = 0; glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
	glBindVertexArray(0); glDeleteBuffers(1, &buffer);
}

void xmb_wave_widget::initializeGL()
{
	initializeOpenGLFunctions();
	m_line_program = compile_program(line_vertex, line_fragment);
	m_comp_program = compile_program(quad_vertex, composite_fragment);
	if (!m_line_program || !m_comp_program) return;
	create_grid(); create_quad();
	glGenTextures(1, &m_ctrl_texture); glBindTexture(GL_TEXTURE_2D, m_ctrl_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, control_side, control_side, 0, GL_RGBA, GL_FLOAT, nullptr);
	glGenTextures(1, &m_strand_texture); glBindTexture(GL_TEXTURE_2D, m_strand_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	QVector<float> strands = m_firmware.wave_strands();
	if (strands.size() != 512)
	{
		// Same 512-texel fallback as XmbWaveModel.fallbackStrands on Android.
		strands.resize(512);
		for (int i = 0; i < strands.size(); ++i)
		{
			const float x = static_cast<float>(i);
			strands[i] = x < 29.f ? .988f * std::exp(-std::pow((x - 29.f) / 7.8f, 2.f)) : .988f * std::exp(-(x - 29.f) / 21.5f);
		}
	}
	QVector<quint8> bytes(strands.size());
	for (int i = 0; i < strands.size(); ++i) bytes[i] = static_cast<quint8>(std::clamp(static_cast<int>(strands[i] * 255.f + .5f), 0, 255));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, bytes.size(), 1, 0, GL_RED, GL_UNSIGNED_BYTE, bytes.constData());
	glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	ensure_target(std::max(1, width()), std::max(1, height()));
	m_ready = true;
}

void xmb_wave_widget::resizeGL(int width, int height)
{
	const qreal dpr = devicePixelRatioF();
	ensure_target(std::max(1, qRound(width * dpr)), std::max(1, qRound(height * dpr)));
}

void xmb_wave_widget::ensure_target(int width, int height)
{
	if (m_fbo && width == m_fbo_width && height == m_fbo_height) return;
	if (m_fbo) { glDeleteFramebuffers(1, &m_fbo); glDeleteTextures(1, &m_fbo_texture); }
	m_fbo_width = width; m_fbo_height = height;
	glGenTextures(1, &m_fbo_texture); glBindTexture(GL_TEXTURE_2D, m_fbo_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
	glGenFramebuffers(1, &m_fbo); glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fbo_texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		glBindTexture(GL_TEXTURE_2D, m_fbo_texture); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fbo_texture, 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
}

void xmb_wave_widget::draw_firmware_wave(int width, int height)
{
	m_model.update(1.f / 30.f, static_cast<float>(width) / std::max(1, height));
	std::array<float, control_side * control_side * 4> controls{};
	const auto& points = m_model.points();
	for (int i = 0; i < points.size(); ++i)
	{
		controls[i * 4] = points[i].x(); controls[i * 4 + 1] = points[i].y();
		controls[i * 4 + 2] = points[i].z(); controls[i * 4 + 3] = points[i].w();
	}
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_ctrl_texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, control_side, control_side, GL_RGBA, GL_FLOAT, controls.data());
	glBindFramebuffer(GL_FRAMEBUFFER, m_fbo); glViewport(0, 0, width, height); glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); glUseProgram(m_line_program);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_ctrl_texture); glUniform1i(glGetUniformLocation(m_line_program, "uCtrl"), 0);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_strand_texture); glUniform1i(glGetUniformLocation(m_line_program, "uStrands"), 1);
	glUniform1f(glGetUniformLocation(m_line_program, "uFresnel"), m_firmware.wave_value(QStringLiteral("FRESNEL"), .638971f));
	glUniform1f(glGetUniformLocation(m_line_program, "uBrightness"), m_firmware.wave_value(QStringLiteral("BRIGHTNESS"), .701917f));
	glUniform1f(glGetUniformLocation(m_line_program, "uMipBias"), m_firmware.wave_value(QStringLiteral("MIPMAP BIAS"), 1.86707f));
	glEnable(GL_PRIMITIVE_RESTART); glPrimitiveRestartIndex(0xffff); glBindVertexArray(m_grid_vao);
	glDrawElements(GL_TRIANGLE_STRIP, m_index_count, GL_UNSIGNED_SHORT, nullptr);
	glBindVertexArray(0); glDisable(GL_PRIMITIVE_RESTART);

	glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject()); glViewport(0, 0, width, height);
	// Match LinesController's compositor: add the already-lit firmware colour to the
	// background while preserving its alpha channel.
	glEnable(GL_BLEND); glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE); glUseProgram(m_comp_program);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_fbo_texture); glUniform1i(glGetUniformLocation(m_comp_program, "uLines"), 0);
	const QColor c3 = m_firmware.wave_colour(3, QColor(216, 216, 216)), c4 = m_firmware.wave_colour(4, QColor(236, 235, 237));
	glUniform3f(glGetUniformLocation(m_comp_program, "uCol3"), c3.redF(), c3.greenF(), c3.blueF());
	glUniform3f(glGetUniformLocation(m_comp_program, "uCol4"), c4.redF(), c4.greenF(), c4.blueF());
	glUniform1f(glGetUniformLocation(m_comp_program, "uGain"), wave_gain);
	glBindVertexArray(m_quad_vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glBindVertexArray(0);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void xmb_wave_widget::paintGL()
{
	const qreal dpr = devicePixelRatioF();
	const int width_px = std::max(1, qRound(width() * dpr)), height_px = std::max(1, qRound(height() * dpr));
	glViewport(0, 0, width_px, height_px); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
	if (!m_ready) return;
	ensure_target(width_px, height_px);
	draw_firmware_wave(width_px, height_px);
}
