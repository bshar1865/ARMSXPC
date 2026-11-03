#include "stdafx.h"
#include "xmb_mode_dialog.h"
#include "xmb_firmware.h"
#include "xmb_p3t.h"
#include "xmb_p3t_animation.h"
#include "xmb_wave_model.h"
#include "xmb_wave_widget.h"
#include "category.h"

#include <QDateTime>
#include <QFont>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QLinearGradient>
#include <QEasingCurve>
#include <QtMath>
#include <QSettings>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QRandomGenerator>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include "Loader/ISO.h"
#include "Emu/vfs_config.h"
#include "Emu/system_utils.hpp"

namespace
{
	void draw_cover(QPainter& painter, const QRectF& target, const QImage& image)
	{
		if (image.isNull() || target.width() <= 0 || target.height() <= 0) return;
		QRectF source(image.rect());
		const qreal source_aspect = source.width() / source.height();
		const qreal target_aspect = target.width() / target.height();
		if (source_aspect > target_aspect)
		{
			const qreal width = source.height() * target_aspect;
			source.setLeft((source.width() - width) / 2);
			source.setWidth(width);
		}
		else
		{
			const qreal height = source.width() / target_aspect;
			source.setTop((source.height() - height) / 2);
			source.setHeight(height);
		}
		painter.drawImage(target, image, source);
	}

	QImage load_game_icon_image(const game_info& game)
	{
		if (!game) return {};
		const auto [path, in_archive] = rpcs3::utils::get_game_content_path(game_content_type::content_icon, *game);
		if (path.empty()) return {};
		if (!in_archive) return QImageReader(QString::fromStdString(path)).read();
		iso_archive archive(game->path);
		if (!archive.is_valid()) return {};
		auto stream = archive.open(path);
		if (!stream) return {};
		const u64 size = stream->size();
		if (!size || size > (16ull << 20)) return {};
		QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
		if (stream->read(data.data(), size) != size) return {};
		QImage image;
		image.loadFromData(data, "PNG");
		return image;
	}

	struct xmb_gradient { qreal angle; QColor start; QColor end; };
	const std::array<xmb_gradient, 12> original_gradients{{
		{90.25, QColor(0xC5,0xC5,0xC5), QColor(0xC9,0xC9,0xC9)}, {67, QColor(0xCB,0x9E,0x0D), QColor(0xDB,0xD6,0x29)},
		{106, QColor(0x8E,0xBE,0x28), QColor(0x68,0xA8,0x16)}, {136.75, QColor(0xD8,0xB6,0xB6), QColor(0xE7,0x42,0x75)},
		{1.5, QColor(0x13,0x6C,0x13), QColor(0x18,0x9C,0x18)}, {148.75, QColor(0xC6,0x78,0xEE), QColor(0x67,0x4D,0xA1)},
		{26.5, QColor(0x00,0xA7,0x92), QColor(0x0A,0xF0,0xEF)}, {62.5, QColor(0x00,0x00,0x5F), QColor(0x21,0xD9,0xFF)},
		{148.5, QColor(0x92,0x2C,0x9B), QColor(0xD9,0x62,0xEC)}, {160, QColor(0xAD,0xB3,0x27), QColor(0xD0,0xBF,0x3C)},
		{90, QColor(0x73,0x44,0x14), QColor(0x9A,0x76,0x2F)}, {170.5, QColor(0xEC,0x44,0x2D), QColor(0xD6,0x3F,0x2B)}
	}};
	void fill_original_background(QPainter& p, qreal w, qreal h, int pinned_month)
	{
		const QDateTime now = QDateTime::currentDateTime();
		int month = pinned_month >= 0 && pinned_month < 12 ? pinned_month : now.date().month() - 1;
		qreal angle = original_gradients[month].angle;
		QColor start = original_gradients[month].start;
		QColor end = original_gradients[month].end;
		if (pinned_month == 12)
		{
			constexpr qreal cycle_month_ms = 20000.0;
			const qreal t = std::fmod(QDateTime::currentMSecsSinceEpoch(), 12.0 * cycle_month_ms) / cycle_month_ms;
			month = static_cast<int>(t) % 12;
			const qreal phase = t - std::floor(t);
			const qreal x = std::clamp((phase - .75) / .25, 0.0, 1.0);
			const qreal blend = x * x * (3.0 - 2.0 * x);
			const auto& a = original_gradients[month];
			const auto& b = original_gradients[(month + 1) % 12];
			angle = a.angle + (b.angle - a.angle) * blend;
			start = QColor::fromRgbF(a.start.redF() + (b.start.redF() - a.start.redF()) * blend,
				a.start.greenF() + (b.start.greenF() - a.start.greenF()) * blend,
				a.start.blueF() + (b.start.blueF() - a.start.blueF()) * blend);
			end = QColor::fromRgbF(a.end.redF() + (b.end.redF() - a.end.redF()) * blend,
				a.end.greenF() + (b.end.greenF() - a.end.greenF()) * blend,
				a.end.blueF() + (b.end.blueF() - a.end.blueF()) * blend);
		}
		const qreal hour = now.time().hour() + now.time().minute() / 60.0;
		qreal night = 0;
		const auto ease = [](qreal x) { x = std::clamp(x, 0.0, 1.0); return x*x*(3-2*x); };
		if (hour < 4) night = 1; else if (hour < 6.5) night = 1-ease((hour-4)/2.5); else if (hour >= 23) night = ease(hour-23);
		const qreal start_scale = 1 + (.10-1)*night, end_scale = 1 + (.30-1)*night;
		const auto scale_color = [](QColor c, qreal f) { return QColor::fromRgbF(c.redF()*f, c.greenF()*f, c.blueF()*f); };
		const qreal radians = qDegreesToRadians(angle);
		const QPointF dir(std::sin(radians), -std::cos(radians));
		const std::array<QPointF, 4> corners{QPointF(0,0), QPointF(w,0), QPointF(0,h), QPointF(w,h)};
		qreal lo = std::numeric_limits<qreal>::max(), hi = std::numeric_limits<qreal>::lowest();
		for (const auto& c : corners) { const qreal v = QPointF::dotProduct(c, dir); lo=std::min(lo,v); hi=std::max(hi,v); }
		const QPointF center(w / 2, h / 2);
		const qreal center_projection = QPointF::dotProduct(center, dir);
		QLinearGradient bg(center + dir * (lo - center_projection), center + dir * (hi - center_projection));
		bg.setColorAt(0, scale_color(start, start_scale)); bg.setColorAt(1, scale_color(end, end_scale));
		p.fillRect(QRectF(0,0,w,h), bg);
	}

