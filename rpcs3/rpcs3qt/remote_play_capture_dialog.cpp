#include "stdafx.h"
#include "remote_play_capture_dialog.h"

#include "remote_play_capture.h"
#include "remote_play_protocol.h"
#include "remote_play_lobby.h"
#include "remote_play_webrtc.h"
#include "Input/remote_play_input.h"
#include "Emu/System.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMediaDevices>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QPointer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#endif

extern "C"
{
#include <libavutil/pixfmt.h>
}

remote_play_capture_dialog::remote_play_capture_dialog(QWidget* parent) : QDialog(parent)
{
	setWindowTitle(tr("ARMSX3 Remote Play — Connection Codes"));
	resize(1024, 780);
	auto* root = new QVBoxLayout(this);
	m_status = new QLabel(tr("Share the ARMSX3 offer code with your friend. They return an answer code; no host IP or custom PIN is involved."), this);
	m_status->setWordWrap(true); root->addWidget(m_status);
	auto* buttons = new QHBoxLayout;
	m_host_button = new QPushButton(tr("Host This Game"), this);
	m_viewer_button = new QPushButton(tr("Join with Offer Code"), this);
	m_answer_button = new QPushButton(tr("Apply Host Answer Code"), this);
	m_copy_button = new QPushButton(tr("Copy Generated Code"), this);
	m_stop_button = new QPushButton(tr("End Remote Play"), this); m_stop_button->setEnabled(false);
	buttons->addWidget(m_host_button); buttons->addWidget(m_viewer_button); buttons->addWidget(m_answer_button);
	buttons->addWidget(m_copy_button); buttons->addWidget(m_stop_button); root->addLayout(buttons);
	root->addWidget(new QLabel(tr("Paste an offer to join, or paste the viewer's answer here while hosting:"), this));
	m_code_input = new QPlainTextEdit(this); m_code_input->setPlaceholderText(tr("ARMSX3-RP1.…")); m_code_input->setMaximumHeight(100); root->addWidget(m_code_input);
	root->addWidget(new QLabel(tr("Your generated offer or answer code:"), this));
	m_code_output = new QPlainTextEdit(this); m_code_output->setReadOnly(true); m_code_output->setMaximumHeight(100); root->addWidget(m_code_output);
	m_preview = new QLabel(tr("Remote game video"), this); m_preview->setAlignment(Qt::AlignCenter); m_preview->setMinimumSize(640, 360);
	m_preview->setStyleSheet("background:#071423;border:1px solid #34719d;color:white;"); root->addWidget(m_preview, 1);
	connect(m_host_button, &QPushButton::clicked, this, &remote_play_capture_dialog::start_host);
	connect(m_viewer_button, &QPushButton::clicked, this, &remote_play_capture_dialog::start_viewer);
	connect(m_answer_button, &QPushButton::clicked, this, &remote_play_capture_dialog::apply_answer);
	connect(m_copy_button, &QPushButton::clicked, this, [this]
	{
		QApplication::clipboard()->setText(m_code_output->toPlainText());
		set_status(tr("Connection code copied to the clipboard."));
	});
	connect(m_stop_button, &QPushButton::clicked, this, &remote_play_capture_dialog::stop_session);
	#ifdef _WIN32
	HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
	if (!xinput) xinput = LoadLibraryW(L"xinput9_1_0.dll");
	m_xinput_module = xinput;
	if (xinput)
	{
		m_xinput_get_state = reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState"));
		m_xinput_set_state = reinterpret_cast<void*>(GetProcAddress(xinput, "XInputSetState"));
	}
	#endif
	m_timer = new QTimer(this); m_timer->setInterval(16);
	connect(m_timer, &QTimer::timeout, this, &remote_play_capture_dialog::refresh_capture);
}

remote_play_capture_dialog::~remote_play_capture_dialog()
{
	stop_session();
	#ifdef _WIN32
	if (m_xinput_module) FreeLibrary(static_cast<HMODULE>(m_xinput_module));
	#endif
}

