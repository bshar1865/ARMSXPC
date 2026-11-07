#include "stdafx.h"
#include "xmb_file_browser_dialog.h"

#include <QDir>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QFileInfoList>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QVBoxLayout>

xmb_file_browser_dialog::xmb_file_browser_dialog(QString title, QStringList extensions, bool multiple,
	bool select_folder, QWidget* parent)
	: QDialog(parent)
	, m_extensions(std::move(extensions))
{
	setWindowTitle(title);
	setWindowFlags(Qt::Widget | Qt::FramelessWindowHint);
	setFocusPolicy(Qt::StrongFocus);
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(28, 22, 28, 24);
	layout->setSpacing(12);

	auto* heading = new QLabel(title, this);
	QFont heading_font = heading->font();
	heading_font.setPointSize(18);
	heading_font.setWeight(QFont::DemiBold);
	heading->setFont(heading_font);
	layout->addWidget(heading);

	m_location = new QLabel(this);
	m_location->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_location->setStyleSheet(QStringLiteral("color: rgba(255,255,255,170); padding: 3px 0;"));
	layout->addWidget(m_location);

	m_files = new QListWidget(this);
	m_files->setSelectionMode(multiple ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
	m_files->setIconSize(QSize(24, 24));
	layout->addWidget(m_files, 1);

	auto* buttons = new QHBoxLayout;
	m_up = new QPushButton(tr("Up One Folder"), this);
	m_use_folder = new QPushButton(tr("Use This Folder"), this);
	m_use_folder->setVisible(select_folder);
	m_select = new QPushButton(multiple ? tr("Select Files") : tr("Select"), this);
	QPushButton* cancel = new QPushButton(tr("Cancel"), this);
	buttons->addWidget(m_up);
	buttons->addWidget(m_use_folder);
	buttons->addStretch();
	buttons->addWidget(cancel);
	buttons->addWidget(m_select);
	layout->addLayout(buttons);

	setStyleSheet(QStringLiteral(
		"QDialog { background: rgba(12,14,20,248); color: #f2f2f2; }"
		"QLabel { color: #f2f2f2; }"
		"QListWidget { background: rgba(0,0,0,110); color: white; border: 1px solid rgba(255,255,255,55); outline: 0; }"
		"QListWidget::item { min-height: 34px; padding: 4px 8px; border-bottom: 1px solid rgba(255,255,255,15); }"
		"QListWidget::item:selected { background: rgba(145,115,205,145); }"
		"QPushButton { background: rgba(255,255,255,24); color: white; border: 1px solid rgba(255,255,255,65); padding: 7px 12px; }"
		"QPushButton:hover { background: rgba(150,125,210,100); }"
		"QPushButton:disabled { color: rgba(255,255,255,90); }"));

	connect(m_up, &QPushButton::clicked, this, [this]
	{
		const QDir current(m_directory);
		if (current.isRoot()) show_directory(QString());
		else show_directory(current.absoluteFilePath(QStringLiteral("..")));
	});
	connect(m_use_folder, &QPushButton::clicked, this, [this]
	{
		if (!m_directory.isEmpty()) { m_selected_paths = {m_directory}; accept(); }
	});
	connect(m_select, &QPushButton::clicked, this, &xmb_file_browser_dialog::accept_selection);
	connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
	connect(m_files, &QListWidget::itemActivated, this, [this](QListWidgetItem* item)
	{
		const QString path = item->data(Qt::UserRole).toString();
		if (QFileInfo(path).isDir()) show_directory(path);
		else if (m_files->selectionMode() != QAbstractItemView::ExtendedSelection) accept_selection();
	});
	connect(m_files, &QListWidget::itemSelectionChanged, this, [this, multiple]
	{
		m_select->setEnabled(multiple ? !m_files->selectedItems().isEmpty() : !m_files->selectedItems().isEmpty());
	});

	QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
	if (start.isEmpty() || !QFileInfo(start).isDir()) start = QDir::homePath();
	show_directory(start);
}

void xmb_file_browser_dialog::show_directory(const QString& path)
{
	if (path.isEmpty())
	{
		m_directory.clear();
		m_location->setText(tr("Storage"));
		m_up->setEnabled(false);
		m_use_folder->setEnabled(false);
		m_files->clear();
		QFileIconProvider icons;
		for (const QStorageInfo& storage : QStorageInfo::mountedVolumes())
		{
			if (!storage.isValid() || !storage.isReady()) continue;
			const QString root = storage.rootPath();
			if (root.isEmpty()) continue;
			auto* row = new QListWidgetItem(icons.icon(QFileInfo(root)), storage.displayName().isEmpty() ? root : storage.displayName(), m_files);
			row->setData(Qt::UserRole, root);
			row->setData(Qt::UserRole + 1, true);
		}
		m_select->setEnabled(false);
		return;
	}
	QDir dir(path);
	if (!dir.exists()) return;
	m_directory = dir.absolutePath();
	m_location->setText(QDir::toNativeSeparators(m_directory));
	m_up->setEnabled(true);
	m_use_folder->setEnabled(true);
	m_files->clear();
	QFileIconProvider icons;
	const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::IgnoreCase | QDir::DirsFirst);
	for (const QFileInfo& entry : entries)
	{
		if (entry.fileName().startsWith(QLatin1Char('.'))) continue;
		if (!entry.isDir() && !m_extensions.isEmpty() && !m_extensions.contains(entry.suffix(), Qt::CaseInsensitive)) continue;
		auto* row = new QListWidgetItem(icons.icon(entry), entry.fileName(), m_files);
		row->setData(Qt::UserRole, entry.absoluteFilePath());
		row->setToolTip(entry.absoluteFilePath());
	}
	m_select->setEnabled(false);
	if (m_files->count()) m_files->setCurrentRow(0);
}

void xmb_file_browser_dialog::accept_selection()
{
	m_selected_paths.clear();
	for (QListWidgetItem* item : m_files->selectedItems())
	{
		const QString path = item->data(Qt::UserRole).toString();
		if (QFileInfo(path).isFile()) m_selected_paths.push_back(path);
	}
	if (!m_selected_paths.isEmpty()) accept();
}

void xmb_file_browser_dialog::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Backspace)
	{
		if (m_directory.isEmpty()) reject();
		else
		{
			const QDir current(m_directory);
			show_directory(current.isRoot() ? QString() : current.absoluteFilePath(QStringLiteral("..")));
		}
		return;
	}
	if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
	{
		if (QListWidgetItem* selected = m_files->currentItem())
		{
			const QString path = selected->data(Qt::UserRole).toString();
			if (QFileInfo(path).isDir()) show_directory(path);
			else accept_selection();
		}
		return;
	}
	QDialog::keyPressEvent(event);
}
