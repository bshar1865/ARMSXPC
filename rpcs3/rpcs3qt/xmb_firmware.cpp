#include "stdafx.h"
#include "xmb_firmware.h"

#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QImage>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace
{
	constexpr quint32 none = 0xffffffffu;
	constexpr quint32 header_size = 0x28;

	quint16 be16(const QByteArray& b, qsizetype at)
	{
		if (at < 0 || at + 2 > b.size()) return 0;
		return (quint16(static_cast<uchar>(b[at])) << 8) | quint16(static_cast<uchar>(b[at + 1]));
	}
	quint32 be32(const QByteArray& b, qsizetype at)
	{
		if (at < 0 || at + 4 > b.size()) return none;
		return (quint32(static_cast<uchar>(b[at])) << 24) | (quint32(static_cast<uchar>(b[at + 1])) << 16) |
			(quint32(static_cast<uchar>(b[at + 2])) << 8) | quint32(static_cast<uchar>(b[at + 3]));
	}
	QByteArray inflate_block(const QByteArray& source, qsizetype expected)
	{
		if (source.isEmpty() || expected <= 0 || expected > (16 << 20)) return {};
		QByteArray out(expected, Qt::Uninitialized);
		uLongf length = static_cast<uLongf>(expected);
		if (uncompress(reinterpret_cast<Bytef*>(out.data()), &length, reinterpret_cast<const Bytef*>(source.constData()), static_cast<uLong>(source.size())) != Z_OK || length != static_cast<uLongf>(expected)) return {};
		return out;
	}
	QString label(const QByteArray& table, quint32 labels_at, quint32 offset)
	{
		const quint64 at = quint64(labels_at) + offset;
		if (at >= static_cast<quint64>(table.size())) return {};
		const char* begin = table.constData() + at;
		const qsizetype max = std::min<qsizetype>(256, table.size() - static_cast<qsizetype>(at));
		qsizetype len = 0;
		while (len < max && begin[len] != '\0') ++len;
		return QString::fromLatin1(begin, static_cast<qsizetype>(len));
	}
	quint16 le16(const QByteArray& b, qsizetype at) { return quint16(static_cast<uchar>(b[at])) | (quint16(static_cast<uchar>(b[at + 1])) << 8); }
	quint32 le32(const QByteArray& b, qsizetype at) { return quint32(static_cast<uchar>(b[at])) | (quint32(static_cast<uchar>(b[at + 1])) << 8) | (quint32(static_cast<uchar>(b[at + 2])) << 16) | (quint32(static_cast<uchar>(b[at + 3])) << 24); }
	QRgb mix565(quint16 a, quint16 b, int wa, int wb, int div)
	{
		const int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
		const int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
		const int r = ((ar * 255 / 31) * wa + (br * 255 / 31) * wb) / div;
		const int g = ((ag * 255 / 63) * wa + (bg * 255 / 63) * wb) / div;
		const int bl = ((ab * 255 / 31) * wa + (bb * 255 / 31) * wb) / div;
		return qRgb(r, g, bl);
	}
	QImage decode_gim(const QByteArray& gim)
	{
		if (gim.size() < 16) return {};
		const bool big = gim.startsWith(".GIM1.00");
		if (!big && !gim.startsWith("MIG.00.1")) return {};
		auto u16 = [&](qsizetype at) -> quint16 { if (at < 0 || at + 2 > gim.size()) return 0; const auto a = static_cast<uchar>(gim[at]), b = static_cast<uchar>(gim[at + 1]); return big ? quint16((a << 8) | b) : quint16((b << 8) | a); };
		auto u32 = [&](qsizetype at) -> quint32 { if (at < 0 || at + 4 > gim.size()) return none; quint32 a = static_cast<uchar>(gim[at]), b = static_cast<uchar>(gim[at + 1]), c = static_cast<uchar>(gim[at + 2]), d = static_cast<uchar>(gim[at + 3]); return big ? (a << 24) | (b << 16) | (c << 8) | d : (d << 24) | (c << 16) | (b << 8) | a; };
		std::function<qsizetype(qsizetype, qsizetype, int)> find_image = [&](qsizetype start, qsizetype end, int depth) -> qsizetype
		{
			for (qsizetype off = start; depth < 4 && off + 16 <= end;)
			{
				const quint16 id = u16(off); const quint32 size = u32(off + 4), next = u32(off + 8), data = u32(off + 12);
				if (size < 16 || size > quint32(end - off) || data < 16 || data > size) return -1;
				if (id == 4) return off + data;
				if (id == 2 || id == 3) { const qsizetype found = find_image(off + data, off + size, depth + 1); if (found >= 0) return found; }
				if (!next) break;
				off += next;
			}
			return -1;
		};
		const qsizetype image = find_image(16, gim.size(), 0);
		if (image < 0) return {};
		const quint16 format = u16(image + 4), order = u16(image + 6), width = u16(image + 8), height = u16(image + 10), align = std::max<quint16>(1, u16(image + 14));
		const quint32 pixel_offset = u32(image + 0x1c);
		if (order || !width || !height || width > 4096 || height > 4096 || pixel_offset == none) return {};
		const qsizetype first = image + pixel_offset;
		QImage out(width, height, QImage::Format_ARGB32);
		if (format == 3)
		{
			const quint32 row_bytes = width * 4, pitch = ((row_bytes + align - 1) / align) * align;
			if (first + qsizetype(pitch) * (height - 1) + row_bytes > gim.size()) return {};
			for (quint16 y = 0; y < height; ++y)
			{
				const uchar* src = reinterpret_cast<const uchar*>(gim.constData() + first + qsizetype(pitch) * y);
				for (quint16 x = 0; x < width; ++x) out.setPixel(x, y, qRgba(src[x * 4], src[x * 4 + 1], src[x * 4 + 2], src[x * 4 + 3]));
			}
			return out;
		}
		if (format != 8 && format != 9 && format != 10) return {};
		const int bytes = format == 8 ? 8 : 16, blocks_wide = (width + 3) / 4, blocks_high = (height + 3) / 4;
		if (first + qsizetype(blocks_wide) * blocks_high * bytes > gim.size()) return {};
		qsizetype block = first;
		for (int by = 0; by < blocks_high; ++by) for (int bx = 0; bx < blocks_wide; ++bx)
		{
			const qsizetype ca = block + bytes - 8; const quint16 c0 = le16(gim, ca), c1 = le16(gim, ca + 2);
			QRgb colors[4] = {mix565(c0, c1, 1, 0, 1), mix565(c1, c0, 1, 0, 1), 0, 0};
			if (format != 8 || c0 > c1) { colors[2] = mix565(c0, c1, 2, 1, 3); colors[3] = mix565(c0, c1, 1, 2, 3); }
			else { colors[2] = mix565(c0, c1, 1, 1, 2); colors[3] = qRgba(0, 0, 0, 0); }
			const quint32 indices = le32(gim, ca + 4);
			int alpha[8]{}; quint64 alpha_bits = 0;
			if (format == 10)
			{
				alpha[0] = static_cast<uchar>(gim[block]); alpha[1] = static_cast<uchar>(gim[block + 1]);
				if (alpha[0] > alpha[1]) for (int i = 1; i <= 6; ++i) alpha[i + 1] = ((7 - i) * alpha[0] + i * alpha[1]) / 7;
				else { for (int i = 1; i <= 4; ++i) alpha[i + 1] = ((5 - i) * alpha[0] + i * alpha[1]) / 5; alpha[6] = 0; alpha[7] = 255; }
				for (int i = 0; i < 6; ++i) alpha_bits |= quint64(static_cast<uchar>(gim[block + 2 + i])) << (8 * i);
			}
			else if (format == 9) for (int i = 0; i < 8; ++i) alpha_bits |= quint64(static_cast<uchar>(gim[block + i])) << (8 * i);
			for (int i = 0; i < 16; ++i)
			{
				const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2); if (x >= width || y >= height) continue;
				const int ci = (indices >> (2 * i)) & 3; int a = format == 10 ? alpha[(alpha_bits >> (3 * i)) & 7] : format == 9 ? int((alpha_bits >> (4 * i)) & 15) * 17 : colors[ci] == qRgba(0, 0, 0, 0) && c0 <= c1 && ci == 3 ? 0 : 255;
				const QRgb c = colors[ci]; out.setPixel(x, y, qRgba(qRed(c), qGreen(c), qBlue(c), a));
			}
			block += bytes;
		}
		return out;
	}
}

