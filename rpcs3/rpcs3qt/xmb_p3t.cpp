#include "stdafx.h"
#include "xmb_p3t.h"
#include "xmb_firmware.h"

#include <QFile>
#include <QBuffer>
#include <QImageReader>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <optional>
#include <zlib.h>

namespace
{
	constexpr quint32 magic = 0x50335446; // P3TF
	constexpr qsizetype header_bytes = 56;
	constexpr qsizetype element_bytes = 28;
	constexpr qsizetype attribute_bytes = 16;
	constexpr quint32 type_string = 3;
	constexpr quint32 type_int = 1;
	constexpr quint32 type_file = 6;
	constexpr quint32 type_id = 7;
	constexpr quint64 max_table = 16ull << 20;
	constexpr quint64 max_picture = 32ull << 20;
	constexpr quint64 max_icon = 8ull << 20;
	constexpr quint64 max_inflated = 64ull << 20;
	constexpr quint64 max_packed_preview = 16ull << 20;

	quint32 be32(const QByteArray& bytes, qsizetype at)
	{
		if (at < 0 || at + 4 > bytes.size()) return 0xffffffffu;
		return qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + at));
	}

	QString c_string(const QByteArray& table, quint32 offset)
	{
		if (offset >= static_cast<quint32>(table.size())) return {};
		const qsizetype start = static_cast<qsizetype>(offset);
		qsizetype end = start;
		while (end < table.size() && end - start < 256 && table[end] != '\0') ++end;
		return QString::fromLatin1(table.constData() + start, end - start);
	}

	bool range_ok(quint64 offset, quint64 size, quint64 limit)
	{
		return offset <= limit && size <= limit - offset;
	}

	struct table_ref { quint64 offset = 0; quint64 size = 0; };

	QByteArray inflate_capped(const QByteArray& packed, quint64 cap)
	{
		if (packed.isEmpty() || cap == 0 || cap > max_inflated) return {};
		z_stream stream{};
		stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.constData()));
		stream.avail_in = static_cast<uInt>(packed.size());
		if (inflateInit(&stream) != Z_OK) return {};
		QByteArray out;
		std::array<char, 64 * 1024> chunk{};
		int status = Z_OK;
		while (status == Z_OK)
		{
			stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
			stream.avail_out = static_cast<uInt>(chunk.size());
			status = inflate(&stream, Z_NO_FLUSH);
			const qsizetype made = static_cast<qsizetype>(chunk.size() - stream.avail_out);
			if (static_cast<quint64>(out.size()) + static_cast<quint64>(made) > cap) { status = Z_MEM_ERROR; break; }
			out.append(chunk.data(), made);
		}
		inflateEnd(&stream);
		return status == Z_STREAM_END ? out : QByteArray{};
	}
}