void remote_play_capture_dialog::set_status(const QString& text) { m_status->setText(text); }

void remote_play_capture_dialog::start_host()
{
	if (m_mode != mode::idle) return;
	m_lobby_cancel.store(false, std::memory_order_release);
	std::string error;
	m_sink = utils::remote_play_capture_sink::start(error);
	if (!m_sink) { set_status(QString::fromStdString(error)); return; }
	m_mode = mode::host; m_approval_prompted = false; m_code_output->clear();
	QPointer<remote_play_capture_dialog> self(this);
	m_webrtc = std::make_unique<remote_play_webrtc>(
		[this, self](const QString& text)
		{
			if (!self) return;
			QMetaObject::invokeMethod(self, [self, text]
			{
				if (!self) return;
				self->set_status(text);
				if (self->m_mode == mode::host && text.startsWith(QStringLiteral("Friend connected.")) && !self->m_approval_prompted)
				{
					self->m_approval_prompted = true;
					const bool allow = QMessageBox::question(self, tr("ARMSX3 Remote Play Request"),
						tr("Allow this viewer to join the running game as player two?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
					if (self->m_webrtc) self->m_webrtc->approve_viewer(allow);
					if (!allow) self->stop_session();
				}
			}, Qt::QueuedConnection);
		},
		[this, self](const QImage& image)
		{
			if (!self) return;
			QMetaObject::invokeMethod(self, [self, image]
			{
				if (self) self->m_preview->setPixmap(QPixmap::fromImage(image).scaled(self->m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
			}, Qt::QueuedConnection);
		});
	const QString game = QString::fromStdString(Emu.GetTitleID());
	if (!m_webrtc->start_host(game.isEmpty() ? QStringLiteral("PS3 game") : game, [this, self](const QString& code)
	{
		if (!self) return;
		QMetaObject::invokeMethod(self, [self, code] { if (self) self->publish_host_offer(code); }, Qt::QueuedConnection);
	}))
	{
		stop_session(); set_status(tr("Could not start the ARMSX3 WebRTC host.")); return;
	}
	m_webrtc->set_remote_packet_callback([this, self](const QByteArray& packet)
	{
		if (self) QMetaObject::invokeMethod(self, [self, packet] { if (self) self->receive_packet(packet); }, Qt::QueuedConnection);
	});
	m_host_button->setEnabled(false); m_viewer_button->setEnabled(false); m_stop_button->setEnabled(true);
	set_status(tr("Gathering WebRTC candidates for the ARMSX3 offer code…")); m_timer->start();
}

void remote_play_capture_dialog::start_viewer()
{
	if (m_mode != mode::idle) return;
	const QString entered = m_code_input->toPlainText().trimmed();
	if (entered.isEmpty()) { set_status(tr("Paste the host's ARMSX3 short lobby code or offer code first.")); return; }
	m_lobby_cancel.store(false, std::memory_order_release);
	if (const auto lobby = remote_play_lobby::normalize(entered))
	{
		m_lobby_code = *lobby;
		m_mode = mode::viewer; m_code_output->clear(); m_approval_prompted = false;
		m_host_button->setEnabled(false); m_viewer_button->setEnabled(false); m_stop_button->setEnabled(true);
		set_status(tr("Looking up ARMSX3 lobby %1…").arg(remote_play_lobby::display(*lobby)));
		QPointer<remote_play_capture_dialog> self(this);
		launch_lobby_task([self, lobby = *lobby]
		{
			const auto result = remote_play_lobby::request(QStringLiteral("GET"), QStringLiteral("/%1").arg(lobby), {}, {}, &self->m_lobby_cancel);
			const QJsonObject json = QJsonDocument::fromJson(result.body).object();
			const QString offer_json = json.value(QStringLiteral("offer")).toString();
			const QJsonDocument offer_document = QJsonDocument::fromJson(offer_json.toUtf8());
			const QString offer = offer_document.isObject() ? remote_play_protocol::encode_code(offer_document.object()) : QString{};
			if (!self) return;
			QMetaObject::invokeMethod(self, [self, result, offer]
			{
				if (!self || self->m_mode != mode::viewer) return;
				if (result.status != 200 || offer.isEmpty())
				{
					self->set_status(tr("That ARMSX3 lobby code is unavailable, expired, or already in use."));
					self->stop_session();
					return;
				}
				self->start_viewer_with_offer(offer);
			}, Qt::QueuedConnection);
		});
		return;
	}
	m_lobby_code.clear();
	m_mode = mode::viewer; m_code_output->clear(); m_approval_prompted = false;
	start_viewer_with_offer(entered);
}

void remote_play_capture_dialog::start_viewer_with_offer(const QString& offer)
{
	QPointer<remote_play_capture_dialog> self(this);
	m_webrtc = std::make_unique<remote_play_webrtc>(
		[this, self](const QString& text)
		{
			if (self) QMetaObject::invokeMethod(self, [self, text] { if (self) self->set_status(text); }, Qt::QueuedConnection);
		},
		[this, self](const QImage& image)
		{
			if (self) QMetaObject::invokeMethod(self, [self, image]
			{
				if (self) self->m_preview->setPixmap(QPixmap::fromImage(image).scaled(self->m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
			}, Qt::QueuedConnection);
		});
	m_webrtc->set_remote_packet_callback([this, self](const QByteArray& packet)
	{
		if (self) QMetaObject::invokeMethod(self, [self, packet] { if (self) self->receive_packet(packet); }, Qt::QueuedConnection);
	});
	if (!m_webrtc->start_viewer(offer, QStringLiteral("ARMSXPC Windows"), [this, self](const QString& code)
	{
		if (self) QMetaObject::invokeMethod(self, [self, code]
		{
			if (!self) return;
			if (self->m_lobby_code.isEmpty())
			{
				self->m_code_output->setPlainText(code);
				self->set_status(tr("Share this ARMSX3 answer code with the host. It contains the gathered WebRTC response."));
				return;
			}
			const QString lobby = self->m_lobby_code;
			const auto answer_object = remote_play_protocol::decode_code(code);
			if (!answer_object)
			{
				self->set_status(tr("Could not decode the WebRTC answer for the ARMSX3 lobby."));
				return;
			}
			const QString answer_json = QString::fromUtf8(QJsonDocument(*answer_object).toJson(QJsonDocument::Compact));
			const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("answer"), answer_json}}).toJson(QJsonDocument::Compact);
			self->set_status(tr("Sending the ARMSX3 answer through lobby %1…").arg(remote_play_lobby::display(lobby)));
			QPointer<remote_play_capture_dialog> dialog(self);
			self->launch_lobby_task([dialog, lobby, body]
			{
				const auto result = remote_play_lobby::request(QStringLiteral("POST"), QStringLiteral("/%1/answer").arg(lobby), body, {}, &dialog->m_lobby_cancel);
				if (!dialog) return;
				QMetaObject::invokeMethod(dialog, [dialog, result]
				{
					if (!dialog || dialog->m_mode != mode::viewer) return;
					if (result.status == 204) dialog->set_status(tr("ARMSX3 answer sent. Connecting to the host…"));
					else dialog->set_status(tr("The ARMSX3 lobby could not accept the answer (HTTP %1). Keep using the manual offer/answer code flow.").arg(result.status));
				}, Qt::QueuedConnection);
			});
		}, Qt::QueuedConnection);
	}))
	{
		stop_session(); set_status(tr("That is not a valid ARMSX3-RP1 offer code, or its SDP could not be accepted.")); return;
	}
	QAudioFormat format; format.setSampleRate(48000); format.setChannelCount(2); format.setSampleFormat(QAudioFormat::Int16);
	const auto output = QMediaDevices::defaultAudioOutput();
	if (output.isFormatSupported(format)) { m_audio_sink = new QAudioSink(output, format, this); m_audio_device = m_audio_sink->start(); }
	m_host_button->setEnabled(false); m_viewer_button->setEnabled(false); m_stop_button->setEnabled(true);
	set_status(tr("Preparing an ARMSX3-compatible WebRTC answer code…")); m_timer->start();
}

void remote_play_capture_dialog::launch_lobby_task(std::function<void()> task)
{
	if (m_lobby_thread.joinable()) m_lobby_thread.join();
	m_lobby_thread = std::thread(std::move(task));
}

void remote_play_capture_dialog::publish_host_offer(const QString& offer_code)
{
	const auto offer_object = remote_play_protocol::decode_code(offer_code);
	if (!offer_object)
	{
		set_status(tr("Could not decode the WebRTC offer for the ARMSX3 lobby."));
		return;
	}
	const QString offer_json = QString::fromUtf8(QJsonDocument(*offer_object).toJson(QJsonDocument::Compact));
	const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("offer"), offer_json}}).toJson(QJsonDocument::Compact);
	set_status(tr("Opening an ARMSX3 short-code lobby…"));
	QPointer<remote_play_capture_dialog> self(this);
	launch_lobby_task([self, body, offer_code]
	{
		const auto opened = remote_play_lobby::request(QStringLiteral("POST"), {}, body, {}, &self->m_lobby_cancel);
		const QJsonObject json = QJsonDocument::fromJson(opened.body).object();
		const QString code = json.value(QStringLiteral("code")).toString();
		const QString token = json.value(QStringLiteral("token")).toString();
		if (!self) return;
		if (opened.status != 201 || !remote_play_lobby::normalize(code) || token.isEmpty())
		{
			QMetaObject::invokeMethod(self, [self, offer_code]
			{
				if (self && self->m_mode == mode::host)
				{
					self->m_code_output->setPlainText(offer_code);
					self->set_status(tr("Short-code lobby unavailable. Share this ARMSX3-RP1 offer; the viewer can return an answer code."));
				}
			}, Qt::QueuedConnection);
			return;
		}
		QMetaObject::invokeMethod(self, [self, code]
		{
			if (self && self->m_mode == mode::host)
			{
				self->m_lobby_code = code;
				self->m_code_output->setPlainText(remote_play_lobby::display(code));
				self->set_status(tr("Share lobby code %1 with your ARMSX3 friend. Waiting for them to join…").arg(remote_play_lobby::display(code)));
			}
		}, Qt::QueuedConnection);
		const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10 * 60 * 1000;
		bool answer_collected = false;
		while (!self->m_lobby_cancel.load(std::memory_order_acquire) && QDateTime::currentMSecsSinceEpoch() < deadline)
		{
			const auto answer = remote_play_lobby::request(QStringLiteral("GET"), QStringLiteral("/%1/answer").arg(code), {}, token, &self->m_lobby_cancel);
			if (answer.status == 200)
			{
				answer_collected = true; // The lobby service consumes it when the host collects it.
				const QString answer_json = QJsonDocument::fromJson(answer.body).object().value(QStringLiteral("answer")).toString();
				const QJsonDocument answer_document = QJsonDocument::fromJson(answer_json.toUtf8());
				const QString answer_code = answer_document.isObject() ? remote_play_protocol::encode_code(answer_document.object()) : QString{};
				if (!answer_code.isEmpty()) QMetaObject::invokeMethod(self, [self, answer_code]
				{
					if (self && self->m_mode == mode::host && self->m_webrtc)
					{
						if (self->m_webrtc->accept_answer(answer_code)) self->set_status(tr("ARMSX3 answer received. Connecting directly over WebRTC…"));
						else self->set_status(tr("The ARMSX3 viewer returned an invalid answer code."));
					}
				}, Qt::QueuedConnection);
				break;
			}
			if (answer.status == 404 || answer.status == 409) break;
			for (int i = 0; i < 20 && !self->m_lobby_cancel.load(std::memory_order_acquire); ++i) QThread::msleep(100);
		}
		if (!answer_collected)
		{
			// Match LobbyHostLink.close(): remove a code the host cancelled or never used.
			std::thread([code, token]
			{
				remote_play_lobby::request(QStringLiteral("DELETE"), QStringLiteral("/%1").arg(code), {}, token);
			}).detach();
		}
	});
}