QImage xmb_firmware::decode_gim_image(const QByteArray& bytes)
{
	return decode_gim(bytes);
}

xmb_firmware::xmb_firmware(QString dev_flash) : m_dev_flash(std::move(dev_flash))
{
	const QString font_path = m_dev_flash + QStringLiteral("data/font/SCE-PS3-RD-R-LATIN.TTF");
	const int font_id = QFontDatabase::addApplicationFont(font_path);
	if (font_id >= 0)
	{
		const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
		if (!families.isEmpty()) m_font_family = families.front();
	}
}

bool xmb_firmware::ready(QString& reason)
{
	if (m_font_family.isEmpty())
	{
		reason = QStringLiteral("The PS3 firmware font is missing: dev_flash/data/font/SCE-PS3-RD-R-LATIN.TTF");
		return false;
	}
	const QStringList required_rco{QStringLiteral("xmb_ingame"), QStringLiteral("explore_category_user"),
		QStringLiteral("explore_category_sysconf"), QStringLiteral("explore_category_photo"),
		QStringLiteral("explore_category_music"), QStringLiteral("explore_category_game"),
		QStringLiteral("explore_category_psn"), QStringLiteral("explore_category_network")};
	for (const QString& name : required_rco)
	{
		if (!load(name))
		{
			reason = QStringLiteral("Required PS3 firmware resource is missing or unreadable: dev_flash/vsh/resource/%1.rco").arg(name);
			return false;
		}
	}
	const QStringList required_labels{QStringLiteral("msg_user"), QStringLiteral("msg_setting"), QStringLiteral("msg_photo"),
		QStringLiteral("msg_music"), QStringLiteral("msg_game"), QStringLiteral("msg_net"), QStringLiteral("msg_friend")};
	for (const QString& label : required_labels)
	{
		if (text(QStringLiteral("xmb_ingame"), label).isEmpty())
		{
			reason = QStringLiteral("PS3 firmware XMB label '%1' could not be read from xmb_ingame.rco").arg(label);
			return false;
		}
	}
	const QStringList required_icons{
		QStringLiteral("tex_user"), QStringLiteral("tex_sysconf"), QStringLiteral("tex_photo"),
		QStringLiteral("tex_music"), QStringLiteral("tex_game"), QStringLiteral("tex_network"), QStringLiteral("tex_friend")};
	for (const QString& icon : required_icons)
	{
		if (image(QStringLiteral("xmb_ingame"), icon).isNull())
		{
			reason = QStringLiteral("PS3 firmware XMB icon '%1' could not be decoded from xmb_ingame.rco").arg(icon);
			return false;
		}
	}
	const QList<QPair<QString, QString>> xmb_artwork{
		{QStringLiteral("explore_category_game"), QStringLiteral("item_tex_ps3util")},
		{QStringLiteral("explore_category_user"), QStringLiteral("tex_power_off")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_update")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_game")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_console")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_thema")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_pd")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_display")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_sound")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_network")},
		{QStringLiteral("explore_category_sysconf"), QStringLiteral("tex_security")},
		{QStringLiteral("explore_category_photo"), QStringLiteral("photo_tex_folder")},
		{QStringLiteral("explore_category_music"), QStringLiteral("music_tex_default")},
		{QStringLiteral("explore_category_game"), QStringLiteral("item_tex_plain_folder")},
		{QStringLiteral("explore_category_game"), QStringLiteral("item_tex_ps3logo")},
		{QStringLiteral("explore_category_psn"), QStringLiteral("item_tex_trophy")},
		{QStringLiteral("explore_category_photo"), QStringLiteral("ps3sd_tex_default")},
		{QStringLiteral("explore_category_game"), QStringLiteral("item_tex_edyviewer")},
		{QStringLiteral("explore_category_network"), QStringLiteral("tex_network")},
		{QStringLiteral("explore_category_psn"), QStringLiteral("tex_psn")}
	};
	for (const auto& [rco, icon] : xmb_artwork)
	{
		if (image(rco, icon).isNull())
		{
			reason = QStringLiteral("Required PS3 firmware XMB artwork '%1' is missing or unreadable in %2.rco").arg(icon, rco);
			return false;
		}
	}
	load_wave_files();
	if (wave_file(QStringLiteral("LINE1.mnu")).isEmpty() || wave_file(QStringLiteral("BACKGROUND.mnu")).isEmpty() || wave_strands().isEmpty())
	{
		reason = QStringLiteral("The PS3 firmware XMB wave resources are missing or unreadable under dev_flash/vsh/resource/qgl/lines.qrc");
		return false;
	}
	return true;
}