xmb_p3t::result xmb_p3t::read(const QString& path)
{
	result out;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) { out.error = file.errorString(); return out; }
	const quint64 file_size = static_cast<quint64>(file.size());
	if (file_size < static_cast<quint64>(header_bytes)) { out.error = QStringLiteral("File is too small to be a PS3 theme."); return out; }
	const QByteArray header = file.read(header_bytes);
	if (header.size() != header_bytes || be32(header, 0) != magic) { out.error = QStringLiteral("This file is not a PS3 P3T theme."); return out; }

	std::array<table_ref, 6> tables{};
	for (qsizetype i = 0; i < static_cast<qsizetype>(tables.size()); ++i)
	{
		const quint64 offset = be32(header, 8 + i * 8);
		const quint64 size = be32(header, 12 + i * 8);
		if (!range_ok(offset, size, file_size)) { out.error = QStringLiteral("Theme table points outside the file."); return out; }
		tables[i] = {offset, size};
	}
	const auto elements_ref = tables[0];
	const auto ids_ref = tables[1];
	const auto strings_ref = tables[2];
	const auto files_ref = tables[5];
	if (elements_ref.size > max_table || ids_ref.size > max_table || strings_ref.size > max_table || elements_ref.size > INT_MAX || ids_ref.size > INT_MAX || strings_ref.size > INT_MAX)
	{ out.error = QStringLiteral("Theme tables exceed safe size limits."); return out; }
	if (!file.seek(static_cast<qint64>(elements_ref.offset))) { out.error = QStringLiteral("Could not read the theme element table."); return out; }
	const QByteArray elements = file.read(static_cast<qint64>(elements_ref.size));
	if (!file.seek(static_cast<qint64>(ids_ref.offset))) { out.error = QStringLiteral("Could not read the theme id table."); return out; }
	const QByteArray ids = file.read(static_cast<qint64>(ids_ref.size));
	if (!file.seek(static_cast<qint64>(strings_ref.offset))) { out.error = QStringLiteral("Could not read the theme string table."); return out; }
	const QByteArray strings = file.read(static_cast<qint64>(strings_ref.size));
	if (static_cast<quint64>(elements.size()) != elements_ref.size || static_cast<quint64>(ids.size()) != ids_ref.size || static_cast<quint64>(strings.size()) != strings_ref.size)
	{ out.error = QStringLiteral("Theme tables are truncated."); return out; }

	struct candidate { quint64 offset; quint32 size; quint64 pixels; };
	std::optional<candidate> hd, sd;
	quint64 preview_offset = 0;
	quint32 preview_size = 0;
	quint64 position = 0;
	while (position + element_bytes <= static_cast<quint64>(elements.size()))
	{
		const qsizetype at = static_cast<qsizetype>(position);
		const QString element = c_string(strings, be32(elements, at));
		const quint32 count = be32(elements, at + 4);
		if (count > (static_cast<quint64>(elements.size()) - position - element_bytes) / attribute_bytes)
		{ out.error = QStringLiteral("Theme element has an invalid attribute count."); return out; }
		const qsizetype attrs = at + element_bytes;
		if (element == QStringLiteral("bgimage") || element == QStringLiteral("info") || element == QStringLiteral("icon"))
		{
			for (quint32 i = 0; i < count; ++i)
			{
				const qsizetype a = attrs + static_cast<qsizetype>(i) * attribute_bytes;
				const QString attr = c_string(strings, be32(elements, a));
				const quint32 type = be32(elements, a + 4);
				const quint64 offset = be32(elements, a + 8);
				const quint64 size = be32(elements, a + 12);
				if (element == QStringLiteral("info"))
				{
					if (attr == QStringLiteral("name") && type == type_string)
					{
						const quint64 string_offset = offset, string_size = size;
						if (range_ok(string_offset, string_size, static_cast<quint64>(strings.size())) && string_size <= 256)
							out.name = QString::fromUtf8(strings.constData() + string_offset, static_cast<qsizetype>(string_size)).trimmed();
					}
					else if (attr == QStringLiteral("preview") && type == type_file && size <= max_packed_preview && range_ok(offset, size, files_ref.size))
					{ preview_offset = files_ref.offset + offset; preview_size = static_cast<quint32>(size); }
					continue;
				}
				if (type != type_file) continue;
				if (attr == QStringLiteral("anim"))
				{
					out.dynamic = true;
					if (size >= 8 && range_ok(offset, size, files_ref.size))
					{
						out.animation_offset = files_ref.offset + offset;
						out.animation_size = size;
					}
					continue;
				}
				if (attr != QStringLiteral("hd") && attr != QStringLiteral("sd")) continue;
				if (size < 4 || size > max_picture || !range_ok(offset, size, files_ref.size)) continue;
				if (!file.seek(static_cast<qint64>(files_ref.offset + offset))) continue;
				const QByteArray bytes = file.read(static_cast<qint64>(size));
				if (static_cast<quint64>(bytes.size()) != size) continue;
				QBuffer buffer;
				buffer.setData(bytes);
				buffer.open(QIODevice::ReadOnly);
				QImageReader reader(&buffer);
				const QSize dimensions = reader.size();
				if (!dimensions.isValid() || dimensions.width() > 16384 || dimensions.height() > 16384) continue;
				candidate c{files_ref.offset + offset, static_cast<quint32>(size), static_cast<quint64>(dimensions.width()) * dimensions.height()};
				auto& target = attr == QStringLiteral("hd") ? hd : sd;
				if (!target || c.pixels > target->pixels) target = c;
			}
		}
		position += element_bytes + static_cast<quint64>(count) * attribute_bytes;
	}

	const auto selected = hd ? hd : sd;
	if (selected)
	{
		if (!file.seek(static_cast<qint64>(selected->offset))) { out.error = QStringLiteral("Could not read the theme background."); return out; }
		const QByteArray data = file.read(selected->size);
		QImage image;
		if (static_cast<quint32>(data.size()) != selected->size || !image.loadFromData(data))
		{ out.error = QStringLiteral("Theme background image is damaged or unsupported."); return out; }
		out.background = image;
	}
	// XMB icons are carried in the same element stream as bgimage. Extract their GIM payloads
	// and decode them through the firmware GIM reader, matching XmbThemeAssets.extract on Android.
	position = 0;
	while (position + element_bytes <= static_cast<quint64>(elements.size()))
	{
		const qsizetype at = static_cast<qsizetype>(position);
		const QString element = c_string(strings, be32(elements, at));
		const quint32 count = be32(elements, at + 4);
		if (count > (static_cast<quint64>(elements.size()) - position - element_bytes) / attribute_bytes) break;
		if (element == QStringLiteral("icon") || element == QStringLiteral("info"))
		{
			QString id = element == QStringLiteral("info") ? QStringLiteral("theme") : QString();
			quint64 data_offset = 0, data_size = 0, unpacked = 0;
			for (quint32 i = 0; i < count; ++i)
			{
				const qsizetype a = at + element_bytes + static_cast<qsizetype>(i) * attribute_bytes;
				const QString attr = c_string(strings, be32(elements, a));
				const quint32 type = be32(elements, a + 4);
				const quint64 first = be32(elements, a + 8), second = be32(elements, a + 12);
				if (attr == QStringLiteral("id") && type == type_id && range_ok(first + 4, 1, static_cast<quint64>(ids.size()))) id = c_string(ids, static_cast<quint32>(first + 4));
				else if (((attr == QStringLiteral("src") && element == QStringLiteral("icon")) || (attr == QStringLiteral("icon") && element == QStringLiteral("info"))) && type == type_file)
				{ data_offset = first; data_size = second; }
				else if (attr == QStringLiteral("size") && type == type_int) unpacked = first;
			}
			if (!id.isEmpty() && data_size >= 4 && data_size <= max_icon && range_ok(data_offset, data_size, files_ref.size))
			{
				if (file.seek(static_cast<qint64>(files_ref.offset + data_offset)))
				{
					const QByteArray packed = file.read(static_cast<qint64>(data_size));
					const QByteArray gim = inflate_capped(packed, unpacked > 0 ? std::min(unpacked, max_icon) : max_icon);
					const QImage icon = xmb_firmware::decode_gim_image(gim);
					if (!icon.isNull()) out.icons.insert(id, icon);
				}
			}
		}
		else if (element == QStringLiteral("se"))
		{
			QString id;
			QVector<QPair<quint64, quint64>> channels;
			for (quint32 i = 0; i < count; ++i)
			{
				const qsizetype a = at + element_bytes + static_cast<qsizetype>(i) * attribute_bytes;
				const QString attr = c_string(strings, be32(elements, a));
				const quint32 type = be32(elements, a + 4);
				const quint64 first = be32(elements, a + 8), second = be32(elements, a + 12);
				if (attr == QStringLiteral("id") && type == type_id && range_ok(first + 4, 1, static_cast<quint64>(ids.size())))
					id = c_string(ids, static_cast<quint32>(first + 4));
				else if ((attr == QStringLiteral("left") || attr == QStringLiteral("right")) && type == type_file)
					channels.push_back({first, second});
			}
			QVector<QByteArray> vag_channels;
			for (const auto& [offset, size] : channels)
			{
				if (size < 48 || size > max_icon || !range_ok(offset, size, files_ref.size)) { vag_channels.clear(); break; }
				if (!file.seek(static_cast<qint64>(files_ref.offset + offset))) { vag_channels.clear(); break; }
				const QByteArray bytes = file.read(static_cast<qint64>(size));
				if (static_cast<quint64>(bytes.size()) != size) { vag_channels.clear(); break; }
				vag_channels.push_back(bytes);
			}
			if (!id.isEmpty() && !vag_channels.isEmpty())
			{
				const QByteArray wav = xmb_firmware::decode_vag_audio(vag_channels);
				if (!wav.isEmpty()) out.sounds.insert(id, wav);
			}
		}
		position += element_bytes + static_cast<quint64>(count) * attribute_bytes;
	}
	if (out.background.isNull() && preview_size && file.seek(static_cast<qint64>(preview_offset)))
	{
		const QByteArray packed = file.read(preview_size);
		const QImage preview = xmb_firmware::decode_gim_image(inflate_capped(packed, max_inflated));
		if (!preview.isNull()) out.background = preview;
	}
	if (out.dynamic && out.background.isNull()) out.error = QStringLiteral("Animated P3T scenes are not supported yet.");
	else if (out.background.isNull()) out.error = QStringLiteral("Theme contains no supported HD or SD background image.");
	return out;
}