	void fill_simple_wave(QPainter& p, qreal w, qreal h, qreal t, int pinned_month)
	{
		int month = pinned_month >= 0 && pinned_month < 12 ? pinned_month : QDate::currentDate().month() - 1;
		if (pinned_month == 12) month = static_cast<int>(std::fmod(QDateTime::currentMSecsSinceEpoch(), 240000.0) / 20000.0) % 12;
		const QColor base = original_gradients[month].end;
		constexpr int samples = 64;
		const qreal step = w / samples;
		const qreal two_pi = 2.0 * qAcos(-1.0);
		for (int layer = 0; layer < 4; ++layer)
		{
			const qreal f = layer / 3.0;
			const qreal base_y = h * (.40 + .16 * f);
			const qreal amp = h * (.05 + .028 * (1.0 - f));
			const qreal length = 1.05 + .5 * f;
			const qreal phase = t * (.26 + .14 * f) + layer * 2.2;
			const auto wave_y = [=](qreal nx)
			{
				return base_y + amp * std::sin(nx * length * two_pi + phase)
					+ amp * .34 * std::sin(nx * length * 2.1 * two_pi - phase * 1.35 + layer);
			};
			QPainterPath body;
			body.moveTo(0, h + 2);
			for (int i = 0; i <= samples; ++i) body.lineTo(i * step, wave_y(qreal(i) / samples));
			body.lineTo(w, h + 2); body.closeSubpath();
			const QColor tint = base.lighter(130 + int(22 * f));
			QLinearGradient fill(0, base_y - amp * 1.6, 0, h);
			fill.setColorAt(0, QColor(tint.red(), tint.green(), tint.blue(), 0));
			fill.setColorAt(.05, QColor(tint.red(), tint.green(), tint.blue(), 26 + int(13 * f)));
			fill.setColorAt(.55, QColor(base.red(), base.green(), base.blue(), 10 + int(8 * f)));
			fill.setColorAt(1, QColor(base.red(), base.green(), base.blue(), 0));
			p.fillPath(body, fill);
			QPainterPath crest;
			for (int i = 0; i <= samples; ++i)
			{
				const QPointF point(i * step, wave_y(qreal(i) / samples));
				if (!i) crest.moveTo(point); else crest.lineTo(point);
			}
			QColor line = tint; line.setAlpha(18 + int(18 * f));
			p.setPen(QPen(line, std::min(w, h) * (.008 + .004 * f), Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush); p.drawPath(crest);
		}
	}
	// Port of XmbWordmark.three()/markBadge() from ARMSX3's Android XMB. Keep these
	// dimensions in letter heights so the Windows badge follows the source geometry exactly.
	QPainterPath armsx3_three(qreal w, qreal t, qreal ro, qreal notch)
	{
		const qreal ri = .035;
		const qreal rc = std::min(ro - t, .5 - 1.5 * t - ri);
		QPainterPath path;
		auto arc = [&](qreal cx, qreal cy, qreal r, qreal android_start, qreal android_sweep)
		{
			path.arcTo(QRectF(cx-r, cy-r, 2*r, 2*r), -android_start, -android_sweep);
		};
		path.moveTo(0, 0); path.lineTo(w-ro, 0); arc(w-ro, ro, ro, 270, 90);
		path.lineTo(w, .5-notch); arc(w-notch, .5-notch, notch, 0, 90); arc(w-notch, .5+notch, notch, 270, 90);
		path.lineTo(w, 1-ro); arc(w-ro, 1-ro, ro, 0, 90); path.lineTo(0, 1); path.lineTo(0, 1-t);
		path.lineTo(w-t-rc, 1-t); arc(w-t-rc, 1-t-rc, rc, 90, -90);
		path.lineTo(w-t, .5+t/2+ri); arc(w-t-ri, .5+t/2+ri, ri, 0, -90);
		path.lineTo(0, .5+t/2); path.lineTo(0, .5-t/2); path.lineTo(w-t-ri, .5-t/2);
		arc(w-t-ri, .5-t/2-ri, ri, 90, -90); path.lineTo(w-t, t+rc); arc(w-t-rc, t+rc, rc, 0, -90);
		path.lineTo(0, t); path.closeSubpath();
		return path;
	}

	void draw_armsx3_mark(QPainter& p, const QRectF& box, bool badge)
	{
		p.save();
		if (badge)
		{
			const qreal r = box.width() * .46, c = box.center().x();
			QRadialGradient disc(c, box.center().y() - r * .35, r * 1.25);
			disc.setColorAt(0, QColor(0xB2, 0x8C, 0xFF)); disc.setColorAt(.55, QColor(0x6A, 0x2E, 0xD6)); disc.setColorAt(1, QColor(0x3A, 0x10, 0x86));
			p.setPen(Qt::NoPen); p.setBrush(disc); p.drawEllipse(box.center(), r, r);
			QLinearGradient gloss(0, box.center().y()-r, 0, box.center().y());
			gloss.setColorAt(0, QColor(255,255,255,0x70)); gloss.setColorAt(1, QColor(255,255,255,0));
			p.setBrush(gloss); p.drawEllipse(QRectF(c-r*.82, box.center().y()-r*.94, r*1.64, r*1.04));
			p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(255,255,255,0x90), std::max(1.0, box.width()*.012)));
			p.drawEllipse(box.center(), r-box.width()*.006, r-box.width()*.006);
		}
		const qreal h = badge ? box.width()*.46*.95 : box.height()*.56;
		const qreal w = 1.5*h;
		const QPointF center = box.center();
		p.translate(center.x()-w/2, center.y()-h/2); p.scale(h, h);
		const QPainterPath mark = armsx3_three(1.5, badge ? .25 : .21, badge ? .38 : .34, badge ? .10 : .09);
		QLinearGradient ink(0, 0, 0, 1); ink.setColorAt(0, Qt::white); ink.setColorAt(1, badge ? QColor(0xE6,0xE0,0xF4) : QColor(0xD8,0xD8,0xD8));
		p.setPen(Qt::NoPen); p.setBrush(ink); p.drawPath(mark); p.restore();
	}
}