void xmb_firmware::load_wave_files()
{
	if (m_wave_loaded) return;
	m_wave_loaded = true;
	QFile input(m_dev_flash + QStringLiteral("vsh/resource/qgl/lines.qrc"));
	if (!input.open(QIODevice::ReadOnly) || input.size() < 64 || input.size() > (64 << 20)) return;
	const QByteArray packed = input.readAll();
	if (!packed.startsWith(QByteArrayLiteral("QRCC"))) return;
	const quint32 unpacked_size = be32(packed, 4);
	const QByteArray raw = inflate_block(packed.mid(8), unpacked_size);
	if (raw.size() < 64 || !raw.startsWith(QByteArrayLiteral("QRCF"))) return;
	const quint32 tree_at = be32(raw, 8), names_at = be32(raw, 16), names_size = be32(raw, 20);
	const quint32 data_at = be32(raw, 48), data_size = be32(raw, 52);
	if (tree_at == none || names_at == none || names_size == none || data_at == none || data_size == none ||
		quint64(names_at) + names_size > quint64(raw.size()) || quint64(data_at) + data_size > quint64(raw.size())) return;
	quint32 pos = names_at;
	const quint32 end = names_at + names_size;
	while (pos + 4 < end)
	{
		const quint32 entry = tree_at + be32(raw, pos);
		quint32 label_end = pos + 4;
		while (label_end < end && raw[label_end] != '\0') ++label_end;
		const QString name = QString::fromLatin1(raw.constData() + pos + 4, label_end - pos - 4);
		pos = label_end + 1;
		if (entry + 60 > quint32(raw.size())) continue;
		const quint32 offset = be32(raw, entry + 36), size = be32(raw, entry + 40);
		if (name.isEmpty() || !size || offset == none || quint64(offset) + size > data_size) continue;
		m_wave_files.insert(name, raw.mid(data_at + offset, size));
	}
	const QByteArray settings = m_wave_files.value(QStringLiteral("LINE1.mnu"));
	for (const QByteArray& row : settings.split('\n'))
	{
		const QList<QByteArray> parts = row.trimmed().split(':');
		if (parts.size() == 3 && (parts[1] == "float" || parts[1] == "int"))
		{
			bool ok = false;
			const float value = parts[2].toFloat(&ok);
			if (ok) m_wave_values.insert(QString::fromLatin1(parts[0]), value);
		}
	}
	const QByteArray background = m_wave_files.value(QStringLiteral("BACKGROUND.mnu"));
	for (const QByteArray& row : background.split('\n'))
	{
		const QList<QByteArray> parts = row.trimmed().split(':');
		if (parts.size() == 3 && (parts[1] == "float" || parts[1] == "int"))
		{
			bool ok = false;
			const float value = parts[2].toFloat(&ok);
			if (ok) m_wave_values.insert(QString::fromLatin1(parts[0]), value);
		}
	}
}

