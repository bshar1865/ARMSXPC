#pragma once

#include <QImage>
#include <QHash>
#include <QString>

// Reads PS3 P3TF theme tables using the same element/attribute contract as ARMSX3.
// The parser bounds every table and file reference because themes are untrusted input.
class xmb_p3t final
{
public:
	struct result
	{
		QString name;
		QImage background;
		QHash<QString, QImage> icons;
		bool dynamic = false;
		QString error;
	};

	static result read(const QString& path);
};
