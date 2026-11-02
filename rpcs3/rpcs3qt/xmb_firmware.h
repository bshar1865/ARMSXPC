#pragma once

#include <QImage>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <QColor>

// Reads the same installed PS3 firmware RCO/GIM assets that the Android XMB uses.
class xmb_firmware final
{
public:
	explicit xmb_firmware(QString dev_flash);
	// XMB uses firmware labels, icons, font and background-wave resources directly. There is no
	// substitute UI when this check fails.
	bool ready(QString& reason);
	QImage image(const QString& rco, const QString& name);
	QString text(const QString& rco, const QString& key, int language = 1);
	QByteArray wave_file(const QString& name);
	float wave_value(const QString& name, float fallback);
	QColor wave_colour(int index, const QColor& fallback);
	QVector<float> wave_strands();
	static QImage decode_gim_image(const QByteArray& bytes);
	QString font_family() const { return m_font_family; }

private:
	struct image_entry
	{
		quint32 offset = 0;
		quint32 packed = 0;
		quint32 unpacked = 0;
		quint16 compression = 0;
	};
	struct container
	{
		QByteArray file;
		QByteArray table;
		QHash<QString, image_entry> images;
		QHash<QString, QImage> decoded_images;
		QHash<QString, QString> strings;
	};
	QString m_dev_flash;
	QString m_font_family;
	QHash<QString, QByteArray> m_wave_files;
	QHash<QString, float> m_wave_values;
	bool m_wave_loaded = false;
	QHash<QString, container> m_containers;
	container* load(const QString& name);
	void load_wave_files();
};