QByteArray xmb_firmware::wave_file(const QString& name)
{
	load_wave_files();
	return m_wave_files.value(name);
}

float xmb_firmware::wave_value(const QString& name, float fallback)
{
	load_wave_files();
	return m_wave_values.value(name, fallback);
}

QColor xmb_firmware::wave_colour(int index, const QColor& fallback)
{
	load_wave_files();
	const QString prefix = QString::number(index) + QLatin1Char(' ');
	const float r = m_wave_values.value(prefix + QStringLiteral("RED"), fallback.redF());
	const float g = m_wave_values.value(prefix + QStringLiteral("GREEN"), fallback.greenF());
	const float b = m_wave_values.value(prefix + QStringLiteral("BLUE"), fallback.blueF());
	return QColor::fromRgbF(std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f));
}

QVector<float> xmb_firmware::wave_strands()
{
	load_wave_files();
	const QByteArray tga = m_wave_files.value(QStringLiteral("textures/TGA/freslut1.tga"));
	if (tga.size() < 18) return {};
	const int id = static_cast<uchar>(tga[0]);
	const int width = static_cast<uchar>(tga[12]) | (static_cast<uchar>(tga[13]) << 8);
	const int bytes_per_pixel = static_cast<uchar>(tga[16]) / 8;
	if (width < 1 || bytes_per_pixel < 3 || 18 + id + width * bytes_per_pixel > tga.size()) return {};
	QVector<float> result(512);
	for (int i = 0; i < result.size(); ++i)
		result[i] = static_cast<uchar>(tga[18 + id + std::min(i, width - 1) * bytes_per_pixel + 2]) / 255.f;
	return result;
}

