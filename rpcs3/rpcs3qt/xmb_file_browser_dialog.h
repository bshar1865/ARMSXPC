#pragma once

#include <QDialog>
#include <QStringList>

class QLabel;
class QListWidget;
class QPushButton;

// Desktop port of ARMSX3's XMB file browser. The caller embeds it as an XMB panel and
// supplies the file extensions accepted by the current XMB action.
class xmb_file_browser_dialog final : public QDialog
{
	Q_OBJECT
public:
	explicit xmb_file_browser_dialog(QString title, QStringList extensions, bool multiple = false,
		bool select_folder = false, QWidget* parent = nullptr);
	QStringList selected_paths() const { return m_selected_paths; }

protected:
	void keyPressEvent(QKeyEvent* event) override;

private:
	QStringList m_extensions;
	QStringList m_selected_paths;
	QString m_directory;
	QLabel* m_location = nullptr;
	QListWidget* m_files = nullptr;
	QPushButton* m_up = nullptr;
	QPushButton* m_use_folder = nullptr;
	QPushButton* m_select = nullptr;
	void show_directory(const QString& path);
	void accept_selection();
};
