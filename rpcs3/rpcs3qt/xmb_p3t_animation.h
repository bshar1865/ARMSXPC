#pragma once

#include <QImage>
#include <QVector>
#include <QString>

// Extracts the full-screen DXT layers used by ARMSX3's P3T slideshow themes.
class xmb_p3t_animation final
{
public:
	struct result
	{
		QVector<QImage> frames;
		qreal interval = 10.0;
	qreal fade = 1.5;
	};

	static result read(const QString& path, quint64 offset, quint64 size);
};