xmb_mode_dialog::xmb_mode_dialog(const std::vector<game_info>& games, bool in_game, game_info running_game, QWidget* parent)
	: QDialog(parent)
	, m_in_game(in_game)
	, m_running_game(std::move(running_game))
{
	setWindowTitle(tr("ARMSXPC XMB"));
	setMinimumSize(900, 506);
	setWindowState(windowState() | Qt::WindowMaximized);
	if (m_in_game)
	{
		setWindowFlag(Qt::FramelessWindowHint, true);
		setAttribute(Qt::WA_TranslucentBackground, true);
		setAutoFillBackground(false);
	}
	setFocusPolicy(Qt::StrongFocus);
	m_categories = { tr("Users"), tr("Settings"), tr("Photo"), tr("Music"), tr("Game"), tr("Tools"), tr("Network"), tr("ARMSX3"), tr("RPCN") };
	m_focus.fill(0, m_categories.size());
	m_focus[0] = 1;
	QSettings preferences;
	preferences.beginGroup(QStringLiteral("armsxpc/xmb"));
	m_custom_background = preferences.value(QStringLiteral("background")).toString();
	m_p3t_theme_path = preferences.value(QStringLiteral("p3tTheme")).toString();
	if (!m_p3t_theme_path.isEmpty())
	{
		const auto theme = xmb_p3t::read(m_p3t_theme_path);
		if (!theme.background.isNull())
		{
			m_p3t_background = theme.background; m_p3t_theme_name = theme.name; m_p3t_icons = theme.icons;
			if (theme.dynamic && theme.animation_size)
			{
				const auto animation = xmb_p3t_animation::read(m_p3t_theme_path, theme.animation_offset, theme.animation_size);
				m_p3t_slides = animation.frames; m_p3t_slide_interval = animation.interval; m_p3t_slide_fade = animation.fade;
				if (!m_p3t_slides.isEmpty()) { m_p3t_slide_start = QRandomGenerator::global()->bounded(m_p3t_slides.size()); m_p3t_background = m_p3t_slides[m_p3t_slide_start]; m_p3t_slide_clock.start(); }
			}
		}
		else { m_p3t_theme_path.clear(); preferences.remove(QStringLiteral("p3tTheme")); }
	}
	m_background_mode = std::clamp(preferences.value(QStringLiteral("backgroundMode"), 0).toInt(), 0, 2);
	m_theme_month = std::clamp(preferences.value(QStringLiteral("themeMonth"), -1).toInt(), -1, 12);
	if (preferences.value(QStringLiteral("themeMonth"), -1).toInt() < 0) m_theme_month = -1;
	m_accent_color = preferences.value(QStringLiteral("accent"), QColor(255, 255, 255)).value<QColor>();
	m_show_clock = preferences.value(QStringLiteral("showClock"), true).toBool();
	m_show_game_version = preferences.value(QStringLiteral("showGameVersion"), false).toBool();
	m_show_game_region = preferences.value(QStringLiteral("showGameRegion"), false).toBool();
	m_show_game_backgrounds = preferences.value(QStringLiteral("showGameBackgrounds"), true).toBool();
	m_column = std::clamp(preferences.value(QStringLiteral("column"), 5).toInt(), 0, static_cast<int>(m_categories.size()) - 1);
	for (int i = 0; i < m_focus.size(); ++i) m_focus[i] = std::max(0, preferences.value(QStringLiteral("focus/%1").arg(i), m_focus[i]).toInt());
	m_visual_column = static_cast<float>(m_column);
	m_firmware = std::make_unique<xmb_firmware>(QString::fromStdString(g_cfg_vfs.get_dev_flash()));
	m_default_game_icon = m_firmware->image("explore_category_game", "item_tex_ps3util");
	m_wave_model = std::make_unique<xmb_wave_model>(*m_firmware);
	m_wave_widget = new xmb_wave_widget(*m_wave_model, *m_firmware, this);
	m_wave_widget->setGeometry(rect());
	m_wave_widget->show();
	m_wave_widget->setVisible(!m_in_game && m_p3t_background.isNull() && m_background_mode != 1);
	const auto fw = [this](const QString& rco, const QString& key, const QString& fallback) { const QString text = m_firmware->text(rco, key); return text.isEmpty() ? fallback : text; };
	m_categories = { fw("xmb_ingame", "msg_user", tr("Users")), fw("xmb_ingame", "msg_setting", tr("Settings")),
		fw("xmb_ingame", "msg_photo", tr("Photo")), fw("xmb_ingame", "msg_music", tr("Music")),
		tr("Tools"), fw("xmb_ingame", "msg_game", tr("Game")), fw("xmb_ingame", "msg_net", tr("Network")),
		QStringLiteral("ARMSX3"), fw("xmb_ingame", "msg_friend", tr("RPCN")) };
	m_category_images = {
		m_firmware->image("xmb_ingame", "tex_user"), m_firmware->image("xmb_ingame", "tex_sysconf"),
		m_firmware->image("xmb_ingame", "tex_photo"), m_firmware->image("xmb_ingame", "tex_music"),
		m_firmware->image("explore_category_game", "item_tex_ps3util"), m_firmware->image("xmb_ingame", "tex_game"),
		m_firmware->image("xmb_ingame", "tex_network"), {}, m_firmware->image("xmb_ingame", "tex_friend") };
	m_firmware_category_images = m_category_images;
	for (const game_info& game : games)
		if (game && game->bootable) m_games.push_back(game);

	auto leaf = [](QString key, QString title, QString icon, QString action, QString subtitle = {})
	{
		item node; node.key = std::move(key); node.title = std::move(title); node.icon = std::move(icon); node.action = std::move(action); node.subtitle = std::move(subtitle); return node;
	};
	auto folder = [](QString key, QString title, QString icon, std::vector<item> children, QString subtitle = {})
	{
		item node; node.key = std::move(key); node.title = std::move(title); node.icon = std::move(icon); node.subtitle = std::move(subtitle); node.children = std::move(children); return node;
	};
	m_items.resize(m_categories.size());
	// This is the same top-level order and content grouping as XmbHome.xmbHomeColumns on Android.
	m_items[0] = { leaf("users.off", tr("Turn Off System"), QString::fromUtf8("⏻"), "system.poweroff"),
		leaf("users.exit", tr("Exit ARMSXPC"), QStringLiteral("A"), "system.exit") };
	m_items[1] = {
		leaf("settings.update", tr("System Update"), QString::fromUtf8("↻"), "firmware"),
		folder("settings.game", tr("Game Settings"), QString::fromUtf8("⚙"), {leaf("settings.performance", tr("Performance Settings"), QString::fromUtf8("⚙"), "settings.0"), leaf("settings.patches", tr("Manage Game Patches"), QString::fromUtf8("▦"), "patches")}),
		folder("settings.core", tr("All Core Settings"), QString::fromUtf8("⚙"), {
			leaf("core.cpu", tr("CPU"), QString::fromUtf8("⚙"), "settings.0"), leaf("core.gpu", tr("GPU"), QString::fromUtf8("▣"), "settings.1"),
			leaf("core.audio", tr("Audio"), QString::fromUtf8("♫"), "settings.2"), leaf("core.io", tr("Input / Output"), QString::fromUtf8("⌘"), "settings.3"),
			leaf("core.system", tr("System"), QString::fromUtf8("▤"), "settings.4"), leaf("core.network", tr("Network"), QString::fromUtf8("◎"), "settings.5"),
			leaf("core.advanced", tr("Advanced"), QString::fromUtf8("⚙"), "settings.6"), leaf("core.emulator", tr("Emulator"), QString::fromUtf8("⚙"), "settings.7"),
			leaf("core.gui", tr("Interface"), QString::fromUtf8("◐"), "settings.8")}),
		folder("settings.music", tr("Music Settings"), QString::fromUtf8("♫"), {leaf("settings.music.open", tr("Open Music Player"), QString::fromUtf8("♫"), "music"), leaf("settings.music.audio", tr("Audio Settings"), QString::fromUtf8("♫"), "settings.2")}),
		folder("settings.system", tr("System Settings"), QString::fromUtf8("▤"), {leaf("settings.system.general", tr("General Settings"), QString::fromUtf8("▤"), "settings.4"), leaf("settings.system.user", tr("Manage Users"), QString::fromUtf8("♟"), "users")}),
		folder("settings.theme", tr("Theme Settings"), QString::fromUtf8("◐"), {leaf("settings.theme.gui", tr("Interface and Theme"), QString::fromUtf8("◐"), "settings.8")}),
		folder("settings.accessory", tr("Accessory Settings"), QString::fromUtf8("⌘"), {leaf("settings.accessory.pad", tr("Controller Settings"), QString::fromUtf8("⌘"), "padsettings"), leaf("settings.accessory.io", tr("Input / Output Settings"), QString::fromUtf8("⌘"), "settings.3")}),
		folder("settings.display", tr("Display Settings"), QString::fromUtf8("▣"), {leaf("settings.display.graphics", tr("Graphics Settings"), QString::fromUtf8("▣"), "settings.1")}),
		folder("settings.sound", tr("Sound Settings"), QString::fromUtf8("♫"), {leaf("settings.sound.audio", tr("Audio Settings"), QString::fromUtf8("♫"), "settings.2")}),
		folder("settings.network", tr("Network Settings"), QString::fromUtf8("◎"), {leaf("settings.network.open", tr("Network Settings"), QString::fromUtf8("◎"), "settings.5"), leaf("settings.rpcn", tr("RPCN Account"), QString::fromUtf8("♙"), "rpcn")}),
	};
	for (item& node : m_items[1])
		if (node.key == "settings.theme")
			node.children = {
				leaf("settings.theme.background", tr("Choose XMB Background"), QString::fromUtf8("\u25a7"), "xmb.background"),
				leaf("settings.theme.p3t", tr("Import PS3 Theme (.p3t)"), QString::fromUtf8("\u25c8"), "xmb.theme.import"),
				leaf("settings.theme.backgroundMode", tr("XMB Background Style"), QString::fromUtf8("\u25c8"), "xmb.background.mode"),
				leaf("settings.theme.color", tr("XMB Color"), QString::fromUtf8("\u25ce"), "xmb.color"),
				leaf("settings.theme.reset", tr("Restore ARMSX3 Background"), QString::fromUtf8("\u21ba"), "xmb.background.reset"),
				leaf("settings.theme.accent", tr("XMB Accent Color"), QString::fromUtf8("\u25ce"), "xmb.accent"),
				leaf("settings.theme.gameBackgrounds", tr("Show Game Backgrounds (PIC1)"), QString::fromUtf8("\u25a7"), "xmb.gameBackgrounds",
				m_show_game_backgrounds ? tr("On") : tr("Off")),
				leaf("settings.theme.clock", tr("Show / Hide Clock"), QString::fromUtf8("\u25f7"), "xmb.clock"),
				leaf("settings.theme.gui", tr("RPCS3 Interface Settings"), QString::fromUtf8("\u2699"), "settings.8")
			};
	m_items[1].push_back(folder("settings.xmb", tr("XMB Settings"), QString::fromUtf8("\u2699"), {
		leaf("armsx3.background", tr("XMB Background"), QString::fromUtf8("\u25a7"), "xmb.background"),
		leaf("armsx3.accent", tr("XMB Accent Color"), QString::fromUtf8("\u25ce"), "xmb.accent"),
		leaf("armsx3.clock", tr("XMB Clock"), QString::fromUtf8("\u25f7"), "xmb.clock"),
		leaf("armsx3.advanced", tr("Advanced Settings"), QString::fromUtf8("\u2699"), "settings.6"),
		leaf("armsx3.remoteplay", tr("Remote Play Setup"), QString::fromUtf8("\u25ce"), "remoteplay"),
		leaf("armsx3.gameRegion", tr("Show Game Region Number"), QString::fromUtf8("\u25ce"), "xmb.gameRegion",
			m_show_game_region ? tr("On") : tr("Off")),
		leaf("armsx3.gameVersion", tr("Show Game Update Version"), QString::fromUtf8("\u25ce"), "xmb.gameVersion",
			m_show_game_version ? tr("On") : tr("Off"))
	}));
	m_items[2] = { folder("photo.screenshots", tr("Screenshots"), QString::fromUtf8("▧"), {leaf("photo.open", tr("Open Screenshot Manager"), QString::fromUtf8("▧"), "screenshots")}),
		leaf("photo.empty", tr("There are no images."), QString::fromUtf8("▧"), "") };
	{
		QStringList screenshot_paths;
		const QStringList patterns{QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.webp"), QStringLiteral("*.bmp")};
		const QStringList roots{QString::fromStdString(fs::get_config_dir() + "screenshots/"), QString::fromStdString(rpcs3::utils::get_hdd0_dir() + "/photo/")};
		for (const QString& root : roots)
		{
			QDirIterator it(root, patterns, QDir::Files, QDirIterator::Subdirectories);
			while (it.hasNext() && screenshot_paths.size() < 150) screenshot_paths.push_back(it.next());
		}
		std::sort(screenshot_paths.begin(), screenshot_paths.end(), [](const QString& a, const QString& b) { return QFileInfo(a).lastModified() > QFileInfo(b).lastModified(); });
		std::vector<item> photos;
		for (int i = 0; i < screenshot_paths.size(); ++i)
		{
			const QString& path = screenshot_paths[i];
			const QFileInfo info(path);
			item photo = leaf(QStringLiteral("photo.file.%1").arg(i), info.completeBaseName(), QString::fromUtf8("\u25a7"), QStringLiteral("photo.view:") + path, info.dir().dirName());
			QImageReader reader(path);
			const QSize source_size = reader.size();
			if (source_size.isValid()) reader.setScaledSize(source_size.scaled(128, 128, Qt::KeepAspectRatio));
			photo.image = reader.read();
			photos.push_back(std::move(photo));
		}
		m_items[2].clear();
		if (photos.empty()) m_items[2].push_back(leaf("photo.empty", tr("There are no images."), QString::fromUtf8("\u25a7"), ""));
		else m_items[2].push_back(folder("photo.screenshots", tr("Screenshots"), QString::fromUtf8("\u25a7"), std::move(photos)));
		m_items[2].push_back(leaf("photo.manager", tr("Open Screenshot Manager"), QString::fromUtf8("\u25a7"), "screenshots"));
	}
	m_items[3] = { leaf("music.library", tr("Music Library"), QString::fromUtf8("♫"), "music"),
		leaf("music.player", tr("Music Player"), QString::fromUtf8("♫"), "music") };
	{
		const QString music_root = QString::fromStdString(fs::get_config_dir()) + QStringLiteral("armsxpc/music/");
		QDir().mkpath(music_root);
		const QStringList audio_patterns{QStringLiteral("*.mp3"), QStringLiteral("*.ogg"), QStringLiteral("*.wav"), QStringLiteral("*.flac"), QStringLiteral("*.m4a"), QStringLiteral("*.aac"), QStringLiteral("*.opus")};
		QStringList tracks;
		QDirIterator it(music_root, audio_patterns, QDir::Files);
		while (it.hasNext()) tracks.push_back(it.next());
		std::sort(tracks.begin(), tracks.end(), [](const QString& a, const QString& b) { return QFileInfo(a).fileName().compare(QFileInfo(b).fileName(), Qt::CaseInsensitive) < 0; });
		std::vector<item> track_items;
		for (int i = 0; i < tracks.size(); ++i)
		{
			const QFileInfo info(tracks[i]);
			track_items.push_back(leaf(QStringLiteral("music.track.%1").arg(i), info.completeBaseName(), QString::fromUtf8("\u266b"), QStringLiteral("music.play:") + tracks[i], info.fileName()));
		}
		m_items[3] = {
			leaf("music.player", tr("Music Player"), QString::fromUtf8("\u266b"), "music"),
			leaf("music.import", tr("Import Music"), QString::fromUtf8("\u2795"), "music.import"),
			folder("music.library", tr("Music Library"), QString::fromUtf8("\u266b"), std::move(track_items))
		};
	}
	m_items[4] = { folder("game.packages", tr("Package Manager"), QString::fromUtf8("▣"), {leaf("game.pkg", tr("Install Package"), QString::fromUtf8("▣"), "packages")}),
		leaf("game.launch", tr("Launch Game"), QString::fromUtf8("▶"), "boot.file"),
		leaf("game.vsh", tr("PlayStation 3 System"), QString::fromUtf8("▶"), "boot.vsh"),
		leaf("game.trophies", tr("Trophy Collection"), QString::fromUtf8("🏆"), "trophies"),
		leaf("game.saves", tr("Saved Data Utility"), QString::fromUtf8("▤"), "savedata"),
		leaf("game.mods", tr("Manage Game Patches"), QString::fromUtf8("▦"), "patches") };
	for (int i = 0; i < m_games.size(); ++i)
	{
		item game; game.key = QString::fromStdString(m_games[i]->serial); game.title = QString::fromStdString(m_games[i]->name.empty() ? m_games[i]->serial : m_games[i]->name);
		game.subtitle = game_subtitle(i); game.icon = QStringLiteral("🎮"); game.game = i;
		m_items[5].push_back(std::move(game));
	}
	if (m_in_game)
	{
		const QString current_title = m_running_game ? QString::fromStdString(m_running_game->name) : tr("Running Game");
		m_items[5] = {
			leaf("ingame.resume", current_title.isEmpty() ? tr("Resume Game") : current_title, QString::fromUtf8("\u25b6"), "game.resume", tr("Return to the running game")),
			leaf("ingame.restart", tr("Restart Game"), QString::fromUtf8("\u21bb"), "game.restart"),
			leaf("ingame.stop", tr("Quit Game"), QString::fromUtf8("\u25a0"), "game.stop"),
			leaf("ingame.settings", tr("Game Settings"), QString::fromUtf8("\u2699"), "settings.0"),
			leaf("ingame.screenshot", tr("Take Screenshot"), QString::fromUtf8("\U0001f4f7"), "screenshots"),
			folder("ingame.remoteplay", tr("Remote Play"), QString::fromUtf8("\u25ce"), {leaf("ingame.remoteplay.host", tr("Host This Game"), QString::fromUtf8("\u25ce"), "remoteplay")}),
		};
	}
	if (m_items[5].empty()) m_items[5].push_back(leaf("game.none", tr("No games in your library"), QString::fromUtf8("▶"), ""));
	m_items[6] = { folder("network.remote", tr("Remote Play"), QString::fromUtf8("◎"), {leaf("network.remote.host", tr("Host This Game"), QString::fromUtf8("◎"), "remoteplay"), leaf("network.remote.join", tr("Join a Host"), QString::fromUtf8("◎"), "remoteplay")}),
		leaf("network.browser", tr("Internet Browser"), QString::fromUtf8("🌐"), "website") };
	m_items[7] = { leaf("armsx3.news", tr("News"), QStringLiteral("ARMSX3_MARK"), "website", tr("ARMSX3 project updates")),
		leaf("armsx3.github", tr("GitHub"), QStringLiteral("ARMSX3_MARK"), "github"), leaf("armsx3.about", tr("About ARMSX3"), QStringLiteral("ARMSX3_MARK"), "about") };
	m_items[8] = { leaf("rpcn.friends", tr("Friends"), QString::fromUtf8("♙"), "rpcn"), leaf("rpcn.account", tr("Account Management"), QString::fromUtf8("♙"), "rpcn") };
	const auto fw_title = [&](item& node, const QString& rco, const QString& key)
	{
		const QString title = m_firmware->text(rco, key);
		if (!title.isEmpty()) node.title = title;
	};
	fw_title(m_items[0][0], "explore_category_user", "msg_power_off_console2");
	fw_title(m_items[1][0], "explore_category_sysconf", "msg_update");
	fw_title(m_items[1][1], "explore_category_sysconf", "msg_game_setting");
	fw_title(m_items[1][4], "explore_category_sysconf", "msg_console_setting");
	fw_title(m_items[1][6], "explore_category_sysconf", "msg_pd_setting");
	fw_title(m_items[1][7], "explore_category_sysconf", "msg_display_setting");
	fw_title(m_items[1][8], "explore_category_sysconf", "msg_sound_setting");
	fw_title(m_items[1][9], "explore_category_sysconf", "msg_net_setting");
	// Resolve artwork from the installed PS3 firmware. Missing firmware icons are never replaced by
	// Unicode glyphs or desktop-drawn stand-ins.
	std::function<void(item&)> resolve_icons = [&](item& node)
	{
		const QString key = node.key;
		QString rco, image;
		if (key.startsWith("users.off")) { rco = "explore_category_user"; image = "tex_power_off"; }
		else if (key.startsWith("settings.update")) { rco = "explore_category_sysconf"; image = "tex_update"; }
		else if (key.startsWith("settings.game") || key.startsWith("settings.core")) { rco = "explore_category_sysconf"; image = "tex_game"; }
		else if (key.startsWith("settings.music")) { rco = "explore_category_sysconf"; image = "tex_music"; }
		else if (key.startsWith("settings.system")) { rco = "explore_category_sysconf"; image = "tex_console"; }
		else if (key.startsWith("settings.theme")) { rco = "explore_category_sysconf"; image = "tex_thema"; }
		else if (key.startsWith("settings.accessory")) { rco = "explore_category_sysconf"; image = "tex_pd"; }
		else if (key.startsWith("settings.display")) { rco = "explore_category_sysconf"; image = "tex_display"; }
		else if (key.startsWith("settings.sound")) { rco = "explore_category_sysconf"; image = "tex_sound"; }
		else if (key.startsWith("settings.network")) { rco = "explore_category_sysconf"; image = "tex_network"; }
		else if (key.startsWith("settings.debug") || key.startsWith("settings.xmb")) { rco = "explore_category_sysconf"; image = "tex_security"; }
		else if (key.startsWith("photo.")) { rco = "explore_category_photo"; image = "photo_tex_folder"; }
		else if (key.startsWith("music.")) { rco = "explore_category_music"; image = "music_tex_default"; }
		else if (key.startsWith("game.packages")) { rco = "explore_category_game"; image = "item_tex_plain_folder"; }
		else if (key.startsWith("game.launch")) { rco = "explore_category_game"; image = "item_tex_plain_folder"; }
		else if (key.startsWith("game.vsh")) { rco = "explore_category_game"; image = "item_tex_ps3logo"; }
		else if (key.startsWith("game.trophies")) { rco = "explore_category_psn"; image = "item_tex_trophy"; }
		else if (key.startsWith("game.saves")) { rco = "explore_category_photo"; image = "ps3sd_tex_default"; }
		else if (key.startsWith("game.mods")) { rco = "explore_category_game"; image = "item_tex_edyviewer"; }
		else if (key.startsWith("network.")) { rco = "explore_category_network"; image = "tex_network"; }
		else if (key.startsWith("rpcn.")) { rco = "explore_category_psn"; image = "tex_psn"; }
		if (!rco.isEmpty() && node.image.isNull()) node.image = m_firmware->image(rco, image);
		for (item& child : node.children) resolve_icons(child);
	};
	for (auto& column : m_items) for (item& node : column) resolve_icons(node);
	std::function<void(const item&)> remember_firmware_icons = [&](const item& node)
	{
		m_firmware_item_images.insert(node.key, node.image);
		for (const item& child : node.children) remember_firmware_icons(child);
	};
	for (const auto& column : m_items) for (const item& node : column) remember_firmware_icons(node);
	apply_p3t_icons();
	for (int i = 0; i < m_focus.size(); ++i) m_focus[i] = std::clamp(m_focus[i], 0, std::max(0, static_cast<int>(m_items[i].size()) - 1));

	m_animation = new QTimer(this);
	m_animation->setInterval(16);
	connect(m_animation, &QTimer::timeout, this, qOverload<>(&xmb_mode_dialog::update));
	connect(m_animation, &QTimer::timeout, this, [this]
	{
		refresh_game_backdrop();
		static const QEasingCurve fast_out_slow_in = []
		{
			QEasingCurve curve(QEasingCurve::BezierSpline);
			curve.addCubicBezierSegment(QPointF(.4, 0), QPointF(.2, 1), QPointF(1, 1));
			return curve;
		}();
		const int column_target = m_column;
		if (column_target != m_target_column)
		{
			m_column_from = m_visual_column;
			m_target_column = column_target;
			m_column_clock.restart();
		}
		const qreal column_progress = std::clamp(m_column_clock.isValid() ? m_column_clock.elapsed() / 220.0 : 1.0, 0.0, 1.0);
		m_visual_column = m_column_from + (column_target - m_column_from) * fast_out_slow_in.valueForProgress(column_progress);

		const int focus_target = current_focus();
		if (focus_target != m_target_focus)
		{
			m_focus_from = m_visual_focus;
			m_target_focus = focus_target;
			m_focus_clock.restart();
		}
		const qreal focus_progress = std::clamp(m_focus_clock.isValid() ? m_focus_clock.elapsed() / 170.0 : 1.0, 0.0, 1.0);
		m_visual_focus = m_focus_from + (focus_target - m_focus_from) * fast_out_slow_in.valueForProgress(focus_progress);

		const int level_target = m_levels.empty() ? 0 : 1;
		if (level_target != m_target_level)
		{
			m_level_from = m_visual_level;
			m_target_level = level_target;
			m_level_clock.restart();
		}
		const qreal level_progress = std::clamp(m_level_clock.isValid() ? m_level_clock.elapsed() / 240.0 : 1.0, 0.0, 1.0);
		m_visual_level = m_level_from + (level_target - m_level_from) * fast_out_slow_in.valueForProgress(level_progress);
	});
	m_game_backdrop_delay = new QTimer(this);
	m_game_backdrop_delay->setSingleShot(true);
	m_game_backdrop_delay->setInterval(650);
	connect(m_game_backdrop_delay, &QTimer::timeout, this, &xmb_mode_dialog::load_game_backdrop);
	m_game_backdrop_watcher = new QFutureWatcher<QImage>(this);
	connect(m_game_backdrop_watcher, &QFutureWatcher<QImage>::finished, this, [this]
	{
		const QString loaded_key = m_game_backdrop_loading_key;
		m_game_backdrop_loading_key.clear();
		if (loaded_key == m_game_backdrop_key)
		{
			m_game_background = m_game_backdrop_watcher->result();
			if (!m_game_background.isNull()) m_game_backdrop_fade.start();
		}
		if (loaded_key != m_game_backdrop_key && !m_game_backdrop_key.isEmpty()) m_game_backdrop_delay->start(0);
		update();
	});
	m_game_icon_watcher = new QFutureWatcher<QHash<int, QImage>>(this);
	connect(m_game_icon_watcher, &QFutureWatcher<QHash<int, QImage>>::finished, this, [this]
	{
		const QHash<int, QImage> icons = m_game_icon_watcher->result();
		for (auto it = icons.cbegin(); it != icons.cend(); ++it)
			if (it.key() >= 0 && it.key() < m_games.size() && m_games[it.key()]->icon.isNull() && !it.value().isNull())
				m_games[it.key()]->icon = QPixmap::fromImage(it.value());
		update();
	});
	load_missing_game_icons();
	m_visual_focus = static_cast<float>(current_focus());
	m_target_column = m_column;
	m_target_focus = current_focus();
	m_target_level = 0;
	m_animation->start();
}

xmb_mode_dialog::~xmb_mode_dialog()
{
	QSettings preferences;
	preferences.beginGroup(QStringLiteral("armsxpc/xmb"));
	preferences.setValue(QStringLiteral("column"), m_column);
	for (int i = 0; i < m_focus.size(); ++i) preferences.setValue(QStringLiteral("focus/%1").arg(i), m_focus[i]);
}

void xmb_mode_dialog::apply_p3t_icons()
{
	m_category_images = m_firmware_category_images;
	const std::array<QString, 9> category_ids{QStringLiteral("icon_user"), QStringLiteral("icon_setting"), QStringLiteral("icon_photo"),
		QStringLiteral("icon_music"), QStringLiteral("icon_savedata"), QStringLiteral("icon_game"), QStringLiteral("icon_network"), QString(), QStringLiteral("icon_friend")};
	for (qsizetype i = 0; i < std::min<qsizetype>(m_category_images.size(), category_ids.size()); ++i)
		if (!category_ids[i].isEmpty() && m_p3t_icons.contains(category_ids[i])) m_category_images[i] = m_p3t_icons.value(category_ids[i]);

	std::function<void(item&)> apply = [&](item& node)
	{
		node.image = m_firmware_item_images.value(node.key);
		if (node.key == QStringLiteral("settings.theme.p3t"))
			node.subtitle = m_p3t_theme_name.isEmpty() ? tr("Original firmware theme") : m_p3t_theme_name;
		QString id;
		const QString& key = node.key;
		if (key.startsWith("users.off")) id = "icon_poweroff";
		else if (key.startsWith("settings.update")) id = "icon_update";
		else if (key.startsWith("settings.game")) id = "icon_game_setting";
		else if (key.startsWith("settings.theme")) id = "icon_theme_setting";
		else if (key.startsWith("settings.display")) id = "icon_display_setting";
		else if (key.startsWith("settings.sound")) id = "icon_sound_setting";
		else if (key.startsWith("settings.network")) id = "icon_network_setting";
		else if (key.startsWith("photo.")) id = "icon_photo_default";
		else if (key.startsWith("music.")) id = "icon_music_default";
		else if (key.startsWith("network.remote")) id = "icon_remoteplay";
		else if (key.startsWith("network.")) id = "icon_browser";
		else if (key.startsWith("game.saves")) id = "icon_savedata";
		if (!id.isEmpty() && m_p3t_icons.contains(id)) node.image = m_p3t_icons.value(id);
		for (item& child : node.children) apply(child);
	};
	for (auto& column : m_items) for (item& node : column) apply(node);
}

void xmb_mode_dialog::set_custom_background(const QString& path)
{
	m_custom_background = path;
	m_p3t_background = {};
	m_p3t_slides.clear();
	m_p3t_theme_path.clear();
	m_p3t_theme_name.clear();
	m_p3t_icons.clear();
	apply_p3t_icons();
	QSettings().setValue(QStringLiteral("armsxpc/xmb/background"), path);
	QSettings().remove(QStringLiteral("armsxpc/xmb/p3tTheme"));
	set_background_mode(2);
	update();
}

QString xmb_mode_dialog::import_p3t_theme(const QString& path)
{
	const auto theme = xmb_p3t::read(path);
	if (theme.background.isNull()) return theme.error;
	m_p3t_theme_path = path;
	m_p3t_theme_name = theme.name;
	m_p3t_background = theme.background;
	m_p3t_slides.clear(); m_p3t_slide_clock.invalidate();
	if (theme.dynamic && theme.animation_size)
	{
		const auto animation = xmb_p3t_animation::read(path, theme.animation_offset, theme.animation_size);
		m_p3t_slides = animation.frames; m_p3t_slide_interval = animation.interval; m_p3t_slide_fade = animation.fade;
		if (!m_p3t_slides.isEmpty()) { m_p3t_background = m_p3t_slides.front(); m_p3t_slide_start = QRandomGenerator::global()->bounded(m_p3t_slides.size()); m_p3t_slide_clock.start(); }
	}
	m_p3t_icons = theme.icons;
	apply_p3t_icons();
	m_custom_background.clear();
	QSettings settings;
	settings.setValue(QStringLiteral("armsxpc/xmb/p3tTheme"), path);
	settings.remove(QStringLiteral("armsxpc/xmb/background"));
	set_background_mode(0);
	if (m_wave_widget) m_wave_widget->setVisible(false);
	update();
	return {};
}

void xmb_mode_dialog::reset_custom_background()
{
	m_custom_background.clear();
	m_p3t_background = {};
	m_p3t_slides.clear(); m_p3t_slide_clock.invalidate();
	m_p3t_theme_path.clear();
	m_p3t_theme_name.clear();
	m_p3t_icons.clear();
	apply_p3t_icons();
	m_p3t_icons.clear();
	QSettings().remove(QStringLiteral("armsxpc/xmb/background"));
	QSettings().remove(QStringLiteral("armsxpc/xmb/p3tTheme"));
	set_background_mode(0);
	update();
}

void xmb_mode_dialog::set_background_mode(int mode)
{
	m_background_mode = std::clamp(mode, 0, 2);
	QSettings().setValue(QStringLiteral("armsxpc/xmb/backgroundMode"), m_background_mode);
	if (m_wave_widget) m_wave_widget->setVisible(!m_in_game && m_p3t_background.isNull() && m_background_mode != 1);
	update();
}

void xmb_mode_dialog::set_theme_month(int month)
{
	m_theme_month = std::clamp(month, -1, 12);
	QSettings().setValue(QStringLiteral("armsxpc/xmb/themeMonth"), m_theme_month);
	update();
}

void xmb_mode_dialog::set_accent_color(const QColor& color)
{
	if (!color.isValid()) return;
	m_accent_color = color;
	QSettings().setValue(QStringLiteral("armsxpc/xmb/accent"), color);
	update();
}

void xmb_mode_dialog::toggle_clock()
{
	m_show_clock = !m_show_clock;
	QSettings().setValue(QStringLiteral("armsxpc/xmb/showClock"), m_show_clock);
	update();
}

void xmb_mode_dialog::toggle_game_version()
{
	m_show_game_version = !m_show_game_version;
	QSettings().setValue(QStringLiteral("armsxpc/xmb/showGameVersion"), m_show_game_version);
	update_game_version_labels();
	update();
}

void xmb_mode_dialog::toggle_game_region()
{
	m_show_game_region = !m_show_game_region;
	QSettings().setValue(QStringLiteral("armsxpc/xmb/showGameRegion"), m_show_game_region);
	update_game_version_labels();
	update();
}

void xmb_mode_dialog::toggle_game_backgrounds()
{
	m_show_game_backgrounds = !m_show_game_backgrounds;
	QSettings().setValue(QStringLiteral("armsxpc/xmb/showGameBackgrounds"), m_show_game_backgrounds);
	refresh_game_backdrop();
	update();
}

QString xmb_mode_dialog::game_subtitle(int index) const
{
	if (index < 0 || index >= m_games.size()) return {};
	QStringList details;
	if (m_show_game_region) details.push_back(QString::fromStdString(m_games[index]->serial));
	if (m_show_game_version)
	{
		const QString version = QString::fromStdString(m_games[index]->GetGameVersion());
		if (!version.isEmpty()) details.push_back(tr("Version %1").arg(version));
	}
	return details.join(QStringLiteral(" · "));
}

void xmb_mode_dialog::update_game_version_labels()
{
	if (!m_in_game && m_items.size() > 5)
	{
		for (item& node : m_items[5])
			if (node.game >= 0 && node.game < m_games.size())
				node.subtitle = game_subtitle(node.game);
	}
	if (m_items.size() > 1)
		for (item& group : m_items[1])
			if (group.key == QStringLiteral("settings.xmb"))
				for (item& setting : group.children)
				{
					if (setting.key == QStringLiteral("armsx3.gameRegion")) setting.subtitle = m_show_game_region ? tr("On") : tr("Off");
					else if (setting.key == QStringLiteral("armsx3.gameVersion")) setting.subtitle = m_show_game_version ? tr("On") : tr("Off");
				}
	std::function<void(item&)> update_background_setting = [&](item& node)
	{
		if (node.key == QStringLiteral("settings.theme.gameBackgrounds")) node.subtitle = m_show_game_backgrounds ? tr("On") : tr("Off");
		for (item& child : node.children) update_background_setting(child);
	};
	for (item& node : m_items[1]) update_background_setting(node);
}

void xmb_mode_dialog::refresh_game_backdrop()
{
	game_info target;
	if (m_show_game_backgrounds)
	{
		if (m_in_game) target = m_running_game;
		else if (m_column == 5 && m_levels.empty())
		{
			const int focus = m_focus[m_column];
			if (focus >= 0 && focus < static_cast<int>(m_items[m_column].size()))
			{
				const int index = m_items[m_column][focus].game;
				if (index >= 0 && index < m_games.size()) target = m_games[index];
			}
		}
	}
	const QString key = target ? QString::fromStdString(target->serial + "|" + target->path) : QString();
	if (key == m_game_backdrop_key) return;
	m_game_backdrop_key = key;
	m_game_backdrop_pending = target;
	m_game_background = {};
	m_game_backdrop_fade.invalidate();
	if (m_game_backdrop_delay)
	{
		m_game_backdrop_delay->stop();
		if (!key.isEmpty()) m_game_backdrop_delay->start(650);
	}
	update();
}

void xmb_mode_dialog::load_game_backdrop()
{
	if (!m_game_backdrop_pending || m_game_backdrop_key.isEmpty()) return;
	if (m_game_backdrop_watcher->isRunning()) return;
	const game_info game = m_game_backdrop_pending;
	const QString key = m_game_backdrop_key;
	m_game_backdrop_loading_key = key;
	m_game_backdrop_watcher->setFuture(QtConcurrent::run([game]()
	{
		const auto [path, in_archive] = rpcs3::utils::get_game_content_path(game_content_type::background_picture, *game);
		if (path.empty()) return QImage{};
		if (!in_archive) return QImageReader(QString::fromStdString(path)).read();
		iso_archive archive(game->path);
		if (!archive.is_valid()) return QImage{};
		auto stream = archive.open(path);
		if (!stream) return QImage{};
		const u64 size = stream->size();
		if (!size || size > (64ull << 20)) return QImage{};
		QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
		if (stream->read(data.data(), size) != size) return QImage{};
		QImage image;
		image.loadFromData(data, "PNG");
		return image;
	}));
}

void xmb_mode_dialog::load_missing_game_icons()
{
	QVector<QPair<int, game_info>> missing;
	for (int i = 0; i < m_games.size(); ++i)
		if (m_games[i]->icon.isNull()) missing.push_back({i, m_games[i]});
	if (missing.isEmpty()) return;
	m_game_icon_watcher->setFuture(QtConcurrent::run([missing = std::move(missing)]()
	{
		QHash<int, QImage> icons;
		for (const auto& entry : missing)
		{
			const QImage image = load_game_icon_image(entry.second);
			if (!image.isNull()) icons.insert(entry.first, image);
		}
		return icons;
	}));
}

const std::vector<xmb_mode_dialog::item>& xmb_mode_dialog::current_items() const
{
	const auto* items = &m_items[m_column];
	for (const int selected : m_path)
	{
		if (selected < 0 || selected >= static_cast<int>(items->size())) return *items;
		items = &(*items)[selected].children;
	}
	return *items;
}

int& xmb_mode_dialog::current_focus()
{
	return m_levels.empty() ? m_focus[m_column] : m_levels.back();
}

void xmb_mode_dialog::rebuild_items()
{
	if (m_column < 0 || m_column >= static_cast<int>(m_items.size())) return;
	if (m_focus[m_column] >= static_cast<int>(m_items[m_column].size())) m_focus[m_column] = std::max(0, static_cast<int>(m_items[m_column].size()) - 1);
	update();
}

void xmb_mode_dialog::launch_selected()
{
	const auto& items = current_items();
	const int focus = current_focus();
	if (focus < 0 || focus >= static_cast<int>(items.size())) return;
	const item& selected = items[focus];
	if (selected.game >= 0) { emit bootRequested(m_games[selected.game]); return; }
	if (!selected.children.empty())
	{
		m_path.push_back(focus);
		m_levels.push_back(0);
		m_level_titles.push_back(selected.title);
		m_visual_focus = 0.0f;
		update();
		return;
	}
	if (!selected.action.isEmpty()) emit actionRequested(selected.action);
	if (m_in_game && (selected.action == "game.resume" || selected.action == "game.restart" || selected.action == "game.stop")) accept();
}

void xmb_mode_dialog::keyPressEvent(QKeyEvent* e)
{
	if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Backspace)
	{
		if (m_options_open) { m_options_open = false; update(); }
		else if (!m_levels.empty()) { m_levels.pop_back(); m_path.pop_back(); m_level_titles.pop_back(); m_visual_focus = static_cast<float>(current_focus()); update(); }
		else reject();
		return;
	}
	if (e->key() == Qt::Key_Left || e->key() == Qt::Key_A)
	{
		if (m_options_open) { m_options_open = false; update(); return; }
		if (!m_levels.empty()) { m_levels.pop_back(); m_path.pop_back(); m_level_titles.pop_back(); m_visual_focus = static_cast<float>(current_focus()); update(); return; }
		m_column = std::max(0, m_column - 1); update(); return;
	}
	if (e->key() == Qt::Key_Right || e->key() == Qt::Key_D)
	{
		if (m_options_open) return;
		if (m_levels.empty()) m_column = std::min(static_cast<int>(m_categories.size()) - 1, m_column + 1);
		else if (!current_items().empty()) current_focus() = std::min(static_cast<int>(current_items().size()) - 1, current_focus() + 1);
		update(); return;
	}
	if (m_options_open && (e->key() == Qt::Key_Up || e->key() == Qt::Key_W)) { m_option_focus = std::max(0, m_option_focus - 1); update(); return; }
	if (m_options_open && (e->key() == Qt::Key_Down || e->key() == Qt::Key_S)) { m_option_focus = std::min(2, m_option_focus + 1); update(); return; }
	if (e->key() == Qt::Key_Up || e->key() == Qt::Key_W)
	{
		current_focus() = std::max(0, current_focus() - 1); update(); return;
	}
	if (e->key() == Qt::Key_Down || e->key() == Qt::Key_S)
	{
		current_focus() = std::min(static_cast<int>(current_items().size()) - 1, current_focus() + 1); update(); return;
	}
	if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space)
	{
		if (m_options_open)
		{
			m_options_open = false;
			if (m_in_game)
			{
				static const QStringList options{QStringLiteral("game.resume"), QStringLiteral("game.restart"), QStringLiteral("game.stop")};
				if (m_option_focus >= 0 && m_option_focus < options.size()) emit actionRequested(options[m_option_focus]);
				accept();
			}
			else if (m_option_focus == 0) emit actionRequested(QStringLiteral("game.play.%1").arg(m_option_game));
			else if (m_option_focus == 1) emit actionRequested(QStringLiteral("game.settings.%1").arg(m_option_game));
			else if (m_option_focus == 2) emit actionRequested(QStringLiteral("game.cache.%1").arg(m_option_game));
			update(); return;
		}
		launch_selected(); return;
	}
	if (e->key() == Qt::Key_Q && m_levels.empty() && m_column == 5 && (m_in_game || !m_games.empty()))
	{
		m_options_open = true; m_option_focus = 0; m_option_game = m_focus[m_column]; update(); return;
	}
	QDialog::keyPressEvent(e);
}

