#pragma once

#include <QString>

#include <atomic>
#include <optional>

namespace remote_play_lobby
{
	struct response
	{
		long status = 0;
		QByteArray body;
	};

	std::optional<QString> normalize(const QString& text);
	QString display(const QString& code);
	response request(const QString& method, const QString& path, const QByteArray& body = {}, const QString& token = {}, const std::atomic_bool* cancel = nullptr);
}