void remote_play_capture_dialog::apply_answer()
{
	if (m_mode != mode::host || !m_webrtc) { set_status(tr("Start hosting before applying a viewer answer code.")); return; }
	if (!m_webrtc->accept_answer(m_code_input->toPlainText())) { set_status(tr("The code is not a valid ARMSX3-RP1 answer.")); return; }
	set_status(tr("Answer accepted. Connecting directly over WebRTC; no host address is exchanged."));
}

void remote_play_capture_dialog::refresh_capture()
{
	if (m_mode == mode::viewer)
	{
		poll_gamepad(); publish_pad();
		return;
	}
	if (m_mode != mode::host || !m_sink || !m_webrtc) return;
	if (m_sink->stopped()) { set_status(tr("The game stopped; the ARMSX3 Remote Play session ended.")); stop_session(); return; }
	utils::video_sink::encoder_frame frame;
	if (m_sink->take_latest_frame(frame))
	{
		QImage::Format format = frame.av_pixel_format == AV_PIX_FMT_RGBA ? QImage::Format_RGBA8888 : frame.av_pixel_format == AV_PIX_FMT_BGRA ? QImage::Format_ARGB32 : QImage::Format_Invalid;
		if (format != QImage::Format_Invalid)
		{
			QImage view(frame.data.data(), static_cast<int>(frame.width), static_cast<int>(frame.height), static_cast<qsizetype>(frame.pitch), format);
			m_preview->setPixmap(QPixmap::fromImage(view).scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
			m_webrtc->send_video_frame(frame.data.data(), static_cast<int>(frame.width), static_cast<int>(frame.height), static_cast<int>(frame.pitch), frame.av_pixel_format);
		}
	}
	std::deque<utils::video_sink::encoder_sample> samples;
	if (m_sink->take_audio(samples)) for (const auto& sample : samples)
	{
		const qsizetype count = static_cast<qsizetype>(sample.sample_count) * sample.channels;
		if (sample.channels < 1 || sample.channels > 2 || sample.data.size() < static_cast<usz>(count * static_cast<qsizetype>(sizeof(float)))) continue;
		QByteArray pcm(static_cast<qsizetype>(sample.sample_count) * 4, Qt::Uninitialized);
		auto* dst = reinterpret_cast<qint16*>(pcm.data()); const auto* src = reinterpret_cast<const float*>(sample.data.data());
		for (u32 i = 0; i < sample.sample_count; ++i)
		{
			const float left = std::clamp(src[i * sample.channels], -1.0f, 1.0f);
			const float right = sample.channels == 2 ? std::clamp(src[i * 2 + 1], -1.0f, 1.0f) : left;
			dst[i * 2] = static_cast<qint16>(left * 32767.0f); dst[i * 2 + 1] = static_cast<qint16>(right * 32767.0f);
		}
		m_webrtc->send_audio(pcm);
	}
	if ((++m_waiting_ticks % 32) == 0) m_webrtc->send_waiting();
	if ((++m_rumble_ticks % 12) == 0)
		m_webrtc->send_rumble(m_rumble_sequence++, remote_play_input::rumble_large.load(std::memory_order_relaxed),
			remote_play_input::rumble_small.load(std::memory_order_relaxed), Emu.GetStatus() != system_state::running);
}

void remote_play_capture_dialog::append_audio(const QByteArray& pcm)
{
	if (!m_audio_device || !m_audio_sink || pcm.isEmpty()) return;
	m_audio_pending += pcm;
	const qsizetype bytes = std::min(m_audio_pending.size(), static_cast<qsizetype>(256 * 1024));
	if (bytes > 0) { m_audio_device->write(m_audio_pending.constData(), bytes); m_audio_pending.remove(0, bytes); }
}

void remote_play_capture_dialog::receive_packet(const QByteArray& packet)
{
	if (packet.isEmpty()) return;
	const quint8 type = static_cast<quint8>(packet[0]);
	if (type == 2 && packet.size() >= 8) append_audio(packet.mid(8));
	else if (type == 7) set_status(tr("Connected to the host. Waiting for them to approve the Remote Play request…"));
	else if (type == 3) { set_status(tr("The host ended Remote Play.")); stop_session(); }
	else if (type == 6)
	{
		const auto rumble = remote_play_protocol::decode_rumble(packet);
		#ifdef _WIN32
		if (rumble && m_xinput_set_state)
		{
			using set_state = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
			auto set = reinterpret_cast<set_state>(m_xinput_set_state);
			for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
			{
				using get_state = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
				auto get = reinterpret_cast<get_state>(m_xinput_get_state);
				XINPUT_STATE state{};
				if (get && get(i, &state) == ERROR_SUCCESS)
				{
					XINPUT_VIBRATION vibration{static_cast<WORD>(rumble->large * 257), static_cast<WORD>(rumble->small * 257)};
					set(i, &vibration); break;
				}
			}
		}
		#endif
	}
}

void remote_play_capture_dialog::poll_gamepad()
{
	#ifdef _WIN32
	if (!m_xinput_get_state) return;
	using get_state = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
	auto get = reinterpret_cast<get_state>(m_xinput_get_state); XINPUT_STATE state{}; bool found = false;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) if (get(i, &state) == ERROR_SUCCESS) { found = true; break; }
	if (!found) { m_gamepad_buttons = 0; m_gamepad_lx = m_gamepad_ly = m_gamepad_rx = m_gamepad_ry = 128; m_gamepad_l2 = m_gamepad_r2 = 0; return; }
	const auto& g = state.Gamepad; quint32 b = 0;
	const auto set = [&b, &g](WORD native, unsigned bit) { if (g.wButtons & native) b |= 1u << bit; };
	set(XINPUT_GAMEPAD_DPAD_UP,0); set(XINPUT_GAMEPAD_DPAD_RIGHT,1); set(XINPUT_GAMEPAD_DPAD_DOWN,2); set(XINPUT_GAMEPAD_DPAD_LEFT,3);
	set(XINPUT_GAMEPAD_BACK,4); set(XINPUT_GAMEPAD_START,5); set(XINPUT_GAMEPAD_LEFT_THUMB,6); set(XINPUT_GAMEPAD_RIGHT_THUMB,7);
	set(XINPUT_GAMEPAD_Y,8); set(XINPUT_GAMEPAD_B,9); set(XINPUT_GAMEPAD_A,10); set(XINPUT_GAMEPAD_X,11);
	set(XINPUT_GAMEPAD_LEFT_SHOULDER,12); set(XINPUT_GAMEPAD_RIGHT_SHOULDER,13);
	const auto axis = [](SHORT v, SHORT dz, bool inv = false) { if (std::abs(static_cast<int>(v)) < dz) return quint8(128); int n = inv ? -static_cast<int>(v) : static_cast<int>(v); return static_cast<quint8>(std::clamp(128 + n * 127 / 32767, 0, 255)); };
	m_gamepad_buttons = b; m_gamepad_lx = axis(g.sThumbLX,XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE); m_gamepad_ly = axis(g.sThumbLY,XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,true);
	m_gamepad_rx = axis(g.sThumbRX,XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE); m_gamepad_ry = axis(g.sThumbRY,XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE,true);
	m_gamepad_l2 = g.bLeftTrigger; m_gamepad_r2 = g.bRightTrigger;
	#endif
}

