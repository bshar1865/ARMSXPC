#pragma once

#include "gui_game_info.h"

#include <QDialog>
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QHash>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

class xmb_firmware;
class xmb_wave_model;
class xmb_wave_widget;

class QTimer;
template <typename T> class QFutureWatcher;

// Desktop rendition of the ARMSX3 phone XMB, laid out in the same 1280x720 console space.
class xmb_mode_dialog final : public QDialog
{
	Q_OBJECT
public:
	explicit xmb_mode_dialog(const std::vector<game_info>& games, bool in_game = false, game_info running_game = {}, QWidget* parent = nullptr);
	~xmb_mode_dialog() override;
	void set_custom_background(const QString& path);
	QString import_p3t_theme(const QString& path);
	void reset_custom_background();
	void set_background_mode(int mode);
	void set_theme_month(int month);
	void set_accent_color(const QColor& color);
	void toggle_clock();
	void toggle_game_version();
	void toggle_game_region();
	void toggle_game_backgrounds();

Q_SIGNALS:
	void bootRequested(const game_info& game);
	void actionRequested(const QString& action);

protected:
	void paintEvent(QPaintEvent*) override;
	void keyPressEvent(QKeyEvent*) override;

private:
	QVector<game_info> m_games;
	bool m_in_game = false;
	game_info m_running_game;
	struct item
	{
		QString key;
		QString title;
		QString subtitle;
		QString icon;
		QString action;
		QImage image;
		int game = -1;
		std::vector<item> children;
	};
	std::vector<std::vector<item>> m_items;
	QStringList m_categories;
	QStringList m_category_glyphs;
	QVector<QImage> m_category_images;
	QVector<QImage> m_firmware_category_images;
	QHash<QString, QImage> m_firmware_item_images;
	QImage m_default_game_icon;
	std::unique_ptr<xmb_firmware> m_firmware;
	std::unique_ptr<xmb_wave_model> m_wave_model;
	xmb_wave_widget* m_wave_widget = nullptr;
	QString m_custom_background;
	QString m_p3t_theme_path;
	QString m_p3t_theme_name;
	QImage m_p3t_background;
	QImage m_game_background;
	QVector<QImage> m_p3t_slides;
	qreal m_p3t_slide_interval = 10.0;
	qreal m_p3t_slide_fade = 1.5;
	QElapsedTimer m_p3t_slide_clock;
	int m_p3t_slide_start = 0;
	QHash<QString, QImage> m_p3t_icons;
	int m_background_mode = 0; // 0 Original firmware wave, 1 the Android 2D gradient, 2 imported image
	int m_theme_month = -1; // -1 follows the calendar, 0..11 pins a month
	QColor m_accent_color = QColor(255, 255, 255);
	bool m_show_clock = true;
	bool m_show_game_version = false;
	bool m_show_game_region = false;
	bool m_show_game_backgrounds = true;
	int m_column = 5;
	QVector<int> m_focus;
	QVector<int> m_path;
	QVector<int> m_levels;
	QVector<QString> m_level_titles;
	float m_visual_column = 5.0f;
	float m_visual_focus = 0.0f;
	float m_visual_level = 0.0f;
	float m_column_from = 5.0f;
	float m_focus_from = 0.0f;
	float m_level_from = 0.0f;
	int m_target_column = -1;
	int m_target_focus = -1;
	int m_target_level = -1;
	QElapsedTimer m_column_clock;
	QElapsedTimer m_focus_clock;
	QElapsedTimer m_level_clock;
	bool m_options_open = false;
	int m_option_focus = 0;
	int m_option_game = -1;
	QTimer* m_animation = nullptr;
	QTimer* m_game_backdrop_delay = nullptr;
	QFutureWatcher<QImage>* m_game_backdrop_watcher = nullptr;
	QFutureWatcher<QHash<int, QImage>>* m_game_icon_watcher = nullptr;
	QString m_game_backdrop_key;
	QString m_game_backdrop_loading_key;
	game_info m_game_backdrop_pending;
	QElapsedTimer m_game_backdrop_fade;
	const std::vector<item>& current_items() const;
	int& current_focus();
	void rebuild_items();
	void apply_p3t_icons();
	void update_game_version_labels();
	QString game_subtitle(int index) const;
	void refresh_game_backdrop();
	void load_game_backdrop();
	void load_missing_game_icons();
	void launch_selected();
};