xmb_firmware::container* xmb_firmware::load(const QString& name)
{
	if (m_containers.contains(name)) return &m_containers[name];
	QFile input(m_dev_flash + QStringLiteral("vsh/resource/") + name + QStringLiteral(".rco"));
	if (!input.open(QIODevice::ReadOnly) || input.size() < 0xb0 || input.size() > (32 << 20)) return nullptr;
	container data; data.file = input.readAll();
	if (!data.file.startsWith(QByteArrayLiteral("FRP\0"))) return nullptr;
	if ((be32(data.file, 0x0c) & 0xf0) == 0x10)
	{
		const quint32 packed = be32(data.file, 0xa4), unpacked = be32(data.file, 0xa8);
		if (packed == none || unpacked == none || quint64(0xb0) + packed > quint64(data.file.size())) return nullptr;
		const QByteArray inflated = inflate_block(data.file.mid(0xb0, packed), unpacked);
		if (inflated.isEmpty()) return nullptr;
		data.table = data.file.left(0xa4) + inflated;
	}
	else data.table = data.file;
	const quint32 labels_at = be32(data.table, 0x40), image_table = be32(data.table, 0x24), image_data = be32(data.file, 0x80);
	if (labels_at == none || image_table == none || image_data == none) return nullptr;
	const quint32 count = be32(data.table, image_table + 0x10);
	if (count > 4096 || image_table + header_size > quint32(data.table.size())) return nullptr;
	quint32 entry = image_table + header_size;
	for (quint32 i = 0; i < count; ++i)
	{
		if (entry + header_size > quint32(data.table.size())) break;
		const QString key = label(data.table, labels_at, be32(data.table, entry + 4));
		const quint16 compression = be16(data.table, entry + 0x2a);
		const quint32 packed = be32(data.table, entry + 0x2c), offset = be32(data.table, entry + 0x30);
		const quint32 unpacked = compression ? be32(data.table, entry + 0x38) : packed;
		if (!key.isEmpty() && packed && packed <= (8 << 20) && unpacked && unpacked <= (8 << 20) && quint64(image_data) + offset + packed <= quint64(data.file.size()))
		{
			data.images.insert(key, {offset, packed, unpacked, compression});
		}
		const quint32 next = be32(data.table, entry + 0x14);
		if (!next || quint64(entry) + next > quint64(data.table.size())) break;
		entry += next;
	}
	const quint32 sound_table = be32(data.table, 0x1c), sound_data = be32(data.file, 0x88);
	if (sound_table != none && sound_data != none && sound_table + header_size <= quint32(data.table.size()))
	{
		const quint32 sound_count = be32(data.table, sound_table + 0x10);
		if (sound_count <= 4096)
		{
			quint32 sound_entry = sound_table + header_size;
			for (quint32 i = 0; i < sound_count && sound_entry + header_size <= quint32(data.table.size()); ++i)
			{
				const QString key = label(data.table, labels_at, be32(data.table, sound_entry + 4));
				const quint16 format = be16(data.table, sound_entry + 0x28);
				const quint8 channel_count = sound_entry + 0x2a < quint32(data.table.size()) ? static_cast<quint8>(data.table[sound_entry + 0x2a]) : 0;
				QVector<QByteArray> channels;
				if (format == 1 && !key.isEmpty() && channel_count >= 1 && channel_count <= 2)
				{
					for (quint8 channel = 0; channel < channel_count; ++channel)
					{
						const quint32 size = be32(data.table, sound_entry + 0x34 + 8 * channel);
						const quint32 offset = be32(data.table, sound_entry + 0x38 + 8 * channel);
						if (!size || size > (4 << 20) || quint64(sound_data) + offset + size > quint64(data.file.size())) { channels.clear(); break; }
						channels.push_back(data.file.mid(sound_data + offset, size));
					}
					const QByteArray wav = decode_vag_audio(channels);
					if (!wav.isEmpty()) data.sounds.insert(key, wav);
				}
				const quint32 next = be32(data.table, sound_entry + 0x14);
				if (!next || quint64(sound_entry) + next > quint64(data.table.size())) break;
				sound_entry += next;
			}
		}
	}
	return &m_containers.insert(name, std::move(data)).value();
}

