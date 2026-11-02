#include "stdafx.h"
#include "remote_play_protocol.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QtEndian>

#include <array>
#include <zlib.h>

namespace
{
	constexpr qsizetype max_code_json = 64 * 1024;
	constexpr QByteArrayView code_prefix = "ARMSX3-RP1.";
	constexpr char code_pattern[] = "ARMSX3-RP1\\.[A-Za-z0-9_-]+";

	QByteArray json_escape(const QByteArray& text)
	{
		QByteArray escaped;
		escaped.reserve(text.size() + 16);
		for (const char c : text)
		{
			switch (c)
			{
			case '"': escaped += "\\\""; break;
			case '\\': escaped += "\\\\"; break;
		case '\b': escaped += "\\b"; break;
		case '\f': escaped += "\\f"; break;
		case '\n': escaped += "\\n"; break;
		case '\r': escaped += "\\r"; break;
		case '\t': escaped += "\\t"; break;
		default: escaped += c; break;
			}
		}
		return escaped;
	}

	// Kept byte-for-byte compatible with RemotePlayProtocol.CODE_DICTIONARY in the Android app.
	QByteArray code_dictionary()
	{
		static const QByteArray dictionary = []
		{
			const QStringList sdp{
				"v=0", "o=- 0 2 IN IP4 127.0.0.1", "s=-", "t=0 0", "a=group:BUNDLE 0 1", "a=extmap-allow-mixed",
				"a=msid-semantic: WMS armsx3", "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101 102 103",
				"c=IN IP4 0.0.0.0", "a=rtcp:9 IN IP4 0.0.0.0",
				"a=candidate:1 1 udp 2122260223 192.168.1.2 50000 typ host generation 0 network-id 1 network-cost 10",
				"a=candidate:2 1 udp 1686052607 1.2.3.4 50000 typ srflx raddr 192.168.1.2 rport 50000 generation 0 network-id 1 network-cost 10",
				"a=candidate:3 1 udp 2122129151 fe80::1 50000 typ host generation 0 network-id 2 network-cost 10",
				"a=ice-ufrag:", "a=ice-pwd:", "a=ice-options:trickle renomination", "a=fingerprint:sha-256 ",
				"a=setup:actpass", "a=setup:active", "a=mid:0", "a=mid:1",
				"a=extmap:1 urn:ietf:params:rtp-hdrext:toffset",
				"a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time",
				"a=extmap:3 urn:3gpp:video-orientation",
				"a=extmap:4 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01",
				"a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay",
				"a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type",
				"a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing",
				"a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space",
				"a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid",
				"a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id",
				"a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id",
				"a=sendonly", "a=recvonly", "a=msid:armsx3 game", "a=rtcp-mux", "a=rtcp-rsize",
				"a=rtpmap:96 H264/90000", "a=rtcp-fb:96 goog-remb", "a=rtcp-fb:96 transport-cc",
				"a=rtcp-fb:96 ccm fir", "a=rtcp-fb:96 nack", "a=rtcp-fb:96 nack pli",
				"a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
				"a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c1f",
				"a=rtpmap:97 rtx/90000", "a=fmtp:97 apt=96", "a=rtpmap:100 VP8/90000",
				"a=rtpmap:102 red/90000", "a=rtpmap:103 ulpfec/90000", "a=ssrc-group:FID ",
				"a=ssrc:1 cname:", "a=ssrc:1 msid:armsx3 game",
				"m=application 9 UDP/DTLS/SCTP webrtc-datachannel", "a=sctp-port:5000",
				"a=max-message-size:262144", "",
			};
			QByteArray result = "{\"t\":\"offer\",\"v\":1,\"sdp\":\"";
			result += json_escape(sdp.join(QStringLiteral("\r\n")).toUtf8());
			result += "\",\"game\":\"\",\"host\":\"\",\"name\":\"\"}";
			return result;
		}();
		return dictionary;
	}
}

