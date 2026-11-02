#include "stdafx.h"
#include "remote_play_webrtc.h"

#include "remote_play_protocol.h"
#include "Input/remote_play_input.h"

#include <rtc/rtc.hpp>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <codec_api.h>

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <limits>
#include <vector>

struct remote_play_webrtc::impl
{
	status_callback status;
	video_callback video;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<rtc::DataChannel> input, audio;
	bool host = false;
	std::atomic<bool> approved{false};
	std::atomic<bool> is_connected{false};
	std::atomic<bool> offer_ready{false};
	std::atomic<quint32> audio_seq{0};
	std::mutex codec_mutex;
	ISVCEncoder* encoder = nullptr;
	std::vector<uint8_t> encode_yuv;
	SwsContext* encode_scale = nullptr;
	int encode_width = 0, encode_height = 0, encode_src_format = -1;
	int64_t video_pts = 0;
	AVCodecContext* decoder = nullptr;
	AVFrame* decode_frame = nullptr;
	AVPacket* decode_packet = nullptr;
	SwsContext* decode_scale = nullptr;

	explicit impl(status_callback s, video_callback v) : status(std::move(s)), video(std::move(v)) {}

	~impl()
	{
		shutdown_codec();
		if (pc) pc->close();
	}

	void tell(QString text) { if (status) status(text); }