QImage xmb_firmware::image(const QString& rco, const QString& name)
{
	if (auto* resource = load(rco))
	{
		if (resource->decoded_images.contains(name)) return resource->decoded_images.value(name);
		auto it = resource->images.constFind(name);
		// Firmware RCOs vary their asset names by release. Resolve ARMSX3's logical artwork IDs
		// against the equivalent IDs used in installed retail firmware.
		if (it == resource->images.cend() && rco == QStringLiteral("explore_category_photo") && name == QStringLiteral("photo_tex_folder"))
			it = resource->images.constFind(QStringLiteral("photo_tex_album_default"));
		if (it == resource->images.cend() && rco == QStringLiteral("explore_category_game") && name == QStringLiteral("item_tex_edyviewer"))
			it = resource->images.constFind(QStringLiteral("item_tex_ps3util"));
		if (it == resource->images.cend() && rco == QStringLiteral("explore_category_network") && name == QStringLiteral("tex_network"))
			it = resource->images.constFind(QStringLiteral("tex_browser"));
		if (it == resource->images.cend()) return {};
		const quint32 image_data = be32(resource->file, 0x80);
		if (image_data == none || quint64(image_data) + it->offset + it->packed > quint64(resource->file.size())) return {};
		QByteArray bytes = resource->file.mid(image_data + it->offset, it->packed);
		if (it->compression == 1) bytes = inflate_block(bytes, it->unpacked);
		QImage decoded = decode_gim(bytes);
		if (!decoded.isNull()) resource->decoded_images.insert(name, decoded);
		return decoded;
	}
	return {};
}

QByteArray xmb_firmware::sound(const QString& rco, const QString& name)
{
	if (auto* resource = load(rco)) return resource->sounds.value(name);
	return {};
}

QByteArray xmb_firmware::decode_vag_audio(const QVector<QByteArray>& channels)
{
	if (channels.isEmpty() || channels.size() > 2) return {};
	constexpr int coef0[5] = {0, 60, 115, 98, 122};
	constexpr int coef1[5] = {0, 0, -52, -55, -60};
	QVector<QVector<qint16>> decoded;
	int sample_rate = 0;
	for (const QByteArray& vag : channels)
	{
		if (vag.size() < 48 || !vag.startsWith("VAGp")) return {};
		const quint32 size = std::min(be32(vag, 12), quint32(vag.size() - 48));
		const quint32 rate = be32(vag, 16);
		if (rate < 8000 || rate > 96000 || size > (8u << 20)) return {};
		if (sample_rate && sample_rate != static_cast<int>(rate)) return {};
		sample_rate = static_cast<int>(rate);
		QVector<qint16> pcm;
		pcm.reserve(static_cast<qsizetype>(size / 16 * 28));
		int h1 = 0, h2 = 0;
		for (quint32 frame = 48; frame + 16 <= 48 + size; frame += 16)
		{
			const quint8 predictor_shift = static_cast<quint8>(vag[frame]);
			const int predictor = predictor_shift >> 4;
			const int shift = predictor_shift & 0x0f;
			if (static_cast<quint8>(vag[frame + 1]) == 7) break;
			const int c0 = predictor < 5 ? coef0[predictor] : 0;
			const int c1 = predictor < 5 ? coef1[predictor] : 0;
			for (int i = 0; i < 28; ++i)
			{
				const quint8 byte = static_cast<quint8>(vag[frame + 2 + i / 2]);
				const int nibble = (byte >> (4 * (i & 1))) & 0x0f;
				int sample = (nibble >= 8 ? nibble - 16 : nibble) << 12;
				sample >>= shift;
				sample += (h1 * c0 + h2 * c1 + 32) >> 6;
				sample = std::clamp(sample, -32768, 32767);
				pcm.push_back(static_cast<qint16>(sample));
				h2 = h1; h1 = sample;
			}
		}
		decoded.push_back(std::move(pcm));
	}
	qsizetype frames = decoded.front().size();
	for (const auto& channel : decoded) frames = std::min(frames, channel.size());
	if (!frames || !sample_rate) return {};
	const quint16 channel_count = static_cast<quint16>(decoded.size());
	const quint32 data_size = static_cast<quint32>(frames * channel_count * 2);
	QByteArray wav(44 + data_size, '\0');
	std::memcpy(wav.data(), "RIFF", 4); std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
	auto put16 = [&](int at, quint16 value) { wav[at] = static_cast<char>(value); wav[at + 1] = static_cast<char>(value >> 8); };
	auto put32 = [&](int at, quint32 value) { for (int i = 0; i < 4; ++i) wav[at + i] = static_cast<char>(value >> (8 * i)); };
	put32(4, 36 + data_size); put32(16, 16); put16(20, 1); put16(22, channel_count);
	put32(24, sample_rate); put32(28, sample_rate * channel_count * 2); put16(32, channel_count * 2); put16(34, 16);
	std::memcpy(wav.data() + 36, "data", 4); put32(40, data_size);
	qsizetype at = 44;
	for (qsizetype i = 0; i < frames; ++i)
		for (const auto& channel : decoded)
		{
			const quint16 sample = static_cast<quint16>(channel[i]);
			wav[at++] = static_cast<char>(sample); wav[at++] = static_cast<char>(sample >> 8);
		}
	return wav;
}

