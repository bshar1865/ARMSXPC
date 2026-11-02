#pragma once

#include <QDialog>
#include <QByteArray>

#include <memory>
#include <atomic>
#include <thread>
#include <functional>

class QLabel;
class QTimer;
class QPlainTextEdit;
class QPushButton;
class QAudioSink;
class QIODevice;
class QKeyEvent;
namespace utils { class remote_play_capture_sink; }
class remote_play_webrtc;

// Code-based ARMSX3 offer/answer flow. No host address or locally invented PIN is used.
class remote_play_capture_dialog final : public QDialog
{
	Q_OBJECT
public:
	explicit remote_play_capture_dialog(QWidget* parent = nullptr);
	~remote_play_capture_dialog() override;

protected:
	void keyPressEvent(QKeyEvent*) override;
	void keyReleaseEvent(QKeyEvent*) override;

private:
	enum class mode { idle, host, viewer };
	void start_host();
	void start_viewer();
	void start_viewer_with_offer(const QString& offer_code);
	void publish_host_offer(const QString& offer_code);
	void launch_lobby_task(std::function<void()> task);
	void apply_answer();
	void stop_session();
	void refresh_capture();
	void poll_gamepad();
	void publish_pad();
	void receive_packet(const QByteArray& packet);
	void append_audio(const QByteArray& pcm);
	void set_status(const QString& text);

	std::shared_ptr<utils::remote_play_capture_sink> m_sink;
	std::unique_ptr<remote_play_webrtc> m_webrtc;
	QLabel* m_preview = nullptr;
	QLabel* m_status = nullptr;
	QPlainTextEdit* m_code_input = nullptr;
	QPlainTextEdit* m_code_output = nullptr;
	QPushButton* m_host_button = nullptr;
	QPushButton* m_viewer_button = nullptr;
	QPushButton* m_answer_button = nullptr;
	QPushButton* m_copy_button = nullptr;
	QPushButton* m_stop_button = nullptr;
	QTimer* m_timer = nullptr;
	QAudioSink* m_audio_sink = nullptr;
	QIODevice* m_audio_device = nullptr;
	QByteArray m_audio_pending;
	std::thread m_lobby_thread;
	std::atomic_bool m_lobby_cancel{false};
	QString m_lobby_code;
	mode m_mode = mode::idle;
	quint32 m_buttons = 0;
	quint32 m_gamepad_buttons = 0;
	quint8 m_lx = 128, m_ly = 128, m_rx = 128, m_ry = 128, m_l2 = 0, m_r2 = 0;
	quint8 m_gamepad_lx = 128, m_gamepad_ly = 128, m_gamepad_rx = 128, m_gamepad_ry = 128, m_gamepad_l2 = 0, m_gamepad_r2 = 0;
	void* m_xinput_module = nullptr;
	void* m_xinput_get_state = nullptr;
	void* m_xinput_set_state = nullptr;
	quint32 m_sequence = 0;
	quint32 m_rumble_sequence = 0;
	quint64 m_waiting_ticks = 0;
	quint64 m_rumble_ticks = 0;
	bool m_approval_prompted = false;
};