void remote_play_capture_dialog::publish_pad()
{
	if (!m_webrtc || m_mode != mode::viewer) return;
	const quint32 buttons = m_buttons | m_gamepad_buttons;
	m_webrtc->send_pad(m_sequence++, buttons, m_lx != 128 ? m_lx : m_gamepad_lx, m_ly != 128 ? m_ly : m_gamepad_ly,
		m_rx != 128 ? m_rx : m_gamepad_rx, m_ry != 128 ? m_ry : m_gamepad_ry,
		std::max(m_l2, m_gamepad_l2), std::max(m_r2, m_gamepad_r2));
}

void remote_play_capture_dialog::stop_session()
{
	m_lobby_cancel.store(true, std::memory_order_release);
	if (m_lobby_thread.joinable() && m_lobby_thread.get_id() != std::this_thread::get_id()) m_lobby_thread.join();
	if (m_timer) m_timer->stop();
	if (m_webrtc) { m_webrtc->close(); m_webrtc.reset(); }
	if (m_sink) { utils::remote_play_capture_sink::stop_capture(); m_sink.reset(); }
	if (m_audio_sink) { m_audio_sink->stop(); m_audio_sink->deleteLater(); m_audio_sink = nullptr; m_audio_device = nullptr; }
	m_audio_pending.clear(); remote_play_input::clear(); m_mode = mode::idle; m_approval_prompted = false; m_lobby_code.clear();
	m_host_button->setEnabled(true); m_viewer_button->setEnabled(true); m_stop_button->setEnabled(false);
	}