void xmb_mode_dialog::paintEvent(QPaintEvent*)
{
	if (m_wave_widget && m_wave_widget->geometry() != rect()) m_wave_widget->setGeometry(rect());
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	const qreal scale = std::min(width() / 1280.0, height() / 720.0);
	p.scale(scale, scale);
	const qreal w = width() / scale, h = height() / scale;
	const qreal elapsed = m_p3t_slide_clock.isValid() ? m_p3t_slide_clock.elapsed() / 1000.0 : 0.0;
	const qreal cycle = std::max(0.25, m_p3t_slide_interval);
	const int step = static_cast<int>(elapsed / cycle);
	const int current = m_p3t_slides.isEmpty() ? 0 : (m_p3t_slide_start + step) % m_p3t_slides.size();
	if (!m_game_background.isNull())
	{
		p.save();
		if (m_game_backdrop_fade.isValid()) p.setOpacity(std::clamp(m_game_backdrop_fade.elapsed() / 700.0, 0.0, 1.0));
		draw_cover(p, QRectF(0, 0, w, h), m_game_background);
		p.restore();
	}
	else if (!m_p3t_slides.isEmpty()) draw_cover(p, QRectF(0, 0, w, h), m_p3t_slides[current]);
	else if (!m_p3t_background.isNull()) draw_cover(p, QRectF(0, 0, w, h), m_p3t_background);
	else if (!m_in_game && m_background_mode == 2 && !m_custom_background.isEmpty())
	{
		QPixmap backdrop(m_custom_background);
		if (!backdrop.isNull()) draw_cover(p, QRectF(0, 0, w, h), backdrop.toImage());
		else fill_original_background(p, w, h, m_theme_month);
	}
	else
	{
		fill_original_background(p, w, h, m_theme_month);
		if (m_background_mode == 1) fill_simple_wave(p, w, h, QDateTime::currentMSecsSinceEpoch() / 1000.0, m_theme_month);
	}
	if (m_game_background.isNull() && m_p3t_slides.size() > 1 && m_p3t_slide_clock.isValid())
	{
		const int next = (current + 1) % m_p3t_slides.size();
		const qreal phase = std::fmod(elapsed, cycle);
		if (m_p3t_slide_fade > 0 && phase >= cycle - m_p3t_slide_fade)
		{
			p.save(); p.setOpacity(std::clamp((phase - (cycle - m_p3t_slide_fade)) / m_p3t_slide_fade, 0.0, 1.0));
			draw_cover(p, QRectF(0, 0, w, h), m_p3t_slides[next]); p.restore();
		}
	}
	if (m_in_game) p.fillRect(QRectF(0, 0, w, h), QColor(0, 0, 0, 165));
	QFont font(m_firmware->font_family().isEmpty() ? QStringLiteral("Segoe UI Light") : m_firmware->font_family());
	const qreal focus_x = std::min(376.0, w * .3);
	const qreal cat_y = 193.0;
	const qreal focus_y = cat_y + 140.0;
	const auto& items = current_items();
	const auto& root_items = m_items[m_column];
	const int root_focus = m_focus[m_column];
	const qreal horizontal_shift = 168 * m_visual_level;
	for (int c = 0; c < m_categories.size(); ++c)
	{
		const qreal column_distance = c - m_visual_column;
		const qreal x = focus_x + column_distance * 137 - horizontal_shift;
		if (x < -120 || x > w + 120) continue;
		const qreal near = std::clamp(1.0 - std::abs(column_distance), 0.0, 1.0);
		// CategoryIcon in XmbView.kt: keep the neighboring columns visible, and fade
		// every non-current category away as a sub-list slides in.
		const qreal alpha = (.78 + .22 * near) * (1.0 - m_visual_level * (1.0 - near));
		p.setOpacity(alpha);
		if (c < m_category_images.size() && !m_category_images[c].isNull())
		{
			const qreal icon_size = 50 + 30 * near;
			const QRectF icon_rect(x - icon_size / 2, cat_y - icon_size / 2, icon_size, icon_size);
			p.drawImage(icon_rect, m_category_images[c]);
			// XmbView.kt draws the focused firmware icon over itself to remove its authored translucency.
			p.setOpacity(alpha * near);
			p.drawImage(icon_rect, m_category_images[c]);
		}
		else if (c == 7) draw_armsx3_mark(p, QRectF(x - 40, cat_y - 40, 80, 80), true);
		font.setPixelSize(18); p.setFont(font);
		p.setOpacity(std::clamp(1.0 - std::abs(column_distance) * 2.0, 0.0, 1.0));
		p.drawText(QRectF(x - 120, cat_y + 41, 240, 26), Qt::AlignCenter, m_categories[c]);
	}
	p.setOpacity(1.0);
	// Android keeps the adjacent columns' item stacks on screen during horizontal movement. The
	// current column is drawn below with its larger focused row; these neighboring rows use the same
	// Metrics formulas and firmware art, fading exactly with the category animation.
	for (int c = std::max(0, m_column - 1); c <= std::min(static_cast<int>(m_items.size()) - 1, m_column + 1); ++c)
	{
		if ((c == m_column && m_levels.empty()) || std::abs(c - m_visual_column) >= 1.0f) continue;
		const auto& column_items = m_items[c];
		if (column_items.empty()) continue;
		const int column_focus = m_focus[c];
		const qreal column_x = focus_x + (c - m_visual_column) * 137 - horizontal_shift;
		const qreal column_alpha = std::clamp(1.0 - std::abs(c - m_visual_column), 0.0, 1.0);
		for (int i = std::max(0, column_focus - 4); i <= std::min(static_cast<int>(column_items.size()) - 1, column_focus + 12); ++i)
		{
			const item& node = column_items[i];
			const qreal d = i - column_focus;
			const qreal y = d >= 1 ? 193 + 140 + 120 + (d - 1) * 53 : d >= 0 ? 193 + 140 + d * 120 : d >= -1 ? 193 + 140 + d * ((193 + 140) - 80) : 80 + (d + 1) * 53;
			const bool selected = i == column_focus;
			const qreal near = std::clamp(1.0 - std::abs(d), 0.0, 1.0);
			const qreal base_w = node.game >= 0 ? 207 : 84;
			const qreal base_h = node.game >= 0 ? 113 : 84;
			const qreal small = node.game >= 0 ? 97.0 / 207.0 : 52.0 / 84.0;
			const qreal scale_item = small + (1.0 - small) * near;
			const qreal iw = base_w * scale_item, ih = base_h * scale_item;
			const qreal far = d < -1 ? std::clamp(1.0 + (d + 1.0) * .5, 0.0, 1.0) : 1.0;
			const qreal alpha = column_alpha * (selected ? 1.0 : 1.0 - .55) * far;
			const QRectF icon_rect(column_x - iw / 2, y - ih / 2, iw, ih);
			p.setOpacity(alpha);
			if (node.game >= 0 && node.game < m_games.size() && !m_games[node.game]->icon.isNull())
				p.drawPixmap(icon_rect, m_games[node.game]->icon, m_games[node.game]->icon.rect());
			else if (!node.image.isNull())
			{
				p.drawImage(icon_rect, node.image);
				p.setOpacity(alpha * near);
				p.drawImage(icon_rect, node.image);
			}
			else if (node.game >= 0 && !m_default_game_icon.isNull()) p.drawImage(icon_rect, m_default_game_icon);
			else if (node.icon == QStringLiteral("ARMSX3_MARK")) draw_armsx3_mark(p, icon_rect, false);
			if (selected || node.game < 0)
			{
				const qreal tx = column_x + iw / 2 + 28;
				p.setOpacity(alpha * (selected ? 1.0 : .82) * (1.0 - m_visual_level));
				p.setPen(selected ? m_accent_color : Qt::white);
				font.setPixelSize(selected ? 23 : 19); p.setFont(font);
				p.drawText(QRectF(tx, y - 17, std::max(100.0, w - tx - 80), 32), Qt::AlignLeft | Qt::AlignVCenter, node.title);
				if (selected && !node.subtitle.isEmpty())
				{
					font.setPixelSize(14); p.setFont(font); p.setPen(QColor(255, 255, 255, 220));
					p.drawText(QRectF(tx, y + 15, std::max(100.0, w - tx - 80), 23), Qt::AlignLeft | Qt::AlignVCenter, node.subtitle);
				}
			}
		}
	}
	p.setOpacity(1.0);
	const QRectF clock(w - 350, 49, 305, 38);
	p.setPen(QColor(255,255,255,105)); p.setBrush(QColor(255,255,255,18)); p.drawRoundedRect(clock, 3, 3);
	p.setPen(Qt::white); font.setPixelSize(17); p.setFont(font);
	if (m_show_clock) p.drawText(clock.adjusted(10, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, QDateTime::currentDateTime().toString("d MMM  h:mm AP"));
	if (!m_levels.empty() && root_focus >= 0 && root_focus < static_cast<int>(root_items.size()))
	{
		font.setPixelSize(19); p.setFont(font); p.setPen(QColor(255, 255, 255, static_cast<int>(220 * m_visual_level)));
		p.drawText(QRectF(focus_x - 58, focus_y - 14, 24, 28), Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8("◂"));
	}
	if (m_options_open)
	{
		const qreal panel_x = w * .69;
		p.setPen(QColor(255,255,255,65));
		QLinearGradient panel(panel_x, 0, w, 0); panel.setColorAt(0, QColor(255,255,255,12)); panel.setColorAt(.55, QColor(255,255,255,35)); panel.setColorAt(1, QColor(255,255,255,20));
		p.setBrush(panel); p.drawRect(QRectF(panel_x, 0, w - panel_x, h));
		p.setPen(QColor(255,255,255,210)); font.setPixelSize(19); p.setFont(font);
		const QString panel_title = m_in_game
			? (m_running_game ? QString::fromStdString(m_running_game->name) : tr("Running Game"))
			: (m_option_game >= 0 && m_option_game < m_games.size() ? QString::fromStdString(m_games[m_option_game]->name) : tr("Game Options"));
		p.drawText(QRectF(panel_x + 28, 130, w - panel_x - 50, 35), Qt::AlignLeft | Qt::AlignVCenter, panel_title);
		if (m_options_open)
		{
			const QStringList options = m_in_game ? QStringList{tr("Resume"), tr("Restart"), tr("Quit Game")} : QStringList{tr("Play"), tr("Settings"), tr("Clear Cache")};
			for (int i = 0; i < options.size(); ++i)
			{
				const qreal y = focus_y + (i - m_option_focus) * 30.0 - 15.0;
				if (i == m_option_focus) { p.setPen(Qt::NoPen); p.setBrush(QColor(255,255,255,38)); p.drawRoundedRect(QRectF(panel_x + 14, y, w - panel_x - 32, 30), 3, 3); }
				p.setPen(QColor(255,255,255, i == m_option_focus ? 255 : 160)); font.setPixelSize(19); p.setFont(font);
				p.drawText(QRectF(panel_x + 26, y, w - panel_x - 50, 30), Qt::AlignLeft | Qt::AlignVCenter, options[i]);
			}
		}
	}
	const int focus = current_focus();
	const qreal animated_focus = m_visual_focus;
	const int first = std::max(0, focus - 6), last = std::min(static_cast<int>(items.size()) - 1, focus + 10);
	for (int i = first; i <= last; ++i)
	{
		const item& node = items[i];
		const qreal d = i - animated_focus;
		const qreal y = d >= 1 ? focus_y + 120 + (d - 1) * 53 : d >= 0 ? focus_y + d * 120 : d >= -1 ? focus_y + d * (focus_y - 80) : 80 + (d + 1) * 53;
		const bool selected = i == focus;
		const bool sublist = !m_levels.empty();
		const qreal x = sublist ? focus_x + 30 * (1.0 - m_visual_level) : focus_x - horizontal_shift;
		const qreal near = std::clamp(1.0 - std::abs(i - animated_focus), 0.0, 1.0);
		const qreal icon_base_w = sublist ? 26.0 : node.game >= 0 ? 207.0 : 84.0;
		const qreal icon_base_h = sublist ? 26.0 : node.game >= 0 ? 113.0 : 84.0;
		const qreal small_scale = node.game >= 0 ? 97.0 / 207.0 : 52.0 / 84.0;
		const qreal icon_scale = sublist ? 1.0 + .35 * near : small_scale + (1.0 - small_scale) * near;
		const qreal icon_w = icon_base_w * icon_scale;
		const qreal icon_h = icon_base_h * icon_scale;
		const qreal icon_x = x - icon_w / 2;
		const qreal icon_y = y - icon_h / 2;
		const qreal column_alpha = std::clamp(1.0 - std::abs(m_column - m_visual_column), 0.0, 1.0);
		const qreal depth_alpha = selected ? 1.0 : 1.0 - .55 * m_visual_level;
		const qreal far_alpha = d < -1 ? std::clamp(1.0 + (d + 1.0) * .5, 0.0, 1.0) : 1.0;
		const qreal alpha = column_alpha * depth_alpha * far_alpha * (sublist ? m_visual_level : 1.0);
		p.setOpacity(alpha);
		if (!sublist && node.game >= 0 && !m_games[node.game]->icon.isNull())
			p.drawPixmap(QRectF(icon_x, icon_y, icon_w, icon_h), m_games[node.game]->icon, m_games[node.game]->icon.rect());
		else if (!node.image.isNull())
		{
			const QRectF icon_rect(icon_x, icon_y, icon_w, icon_h);
			p.drawImage(icon_rect, node.image);
			p.setOpacity(alpha * near);
			p.drawImage(icon_rect, node.image);
			p.setOpacity(alpha);
		}
		else if (!sublist && node.game >= 0 && !m_default_game_icon.isNull())
			p.drawImage(QRectF(icon_x, icon_y, icon_w, icon_h), m_default_game_icon);
		else if (node.icon == QStringLiteral("ARMSX3_MARK"))
			draw_armsx3_mark(p, QRectF(icon_x, icon_y, icon_w, icon_h), false);
		// Firmware-backed XMB artwork is the only icon source; do not substitute text glyphs.
		p.setOpacity(alpha * (selected ? 1.0 : .82));
		font.setPixelSize(selected ? 23 : 19); p.setFont(font);
		p.setPen(selected ? m_accent_color : Qt::white);
		if (sublist || selected || node.game < 0)
		{
			const qreal tx = sublist ? focus_x + 34 + 30 * (1.0 - m_visual_level) : x + icon_w / 2 + 28;
			p.drawText(QRectF(tx, y - 17, std::max(100.0, w - tx - 80), 32), Qt::AlignLeft | Qt::AlignVCenter, node.title);
			if (selected && !node.subtitle.isEmpty())
			{
				font.setPixelSize(14); p.setFont(font); p.setPen(QColor(255,255,255,220));
				p.drawText(QRectF(tx, y + 15, std::max(100.0, w - tx - 80), 23), Qt::AlignLeft | Qt::AlignVCenter, node.subtitle);
			}
		}
		p.setOpacity(1.0);
	}
	p.setPen(QColor(255,255,255,225)); font.setPixelSize(15); p.setFont(font);
	const QString help = !m_levels.empty() ? tr("Enter Open     ← Back     Esc Exit XMB") : tr("Q Options     Enter Open / Launch     ← → Category     Esc Exit XMB");
	p.drawText(QRectF(w - 640, h - 56, 600, 28), Qt::AlignRight | Qt::AlignVCenter, help);
}
