#pragma once

#include <QImage>
#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>
#include <cstdint>

namespace rtc { class PeerConnection; class DataChannel; class Track; }

// Native desktop endpoint for ARMSX3's RemotePlayProtocol and RemotePlayRtc wire contract.
// Signaling is carried in the same copyable ARMSX3-RP1 offer/answer codes as Android.
class remote_play_webrtc final
{
public:
	using code_callback = std::function<void(const QString&)>;
	using status_callback = std::function<void(const QString&)>;
	using video_callback = std::function<void(const QImage&)>;
	explicit remote_play_webrtc(status_callback status, video_callback video);
	~remote_play_webrtc();
	bool start_host(const QString& game, code_callback offer);
	bool accept_answer(const QString& code);
	bool start_viewer(const QString& offer, const QString& viewer_name, code_callback answer);
	void set_remote_packet_callback(std::function<void(const QByteArray&)> callback);
	void send_waiting();
	void approve_viewer(bool approved);
	void send_video_frame(const uint8_t* data, int width, int height, int pitch, int pixel_format);
	void send_audio(const QByteArray& pcm);
	void send_rumble(quint32 sequence, quint8 large, quint8 small, bool paused);
	void send_pad(quint32 sequence, quint32 buttons, quint8 lx, quint8 ly, quint8 rx, quint8 ry, quint8 l2, quint8 r2);
	void close();
	bool connected() const;
	bool hosting() const;

private:
	struct impl;
	std::unique_ptr<impl> m;
};
