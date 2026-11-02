#include "stdafx.h"
#include "remote_play_lobby.h"

#include <curl/curl.h>
#include <QRegularExpression>

namespace
{
	constexpr auto lobby_base = "https://remote-play.armsx2mail.workers.dev/v1/armsx3/lobbies";

	size_t append_response(char* data, size_t size, size_t count, void* userdata)
	{
		const size_t bytes = size * count;
		static_cast<QByteArray*>(userdata)->append(data, static_cast<qsizetype>(bytes));
		return bytes;
	}

	int should_cancel(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		const auto* cancel = static_cast<const std::atomic_bool*>(userdata);
		return cancel && cancel->load(std::memory_order_acquire) ? 1 : 0;
	}
}

std::optional<QString> remote_play_lobby::normalize(const QString& text)
{
	QString code = text.toUpper();
	code.remove(QRegularExpression(QStringLiteral("[\\s-]")));
	if (code.size() != 6) return std::nullopt;
	for (const QChar c : code)
		if (!QStringLiteral("ABCDEFGHJKMNPQRSTUVWXYZ23456789").contains(c)) return std::nullopt;
	return code;
}

QString remote_play_lobby::display(const QString& code)
{
	return code.size() == 6 ? code.left(3) + QLatin1Char('-') + code.mid(3) : code;
}

remote_play_lobby::response remote_play_lobby::request(const QString& method, const QString& path, const QByteArray& body, const QString& token, const std::atomic_bool* cancel)
{
	static const int curl_initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
	if (curl_initialized != CURLE_OK) return {};

	CURL* handle = curl_easy_init();
	if (!handle) return {};
	response result;
	const QByteArray url = QByteArray(lobby_base) + path.toUtf8();
	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Accept: application/json");
	if (!body.isEmpty()) headers = curl_slist_append(headers, "Content-Type: application/json");
	QByteArray authorization;
	if (!token.isEmpty())
	{
		authorization = "Authorization: Bearer " + token.toUtf8();
		headers = curl_slist_append(headers, authorization.constData());
	}
	curl_easy_setopt(handle, CURLOPT_URL, url.constData());
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 10000L);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
	if (cancel)
	{
		curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, should_cancel);
		curl_easy_setopt(handle, CURLOPT_XFERINFODATA, const_cast<std::atomic_bool*>(cancel));
	}
	if (method == QStringLiteral("POST"))
	{
		curl_easy_setopt(handle, CURLOPT_POST, 1L);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.constData());
		curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	}
	else if (method != QStringLiteral("GET"))
	{
		const QByteArray verb = method.toUtf8();
		curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, verb.constData());
		if (!body.isEmpty())
		{
			curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.constData());
			curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
		}
	}
	if (curl_easy_perform(handle) == CURLE_OK) curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(handle);
	return result;
}