QString xmb_firmware::text(const QString& rco, const QString& key, int language)
{
	if (auto* resource = load(rco))
	{
		if (resource->strings.contains(key)) return resource->strings.value(key);
		const quint32 labels_at = be32(resource->table, 0x40), text_table = be32(resource->table, 0x18), data_start = be32(resource->file, 0x38), data_size = be32(resource->file, 0x3c);
		if (text_table == none || data_start == none || quint64(data_start) + data_size > quint64(resource->file.size())) return {};
		const quint32 text_count = be32(resource->table, text_table + 0x10);
		if (text_count > 4096) return {};
		quint32 block = data_start, data_end = data_start + data_size;
		QByteArray strings;
		while (block + 16 <= data_end)
		{
			const quint32 next = be32(resource->file, block + 4), packed = be32(resource->file, block + 8), unpacked = be32(resource->file, block + 12);
			if (be16(resource->file, block) == language && packed && quint64(block) + 16 + packed <= data_end) { strings = inflate_block(resource->file.mid(block + 16, packed), unpacked); break; }
			if (!next) break;
			block += next;
		}
		if (strings.isEmpty()) return {};
		quint32 entry = text_table + header_size;
		for (quint32 i = 0; i < text_count && entry + header_size <= quint32(resource->table.size()); ++i)
		{
			if (be16(resource->table, entry + 0x28) == language)
			{
				const quint16 encoding = be16(resource->table, entry + 0x2a);
				const quint32 n = be32(resource->table, entry + 0x2c);
				if (n > 8192) return {};
				for (quint32 k = 0; k < n; ++k)
				{
					const quint32 at = entry + 0x30 + 12 * k;
					const QString name = label(resource->table, labels_at, be32(resource->table, at));
					const quint32 length = be32(resource->table, at + 4), offset = be32(resource->table, at + 8);
					if (name != key || quint64(offset) + length > quint64(strings.size())) continue;
					const QByteArray slice = strings.mid(offset, length);
					QString value;
					if (encoding == 1)
					{
						QVector<ushort> units(slice.size() / 2);
						for (qsizetype j = 0; j < units.size(); ++j) units[j] = qFromBigEndian<ushort>(reinterpret_cast<const uchar*>(slice.constData() + j * 2));
						value = QString::fromUtf16(reinterpret_cast<const char16_t*>(units.constData()), units.size());
					}
					else value = QString::fromUtf8(slice);
					value.remove(QChar::Null);
					resource->strings.insert(key, value);
					return value;
				}
			}
			const quint32 next = be32(resource->table, entry + 0x14); if (!next) break; entry += next;
		}
	}
	return {};
}
