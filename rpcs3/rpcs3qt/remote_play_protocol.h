#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace remote_play_protocol
{
	inline constexpr int version = 1;
	inline constexpr quint16 port = 38433;
	inline constexpr char service_type[] = "_armsx3rp._tcp";
	inline constexpr char input_channel[] = "input";
	inline constexpr char audio_channel[] = "audio";

	struct pad_state
	{
		quint32 sequence = 0;
		quint32 buttons = 0;
		quint8 lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
	};
	struct rumble_state { quint32 sequence = 0; quint8 large = 0, small = 0; bool paused = false; };

	// ARMSX3's shareable offer/answer code. These are protocol primitives; the desktop session must
	// use the same WebRTC SDP and data-channel transport before the code can be used to play.
	QString encode_code(const QJsonObject& message);
	std::optional<QJsonObject> decode_code(const QString& text);

	QByteArray encode_pad(const pad_state& pad);
	std::optional<pad_state> decode_pad(const QByteArray& packet);
	QByteArray encode_audio(quint32 sequence, const QByteArray& pcm);
	QByteArray encode_bye(quint8 reason = 0);
	QByteArray encode_waiting();
	QByteArray encode_rumble(quint32 sequence, quint8 large, quint8 small, bool paused);
	std::optional<rumble_state> decode_rumble(const QByteArray& packet);
}