QString remote_play_protocol::encode_code(const QJsonObject& message)
{
	const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
	z_stream stream{};
	if (deflateInit(&stream, Z_BEST_COMPRESSION) != Z_OK) return {};
	const QByteArray dictionary = code_dictionary();
	if (deflateSetDictionary(&stream, reinterpret_cast<const Bytef*>(dictionary.constData()), static_cast<uInt>(dictionary.size())) != Z_OK)
	{
		deflateEnd(&stream);
		return {};
	}
	stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(json.constData()));
	stream.avail_in = static_cast<uInt>(json.size());
	QByteArray compressed;
	std::array<char, 4096> buffer{};
	int result = Z_OK;
	while (result != Z_STREAM_END)
	{
		stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
		stream.avail_out = static_cast<uInt>(buffer.size());
		result = deflate(&stream, Z_FINISH);
		if (result != Z_OK && result != Z_STREAM_END) { deflateEnd(&stream); return {}; }
		compressed.append(buffer.data(), static_cast<qsizetype>(buffer.size() - stream.avail_out));
	}
	deflateEnd(&stream);
	return QString::fromLatin1(code_prefix.data(), code_prefix.size()) + QString::fromLatin1(compressed.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

std::optional<QJsonObject> remote_play_protocol::decode_code(const QString& text)
{
	static const QRegularExpression pattern(QString::fromLatin1(code_pattern));
	const auto match = pattern.match(text);
	if (!match.hasMatch()) return std::nullopt;
	const QByteArray token = match.captured().mid(static_cast<qsizetype>(code_prefix.size())).toLatin1();
	const QByteArray compressed = QByteArray::fromBase64(token, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
	if (compressed.isEmpty()) return std::nullopt;
	z_stream stream{};
	if (inflateInit(&stream) != Z_OK) return std::nullopt;
	stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
	stream.avail_in = static_cast<uInt>(compressed.size());
	QByteArray json;
	std::array<char, 4096> buffer{};
	int status = Z_OK;
	while (status != Z_STREAM_END)
	{
		stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
		stream.avail_out = static_cast<uInt>(buffer.size());
		status = inflate(&stream, Z_NO_FLUSH);
		if (status == Z_NEED_DICT)
		{
			const QByteArray dictionary = code_dictionary();
			status = inflateSetDictionary(&stream, reinterpret_cast<const Bytef*>(dictionary.constData()), static_cast<uInt>(dictionary.size()));
			if (status == Z_OK) continue;
		}
		if (status != Z_OK && status != Z_STREAM_END) { inflateEnd(&stream); return std::nullopt; }
		json.append(buffer.data(), static_cast<qsizetype>(buffer.size() - stream.avail_out));
		if (json.size() > max_code_json) { inflateEnd(&stream); return std::nullopt; }
	}
	inflateEnd(&stream);
	const QJsonDocument document = QJsonDocument::fromJson(json);
	if (!document.isObject()) return std::nullopt;
	return document.object();
}

QByteArray remote_play_protocol::encode_pad(const pad_state& pad)
{
	QByteArray packet(16, '\0');
	packet[0] = 1;
	qToLittleEndian(pad.sequence, reinterpret_cast<uchar*>(packet.data() + 1));
	qToLittleEndian(pad.buttons, reinterpret_cast<uchar*>(packet.data() + 5));
	packet[9] = static_cast<char>(pad.lx); packet[10] = static_cast<char>(pad.ly);
	packet[11] = static_cast<char>(pad.rx); packet[12] = static_cast<char>(pad.ry);
	packet[13] = static_cast<char>(pad.l2); packet[14] = static_cast<char>(pad.r2);
	return packet;
}

std::optional<remote_play_protocol::pad_state> remote_play_protocol::decode_pad(const QByteArray& packet)
{
	if (packet.size() != 16 || static_cast<quint8>(packet[0]) != 1) return std::nullopt;
	pad_state result;
	result.sequence = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(packet.constData() + 1));
	result.buttons = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(packet.constData() + 5));
	result.lx = static_cast<quint8>(packet[9]); result.ly = static_cast<quint8>(packet[10]);
	result.rx = static_cast<quint8>(packet[11]); result.ry = static_cast<quint8>(packet[12]);
	result.l2 = static_cast<quint8>(packet[13]); result.r2 = static_cast<quint8>(packet[14]);
	return result;
}

QByteArray remote_play_protocol::encode_audio(quint32 sequence, const QByteArray& pcm)
{
	QByteArray packet(8, '\0');
	packet[0] = 2;
	qToLittleEndian(sequence, reinterpret_cast<uchar*>(packet.data() + 4));
	packet += pcm.left(pcm.size() & ~3);
	return packet;
}

QByteArray remote_play_protocol::encode_bye(quint8 reason)
{
	QByteArray packet(4, '\0');
	packet[0] = 3;
	packet[1] = static_cast<char>(reason);
	return packet;
}

QByteArray remote_play_protocol::encode_waiting()
{
	QByteArray packet(4, '\0');
	packet[0] = 7;
	return packet;
}

QByteArray remote_play_protocol::encode_rumble(quint32 sequence, quint8 large, quint8 small, bool paused)
{
	QByteArray packet(8, '\0');
	packet[0] = 6; packet[1] = static_cast<char>(large); packet[2] = static_cast<char>(small); packet[3] = paused ? 1 : 0;
	qToLittleEndian(sequence, reinterpret_cast<uchar*>(packet.data() + 4));
	return packet;
}

std::optional<remote_play_protocol::rumble_state> remote_play_protocol::decode_rumble(const QByteArray& packet)
{
	if (packet.size() != 8 || static_cast<quint8>(packet[0]) != 6) return std::nullopt;
	return rumble_state{qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(packet.constData() + 4)),
		static_cast<quint8>(packet[1]), static_cast<quint8>(packet[2]), (static_cast<quint8>(packet[3]) & 1) != 0};
}