	void configure_peer()
	{
		rtc::InitLogger(rtc::LogLevel::Warning);
		rtc::Configuration config;
		config.iceServers.emplace_back("stun:stun.l.google.com:19302");
		config.iceServers.emplace_back("stun:stun.cloudflare.com:3478");
		config.maxMessageSize = 262144;
		pc = std::make_shared<rtc::PeerConnection>(config);
		pc->onStateChange([this](rtc::PeerConnection::State state)
		{
			is_connected.store(state == rtc::PeerConnection::State::Connected, std::memory_order_release);
			if (state == rtc::PeerConnection::State::Connected) tell(host ? QStringLiteral("Friend connected. Approve the viewer to start the game stream.") : QStringLiteral("Connected to host."));
			else if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Closed)
			{
				remote_play_input::clear();
				tell(QStringLiteral("Remote Play connection ended or failed."));
			}
		});
		pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state)
		{
			if (state != rtc::PeerConnection::GatheringState::Complete || offer_ready.exchange(true)) return;
			const auto local = pc->localDescription();
			if (!local) { tell(QStringLiteral("WebRTC did not produce a local description.")); return; }
			QJsonObject message{{QStringLiteral("t"), host ? QStringLiteral("offer") : QStringLiteral("answer")},
				{QStringLiteral("v"), remote_play_protocol::version},
				{QStringLiteral("sdp"), QString::fromStdString(std::string(*local))}};
			if (host)
			{
				message.insert(QStringLiteral("game"), game_name);
				message.insert(QStringLiteral("host"), QStringLiteral("ARMSXPC"));
				message.insert(QStringLiteral("name"), QStringLiteral("ARMSXPC Windows"));
			}
			else
			{
				message.insert(QStringLiteral("name"), viewer_name);
				message.insert(QStringLiteral("audio"), QStringLiteral("pcm"));
			}
			const QString code = remote_play_protocol::encode_code(message);
			if (code.isEmpty()) { tell(QStringLiteral("Could not encode ARMSX3 Remote Play connection code.")); return; }
			if (code_ready) code_ready(code);
		});
	}

	QString game_name;
	QString viewer_name = QStringLiteral("ARMSXPC Windows");
	code_callback code_ready;

	void add_host_video()
	{
		const std::string cname = "armsx3-game";
		rtc::Description::Video desc(cname);
		desc.addH264Codec(96);
		desc.addSSRC(0x41525833, cname, "armsx3", cname);
		track = pc->addTrack(desc);
		auto config = std::make_shared<rtc::RtpPacketizationConfig>(0x41525833, cname, 96, rtc::H264RtpPacketizer::ClockRate);
		auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, config);
		packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(config));
		packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
		track->setMediaHandler(packetizer);
		rtc::DataChannelInit unreliable;
		unreliable.reliability.unordered = true;
		unreliable.reliability.maxRetransmits = 0;
		input = pc->createDataChannel(remote_play_protocol::input_channel, unreliable);
		audio = pc->createDataChannel(remote_play_protocol::audio_channel, unreliable);
		input->onMessage([this](rtc::message_variant message)
		{
			if (!std::holds_alternative<rtc::binary>(message)) return;
			const auto& data = std::get<rtc::binary>(message);
			if (!approved.load(std::memory_order_acquire)) return;
			QByteArray packet(static_cast<const char*>(static_cast<const void*>(data.data())), static_cast<qsizetype>(data.size()));
			if (const auto pad = remote_play_protocol::decode_pad(packet))
				remote_play_input::set(pad->buttons, pad->lx, pad->ly, pad->rx, pad->ry, pad->l2, pad->r2);
			else if (!packet.isEmpty() && static_cast<quint8>(packet[0]) == 3)
			{
				remote_play_input::clear();
				if (pc) pc->close();
				tell(QStringLiteral("The ARMSX3 viewer ended Remote Play."));
			}
		});
	}

	void attach_viewer_channels()
	{
		pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel)
		{
			if (channel->label() == remote_play_protocol::input_channel) input = channel;
			else if (channel->label() == remote_play_protocol::audio_channel) audio = channel;
			else return;
			if (channel->label() == remote_play_protocol::audio_channel)
			{
				channel->onMessage([this](rtc::message_variant message)
				{
					if (!std::holds_alternative<rtc::binary>(message)) return;
					const auto& data = std::get<rtc::binary>(message);
					const char* bytes = reinterpret_cast<const char*>(data.data());
					if (remote_packet) remote_packet(QByteArray(bytes, static_cast<qsizetype>(data.size())));
				});
			}
			channel->onOpen([this] { tell(QStringLiteral("ARMSX3 Remote Play channel is ready.")); });
		});
		pc->onTrack([this](std::shared_ptr<rtc::Track> received)
		{
			track = received;
			received->onFrame([this](rtc::binary frame, rtc::FrameInfo)
			{
				decode_h264(frame);
			});
		});
	}

	std::function<void(const QByteArray&)> remote_packet;

	bool add_decoder()
	{
		const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) return false;
		decoder = avcodec_alloc_context3(codec);
		decode_frame = av_frame_alloc(); decode_packet = av_packet_alloc();
		if (!decoder || !decode_frame || !decode_packet || avcodec_open2(decoder, codec, nullptr) < 0) return false;
		return true;
	}

	void decode_h264(const rtc::binary& bytes)
	{
		std::lock_guard lock(codec_mutex);
		if (!decoder && !add_decoder()) { tell(QStringLiteral("This FFmpeg build has no H.264 decoder.")); return; }
		if (bytes.empty() || bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return;
		if (av_new_packet(decode_packet, static_cast<int>(bytes.size())) < 0) return;
		std::memcpy(decode_packet->data, bytes.data(), bytes.size());
		const int sent = avcodec_send_packet(decoder, decode_packet);
		av_packet_unref(decode_packet);
		if (sent < 0) return;
		while (avcodec_receive_frame(decoder, decode_frame) == 0)
		{
			QImage image(decode_frame->width, decode_frame->height, QImage::Format_RGBA8888);
			decode_scale = sws_getCachedContext(decode_scale, decode_frame->width, decode_frame->height, static_cast<AVPixelFormat>(decode_frame->format),
				decode_frame->width, decode_frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (decode_scale)
			{
				uint8_t* dst[] = {image.bits(), nullptr, nullptr, nullptr}; int lines[] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
				sws_scale(decode_scale, decode_frame->data, decode_frame->linesize, 0, decode_frame->height, dst, lines);
				if (video) video(image);
			}
			av_frame_unref(decode_frame);
		}
	}

	bool init_encoder(int width, int height, int src_format)
	{
		if (encoder && width == encode_width && height == encode_height && src_format == encode_src_format) return true;
		if (encoder) { encoder->Uninitialize(); WelsDestroySVCEncoder(encoder); encoder = nullptr; }
		if (encode_scale) { sws_freeContext(encode_scale); encode_scale = nullptr; }
		const double scale = std::min({1.0, 1280.0 / width, 720.0 / height});
		encode_width = std::max(2, static_cast<int>(width * scale) & ~1);
		encode_height = std::max(2, static_cast<int>(height * scale) & ~1);
		encode_src_format = src_format;
		if (WelsCreateSVCEncoder(&encoder) != 0 || !encoder) { tell(QStringLiteral("Could not create the OpenH264 encoder.")); return false; }
		SEncParamExt params{};
		if (encoder->GetDefaultParams(&params) != 0) { tell(QStringLiteral("Could not initialize OpenH264 parameters.")); return false; }
		params.iUsageType = CAMERA_VIDEO_REAL_TIME;
		params.iPicWidth = encode_width; params.iPicHeight = encode_height;
		params.iTargetBitrate = 4'000'000; params.iRCMode = RC_BITRATE_MODE;
		params.fMaxFrameRate = 60.0f; params.iTemporalLayerNum = 1; params.iSpatialLayerNum = 1;
		params.uiIntraPeriod = 60; params.iMultipleThreadIdc = 1; params.bEnableFrameSkip = true;
		auto& layer = params.sSpatialLayers[0];
		layer.iVideoWidth = encode_width; layer.iVideoHeight = encode_height;
		layer.fFrameRate = 60.0f; layer.iSpatialBitrate = 4'000'000;
		layer.iMaxSpatialBitrate = 4'000'000; layer.uiProfileIdc = PRO_BASELINE;
		layer.uiLevelIdc = LEVEL_3_1; layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
		if (encoder->InitializeExt(&params) != 0)
		{
			WelsDestroySVCEncoder(encoder); encoder = nullptr;
			tell(QStringLiteral("Could not initialize OpenH264.")); return false;
		}
		encode_yuv.resize(static_cast<size_t>(encode_width) * encode_height * 3 / 2);
		encode_scale = sws_getContext(width, height, static_cast<AVPixelFormat>(src_format), encode_width, encode_height,
			AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		return encode_scale != nullptr;
	}

	void encode_video(const uint8_t* data, int width, int height, int pitch, int format)
	{
		std::lock_guard lock(codec_mutex);
		if (!host || !approved.load(std::memory_order_acquire) || !track || !track->isOpen() || !data || width < 2 || height < 2) return;
		if (!init_encoder(width, height, format)) return;
		if (!encode_scale || !encoder) return;
		const uint8_t* src[] = {data, nullptr, nullptr, nullptr}; int strides[] = {pitch, 0, 0, 0};
		const int luma_size = encode_width * encode_height;
		uint8_t* dst[] = {encode_yuv.data(), encode_yuv.data() + luma_size, encode_yuv.data() + luma_size + luma_size / 4, nullptr};
		int lines[] = {encode_width, encode_width / 2, encode_width / 2, 0};
		if (sws_scale(encode_scale, src, strides, 0, height, dst, lines) <= 0) return;
		SSourcePicture picture{};
		picture.iColorFormat = videoFormatI420;
		picture.iPicWidth = encode_width; picture.iPicHeight = encode_height;
		picture.iStride[0] = encode_width; picture.iStride[1] = picture.iStride[2] = encode_width / 2;
		picture.pData[0] = dst[0]; picture.pData[1] = dst[1]; picture.pData[2] = dst[2];
		picture.uiTimeStamp = video_pts * 1000 / 60;
		++video_pts;
		SFrameBSInfo info{};
		if (encoder->EncodeFrame(&picture, &info) != 0 || info.iFrameSizeInBytes <= 0) return;
		for (int layer_index = 0; layer_index < info.iLayerNum; ++layer_index)
		{
			const auto& layer = info.sLayerInfo[layer_index];
			int size = 0;
			for (int i = 0; i < layer.iNalCount; ++i) size += layer.pNalLengthInByte[i];
			if (size <= 0 || !layer.pBsBuf) continue;
			rtc::binary packet(static_cast<size_t>(size));
			std::memcpy(packet.data(), layer.pBsBuf, packet.size());
			track->sendFrame(std::move(packet), rtc::FrameInfo(static_cast<uint32_t>((video_pts - 1) * 1500)));
		}
	}

	void shutdown_codec()
	{
		std::lock_guard lock(codec_mutex);
		if (encoder) { encoder->Uninitialize(); WelsDestroySVCEncoder(encoder); encoder = nullptr; }
		if (decode_frame) av_frame_free(&decode_frame);
		if (decode_packet) av_packet_free(&decode_packet);
		if (decoder) avcodec_free_context(&decoder);
		if (encode_scale) sws_freeContext(encode_scale);
		if (decode_scale) sws_freeContext(decode_scale);
		encode_scale = decode_scale = nullptr;
		encode_yuv.clear();
	}
};

remote_play_webrtc::remote_play_webrtc(status_callback status, video_callback video)
	: m(std::make_unique<impl>(std::move(status), std::move(video))) {}
remote_play_webrtc::~remote_play_webrtc() = default;

bool remote_play_webrtc::start_host(const QString& game, code_callback offer)
{
	if (m->pc) return false;
	m->host = true; m->game_name = game; m->code_ready = std::move(offer); m->configure_peer(); m->add_host_video();
	m->pc->setLocalDescription(rtc::Description::Type::Offer);
	return true;
}

bool remote_play_webrtc::accept_answer(const QString& code)
{
	if (!m->pc || !m->host) return false;
	const auto message = remote_play_protocol::decode_code(code);
	if (!message || message->value(QStringLiteral("t")).toString() != QStringLiteral("answer") || message->value(QStringLiteral("v")).toInt() != remote_play_protocol::version) return false;
	m->pc->setRemoteDescription(rtc::Description(message->value(QStringLiteral("sdp")).toString().toStdString(), "answer"));
	return true;
}

bool remote_play_webrtc::start_viewer(const QString& offer, const QString& viewer_name, code_callback answer)
{
	if (m->pc) return false;
	const auto message = remote_play_protocol::decode_code(offer);
	if (!message || message->value(QStringLiteral("t")).toString() != QStringLiteral("offer") || message->value(QStringLiteral("v")).toInt() != remote_play_protocol::version) return false;
	m->host = false; m->viewer_name = viewer_name; m->code_ready = std::move(answer); m->configure_peer(); m->attach_viewer_channels();
	rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly); video.addH264Codec(96);
	m->track = m->pc->addTrack(video);
	m->pc->setRemoteDescription(rtc::Description(message->value(QStringLiteral("sdp")).toString().toStdString(), "offer"));
	m->pc->setLocalDescription(rtc::Description::Type::Answer);
	return true;
}

void remote_play_webrtc::set_remote_packet_callback(std::function<void(const QByteArray&)> callback)
{
	m->remote_packet = std::move(callback);
}

void remote_play_webrtc::send_waiting()
{
	if (m->host && !m->approved.load(std::memory_order_acquire) && m->audio && m->audio->isOpen())
		m->audio->sendBuffer(remote_play_protocol::encode_waiting());
}

void remote_play_webrtc::approve_viewer(bool approved)
{
	if (!m->host) return;
	m->approved.store(approved, std::memory_order_release);
	if (!approved)
	{
		if (m->audio && m->audio->isOpen()) m->audio->sendBuffer(remote_play_protocol::encode_bye(1));
		remote_play_input::clear();
		m->tell(QStringLiteral("Remote Play request declined."));
		return;
	}
	m->tell(QStringLiteral("Viewer approved; streaming the game to ARMSX3."));
}

void remote_play_webrtc::send_video_frame(const uint8_t* data, int width, int height, int pitch, int pixel_format)
{
	m->encode_video(data, width, height, pitch, pixel_format);
}

void remote_play_webrtc::send_audio(const QByteArray& pcm)
{
	if (!m->host || !m->approved.load(std::memory_order_acquire) || !m->audio || !m->audio->isOpen()) return;
	const QByteArray packet = remote_play_protocol::encode_audio(m->audio_seq.fetch_add(1), pcm);
	m->audio->sendBuffer(packet);
}

void remote_play_webrtc::send_rumble(quint32 sequence, quint8 large, quint8 small, bool paused)
{
	if (!m->host || !m->approved.load(std::memory_order_acquire) || !m->audio || !m->audio->isOpen()) return;
	m->audio->sendBuffer(remote_play_protocol::encode_rumble(sequence, large, small, paused));
}

void remote_play_webrtc::send_pad(quint32 sequence, quint32 buttons, quint8 lx, quint8 ly, quint8 rx, quint8 ry, quint8 l2, quint8 r2)
{
	if (m->host || !m->input || !m->input->isOpen()) return;
	remote_play_protocol::pad_state pad{sequence, buttons, lx, ly, rx, ry, l2, r2};
	m->input->sendBuffer(remote_play_protocol::encode_pad(pad));
}

void remote_play_webrtc::close()
{
	if (!m || !m->pc) return;
	if (m->audio && m->audio->isOpen()) m->audio->sendBuffer(remote_play_protocol::encode_bye());
	remote_play_input::clear();
	m->pc->close(); m->pc.reset(); m->input.reset(); m->audio.reset(); m->track.reset();
	m->is_connected = false; m->approved = false;
}

bool remote_play_webrtc::connected() const { return m->is_connected.load(std::memory_order_acquire); }
bool remote_play_webrtc::hosting() const { return m->host; }