void remote_play_capture_dialog::keyPressEvent(QKeyEvent* e)
{
	quint32 bit = 0;
	switch (e->key())
	{
	case Qt::Key_Up: bit=1u<<0; break; case Qt::Key_Right: bit=1u<<1; break; case Qt::Key_Down: bit=1u<<2; break; case Qt::Key_Left: bit=1u<<3; break;
	case Qt::Key_Q: bit=1u<<4; break; case Qt::Key_E: bit=1u<<5; break; case Qt::Key_1: bit=1u<<6; break; case Qt::Key_2: bit=1u<<7; break;
	case Qt::Key_Z: bit=1u<<8; break; case Qt::Key_X: bit=1u<<9; break; case Qt::Key_Return: case Qt::Key_Enter: bit=1u<<10; break; case Qt::Key_C: bit=1u<<11; break;
	case Qt::Key_BracketLeft: bit=1u<<12; break; case Qt::Key_BracketRight: bit=1u<<13; break;
	default: QDialog::keyPressEvent(e); return;
	}
	m_buttons |= bit; publish_pad(); e->accept();
}

void remote_play_capture_dialog::keyReleaseEvent(QKeyEvent* e)
{
	quint32 bit = 0;
	switch (e->key())
	{
	case Qt::Key_Up: bit=1u<<0; break; case Qt::Key_Right: bit=1u<<1; break; case Qt::Key_Down: bit=1u<<2; break; case Qt::Key_Left: bit=1u<<3; break;
	case Qt::Key_Q: bit=1u<<4; break; case Qt::Key_E: bit=1u<<5; break; case Qt::Key_1: bit=1u<<6; break; case Qt::Key_2: bit=1u<<7; break;
	case Qt::Key_Z: bit=1u<<8; break; case Qt::Key_X: bit=1u<<9; break; case Qt::Key_Return: case Qt::Key_Enter: bit=1u<<10; break; case Qt::Key_C: bit=1u<<11; break;
	case Qt::Key_BracketLeft: bit=1u<<12; break; case Qt::Key_BracketRight: bit=1u<<13; break;
	default: QDialog::keyReleaseEvent(e); return;
	}
	m_buttons &= ~bit; publish_pad(); e->accept();
}
