/*
 * Copyright (C) 2007-2009 Sergio Pistone <sergio_pistone@yahoo.com.ar>
 * Copyright (C) 2010-2019 Mladen Milinkovic <max@smoothware.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "application.h"
#include "actions/useraction.h"
#include "actions/useractionnames.h"
#include "actions/kcodecactionext.h"
#include "actions/krecentfilesactionext.h"
#include "configs/configdialog.h"
#include "core/subtitleiterator.h"
#include "core/undo/undostack.h"
#include "gui/currentlinewidget.h"
#include "dialogs/actionwithtargetdialog.h"
#include "dialogs/shifttimesdialog.h"
#include "dialogs/adjusttimesdialog.h"
#include "dialogs/durationlimitsdialog.h"
#include "dialogs/autodurationsdialog.h"
#include "dialogs/changetextscasedialog.h"
#include "dialogs/fixoverlappingtimesdialog.h"
#include "dialogs/fixpunctuationdialog.h"
#include "dialogs/translatedialog.h"
#include "dialogs/smarttextsadjustdialog.h"
#include "dialogs/changeframeratedialog.h"
#include "dialogs/insertlinedialog.h"
#include "dialogs/removelinesdialog.h"
#include "dialogs/intinputdialog.h"
#include "dialogs/subtitlecolordialog.h"
#include "errors/errorfinder.h"
#include "errors/errortracker.h"
#include "formats/formatmanager.h"
#include "formats/outputformat.h"
#include "formats/textdemux/textdemux.h"
#include "helpers/commondefs.h"
#include "helpers/fileloadhelper.h"
#include "helpers/filesavehelper.h"
#include "gui/treeview/lineswidget.h"
#include "mainwindow.h"
#include "gui/playerwidget.h"
#include "profiler.h"
#include "scripting/scriptsmanager.h"
#include "speechprocessor/speechprocessor.h"
#include "utils/finder.h"
#include "utils/replacer.h"
#include "utils/speller.h"
#include "utils/translator.h"
#include "videoplayer/videoplayer.h"
#include "gui/waveformwidget.h"

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QStringBuilder>
#include <QTextCodec>
#include <QThread>
#include <QUndoGroup>
#include <QUndoStack>

#include <KActionCollection>
#include <KCharsets>
#include <KComboBox>
#include <KConfig>
#include <KMessageBox>
#include <KSelectAction>
#include <KStandardShortcut>
#include <KStandardAction>
#include <KToggleAction>
#include <KToolBar>
#include <kxmlgui_version.h>

using namespace SubtitleComposer;

Application::Application(int &argc, char **argv) :
	QApplication(argc, argv),
	m_subtitle(nullptr),
	m_subtitleUrl(),
	m_subtitleFileName(),
	m_subtitleEncoding(),
	m_subtitleFormat(),
	m_translationMode(false),
	m_subtitleTrUrl(),
	m_subtitleTrFileName(),
	m_subtitleTrEncoding(),
	m_subtitleTrFormat(),
	m_player(VideoPlayer::instance()),
	m_textDemux(nullptr),
	m_speechProcessor(nullptr),
	m_lastFoundLine(nullptr),
	m_mainWindow(nullptr),
	m_lastSubtitleUrl(QDir::homePath()),
	m_lastVideoUrl(QDir::homePath()),
	m_linkCurrentLineToPosition(false)
{
}

void
Application::init()
{
	setAttribute(Qt::AA_UseHighDpiPixmaps, true);

	m_mainWindow = new MainWindow();

	m_playerWidget = m_mainWindow->m_playerWidget;
	m_linesWidget = m_mainWindow->m_linesWidget;
	m_curLineWidget = m_mainWindow->m_curLineWidget;

	m_finder = new Finder(m_linesWidget);
	m_replacer = new Replacer(m_linesWidget);
	m_errorFinder = new ErrorFinder(m_linesWidget);
	m_speller = new Speller(m_linesWidget);

	m_errorTracker = new ErrorTracker(this);

	QStatusBar *statusBar = m_mainWindow->statusBar();
	QMargins m = statusBar->contentsMargins();
	m.setRight(m.left() + 5);
	statusBar->setContentsMargins(m);

	m_labSubFormat = new QLabel();
	statusBar->addPermanentWidget(m_labSubFormat);

	m_labSubEncoding = new QLabel();
	statusBar->addPermanentWidget(m_labSubEncoding);

	m_textDemux = new TextDemux(m_mainWindow);
	statusBar->addPermanentWidget(m_textDemux->progressWidget());

	m_speechProcessor = new SpeechProcessor(m_mainWindow);
	statusBar->addPermanentWidget(m_speechProcessor->progressWidget());

	m_scriptsManager = new ScriptsManager(this);

	m_undoStack = new UndoStack(m_mainWindow);

	UserActionManager *actionManager = UserActionManager::instance();
	actionManager->setLinesWidget(m_linesWidget);
	actionManager->setPlayer(m_player);
	actionManager->setFullScreenMode(false);

	setupActions();

	connect(m_undoStack, &UndoStack::undoTextChanged, [&](const QString &text){ static QAction *a = action(ACT_UNDO); a->setToolTip(text); });
	connect(m_undoStack, &UndoStack::redoTextChanged, [&](const QString &text){ static QAction *a = action(ACT_REDO); a->setToolTip(text); });
	connect(m_undoStack, &UndoStack::indexChanged, [&](int){ if(m_subtitle) m_subtitle->updateState(); });
	connect(m_undoStack, &UndoStack::cleanChanged, [&](bool){ if(m_subtitle) m_subtitle->updateState(); });

	connect(SCConfig::self(), &KCoreConfigSkeleton::configChanged, this, &Application::onConfigChanged);

	connect(m_player, &VideoPlayer::fileOpened, this, &Application::onPlayerFileOpened);
	connect(m_player, &VideoPlayer::playing, this, &Application::onPlayerPlaying);
	connect(m_player, &VideoPlayer::paused, this, &Application::onPlayerPaused);
	connect(m_player, &VideoPlayer::stopped, this, &Application::onPlayerStopped);
	connect(m_player, &VideoPlayer::audioStreamsChanged, this, &Application::onPlayerAudioStreamsChanged);
	connect(m_player, &VideoPlayer::textStreamsChanged, this, &Application::onPlayerTextStreamsChanged);
	connect(m_player, &VideoPlayer::activeAudioStreamChanged, this, &Application::onPlayerActiveAudioStreamChanged);
	connect(m_player, &VideoPlayer::muteChanged, this, &Application::onPlayerMuteChanged);

#define CONNECT_SUB(c, x) \
	connect(this, &Application::subtitleOpened, x, &c::setSubtitle); \
	connect(this, &Application::subtitleClosed, x, [this](){ x->setSubtitle(nullptr); });

	CONNECT_SUB(UserActionManager, UserActionManager::instance());
	CONNECT_SUB(MainWindow, m_mainWindow);
	CONNECT_SUB(PlayerWidget, m_playerWidget);
	CONNECT_SUB(LinesWidget, m_linesWidget);
	CONNECT_SUB(CurrentLineWidget, m_curLineWidget);
	CONNECT_SUB(Finder, m_finder);
	CONNECT_SUB(Replacer, m_replacer);
	CONNECT_SUB(ErrorFinder, m_errorFinder);
	CONNECT_SUB(Speller, m_speller);
	CONNECT_SUB(ErrorTracker, m_errorTracker);
	CONNECT_SUB(ScriptsManager, m_scriptsManager);
	CONNECT_SUB(WaveformWidget, m_mainWindow->m_waveformWidget);
	CONNECT_SUB(SpeechProcessor, m_speechProcessor);

#define CONNECT_TRANS(c, x) \
	connect(this, &Application::translationModeChanged, x, &c::setTranslationMode);

	CONNECT_TRANS(UserActionManager, UserActionManager::instance());
	CONNECT_TRANS(PlayerWidget, m_playerWidget);
	CONNECT_TRANS(WaveformWidget, m_mainWindow->m_waveformWidget);
	CONNECT_TRANS(LinesWidget, m_linesWidget);
	CONNECT_TRANS(CurrentLineWidget, m_curLineWidget);
	CONNECT_TRANS(Finder, m_finder);
	CONNECT_TRANS(Replacer, m_replacer);
	CONNECT_TRANS(ErrorFinder, m_errorFinder);
	CONNECT_TRANS(Speller, m_speller);

	connect(this, &Application::fullScreenModeChanged, actionManager, &UserActionManager::setFullScreenMode);

	connect(m_mainWindow->m_waveformWidget, &WaveformWidget::doubleClick, this, &Application::onWaveformDoubleClicked);
	connect(m_mainWindow->m_waveformWidget, &WaveformWidget::middleMouseDown, this, &Application::onWaveformMiddleMouseDown);
	connect(m_mainWindow->m_waveformWidget, &WaveformWidget::middleMouseUp, this, &Application::onWaveformMiddleMouseUp);

	connect(m_linesWidget, &LinesWidget::currentLineChanged, m_curLineWidget, &CurrentLineWidget::setCurrentLine);
	connect(m_linesWidget, &LinesWidget::lineDoubleClicked, this, &Application::onLineDoubleClicked);

	connect(m_playerWidget, &PlayerWidget::playingLineChanged, this, &Application::onPlayingLineChanged);

	connect(m_finder, &Finder::found, this, &Application::onHighlightLine);
	connect(m_replacer, &Replacer::found, this, &Application::onHighlightLine);
	connect(m_errorFinder, &ErrorFinder::found, [this](SubtitleLine *l){ onHighlightLine(l); });
	connect(m_speller, &Speller::misspelled, this, &Application::onHighlightLine);

	connect(m_textDemux, &TextDemux::onError, [&](const QString &message){ KMessageBox::sorry(m_mainWindow, message); });

	connect(m_speechProcessor, &SpeechProcessor::onError, [&](const QString &message){ KMessageBox::sorry(m_mainWindow, message); });

	m_playerWidget->plugActions();
	m_curLineWidget->setupActions();

	m_mainWindow->setupGUI();
	m_mainWindow->m_waveformWidget->updateActions();

	// Workaround for https://phabricator.kde.org/D13808
	// menubar can't be hidden so always show it
	m_mainWindow->findChild<QMenuBar *>(QString(), Qt::FindDirectChildrenOnly)->show();
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 1) && KXMLGUI_VERSION < ((5<<16)|(49<<8)|(0))
	// show rest if we're on broken KF5/Qt
	m_mainWindow->findChild<QStatusBar *>(QString(), Qt::FindDirectChildrenOnly)->show();
	m_mainWindow->findChild<QDockWidget *>(QStringLiteral("player_dock"), Qt::FindDirectChildrenOnly)->show();
	m_mainWindow->findChild<QDockWidget *>(QStringLiteral("waveform_dock"), Qt::FindDirectChildrenOnly)->show();
	foreach(KToolBar *toolbar, m_mainWindow->toolBars())
		toolbar->show();
#endif

	m_scriptsManager->reloadScripts();

	loadConfig();
}

Application::~Application()
{
	// NOTE: The Application destructor is called after all widgets are destroyed
	// (NOT BEFORE). Therefore is not possible to save the program settings (nor do
	// pretty much anything) at this point.

	// delete m_mainWindow; the window is destroyed when it's closed

	delete m_subtitle;
}

void
Application::loadConfig()
{
	KSharedConfig::Ptr config = KSharedConfig::openConfig();
	KConfigGroup group(config->group("Application Settings"));

	m_lastSubtitleUrl = QUrl(group.readPathEntry("LastSubtitleUrl", QDir::homePath()));
	m_recentSubtitlesAction->loadEntries(config->group("Recent Subtitles"));
	m_recentSubtitlesTrAction->loadEntries(config->group("Recent Translation Subtitles"));

	m_lastVideoUrl = QUrl(group.readPathEntry("LastVideoUrl", QDir::homePath()));
	m_recentVideosAction->loadEntries(config->group("Recent Videos"));

	m_player->setMuted(group.readEntry<bool>("Muted", false));
	m_player->setVolume(group.readEntry<double>("Volume", 100.0));

	((KToggleAction *)action(ACT_TOGGLE_MUTED))->setChecked(m_player->isMuted());

	KConfigGroup wfGroup(config->group("Waveform Widget"));
	action(ACT_WAVEFORM_AUTOSCROLL)->setChecked(wfGroup.readEntry<bool>("AutoScroll", true));
	m_mainWindow->m_waveformWidget->setWindowSize(wfGroup.readEntry<quint32>("Zoom", 6000));

	m_mainWindow->loadConfig();
	m_playerWidget->loadConfig();
	m_linesWidget->loadConfig();
	m_curLineWidget->loadConfig();
}

void
Application::saveConfig()
{
	KSharedConfig::Ptr config = KSharedConfig::openConfig();
	KConfigGroup group(config->group("Application Settings"));

	group.writePathEntry("LastSubtitleUrl", m_lastSubtitleUrl.toString());
	m_recentSubtitlesAction->saveEntries(config->group("Recent Subtitles"));
	m_recentSubtitlesTrAction->saveEntries(config->group("Recent Translation Subtitles"));

	group.writePathEntry("LastVideoUrl", m_lastVideoUrl.toString());
	m_recentVideosAction->saveEntries(config->group("Recent Videos"));

	group.writeEntry("Muted", m_player->isMuted());
	group.writeEntry("Volume", m_player->volume());

	KConfigGroup wfGroup(config->group("Waveform Widget"));
	wfGroup.writeEntry("AutoScroll", m_mainWindow->m_waveformWidget->autoScroll());
	wfGroup.writeEntry("Zoom", m_mainWindow->m_waveformWidget->windowSize());

	m_mainWindow->saveConfig();
	m_playerWidget->saveConfig();
	m_linesWidget->saveConfig();
	m_curLineWidget->saveConfig();
}

bool
Application::triggerAction(const QKeySequence &keySequence)
{
	QList<QAction *> actions = m_mainWindow->actionCollection()->actions();

	for(QList<QAction *>::ConstIterator it = actions.constBegin(), end = actions.constEnd(); it != end; ++it) {
		if((*it)->isEnabled()) {
			if(QAction * action = qobject_cast<QAction *>(*it)) {
				QKeySequence shortcut = action->shortcut();
				if(shortcut.matches(keySequence) == QKeySequence::ExactMatch) {
					action->trigger();
					return true;
				}
			} else {
				if((*it)->shortcut() == keySequence) {
					(*it)->trigger();
					return true;
				}
			}
		}
	}

	return false;
}

const QStringList &
Application::availableEncodingNames() const
{
	static QStringList encodingNames;

	if(encodingNames.empty()) {
		QStringList encodings(KCharsets::charsets()->availableEncodingNames());
		for(QStringList::Iterator it = encodings.begin(); it != encodings.end(); ++it) {
			bool found = false;
			QTextCodec *codec = KCharsets::charsets()->codecForName(*it, found);
			if(found)
				encodingNames.append(codec->name().toUpper());
		}
		encodingNames.sort();
	}

	return encodingNames;
}

void
Application::showPreferences()
{
	(new ConfigDialog(m_mainWindow, "scconfig", SCConfig::self()))->show();
}

void
Application::setupActions()
{
	KActionCollection *actionCollection = m_mainWindow->actionCollection();
	UserActionManager *actionManager = UserActionManager::instance();

	QAction *quitAction = KStandardAction::quit(m_mainWindow, SLOT(close()), actionCollection);
	quitAction->setStatusTip(i18n("Exit the application"));

	QAction *prefsAction = KStandardAction::preferences(this, SLOT(showPreferences()), actionCollection);
	prefsAction->setStatusTip(i18n("Configure Subtitle Composer"));

	QAction *newSubtitleAction = new QAction(actionCollection);
	newSubtitleAction->setIcon(QIcon::fromTheme("document-new"));
	newSubtitleAction->setText(i18nc("@action:inmenu Create a new subtitle", "New"));
	newSubtitleAction->setStatusTip(i18n("Create an empty subtitle"));
	actionCollection->setDefaultShortcuts(newSubtitleAction, KStandardShortcut::openNew());
	connect(newSubtitleAction, &QAction::triggered, this, &Application::newSubtitle);
	actionCollection->addAction(ACT_NEW_SUBTITLE, newSubtitleAction);
	actionManager->addAction(newSubtitleAction, UserAction::FullScreenOff);

	QAction *openSubtitleAction = new QAction(actionCollection);
	openSubtitleAction->setIcon(QIcon::fromTheme("document-open"));
	openSubtitleAction->setText(i18nc("@action:inmenu Open subtitle file", "Open Subtitle..."));
	openSubtitleAction->setStatusTip(i18n("Open subtitle file"));
	actionCollection->setDefaultShortcuts(openSubtitleAction, KStandardShortcut::open());
	connect(openSubtitleAction, SIGNAL(triggered()), this, SLOT(openSubtitle()));
	actionCollection->addAction(ACT_OPEN_SUBTITLE, openSubtitleAction);
	actionManager->addAction(openSubtitleAction, 0);

	m_reopenSubtitleAsAction = new KCodecActionExt(actionCollection, KCodecActionExt::Open);
	m_reopenSubtitleAsAction->setIcon(QIcon::fromTheme("view-refresh"));
	m_reopenSubtitleAsAction->setText(i18n("Reload As..."));
	m_reopenSubtitleAsAction->setStatusTip(i18n("Reload opened file with a different encoding"));
	connect(m_reopenSubtitleAsAction, &KCodecActionExt::triggered, this, &Application::reopenSubtitleWithCodec);
	actionCollection->addAction(ACT_REOPEN_SUBTITLE_AS, m_reopenSubtitleAsAction);
	actionManager->addAction(m_reopenSubtitleAsAction, UserAction::SubOpened | UserAction::SubPClean | UserAction::FullScreenOff);

	m_recentSubtitlesAction = new KRecentFilesActionExt(actionCollection);
	m_recentSubtitlesAction->setIcon(QIcon::fromTheme("document-open"));
	m_recentSubtitlesAction->setText(i18nc("@action:inmenu Open rencently used subtitle file", "Open &Recent Subtitle"));
	m_recentSubtitlesAction->setStatusTip(i18n("Open subtitle file"));
	connect(m_recentSubtitlesAction, SIGNAL(urlSelected(const QUrl &)), this, SLOT(openSubtitle(const QUrl &)));
	actionCollection->addAction(ACT_RECENT_SUBTITLES, m_recentSubtitlesAction);

	QAction *saveSubtitleAction = new QAction(actionCollection);
	saveSubtitleAction->setIcon(QIcon::fromTheme("document-save"));
	saveSubtitleAction->setText(i18n("Save"));
	saveSubtitleAction->setStatusTip(i18n("Save opened subtitle"));
	actionCollection->setDefaultShortcuts(saveSubtitleAction, KStandardShortcut::save());
	connect(saveSubtitleAction, &QAction::triggered, [&](){
		saveSubtitle(KCharsets::charsets()->codecForName(m_subtitleEncoding));
	});
	actionCollection->addAction(ACT_SAVE_SUBTITLE, saveSubtitleAction);
	actionManager->addAction(saveSubtitleAction, UserAction::SubPDirty | UserAction::FullScreenOff);

	m_saveSubtitleAsAction = new KCodecActionExt(actionCollection, KCodecActionExt::Save);
	m_saveSubtitleAsAction->setIcon(QIcon::fromTheme("document-save-as"));
	m_saveSubtitleAsAction->setText(i18n("Save As..."));
	m_saveSubtitleAsAction->setStatusTip(i18n("Save opened subtitle with different settings"));
	connect(m_saveSubtitleAsAction, &KCodecActionExt::triggered, this, &Application::saveSubtitleAs);
	connect(m_saveSubtitleAsAction, &QAction::triggered, this, [&](){ saveSubtitleAs(); });
	actionCollection->addAction(ACT_SAVE_SUBTITLE_AS, m_saveSubtitleAsAction);
	actionManager->addAction(m_saveSubtitleAsAction, UserAction::SubOpened | UserAction::FullScreenOff);

	QAction *closeSubtitleAction = new QAction(actionCollection);
	closeSubtitleAction->setIcon(QIcon::fromTheme("window-close"));
	closeSubtitleAction->setText(i18n("Close"));
	closeSubtitleAction->setStatusTip(i18n("Close opened subtitle"));
	actionCollection->setDefaultShortcuts(closeSubtitleAction, KStandardShortcut::close());
	connect(closeSubtitleAction, &QAction::triggered, this, &Application::closeSubtitle);
	actionCollection->addAction(ACT_CLOSE_SUBTITLE, closeSubtitleAction);
	actionManager->addAction(closeSubtitleAction, UserAction::SubOpened | UserAction::FullScreenOff);

	QAction *newSubtitleTrAction = new QAction(actionCollection);
	newSubtitleTrAction->setIcon(QIcon::fromTheme("document-new"));
	newSubtitleTrAction->setText(i18n("New Translation"));
	newSubtitleTrAction->setStatusTip(i18n("Create an empty translation subtitle"));
	actionCollection->setDefaultShortcut(newSubtitleTrAction, QKeySequence("Ctrl+Shift+N"));
	connect(newSubtitleTrAction, &QAction::triggered, this, &Application::newSubtitleTr);
	actionCollection->addAction(ACT_NEW_SUBTITLE_TR, newSubtitleTrAction);
	actionManager->addAction(newSubtitleTrAction, UserAction::SubOpened | UserAction::FullScreenOff);

	QAction *openSubtitleTrAction = new QAction(actionCollection);
	openSubtitleTrAction->setIcon(QIcon::fromTheme("document-open"));
	openSubtitleTrAction->setText(i18n("Open Translation..."));
	openSubtitleTrAction->setStatusTip(i18n("Open translation subtitle file"));
	actionCollection->setDefaultShortcut(openSubtitleTrAction, QKeySequence("Ctrl+Shift+O"));
	connect(openSubtitleTrAction, SIGNAL(triggered()), this, SLOT(openSubtitleTr()));
	actionCollection->addAction(ACT_OPEN_SUBTITLE_TR, openSubtitleTrAction);
	actionManager->addAction(openSubtitleTrAction, UserAction::SubOpened);

	m_reopenSubtitleTrAsAction = new KCodecActionExt(actionCollection, KCodecActionExt::Open);
	m_reopenSubtitleTrAsAction->setIcon(QIcon::fromTheme("view-refresh"));
	m_reopenSubtitleTrAsAction->setText(i18n("Reload Translation As..."));
	m_reopenSubtitleTrAsAction->setStatusTip(i18n("Reload opened translation file with a different encoding"));
	connect(m_reopenSubtitleTrAsAction, &KCodecActionExt::triggered, this, &Application::reopenSubtitleTrWithCodec);
	actionCollection->addAction(ACT_REOPEN_SUBTITLE_TR_AS, m_reopenSubtitleTrAsAction);
	actionManager->addAction(m_reopenSubtitleTrAsAction, UserAction::SubTrOpened | UserAction::SubSClean | UserAction::FullScreenOff);

	m_recentSubtitlesTrAction = new KRecentFilesActionExt(actionCollection);
	m_recentSubtitlesTrAction->setIcon(QIcon::fromTheme("document-open"));
	m_recentSubtitlesTrAction->setText(i18n("Open &Recent Translation"));
	m_recentSubtitlesTrAction->setStatusTip(i18n("Open translation subtitle file"));
	connect(m_recentSubtitlesTrAction, SIGNAL(urlSelected(const QUrl &)), this, SLOT(openSubtitleTr(const QUrl &)));
	actionCollection->addAction(ACT_RECENT_SUBTITLES_TR, m_recentSubtitlesTrAction);
	actionManager->addAction(m_recentSubtitlesTrAction, UserAction::SubOpened | UserAction::FullScreenOff);

	QAction *saveSubtitleTrAction = new QAction(actionCollection);
	saveSubtitleTrAction->setIcon(QIcon::fromTheme("document-save"));
	saveSubtitleTrAction->setText(i18n("Save Translation"));
	saveSubtitleTrAction->setStatusTip(i18n("Save opened translation subtitle"));
	actionCollection->setDefaultShortcut(saveSubtitleTrAction, QKeySequence("Ctrl+Shift+S"));
	connect(saveSubtitleTrAction, &QAction::triggered, [&](){
		saveSubtitleTr(KCharsets::charsets()->codecForName(m_subtitleTrEncoding));
	});
	actionCollection->addAction(ACT_SAVE_SUBTITLE_TR, saveSubtitleTrAction);
	actionManager->addAction(saveSubtitleTrAction, UserAction::SubSDirty | UserAction::FullScreenOff);

	m_saveSubtitleTrAsAction = new KCodecActionExt(actionCollection, KCodecActionExt::Save);
	m_saveSubtitleTrAsAction->setIcon(QIcon::fromTheme("document-save-as"));
	m_saveSubtitleTrAsAction->setText(i18n("Save Translation As..."));
	m_saveSubtitleTrAsAction->setStatusTip(i18n("Save opened translation subtitle with different settings"));
	connect(m_saveSubtitleTrAsAction, &KCodecActionExt::triggered, this, &Application::saveSubtitleTrAs);
	connect(m_saveSubtitleTrAsAction, &QAction::triggered, this, [&](){ saveSubtitleTrAs(); });
	actionCollection->addAction(ACT_SAVE_SUBTITLE_TR_AS, m_saveSubtitleTrAsAction);
	actionManager->addAction(m_saveSubtitleTrAsAction, UserAction::SubTrOpened | UserAction::FullScreenOff);

	QAction *closeSubtitleTrAction = new QAction(actionCollection);
	closeSubtitleTrAction->setIcon(QIcon::fromTheme("window-close"));
	closeSubtitleTrAction->setText(i18n("Close Translation"));
	closeSubtitleTrAction->setStatusTip(i18n("Close opened translation subtitle"));
	actionCollection->setDefaultShortcut(closeSubtitleTrAction, QKeySequence("Ctrl+Shift+F4; Ctrl+Shift+W"));
	connect(closeSubtitleTrAction, &QAction::triggered, this, &Application::closeSubtitleTr);
	actionCollection->addAction(ACT_CLOSE_SUBTITLE_TR, closeSubtitleTrAction);
	actionManager->addAction(closeSubtitleTrAction, UserAction::SubTrOpened | UserAction::FullScreenOff);

	QAction *undoAction = m_undoStack->createUndoAction(actionManager);
	undoAction->setIcon(QIcon::fromTheme("edit-undo"));
	actionCollection->setDefaultShortcuts(undoAction, KStandardShortcut::undo());
	actionCollection->addAction(ACT_UNDO, undoAction);

	QAction *redoAction = m_undoStack->createRedoAction(actionManager);
	redoAction->setIcon(QIcon::fromTheme("edit-redo"));
	actionCollection->setDefaultShortcuts(redoAction, KStandardShortcut::redo());
	actionCollection->addAction(ACT_REDO, redoAction);

	QAction *splitSubtitleAction = new QAction(actionCollection);
	splitSubtitleAction->setText(i18n("Split Subtitle..."));
	splitSubtitleAction->setStatusTip(i18n("Split the opened subtitle in two parts"));
	connect(splitSubtitleAction, &QAction::triggered, this, &Application::splitSubtitle);
	actionCollection->addAction(ACT_SPLIT_SUBTITLE, splitSubtitleAction);
	actionManager->addAction(splitSubtitleAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *joinSubtitlesAction = new QAction(actionCollection);
	joinSubtitlesAction->setText(i18n("Join Subtitles..."));
	joinSubtitlesAction->setStatusTip(i18n("Append to the opened subtitle another one"));
	connect(joinSubtitlesAction, &QAction::triggered, this, &Application::joinSubtitles);
	actionCollection->addAction(ACT_JOIN_SUBTITLES, joinSubtitlesAction);
	actionManager->addAction(joinSubtitlesAction, UserAction::SubOpened | UserAction::FullScreenOff);

	QAction *insertBeforeCurrentLineAction = new QAction(actionCollection);
	insertBeforeCurrentLineAction->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
	insertBeforeCurrentLineAction->setText(i18n("Insert Before"));
	insertBeforeCurrentLineAction->setStatusTip(i18n("Insert empty line before current one"));
	actionCollection->setDefaultShortcut(insertBeforeCurrentLineAction, QKeySequence("Ctrl+Insert"));
	connect(insertBeforeCurrentLineAction, &QAction::triggered, this, &Application::insertBeforeCurrentLine);
	actionCollection->addAction(ACT_INSERT_BEFORE_CURRENT_LINE, insertBeforeCurrentLineAction);
	actionManager->addAction(insertBeforeCurrentLineAction, UserAction::SubOpened | UserAction::FullScreenOff);

	QAction *insertAfterCurrentLineAction = new QAction(actionCollection);
	insertAfterCurrentLineAction->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
	insertAfterCurrentLineAction->setText(i18n("Insert After"));
	insertAfterCurrentLineAction->setStatusTip(i18n("Insert empty line after current one"));
	actionCollection->setDefaultShortcut(insertAfterCurrentLineAction, QKeySequence("Insert"));
	connect(insertAfterCurrentLineAction, &QAction::triggered, this, &Application::insertAfterCurrentLine);
	actionCollection->addAction(ACT_INSERT_AFTER_CURRENT_LINE, insertAfterCurrentLineAction);
	actionManager->addAction(insertAfterCurrentLineAction, UserAction::SubOpened | UserAction::FullScreenOff);

	QAction *removeSelectedLinesAction = new QAction(actionCollection);
	removeSelectedLinesAction->setIcon(QIcon::fromTheme(QStringLiteral("list-remove")));
	removeSelectedLinesAction->setText(i18n("Remove"));
	removeSelectedLinesAction->setStatusTip(i18n("Remove selected lines"));
	actionCollection->setDefaultShortcut(removeSelectedLinesAction, QKeySequence("Delete"));
	connect(removeSelectedLinesAction, &QAction::triggered, this, &Application::removeSelectedLines);
	actionCollection->addAction(ACT_REMOVE_SELECTED_LINES, removeSelectedLinesAction);
	actionManager->addAction(removeSelectedLinesAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *splitSelectedLinesAction = new QAction(actionCollection);
	splitSelectedLinesAction->setText(i18n("Split Lines"));
	splitSelectedLinesAction->setStatusTip(i18n("Split selected lines"));
	connect(splitSelectedLinesAction, &QAction::triggered, this, &Application::splitSelectedLines);
	actionCollection->addAction(ACT_SPLIT_SELECTED_LINES, splitSelectedLinesAction);
	actionManager->addAction(splitSelectedLinesAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *joinSelectedLinesAction = new QAction(actionCollection);
	joinSelectedLinesAction->setText(i18n("Join Lines"));
	joinSelectedLinesAction->setStatusTip(i18n("Join selected lines"));
	connect(joinSelectedLinesAction, &QAction::triggered, this, &Application::joinSelectedLines);
	actionCollection->addAction(ACT_JOIN_SELECTED_LINES, joinSelectedLinesAction);
	actionManager->addAction(joinSelectedLinesAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *selectAllLinesAction = new QAction(actionCollection);
	selectAllLinesAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-select-all")));
	selectAllLinesAction->setText(i18n("Select All"));
	selectAllLinesAction->setStatusTip(i18n("Select all lines"));
	actionCollection->setDefaultShortcuts(selectAllLinesAction, KStandardShortcut::selectAll());
	connect(selectAllLinesAction, &QAction::triggered, this, &Application::selectAllLines);
	actionCollection->addAction(ACT_SELECT_ALL_LINES, selectAllLinesAction);
	actionManager->addAction(selectAllLinesAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *gotoLineAction = new QAction(actionCollection);
	gotoLineAction->setText(i18n("Go to Line..."));
	gotoLineAction->setStatusTip(i18n("Go to specified line"));
	actionCollection->setDefaultShortcuts(gotoLineAction, KStandardShortcut::gotoLine());
	connect(gotoLineAction, &QAction::triggered, this, &Application::gotoLine);
	actionCollection->addAction(ACT_GOTO_LINE, gotoLineAction);
	actionManager->addAction(gotoLineAction, UserAction::SubHasLines);

	QAction *findAction = new QAction(actionCollection);
	findAction->setIcon(QIcon::fromTheme("edit-find"));
	findAction->setText(i18n("Find..."));
	findAction->setStatusTip(i18n("Find occurrences of strings or regular expressions"));
	actionCollection->setDefaultShortcuts(findAction, KStandardShortcut::find());
	connect(findAction, &QAction::triggered, this, &Application::find);
	actionCollection->addAction(ACT_FIND, findAction);
	actionManager->addAction(findAction, UserAction::SubHasLine);

	QAction *findNextAction = new QAction(actionCollection);
	findNextAction->setIcon(QIcon::fromTheme("go-down-search"));
	findNextAction->setText(i18n("Find Next"));
	findNextAction->setStatusTip(i18n("Find next occurrence of string or regular expression"));
	actionCollection->setDefaultShortcuts(findNextAction, KStandardShortcut::findNext());
	connect(findNextAction, &QAction::triggered, this, &Application::findNext);
	actionCollection->addAction(ACT_FIND_NEXT, findNextAction);
	actionManager->addAction(findNextAction, UserAction::SubHasLine);

	QAction *findPreviousAction = new QAction(actionCollection);
	findPreviousAction->setIcon(QIcon::fromTheme("go-up-search"));
	findPreviousAction->setText(i18n("Find Previous"));
	findPreviousAction->setStatusTip(i18n("Find previous occurrence of string or regular expression"));
	actionCollection->setDefaultShortcuts(findPreviousAction, KStandardShortcut::findPrev());
	connect(findPreviousAction, &QAction::triggered, this, &Application::findPrevious);
	actionCollection->addAction(ACT_FIND_PREVIOUS, findPreviousAction);
	actionManager->addAction(findPreviousAction, UserAction::SubHasLine);

	QAction *replaceAction = new QAction(actionCollection);
	replaceAction->setText(i18n("Replace..."));
	replaceAction->setStatusTip(i18n("Replace occurrences of strings or regular expressions"));
	actionCollection->setDefaultShortcuts(replaceAction, KStandardShortcut::replace());
	connect(replaceAction, &QAction::triggered, this, &Application::replace);
	actionCollection->addAction(ACT_REPLACE, replaceAction);
	actionManager->addAction(replaceAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *retrocedeCurrentLineAction = new QAction(actionCollection);
	retrocedeCurrentLineAction->setIcon(QIcon::fromTheme("go-down"));
	retrocedeCurrentLineAction->setText(i18n("Retrocede current line"));
	retrocedeCurrentLineAction->setStatusTip(i18n("Makes the line before the current one active"));
	actionCollection->setDefaultShortcut(retrocedeCurrentLineAction, QKeySequence("Alt+Up"));
	connect(retrocedeCurrentLineAction, &QAction::triggered, this, &Application::retrocedeCurrentLine);
	actionCollection->addAction(ACT_RETROCEDE_CURRENT_LINE, retrocedeCurrentLineAction);
	actionManager->addAction(retrocedeCurrentLineAction, UserAction::SubHasLines | UserAction::HasSelection);

	QAction *advanceCurrentLineAction = new QAction(actionCollection);
	advanceCurrentLineAction->setIcon(QIcon::fromTheme("go-down"));
	advanceCurrentLineAction->setText(i18n("Advance current line"));
	advanceCurrentLineAction->setStatusTip(i18n("Makes the line after the current one active"));
	actionCollection->setDefaultShortcut(advanceCurrentLineAction, QKeySequence("Alt+Down"));
	connect(advanceCurrentLineAction, &QAction::triggered, this, &Application::advanceCurrentLine);
	actionCollection->addAction(ACT_ADVANCE_CURRENT_LINE, advanceCurrentLineAction);
	actionManager->addAction(advanceCurrentLineAction, UserAction::SubHasLines | UserAction::HasSelection);

	QAction *detectErrorsAction = new QAction(actionCollection);
	detectErrorsAction->setIcon(QIcon::fromTheme("edit-find"));
	detectErrorsAction->setText(i18n("Detect Errors..."));
	detectErrorsAction->setStatusTip(i18n("Detect errors in the current subtitle"));
	actionCollection->setDefaultShortcut(detectErrorsAction, QKeySequence("Ctrl+E"));
	connect(detectErrorsAction, &QAction::triggered, this, &Application::detectErrors);
	actionCollection->addAction(ACT_DETECT_ERRORS, detectErrorsAction);
	actionManager->addAction(detectErrorsAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *clearErrorsAction = new QAction(actionCollection);
	clearErrorsAction->setText(i18n("Clear Errors/Marks"));
	clearErrorsAction->setStatusTip(i18n("Clear detected errors and marks in the current subtitle"));
	actionCollection->setDefaultShortcut(clearErrorsAction, QKeySequence("Ctrl+Shift+E"));
	connect(clearErrorsAction, &QAction::triggered, this, &Application::clearErrors);
	actionCollection->addAction(ACT_CLEAR_ERRORS, clearErrorsAction);
	actionManager->addAction(clearErrorsAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *nextErrorAction = new QAction(actionCollection);
	nextErrorAction->setIcon(QIcon::fromTheme("go-down-search"));
	nextErrorAction->setText(i18n("Select Next Error/Mark"));
	nextErrorAction->setStatusTip(i18n("Select next line with error or mark"));
	actionCollection->setDefaultShortcut(nextErrorAction, QKeySequence("F4"));
	connect(nextErrorAction, &QAction::triggered, this, &Application::selectNextError);
	actionCollection->addAction(ACT_SELECT_NEXT_ERROR, nextErrorAction);
	actionManager->addAction(nextErrorAction, UserAction::SubHasLine);

	QAction *prevErrorAction = new QAction(actionCollection);
	prevErrorAction->setIcon(QIcon::fromTheme("go-up-search"));
	prevErrorAction->setText(i18n("Select Previous Error/Mark"));
	prevErrorAction->setStatusTip(i18n("Select previous line with error or mark"));
	actionCollection->setDefaultShortcut(prevErrorAction, QKeySequence("Shift+F4"));
	connect(prevErrorAction, &QAction::triggered, this, &Application::selectPreviousError);
	actionCollection->addAction(ACT_SELECT_PREVIOUS_ERROR, prevErrorAction);
	actionManager->addAction(prevErrorAction, UserAction::SubHasLine);

	QAction *spellCheckAction = new QAction(actionCollection);
	spellCheckAction->setIcon(QIcon::fromTheme("tools-check-spelling"));
	spellCheckAction->setText(i18n("Spelling..."));
	spellCheckAction->setStatusTip(i18n("Check lines spelling"));
	connect(spellCheckAction, &QAction::triggered, this, &Application::spellCheck);
	actionCollection->addAction(ACT_SPELL_CHECK, spellCheckAction);
	actionManager->addAction(spellCheckAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *toggleSelectedLinesMarkAction = new QAction(actionCollection);
	toggleSelectedLinesMarkAction->setText(i18n("Toggle Mark"));
	toggleSelectedLinesMarkAction->setStatusTip(i18n("Toggle mark on selected lines"));
	actionCollection->setDefaultShortcut(toggleSelectedLinesMarkAction, QKeySequence("Ctrl+M"));
	connect(toggleSelectedLinesMarkAction, &QAction::triggered, this, &Application::toggleSelectedLinesMark);
	actionCollection->addAction(ACT_TOGGLE_SELECTED_LINES_MARK, toggleSelectedLinesMarkAction);
	actionManager->addAction(toggleSelectedLinesMarkAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *toggleSelectedLinesBoldAction = new QAction(actionCollection);
	toggleSelectedLinesBoldAction->setIcon(QIcon::fromTheme("format-text-bold"));
	toggleSelectedLinesBoldAction->setText(i18n("Toggle Bold"));
	toggleSelectedLinesBoldAction->setStatusTip(i18n("Toggle selected lines bold attribute"));
	actionCollection->setDefaultShortcut(toggleSelectedLinesBoldAction, QKeySequence("Ctrl+B"));
	connect(toggleSelectedLinesBoldAction, &QAction::triggered, this, &Application::toggleSelectedLinesBold);
	actionCollection->addAction(ACT_TOGGLE_SELECTED_LINES_BOLD, toggleSelectedLinesBoldAction);
	actionManager->addAction(toggleSelectedLinesBoldAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *toggleSelectedLinesItalicAction = new QAction(actionCollection);
	toggleSelectedLinesItalicAction->setIcon(QIcon::fromTheme("format-text-italic"));
	toggleSelectedLinesItalicAction->setText(i18n("Toggle Italic"));
	toggleSelectedLinesItalicAction->setStatusTip(i18n("Toggle selected lines italic attribute"));
	actionCollection->setDefaultShortcut(toggleSelectedLinesItalicAction, QKeySequence("Ctrl+I"));
	connect(toggleSelectedLinesItalicAction, &QAction::triggered, this, &Application::toggleSelectedLinesItalic);
	actionCollection->addAction(ACT_TOGGLE_SELECTED_LINES_ITALIC, toggleSelectedLinesItalicAction);
	actionManager->addAction(toggleSelectedLinesItalicAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *toggleSelectedLinesUnderlineAction = new QAction(actionCollection);
	toggleSelectedLinesUnderlineAction->setIcon(QIcon::fromTheme("format-text-underline"));
	toggleSelectedLinesUnderlineAction->setText(i18n("Toggle Underline"));
	toggleSelectedLinesUnderlineAction->setStatusTip(i18n("Toggle selected lines underline attribute"));
	actionCollection->setDefaultShortcut(toggleSelectedLinesUnderlineAction, QKeySequence("Ctrl+U"));
	connect(toggleSelectedLinesUnderlineAction, &QAction::triggered, this, &Application::toggleSelectedLinesUnderline);
	actionCollection->addAction(ACT_TOGGLE_SELECTED_LINES_UNDERLINE, toggleSelectedLinesUnderlineAction);
	actionManager->addAction(toggleSelectedLinesUnderlineAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *toggleSelectedLinesStrikeThroughAction = new QAction(actionCollection);
	toggleSelectedLinesStrikeThroughAction->setIcon(QIcon::fromTheme("format-text-strikethrough"));
	toggleSelectedLinesStrikeThroughAction->setText(i18n("Toggle Strike Through"));
	toggleSelectedLinesStrikeThroughAction->setStatusTip(i18n("Toggle selected lines strike through attribute"));
	actionCollection->setDefaultShortcut(toggleSelectedLinesStrikeThroughAction, QKeySequence("Ctrl+T"));
	connect(toggleSelectedLinesStrikeThroughAction, &QAction::triggered, this, &Application::toggleSelectedLinesStrikeThrough);
	actionCollection->addAction(ACT_TOGGLE_SELECTED_LINES_STRIKETHROUGH, toggleSelectedLinesStrikeThroughAction);
	actionManager->addAction(toggleSelectedLinesStrikeThroughAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *changeSelectedLinesColorAction = new QAction(actionCollection);
	changeSelectedLinesColorAction->setIcon(QIcon::fromTheme("format-text-color"));
	changeSelectedLinesColorAction->setText(i18n("Change Text Color"));
	changeSelectedLinesColorAction->setStatusTip(i18n("Change text color of selected lines"));
	actionCollection->setDefaultShortcut(changeSelectedLinesColorAction, QKeySequence("Ctrl+Shift+C"));
	connect(changeSelectedLinesColorAction, &QAction::triggered, this, &Application::changeSelectedLinesColor);
	actionCollection->addAction(ACT_CHANGE_SELECTED_LINES_TEXT_COLOR, changeSelectedLinesColorAction);
	actionManager->addAction(changeSelectedLinesColorAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *shiftAction = new QAction(actionCollection);
	shiftAction->setText(i18n("Shift..."));
	shiftAction->setStatusTip(i18n("Shift lines an specified amount of time"));
	actionCollection->setDefaultShortcut(shiftAction, QKeySequence("Ctrl+D"));
	connect(shiftAction, &QAction::triggered, this, &Application::shiftLines);
	actionCollection->addAction(ACT_SHIFT, shiftAction);
	actionManager->addAction(shiftAction, UserAction::SubHasLine | UserAction::FullScreenOff | UserAction::AnchorsNone);

	QAction *shiftSelectedLinesFwdAction = new QAction(actionCollection);
	actionCollection->setDefaultShortcut(shiftSelectedLinesFwdAction, QKeySequence("Shift++"));
	connect(shiftSelectedLinesFwdAction, &QAction::triggered, this, &Application::shiftSelectedLinesForwards);
	actionCollection->addAction(ACT_SHIFT_SELECTED_LINES_FORWARDS, shiftSelectedLinesFwdAction);
	actionManager->addAction(shiftSelectedLinesFwdAction, UserAction::HasSelection | UserAction::FullScreenOff | UserAction::EditableShowTime);

	QAction *shiftSelectedLinesBwdAction = new QAction(actionCollection);
	actionCollection->setDefaultShortcut(shiftSelectedLinesBwdAction, QKeySequence("Shift+-"));
	connect(shiftSelectedLinesBwdAction, &QAction::triggered, this, &Application::shiftSelectedLinesBackwards);
	actionCollection->addAction(ACT_SHIFT_SELECTED_LINES_BACKWARDS, shiftSelectedLinesBwdAction);
	actionManager->addAction(shiftSelectedLinesBwdAction, UserAction::HasSelection | UserAction::FullScreenOff | UserAction::EditableShowTime);

	QAction *adjustAction = new QAction(actionCollection);
	adjustAction->setText(i18n("Adjust..."));
	adjustAction->setStatusTip(i18n("Linearly adjust all lines to a specified interval"));
	actionCollection->setDefaultShortcut(adjustAction, QKeySequence("Ctrl+J"));
	connect(adjustAction, &QAction::triggered, this, &Application::adjustLines);
	actionCollection->addAction(ACT_ADJUST, adjustAction);
	actionManager->addAction(adjustAction, UserAction::SubHasLines | UserAction::FullScreenOff | UserAction::AnchorsNone);

	QAction *sortLinesAction = new QAction(actionCollection);
	sortLinesAction->setText(i18n("Sort..."));
	sortLinesAction->setStatusTip(i18n("Sort lines based on their show time"));
	connect(sortLinesAction, &QAction::triggered, this, &Application::sortLines);
	actionCollection->addAction(ACT_SORT_LINES, sortLinesAction);
	actionManager->addAction(sortLinesAction, UserAction::SubHasLines | UserAction::FullScreenOff);

	QAction *changeFrameRateAction = new QAction(actionCollection);
	changeFrameRateAction->setText(i18n("Change Frame Rate..."));
	changeFrameRateAction->setStatusTip(i18n("Retime subtitle lines by reinterpreting its frame rate"));
	connect(changeFrameRateAction, &QAction::triggered, this, &Application::changeFrameRate);
	actionCollection->addAction(ACT_CHANGE_FRAME_RATE, changeFrameRateAction);
	actionManager->addAction(changeFrameRateAction, UserAction::SubOpened | UserAction::FullScreenOff | UserAction::AnchorsNone);

	QAction *durationLimitsAction = new QAction(actionCollection);
	durationLimitsAction->setText(i18n("Enforce Duration Limits..."));
	durationLimitsAction->setStatusTip(i18n("Enforce lines minimum and/or maximum duration limits"));
	actionCollection->setDefaultShortcut(durationLimitsAction, QKeySequence("Ctrl+L"));
	connect(durationLimitsAction, &QAction::triggered, this, &Application::enforceDurationLimits);
	actionCollection->addAction(ACT_DURATION_LIMITS, durationLimitsAction);
	actionManager->addAction(durationLimitsAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *autoDurationsAction = new QAction(actionCollection);
	autoDurationsAction->setText(i18n("Set Automatic Durations..."));
	autoDurationsAction->setStatusTip(i18n("Set lines durations based on amount of letters, words and line breaks"));
	connect(autoDurationsAction, &QAction::triggered, this, &Application::setAutoDurations);
	actionCollection->addAction(ACT_AUTOMATIC_DURATIONS, autoDurationsAction);
	actionManager->addAction(autoDurationsAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *maximizeDurationsAction = new QAction(actionCollection);
	maximizeDurationsAction->setText(i18n("Maximize Durations..."));
	maximizeDurationsAction->setStatusTip(i18n("Extend lines durations up to their next lines show time"));
	connect(maximizeDurationsAction, &QAction::triggered, this, &Application::maximizeDurations);
	actionCollection->addAction(ACT_MAXIMIZE_DURATIONS, maximizeDurationsAction);
	actionManager->addAction(maximizeDurationsAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *fixOverlappingLinesAction = new QAction(actionCollection);
	fixOverlappingLinesAction->setText(i18n("Fix Overlapping Times..."));
	fixOverlappingLinesAction->setStatusTip(i18n("Fix lines overlapping times"));
	connect(fixOverlappingLinesAction, &QAction::triggered, this, &Application::fixOverlappingLines);
	actionCollection->addAction(ACT_FIX_OVERLAPPING_LINES, fixOverlappingLinesAction);
	actionManager->addAction(fixOverlappingLinesAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *syncWithSubtitleAction = new QAction(actionCollection);
	syncWithSubtitleAction->setText(i18n("Synchronize with Subtitle..."));
	syncWithSubtitleAction->setStatusTip(i18n("Copy timing information from another subtitle"));
	connect(syncWithSubtitleAction, &QAction::triggered, this, &Application::syncWithSubtitle);
	actionCollection->addAction(ACT_SYNC_WITH_SUBTITLE, syncWithSubtitleAction);
	actionManager->addAction(syncWithSubtitleAction, UserAction::SubHasLine | UserAction::FullScreenOff | UserAction::AnchorsNone);

	QAction *breakLinesAction = new QAction(actionCollection);
	breakLinesAction->setText(i18n("Break Lines..."));
	breakLinesAction->setStatusTip(i18n("Automatically set line breaks"));
	connect(breakLinesAction, &QAction::triggered, this, &Application::breakLines);
	actionCollection->addAction(ACT_ADJUST_TEXTS, breakLinesAction);
	actionManager->addAction(breakLinesAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *unbreakTextsAction = new QAction(actionCollection);
	unbreakTextsAction->setText(i18n("Unbreak Lines..."));
	unbreakTextsAction->setStatusTip(i18n("Remove line breaks from lines"));
	connect(unbreakTextsAction, &QAction::triggered, this, &Application::unbreakTexts);
	actionCollection->addAction(ACT_UNBREAK_TEXTS, unbreakTextsAction);
	actionManager->addAction(unbreakTextsAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *simplifySpacesAction = new QAction(actionCollection);
	simplifySpacesAction->setText(i18n("Simplify Spaces..."));
	simplifySpacesAction->setStatusTip(i18n("Remove unneeded spaces from lines"));
	connect(simplifySpacesAction, &QAction::triggered, this, &Application::simplifySpaces);
	actionCollection->addAction(ACT_SIMPLIFY_SPACES, simplifySpacesAction);
	actionManager->addAction(simplifySpacesAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *changeCaseAction = new QAction(actionCollection);
	changeCaseAction->setText(i18n("Change Case..."));
	changeCaseAction->setStatusTip(i18n("Change lines text to upper, lower, title or sentence case"));
	connect(changeCaseAction, &QAction::triggered, this, &Application::changeCase);
	actionCollection->addAction(ACT_CHANGE_CASE, changeCaseAction);
	actionManager->addAction(changeCaseAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *fixPunctuationAction = new QAction(actionCollection);
	fixPunctuationAction->setText(i18n("Fix Punctuation..."));
	fixPunctuationAction->setStatusTip(i18n("Fix punctuation errors in lines"));
	connect(fixPunctuationAction, &QAction::triggered, this, &Application::fixPunctuation);
	actionCollection->addAction(ACT_FIX_PUNCTUATION, fixPunctuationAction);
	actionManager->addAction(fixPunctuationAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *translateAction = new QAction(actionCollection);
	translateAction->setText(i18n("Translate..."));
	translateAction->setStatusTip(i18n("Translate lines texts using Google services"));
	connect(translateAction, SIGNAL(triggered()), this, SLOT(translate()));
	actionCollection->addAction(ACT_TRANSLATE, translateAction);
	actionManager->addAction(translateAction, UserAction::SubHasLine | UserAction::FullScreenOff);

	QAction *editCurrentLineInPlaceAction = new QAction(actionCollection);
	editCurrentLineInPlaceAction->setText(i18n("Edit Line in Place"));
	editCurrentLineInPlaceAction->setStatusTip(i18n("Edit current line text in place"));
	actionCollection->setDefaultShortcut(editCurrentLineInPlaceAction, QKeySequence("F2"));
	connect(editCurrentLineInPlaceAction, &QAction::triggered, m_linesWidget, &LinesWidget::editCurrentLineInPlace);
	actionCollection->addAction(ACT_EDIT_CURRENT_LINE_IN_PLACE, editCurrentLineInPlaceAction);
	actionManager->addAction(editCurrentLineInPlaceAction, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *openVideoAction = new QAction(actionCollection);
	openVideoAction->setIcon(QIcon::fromTheme("document-open"));
	openVideoAction->setText(i18n("Open Video..."));
	openVideoAction->setStatusTip(i18n("Open video file"));
	connect(openVideoAction, SIGNAL(triggered()), this, SLOT(openVideo()));
	actionCollection->addAction(ACT_OPEN_VIDEO, openVideoAction);

	m_recentVideosAction = new KRecentFilesActionExt(actionCollection);
	m_recentVideosAction->setIcon(QIcon::fromTheme("document-open"));
	m_recentVideosAction->setText(i18n("Open &Recent Video"));
	m_recentVideosAction->setStatusTip(i18n("Open video file"));
	connect(m_recentVideosAction, SIGNAL(urlSelected(const QUrl &)), this, SLOT(openVideo(const QUrl &)));
	actionCollection->addAction(ACT_RECENT_VIDEOS, m_recentVideosAction);

	QAction *demuxTextStreamAction = new KSelectAction(actionCollection);
	QMenu *demuxTextStreamActionMenu = new QMenu(m_mainWindow);
	demuxTextStreamAction->setMenu(demuxTextStreamActionMenu);
	demuxTextStreamAction->setIcon(QIcon::fromTheme("select_stream"));
	demuxTextStreamAction->setText(i18n("Import Subtitle Stream"));
	demuxTextStreamAction->setStatusTip(i18n("Import subtitle stream into subtitle editor"));
	connect(demuxTextStreamActionMenu, &QMenu::triggered, [this](QAction *action){ demuxTextStream(action->data().value<int>()); });
	actionCollection->addAction(ACT_DEMUX_TEXT_STREAM, demuxTextStreamAction);

	QAction *speechImportStreamAction = new KSelectAction(actionCollection);
	QMenu *speechImportStreamActionMenu = new QMenu(m_mainWindow);
	speechImportStreamAction->setMenu(speechImportStreamActionMenu);
	speechImportStreamAction->setIcon(QIcon::fromTheme("select-stream"));
	speechImportStreamAction->setText(i18n("Recognize Speech"));
	speechImportStreamAction->setStatusTip(i18n("Recognize speech in audio stream"));
	connect(speechImportStreamActionMenu, &QMenu::triggered, [this](QAction *action){ speechImportAudioStream(action->data().value<int>()); });
	actionCollection->addAction(ACT_ASR_IMPORT_AUDIO_STREAM, speechImportStreamAction);

	QAction *closeVideoAction = new QAction(actionCollection);
	closeVideoAction->setIcon(QIcon::fromTheme("window-close"));
	closeVideoAction->setText(i18n("Close Video"));
	closeVideoAction->setStatusTip(i18n("Close video file"));
	connect(closeVideoAction, &QAction::triggered, m_player, &VideoPlayer::closeFile);
	actionCollection->addAction(ACT_CLOSE_VIDEO, closeVideoAction);
	actionManager->addAction(closeVideoAction, UserAction::VideoOpened);

	KToggleAction *fullScreenAction = new KToggleAction(actionCollection);
	fullScreenAction->setIcon(QIcon::fromTheme("view-fullscreen"));
	fullScreenAction->setText(i18n("Full Screen Mode"));
	fullScreenAction->setStatusTip(i18n("Toggle full screen mode"));
	actionCollection->setDefaultShortcuts(fullScreenAction, KStandardShortcut::fullScreen());
	connect(fullScreenAction, &QAction::toggled, this, &Application::setFullScreenMode);
	actionCollection->addAction(ACT_TOGGLE_FULL_SCREEN, fullScreenAction);

	QAction *stopAction = new QAction(actionCollection);
	stopAction->setIcon(QIcon::fromTheme("media-playback-stop"));
	stopAction->setText(i18n("Stop"));
	stopAction->setStatusTip(i18n("Stop video playback"));
	connect(stopAction, &QAction::triggered, m_player, &VideoPlayer::stop);
	actionCollection->addAction(ACT_STOP, stopAction);
	actionManager->addAction(stopAction, UserAction::VideoPlaying);

	QAction *playPauseAction = new QAction(actionCollection);
	playPauseAction->setIcon(QIcon::fromTheme("media-playback-start"));
	playPauseAction->setText(i18n("Play"));
	playPauseAction->setStatusTip(i18n("Toggle video playing/paused"));
	actionCollection->setDefaultShortcut(playPauseAction, QKeySequence("Ctrl+Space"));
	connect(playPauseAction, &QAction::triggered, m_player, &VideoPlayer::togglePlayPaused);
	actionCollection->addAction(ACT_PLAY_PAUSE, playPauseAction);
	actionManager->addAction(playPauseAction, UserAction::VideoOpened);

	QAction *seekBackwardAction = new QAction(actionCollection);
	seekBackwardAction->setIcon(QIcon::fromTheme("media-seek-backward"));
	seekBackwardAction->setText(i18n("Seek Backward"));
	actionCollection->setDefaultShortcut(seekBackwardAction, QKeySequence("Left"));
	connect(seekBackwardAction, &QAction::triggered, this, &Application::seekBackward);
	actionCollection->addAction(ACT_SEEK_BACKWARD, seekBackwardAction);
	actionManager->addAction(seekBackwardAction, UserAction::VideoPlaying);

	QAction *seekForwardAction = new QAction(actionCollection);
	seekForwardAction->setIcon(QIcon::fromTheme("media-seek-forward"));
	seekForwardAction->setText(i18n("Seek Forward"));
	actionCollection->setDefaultShortcut(seekForwardAction, QKeySequence("Right"));
	connect(seekForwardAction, &QAction::triggered, this, &Application::seekForward);
	actionCollection->addAction(ACT_SEEK_FORWARD, seekForwardAction);
	actionManager->addAction(seekForwardAction, UserAction::VideoPlaying);

	QAction *stepFrameBackwardAction = new QAction(actionCollection);
	stepFrameBackwardAction->setIcon(QIcon::fromTheme("media-step-backward"));
	stepFrameBackwardAction->setText(i18n("Frame Step Backward"));
	actionCollection->setDefaultShortcut(stepFrameBackwardAction, QKeySequence("Ctrl+Left"));
	connect(stepFrameBackwardAction, &QAction::triggered, this, &Application::stepBackward);
	actionCollection->addAction(ACT_STEP_FRAME_BACKWARD, stepFrameBackwardAction);
	actionManager->addAction(stepFrameBackwardAction, UserAction::VideoPlaying);

	QAction *stepFrameForwardAction = new QAction(actionCollection);
	stepFrameForwardAction->setIcon(QIcon::fromTheme("media-step-forward"));
	stepFrameForwardAction->setText(i18n("Frame Step Forward"));
	actionCollection->setDefaultShortcut(stepFrameForwardAction, QKeySequence("Ctrl+Right"));
	connect(stepFrameForwardAction, &QAction::triggered, this, &Application::stepForward);
	actionCollection->addAction(ACT_STEP_FRAME_FORWARD, stepFrameForwardAction);
	actionManager->addAction(stepFrameForwardAction, UserAction::VideoPlaying);

	QAction *seekToPrevLineAction = new QAction(actionCollection);
	seekToPrevLineAction->setIcon(QIcon::fromTheme("media-skip-backward"));
	seekToPrevLineAction->setText(i18n("Jump to Previous Line"));
	seekToPrevLineAction->setStatusTip(i18n("Seek to previous subtitle line show time"));
	actionCollection->setDefaultShortcut(seekToPrevLineAction, QKeySequence("Shift+Left"));
	connect(seekToPrevLineAction, &QAction::triggered, this, &Application::seekToPrevLine);
	actionCollection->addAction(ACT_SEEK_TO_PREVIOUS_LINE, seekToPrevLineAction);
	actionManager->addAction(seekToPrevLineAction, UserAction::SubHasLine | UserAction::VideoPlaying);

	QAction *playrateIncreaseAction = new QAction(actionCollection);
	playrateIncreaseAction->setIcon(QIcon::fromTheme(QStringLiteral("playrate_plus")));
	playrateIncreaseAction->setText(i18n("Increase media play rate"));
	playrateIncreaseAction->setStatusTip(i18n("Increase media playback rate"));
	connect(playrateIncreaseAction, &QAction::triggered, this, &Application::playrateIncrease);
	actionCollection->addAction(ACT_PLAY_RATE_INCREASE, playrateIncreaseAction);
	actionManager->addAction(playrateIncreaseAction, UserAction::VideoPlaying);

	QAction *playrateDecreaseAction = new QAction(actionCollection);
	playrateDecreaseAction->setIcon(QIcon::fromTheme(QStringLiteral("playrate_minus")));
	playrateDecreaseAction->setText(i18n("Decrease media play rate"));
	playrateDecreaseAction->setStatusTip(i18n("Decrease media playback rate"));
	connect(playrateDecreaseAction, &QAction::triggered, this, &Application::playrateDecrease);
	actionCollection->addAction(ACT_PLAY_RATE_DECREASE, playrateDecreaseAction);
	actionManager->addAction(playrateDecreaseAction, UserAction::VideoPlaying);

	QAction *playCurrentLineAndPauseAction = new QAction(actionCollection);
	playCurrentLineAndPauseAction->setText(i18n("Play Current Line and Pause"));
	playCurrentLineAndPauseAction->setStatusTip(i18n("Seek to start of current subtitle line show time, play it and then pause"));
	actionCollection->setDefaultShortcut(playCurrentLineAndPauseAction, QKeySequence("Ctrl+Shift+Left"));
	connect(playCurrentLineAndPauseAction, &QAction::triggered, this, &Application::playOnlyCurrentLine);
	actionCollection->addAction(ACT_PLAY_CURRENT_LINE_AND_PAUSE, playCurrentLineAndPauseAction);
	actionManager->addAction(playCurrentLineAndPauseAction, UserAction::SubHasLine | UserAction::VideoPlaying);

	QAction *seekToNextLineAction = new QAction(actionCollection);
	seekToNextLineAction->setIcon(QIcon::fromTheme("media-skip-forward"));
	seekToNextLineAction->setText(i18n("Jump to Next Line"));
	seekToNextLineAction->setStatusTip(i18n("Seek to next subtitle line show time"));
	actionCollection->setDefaultShortcut(seekToNextLineAction, QKeySequence("Shift+Right"));
	connect(seekToNextLineAction, &QAction::triggered, this, &Application::seekToNextLine);
	actionCollection->addAction(ACT_SEEK_TO_NEXT_LINE, seekToNextLineAction);
	actionManager->addAction(seekToNextLineAction, UserAction::SubHasLine | UserAction::VideoPlaying);

	QAction *setCurrentLineShowTimeFromVideoAction = new QAction(actionCollection);
	setCurrentLineShowTimeFromVideoAction->setIcon(QIcon::fromTheme(QStringLiteral("set_show_time")));
	setCurrentLineShowTimeFromVideoAction->setText(i18n("Set Current Line Show Time"));
	setCurrentLineShowTimeFromVideoAction->setStatusTip(i18n("Set current line show time to video position"));
	actionCollection->setDefaultShortcut(setCurrentLineShowTimeFromVideoAction, QKeySequence("Shift+Z"));
	connect(setCurrentLineShowTimeFromVideoAction, &QAction::triggered, this, &Application::setCurrentLineShowTimeFromVideo);
	actionCollection->addAction(ACT_SET_CURRENT_LINE_SHOW_TIME, setCurrentLineShowTimeFromVideoAction);
	actionManager->addAction(setCurrentLineShowTimeFromVideoAction, UserAction::HasSelection | UserAction::VideoPlaying | UserAction::EditableShowTime);

	QAction *setCurrentLineHideTimeFromVideoAction = new QAction(actionCollection);
	setCurrentLineHideTimeFromVideoAction->setIcon(QIcon::fromTheme(QStringLiteral("set_hide_time")));
	setCurrentLineHideTimeFromVideoAction->setText(i18n("Set Current Line Hide Time"));
	setCurrentLineHideTimeFromVideoAction->setStatusTip(i18n("Set current line hide time to video position"));
	actionCollection->setDefaultShortcut(setCurrentLineHideTimeFromVideoAction, QKeySequence("Shift+X"));
	connect(setCurrentLineHideTimeFromVideoAction, &QAction::triggered, this, &Application::setCurrentLineHideTimeFromVideo);
	actionCollection->addAction(ACT_SET_CURRENT_LINE_HIDE_TIME, setCurrentLineHideTimeFromVideoAction);
	actionManager->addAction(setCurrentLineHideTimeFromVideoAction, UserAction::HasSelection | UserAction::VideoPlaying | UserAction::EditableShowTime);

	KToggleAction *currentLineFollowsVideoAction = new KToggleAction(actionCollection);
	currentLineFollowsVideoAction->setIcon(QIcon::fromTheme(QStringLiteral("current_line_follows_video")));
	currentLineFollowsVideoAction->setText(i18n("Current Line Follows Video"));
	currentLineFollowsVideoAction->setStatusTip(i18n("Make current line follow the playing video position"));
	connect(currentLineFollowsVideoAction, &QAction::toggled, this, &Application::onLinkCurrentLineToVideoToggled);
	actionCollection->addAction(ACT_CURRENT_LINE_FOLLOWS_VIDEO, currentLineFollowsVideoAction);

	KToggleAction *toggleMuteAction = new KToggleAction(actionCollection);
	toggleMuteAction->setIcon(QIcon::fromTheme("audio-volume-muted"));
	toggleMuteAction->setText(i18nc("@action:inmenu Toggle audio muted", "Mute"));
	toggleMuteAction->setStatusTip(i18n("Enable/disable playback sound"));
	actionCollection->setDefaultShortcut(toggleMuteAction, QKeySequence("/"));
	connect(toggleMuteAction, &QAction::toggled, m_player, &VideoPlayer::setMuted);
	actionCollection->addAction(ACT_TOGGLE_MUTED, toggleMuteAction);

	QAction *increaseVolumeAction = new QAction(actionCollection);
	increaseVolumeAction->setIcon(QIcon::fromTheme("audio-volume-high"));
	increaseVolumeAction->setText(i18n("Increase Volume"));
	increaseVolumeAction->setStatusTip(i18n("Increase volume by 5%"));
	actionCollection->setDefaultShortcut(increaseVolumeAction, Qt::Key_Plus);
	connect(increaseVolumeAction, SIGNAL(triggered()), m_player, SLOT(increaseVolume()));
	actionCollection->addAction(ACT_INCREASE_VOLUME, increaseVolumeAction);

	QAction *decreaseVolumeAction = new QAction(actionCollection);
	decreaseVolumeAction->setIcon(QIcon::fromTheme("audio-volume-low"));
	decreaseVolumeAction->setText(i18n("Decrease Volume"));
	decreaseVolumeAction->setStatusTip(i18n("Decrease volume by 5%"));
	actionCollection->setDefaultShortcut(decreaseVolumeAction, Qt::Key_Minus);
	connect(decreaseVolumeAction, SIGNAL(triggered()), m_player, SLOT(decreaseVolume()));
	actionCollection->addAction(ACT_DECREASE_VOLUME, decreaseVolumeAction);

	KSelectAction *setActiveAudioStreamAction = new KSelectAction(actionCollection);
	setActiveAudioStreamAction->setIcon(QIcon::fromTheme(QStringLiteral("select_stream")));
	setActiveAudioStreamAction->setText(i18n("Audio Streams"));
	setActiveAudioStreamAction->setStatusTip(i18n("Select active audio stream"));
	connect(setActiveAudioStreamAction, QOverload<int>::of(&KSelectAction::triggered), m_player, &VideoPlayer::selectAudioStream);
	actionCollection->addAction(ACT_SET_ACTIVE_AUDIO_STREAM, setActiveAudioStreamAction);
	actionManager->addAction(setActiveAudioStreamAction, UserAction::VideoOpened);

	QAction *increaseSubtitleFontAction = new QAction(actionCollection);
	increaseSubtitleFontAction->setIcon(QIcon::fromTheme("format-font-size-more"));
	increaseSubtitleFontAction->setText(i18n("Increase Font Size"));
	increaseSubtitleFontAction->setStatusTip(i18n("Increase subtitles font size by 1 point"));
	actionCollection->setDefaultShortcut(increaseSubtitleFontAction, QKeySequence("Alt++"));
	connect(increaseSubtitleFontAction, &QAction::triggered, m_playerWidget, &PlayerWidget::increaseFontSize);
	actionCollection->addAction(ACT_INCREASE_SUBTITLE_FONT, increaseSubtitleFontAction);

	QAction *decreaseSubtitleFontAction = new QAction(actionCollection);
	decreaseSubtitleFontAction->setIcon(QIcon::fromTheme("format-font-size-less"));
	decreaseSubtitleFontAction->setText(i18n("Decrease Font Size"));
	decreaseSubtitleFontAction->setStatusTip(i18n("Decrease subtitles font size by 1 point"));
	actionCollection->setDefaultShortcut(decreaseSubtitleFontAction, QKeySequence("Alt+-"));
	connect(decreaseSubtitleFontAction, &QAction::triggered, m_playerWidget, &PlayerWidget::decreaseFontSize);
	actionCollection->addAction(ACT_DECREASE_SUBTITLE_FONT, decreaseSubtitleFontAction);

	KSelectAction *setActiveSubtitleStreamAction = new KSelectAction(actionCollection);
	setActiveSubtitleStreamAction->setIcon(QIcon::fromTheme(QStringLiteral("select_stream")));
	setActiveSubtitleStreamAction->setText(i18n("Displayed Subtitle Text"));
	setActiveSubtitleStreamAction->setStatusTip(i18n("Select visible subtitle text"));
	connect(setActiveSubtitleStreamAction, SIGNAL(triggered(int)), this, SLOT(setActiveSubtitleStream(int)));
	actionCollection->addAction(ACT_SET_ACTIVE_SUBTITLE_STREAM, setActiveSubtitleStreamAction);
	actionManager->addAction(setActiveSubtitleStreamAction, UserAction::SubTrOpened);

	QAction *shiftToVideoPositionAction = new QAction(actionCollection);
	shiftToVideoPositionAction->setText(i18n("Shift to Video Position"));
	shiftToVideoPositionAction->setStatusTip(i18n("Set current line show time to video position by equally shifting all lines"));
	actionCollection->setDefaultShortcut(shiftToVideoPositionAction, QKeySequence("Shift+A"));
	connect(shiftToVideoPositionAction, &QAction::triggered, this, &Application::shiftToVideoPosition);
	actionCollection->addAction(ACT_SHIFT_TO_VIDEO_POSITION, shiftToVideoPositionAction);
	actionManager->addAction(shiftToVideoPositionAction, UserAction::HasSelection | UserAction::VideoPlaying | UserAction::FullScreenOff | UserAction::EditableShowTime);

	QAction *adjustToVideoPositionAnchorLastAction = new QAction(actionCollection);
	adjustToVideoPositionAnchorLastAction->setText(i18n("Adjust to Video Position (Anchor Last Line)"));
	adjustToVideoPositionAnchorLastAction->setStatusTip(i18n("Set current line to video position by fixing the last line and linearly adjusting the other ones"));
	actionCollection->setDefaultShortcut(adjustToVideoPositionAnchorLastAction, QKeySequence("Alt+Z"));
	connect(adjustToVideoPositionAnchorLastAction, &QAction::triggered, this, &Application::adjustToVideoPositionAnchorLast);
	actionCollection->addAction(ACT_ADJUST_TO_VIDEO_POSITION_A_L, adjustToVideoPositionAnchorLastAction);
	actionManager->addAction(adjustToVideoPositionAnchorLastAction, UserAction::HasSelection | UserAction::VideoPlaying | UserAction::FullScreenOff | UserAction::AnchorsNone);

	QAction *adjustToVideoPositionAnchorFirstAction = new QAction(actionCollection);
	adjustToVideoPositionAnchorFirstAction->setText(i18n("Adjust to Video Position (Anchor First Line)"));
	adjustToVideoPositionAnchorFirstAction->setStatusTip(i18n("Set current line to video position by fixing the first line and linearly adjusting the other ones"));
	actionCollection->setDefaultShortcut(adjustToVideoPositionAnchorFirstAction, QKeySequence("Alt+X"));
	connect(adjustToVideoPositionAnchorFirstAction, &QAction::triggered, this, &Application::adjustToVideoPositionAnchorFirst);
	actionCollection->addAction(ACT_ADJUST_TO_VIDEO_POSITION_A_F, adjustToVideoPositionAnchorFirstAction);
	actionManager->addAction(adjustToVideoPositionAnchorFirstAction, UserAction::HasSelection | UserAction::VideoPlaying | UserAction::FullScreenOff | UserAction::AnchorsNone);

	QAction *toggleAnchor = new QAction(actionCollection);
	toggleAnchor->setText(i18n("Toggle Anchor"));
	toggleAnchor->setStatusTip(i18n("(Un)Anchor current line's show time to timeline (Editing anchored line's show time will stretch/compact the timeline between adjacent anchors)"));
	actionCollection->setDefaultShortcut(toggleAnchor, QKeySequence("Alt+A"));
	connect(toggleAnchor, &QAction::triggered, this, &Application::anchorToggle);
	actionCollection->addAction(ACT_ANCHOR_TOGGLE, toggleAnchor);
	actionManager->addAction(toggleAnchor, UserAction::HasSelection | UserAction::FullScreenOff);

	QAction *removeAllAnchors = new QAction(actionCollection);
	removeAllAnchors->setText(i18n("Remove All Anchors"));
	removeAllAnchors->setStatusTip(i18n("Unanchor show time from the timeline on all anchored lines"));
	actionCollection->setDefaultShortcut(removeAllAnchors, QKeySequence("Alt+Shift+A"));
	connect(removeAllAnchors, &QAction::triggered, this, &Application::anchorRemoveAll);
	actionCollection->addAction(ACT_ANCHOR_REMOVE_ALL, removeAllAnchors);
	actionManager->addAction(removeAllAnchors, UserAction::HasSelection | UserAction::FullScreenOff | UserAction::AnchorsSome);

	QAction *scriptsManagerAction = new QAction(actionCollection);
	scriptsManagerAction->setIcon(QIcon::fromTheme("folder-development"));
	scriptsManagerAction->setText(i18nc("@action:inmenu Manage user scripts", "Scripts Manager..."));
	scriptsManagerAction->setStatusTip(i18n("Manage user scripts"));
	connect(scriptsManagerAction, &QAction::triggered, m_scriptsManager, &ScriptsManager::showDialog);
	actionCollection->addAction(ACT_SCRIPTS_MANAGER, scriptsManagerAction);
	actionManager->addAction(scriptsManagerAction, UserAction::FullScreenOff);

	QAction *waveformZoomInAction = new QAction(actionCollection);
	waveformZoomInAction->setIcon(QIcon::fromTheme("zoom-in"));
	waveformZoomInAction->setText(i18n("Waveform Zoom In"));
	waveformZoomInAction->setStatusTip(i18n("Waveform Zoom In"));
	connect(waveformZoomInAction, &QAction::triggered, m_mainWindow->m_waveformWidget, &WaveformWidget::zoomIn);
	actionCollection->addAction(ACT_WAVEFORM_ZOOM_IN, waveformZoomInAction);

	QAction *waveformZoomOutAction = new QAction(actionCollection);
	waveformZoomOutAction->setIcon(QIcon::fromTheme("zoom-out"));
	waveformZoomOutAction->setText(i18n("Waveform Zoom Out"));
	waveformZoomOutAction->setStatusTip(i18n("Waveform Zoom Out"));
	connect(waveformZoomOutAction, &QAction::triggered, m_mainWindow->m_waveformWidget, &WaveformWidget::zoomOut);
	actionCollection->addAction(ACT_WAVEFORM_ZOOM_OUT, waveformZoomOutAction);

	QAction *waveformAutoScrollAction = new QAction(actionCollection);
	waveformAutoScrollAction->setCheckable(true);
	waveformAutoScrollAction->setIcon(QIcon::fromTheme(QStringLiteral("current_line_follows_video")));
	waveformAutoScrollAction->setText(i18n("Waveform Auto Scroll"));
	waveformAutoScrollAction->setStatusTip(i18n("Waveform display will automatically scroll to video position"));
	connect(waveformAutoScrollAction, &QAction::toggled, m_mainWindow->m_waveformWidget, &WaveformWidget::setAutoscroll);
	actionCollection->addAction(ACT_WAVEFORM_AUTOSCROLL, waveformAutoScrollAction);

	updateActionTexts();
}

/// BEGIN ACTION HANDLERS

Time
Application::videoPosition(bool compensate)
{
	if(compensate && !m_player->isPaused())
		return Time(double(m_player->position()) * 1000. - SCConfig::grabbedPositionCompensation());
	else
		return Time(double(m_player->position()) * 1000.);
}

void
Application::insertBeforeCurrentLine()
{
	static InsertLineDialog *dlg = new InsertLineDialog(true, m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		SubtitleLine *newLine;
		{
			LinesWidgetScrollToModelDetacher detacher(*m_linesWidget);
			SubtitleLine *currentLine = m_linesWidget->currentLine();
			newLine = m_subtitle->insertNewLine(currentLine ? currentLine->index() : 0, false, dlg->selectedTextsTarget());
		}
		m_linesWidget->setCurrentLine(newLine, true);
	}
}

void
Application::insertAfterCurrentLine()
{
	static InsertLineDialog *dlg = new InsertLineDialog(false, m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		SubtitleLine *newLine;
		{
			LinesWidgetScrollToModelDetacher detacher(*m_linesWidget);

			SubtitleLine *currentLine = m_linesWidget->currentLine();
			newLine = m_subtitle->insertNewLine(currentLine ? currentLine->index() + 1 : 0, true, dlg->selectedTextsTarget());
		}
		m_linesWidget->setCurrentLine(newLine, true);
	}
}

void
Application::removeSelectedLines()
{
	static RemoveLinesDialog *dlg = new RemoveLinesDialog(m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		RangeList selectionRanges = m_linesWidget->selectionRanges();

		if(selectionRanges.isEmpty())
			return;

		{
			LinesWidgetScrollToModelDetacher detacher(*m_linesWidget);
			m_subtitle->removeLines(selectionRanges, dlg->selectedTextsTarget());
		}

		int firstIndex = selectionRanges.firstIndex();
		if(firstIndex < m_subtitle->linesCount())
			m_linesWidget->setCurrentLine(m_subtitle->line(firstIndex), true);
		else if(firstIndex - 1 < m_subtitle->linesCount())
			m_linesWidget->setCurrentLine(m_subtitle->line(firstIndex - 1), true);
	}
}

void
Application::joinSelectedLines()
{
	const RangeList &ranges = m_linesWidget->selectionRanges();

//  if ( ranges.count() > 1 && KMessageBox::Continue != KMessageBox::warningContinueCancel(
//      m_mainWindow,
//      i18n( "Current selection has multiple ranges.\nContinuing will join them all." ),
//      i18n( "Join Lines" ) ) )
//      return;

	m_subtitle->joinLines(ranges);
}

void
Application::splitSelectedLines()
{
	m_subtitle->splitLines(m_linesWidget->selectionRanges());
}

void
Application::selectAllLines()
{
	m_linesWidget->selectAll();
}

void
Application::gotoLine()
{
	IntInputDialog gotoLineDlg(i18n("Go to Line"), i18n("&Go to line:"), 1, m_subtitle->linesCount(), m_linesWidget->currentLineIndex() + 1);

	if(gotoLineDlg.exec() == QDialog::Accepted)
		m_linesWidget->setCurrentLine(m_subtitle->line(gotoLineDlg.value() - 1), true);
}

void
Application::find()
{
	m_lastFoundLine = nullptr;
	m_finder->find(m_linesWidget->selectionRanges(), m_linesWidget->currentLineIndex(), m_curLineWidget->focusedText(), false);
}

void
Application::findNext()
{
	if(!m_finder->findNext()) {
		m_lastFoundLine = nullptr;
		m_finder->find(m_linesWidget->selectionRanges(), m_linesWidget->currentLineIndex(), m_curLineWidget->focusedText(), false);
	}
}

void
Application::findPrevious()
{
	if(!m_finder->findPrevious()) {
		m_lastFoundLine = nullptr;
		m_finder->find(m_linesWidget->selectionRanges(), m_linesWidget->currentLineIndex(), m_curLineWidget->focusedText(), true);
	}
}

void
Application::replace()
{
	m_replacer->replace(m_linesWidget->selectionRanges(), m_linesWidget->currentLineIndex(), m_curLineWidget->focusedText()
						);
}

void
Application::spellCheck()
{
	m_speller->spellCheck(m_linesWidget->currentLineIndex());
}

void
Application::retrocedeCurrentLine()
{
	SubtitleLine *currentLine = m_linesWidget->currentLine();
	if(currentLine && currentLine->prevLine())
		m_linesWidget->setCurrentLine(currentLine->prevLine(), true);
}

void
Application::advanceCurrentLine()
{
	SubtitleLine *currentLine = m_linesWidget->currentLine();
	if(currentLine && currentLine->nextLine())
		m_linesWidget->setCurrentLine(currentLine->nextLine(), true);
}

void
Application::toggleSelectedLinesBold()
{
	m_subtitle->toggleStyleFlag(m_linesWidget->selectionRanges(), SString::Bold);
}

void
Application::toggleSelectedLinesItalic()
{
	m_subtitle->toggleStyleFlag(m_linesWidget->selectionRanges(), SString::Italic);
}

void
Application::toggleSelectedLinesUnderline()
{
	m_subtitle->toggleStyleFlag(m_linesWidget->selectionRanges(), SString::Underline);
}

void
Application::toggleSelectedLinesStrikeThrough()
{
	m_subtitle->toggleStyleFlag(m_linesWidget->selectionRanges(), SString::StrikeThrough);
}

void
Application::changeSelectedLinesColor()
{
	const RangeList range = m_linesWidget->selectionRanges();
	SubtitleIterator it(*m_subtitle, range);
	if(!it.current())
		return;

	QColor color = SubtitleColorDialog::getColor(QColor(it.current()->primaryText().styleColorAt(0)), m_mainWindow);
	if(color.isValid())
		m_subtitle->changeTextColor(range, color.rgba());
}

void
Application::shiftLines()
{
	static ShiftTimesDialog *dlg = new ShiftTimesDialog(m_mainWindow);

	dlg->resetShiftTime();

	if(dlg->exec() == QDialog::Accepted) {
		m_subtitle->shiftLines(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->shiftTimeMillis());
	}
}

void
Application::shiftSelectedLinesForwards()
{
	m_subtitle->shiftLines(m_linesWidget->selectionRanges(), SCConfig::linesQuickShiftAmount());
}

void
Application::shiftSelectedLinesBackwards()
{
	m_subtitle->shiftLines(m_linesWidget->selectionRanges(), -SCConfig::linesQuickShiftAmount());
}

void
Application::adjustLines()
{
	static AdjustTimesDialog *dlg = new AdjustTimesDialog(m_mainWindow);

	dlg->setFirstLineTime(m_subtitle->firstLine()->showTime());
	dlg->setLastLineTime(m_subtitle->lastLine()->showTime());

	if(dlg->exec() == QDialog::Accepted) {
		m_subtitle->adjustLines(Range::full(), dlg->firstLineTime().toMillis(), dlg->lastLineTime().toMillis());
	}
}

void
Application::sortLines()
{
	static ActionWithLinesTargetDialog *dlg = new ActionWithLinesTargetDialog(i18n("Sort"), m_mainWindow);

	PROFILE();

	dlg->setLinesTargetEnabled(ActionWithTargetDialog::Selection, true);

	if(m_linesWidget->selectionHasMultipleRanges()) {
		if(m_linesWidget->showingContextMenu()) {
			KMessageBox::sorry(m_mainWindow, i18n("Can not perform sort on selection with multiple ranges."));
			return;
		} else {
			dlg->setLinesTargetEnabled(ActionWithTargetDialog::Selection, false);
			dlg->setSelectedLinesTarget(ActionWithTargetDialog::AllLines);
		}
	}

	if(dlg->exec() == QDialog::Accepted) {
		RangeList targetRanges(m_linesWidget->targetRanges(dlg->selectedLinesTarget()));
		if(!targetRanges.isEmpty()) {
			m_subtitle->sortLines(*targetRanges.begin());
		}
	}
}

void
Application::changeFrameRate()
{
	static ChangeFrameRateDialog *dlg = new ChangeFrameRateDialog(m_subtitle->framesPerSecond(), m_mainWindow);

	dlg->setFromFramesPerSecond(m_subtitle->framesPerSecond());

	if(dlg->exec() == QDialog::Accepted) {
		m_subtitle->changeFramesPerSecond(dlg->toFramesPerSecond(), dlg->fromFramesPerSecond());
	}
}

void
Application::enforceDurationLimits()
{
	static DurationLimitsDialog *dlg = new DurationLimitsDialog(SCConfig::minDuration(),
																SCConfig::maxDuration(),
																m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		m_subtitle->applyDurationLimits(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->enforceMinDuration() ? dlg->minDuration() : Time(), dlg->enforceMaxDuration() ? dlg->maxDuration() : Time(Time::MaxMseconds), !dlg->preventOverlap());
	}
}

void
Application::setAutoDurations()
{
	static AutoDurationsDialog *dlg = new AutoDurationsDialog(60, 50, 50, m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		m_subtitle->setAutoDurations(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->charMillis(), dlg->wordMillis(), dlg->lineMillis(), !dlg->preventOverlap(), dlg->calculationMode()
									 );
	}
}

void
Application::maximizeDurations()
{
	static ActionWithLinesTargetDialog *dlg = new ActionWithLinesTargetDialog(i18n("Maximize Durations"),
																			  m_mainWindow);

	if(dlg->exec() == QDialog::Accepted)
		m_subtitle->setMaximumDurations(m_linesWidget->targetRanges(dlg->selectedLinesTarget()));
}

void
Application::fixOverlappingLines()
{
	static FixOverlappingTimesDialog *dlg = new FixOverlappingTimesDialog(m_mainWindow);

	if(dlg->exec() == QDialog::Accepted)
		m_subtitle->fixOverlappingLines(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->minimumInterval());
}

void
Application::breakLines()
{
	static SmartTextsAdjustDialog *dlg = new SmartTextsAdjustDialog(30, m_mainWindow);

	if(dlg->exec() == QDialog::Accepted)
		m_subtitle->breakLines(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->minLengthForLineBreak(), dlg->selectedTextsTarget()
							   );
}

void
Application::unbreakTexts()
{
	static ActionWithLinesAndTextsTargetDialog *dlg = new ActionWithLinesAndTextsTargetDialog(i18n("Unbreak Lines"),
																							  m_mainWindow);

	if(dlg->exec() == QDialog::Accepted)
		m_subtitle->unbreakTexts(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->selectedTextsTarget()
								 );
}

void
Application::simplifySpaces()
{
	static ActionWithLinesAndTextsTargetDialog *dlg = new ActionWithLinesAndTextsTargetDialog(i18n("Simplify Spaces"),
																							  m_mainWindow);

	if(dlg->exec() == QDialog::Accepted)
		m_subtitle->simplifyTextWhiteSpace(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->selectedTextsTarget()
										   );
}

void
Application::changeCase()
{
	static ChangeTextsCaseDialog *dlg = new ChangeTextsCaseDialog(m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		switch(dlg->caseOperation()) {
		case ChangeTextsCaseDialog::Upper:
			m_subtitle->upperCase(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->selectedTextsTarget()
								  );
			break;
		case ChangeTextsCaseDialog::Lower:
			m_subtitle->lowerCase(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->selectedTextsTarget()
								  );
			break;
		case ChangeTextsCaseDialog::Title:
			m_subtitle->titleCase(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->lowerFirst(), dlg->selectedTextsTarget()
								  );
			break;
		case ChangeTextsCaseDialog::Sentence:
			m_subtitle->sentenceCase(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->lowerFirst(), dlg->selectedTextsTarget()
									 );
			break;
		}
	}
}

void
Application::fixPunctuation()
{
	static FixPunctuationDialog *dlg = new FixPunctuationDialog(m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		m_subtitle->fixPunctuation(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), dlg->spaces(), dlg->quotes(), dlg->englishI(), dlg->ellipsis(), dlg->selectedTextsTarget()
								   );
	}
}

bool
Application::applyTranslation(RangeList ranges, bool primary, int inputLanguage, int outputLanguage, int textTargets)
{
	Translator translator;

	QString inputLanguageName = Language::name((Language::Value)inputLanguage);
	QString outputLanguageName = Language::name((Language::Value)outputLanguage);

	ProgressDialog progressDialog(i18n("Translate"), i18n("Translating text (%1 to %2)...", inputLanguageName, outputLanguageName), true, m_mainWindow);

	if(textTargets == Both) {
		progressDialog.setDescription(primary
									  ? i18n("Translating primary text (%1 to %2)...", inputLanguageName, outputLanguageName)
									  : i18n("Translating secondary text (%1 to %2)...", inputLanguageName, outputLanguageName));
	}

	QString inputText;
	QRegExp dialogCueRegExp2("-([^-])");
	for(SubtitleIterator it(*m_subtitle, ranges); it.current(); ++it) {
		QString lineText = it.current()->primaryText().richString();
		lineText.replace('\n', ' ').replace("--", "---").replace(dialogCueRegExp2, "- \\1");
		inputText += lineText + "\n()() ";
	}

	translator.syncTranslate(inputText, (Language::Value)inputLanguage, (Language::Value)outputLanguage, &progressDialog);

	if(translator.isAborted())
		return false; // ended with error

	QStringList outputLines;
	QString errorMessage;

	if(translator.isFinishedWithError()) {
		errorMessage = translator.errorMessage();
	} else {
		outputLines = translator.outputText().split(QRegExp("\\s*\n\\(\\) ?\\(\\)\\s*"));

//		qDebug() << translator.inputText();
//		qDebug() << translator.outputText();

		if(outputLines.count() != ranges.indexesCount() + 1)
			errorMessage = i18n("Unable to perform texts synchronization (sent and received lines count do not match).");
	}

	if(errorMessage.isEmpty()) {
		SubtitleCompositeActionExecutor executor(*m_subtitle, primary ? i18n("Translate Primary Text") : i18n("Translate Secondary Text"));

		int index = -1;
		QRegExp ellipsisRegExp("\\s+\\.\\.\\.");
		QRegExp dialogCueRegExp("(^| )- ");
		for(SubtitleIterator it(*m_subtitle, ranges); it.current(); ++it) {
			QString line = outputLines.at(++index);
			line.replace(" ---", "--");
			line.replace(ellipsisRegExp, "...");
			line.replace(dialogCueRegExp, "\n-");
			SString text;
			text.setRichString(line);
			it.current()->setPrimaryText(text.trimmed());
		}
	} else {
		KMessageBox::sorry(m_mainWindow, i18n("There was an error performing the translation:\n\n%1", errorMessage));
	}

	return errorMessage.isEmpty();
}

void
Application::translate()
{
	static TranslateDialog *dlg = new TranslateDialog(m_mainWindow);

	if(dlg->exec() == QDialog::Accepted) {
		if(dlg->selectedTextsTarget() == Primary || dlg->selectedTextsTarget() == Both) {
			if(!applyTranslation(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), true, dlg->inputLanguage(), dlg->outputLanguage(), dlg->selectedTextsTarget()))
				return;
		}

		if(dlg->selectedTextsTarget() == Secondary || dlg->selectedTextsTarget() == Both) {
			if(!applyTranslation(m_linesWidget->targetRanges(dlg->selectedLinesTarget()), false, dlg->inputLanguage(), dlg->outputLanguage(), dlg->selectedTextsTarget()))
				return;
		}
	}
}

void
Application::openVideo(const QUrl &url)
{
	if(url.scheme() != QStringLiteral("file"))
		return;

	m_player->closeFile();

	m_player->openFile(url.toLocalFile());
}

void
Application::openVideo()
{
	QFileDialog openDlg(m_mainWindow, i18n("Open Video"), QString(), buildMediaFilesFilter());

	openDlg.setModal(true);
	openDlg.selectUrl(m_lastVideoUrl);

	if(openDlg.exec() == QDialog::Accepted) {
		m_lastVideoUrl = openDlg.selectedUrls().first();
		openVideo(m_lastVideoUrl);
	}
}

void
Application::toggleFullScreenMode()
{
	setFullScreenMode(!m_playerWidget->fullScreenMode());
}

void
Application::setFullScreenMode(bool enabled)
{
	if(enabled != m_playerWidget->fullScreenMode()) {
		m_playerWidget->setFullScreenMode(enabled);

		KToggleAction *toggleFullScreenAction = static_cast<KToggleAction *>(action(ACT_TOGGLE_FULL_SCREEN));
		toggleFullScreenAction->setChecked(enabled);

		emit fullScreenModeChanged(enabled);
	}
}

void
Application::seekBackward()
{
	double position = m_player->position() - SCConfig::seekJumpLength();
	m_playerWidget->pauseAfterPlayingLine(nullptr);
	m_player->seek(position > 0.0 ? position : 0.0);
}

void
Application::seekForward()
{
	double position = m_player->position() + SCConfig::seekJumpLength();
	m_playerWidget->pauseAfterPlayingLine(nullptr);
	m_player->seek(position <= m_player->duration() ? position : m_player->duration());
}

void
Application::stepBackward()
{
	m_player->step(-SCConfig::stepJumpLength());
}

void
Application::stepForward()
{
	m_player->step(SCConfig::stepJumpLength());
}

void
Application::seekToPrevLine()
{
	int selectedIndex = m_linesWidget->firstSelectedIndex();
	if(selectedIndex < 0)
		return;
	SubtitleLine *currentLine = m_subtitle->line(selectedIndex);
	if(currentLine) {
		SubtitleLine *prevLine = currentLine->prevLine();
		if(prevLine) {
			m_playerWidget->pauseAfterPlayingLine(nullptr);
			m_player->seek(prevLine->showTime().toSeconds() - SCConfig::jumpLineOffset() / 1000.0);
			m_linesWidget->setCurrentLine(prevLine);
			m_curLineWidget->setCurrentLine(prevLine);
		}
	}
}

void
Application::playOnlyCurrentLine()
{
	int selectedIndex = m_linesWidget->firstSelectedIndex();
	if(selectedIndex < 0)
		return;
	SubtitleLine *currentLine = m_subtitle->line(selectedIndex);
	if(currentLine) {
		if(!m_player->isPlaying())
			m_player->play();
		m_player->seek(currentLine->showTime().toSeconds() - SCConfig::jumpLineOffset() / 1000.0);
		m_playerWidget->pauseAfterPlayingLine(currentLine);
	}
}

void
Application::seekToNextLine()
{
	int selectedIndex = m_linesWidget->firstSelectedIndex();
	if(selectedIndex < 0)
		return;
	SubtitleLine *currentLine = m_subtitle->line(selectedIndex);
	if(currentLine) {
		SubtitleLine *nextLine = currentLine->nextLine();
		if(nextLine) {
			m_playerWidget->pauseAfterPlayingLine(nullptr);
			m_player->seek(nextLine->showTime().toSeconds() - SCConfig::jumpLineOffset() / 1000.0);
			m_linesWidget->setCurrentLine(nextLine);
			m_curLineWidget->setCurrentLine(nextLine);
		}
	}
}

void
Application::playrateIncrease()
{
	const double speed = m_player->playSpeed();
	m_player->playSpeed(speed + (speed >= 2.0 ? .5 : .1));
}

void
Application::playrateDecrease()
{
	const double speed = m_player->playSpeed();
	m_player->playSpeed(speed - (speed > 2.0 ? .5 : .1));
}

void
Application::setCurrentLineShowTimeFromVideo()
{
	SubtitleLine *currentLine = m_linesWidget->currentLine();
	if(currentLine)
		currentLine->setShowTime(videoPosition(true), true);
}

void
Application::setCurrentLineHideTimeFromVideo()
{
	SubtitleLine *currentLine = m_linesWidget->currentLine();
	if(currentLine) {
		currentLine->setHideTime(videoPosition(true), true);
		SubtitleLine *nextLine = currentLine->nextLine();
		if(nextLine)
			m_linesWidget->setCurrentLine(nextLine, true);
	}
}

void
Application::setActiveSubtitleStream(int subtitleStream)
{
	KSelectAction *activeSubtitleStreamAction = (KSelectAction *)action(ACT_SET_ACTIVE_SUBTITLE_STREAM);
	activeSubtitleStreamAction->setCurrentItem(subtitleStream);

	const bool translationSelected = bool(subtitleStream);
	m_playerWidget->setShowTranslation(translationSelected);
	m_mainWindow->m_waveformWidget->setShowTranslation(translationSelected);
	m_speller->setUseTranslation(translationSelected);
}


void
Application::anchorToggle()
{
	m_subtitle->toggleLineAnchor(m_linesWidget->currentLine());
}

void
Application::anchorRemoveAll()
{
	m_subtitle->removeAllAnchors();
}

void
Application::shiftToVideoPosition()
{
	SubtitleLine *currentLine = m_linesWidget->currentLine();
	if(currentLine) {
		m_subtitle->shiftLines(Range::full(), videoPosition(true).toMillis() - currentLine->showTime().toMillis());
	}
}

void
Application::adjustToVideoPositionAnchorLast()
{
	SubtitleLine *currentLine = m_linesWidget->currentLine();
	if(currentLine) {
		long lastLineTime = m_subtitle->lastLine()->showTime().toMillis();

		long oldCurrentLineTime = currentLine->showTime().toMillis();
		long oldDeltaTime = lastLineTime - oldCurrentLineTime;

		if(!oldDeltaTime)
			return;

		long newCurrentLineTime = videoPosition(true).toMillis();
		long newDeltaTime = lastLineTime - newCurrentLineTime;

		double scaleFactor = (double)newDeltaTime / oldDeltaTime;
		long shiftTime = (long)(newCurrentLineTime - scaleFactor * oldCurrentLineTime);

		long newFirstLineTime = (long)(shiftTime + m_subtitle->firstLine()->showTime().toMillis() * scaleFactor);

		if(newFirstLineTime < 0) {
			if(KMessageBox::warningContinueCancel(m_mainWindow, i18n("Continuing would result in loss of timing information for some lines.\nAre you sure you want to continue?")) != KMessageBox::Continue)
				return;
		}

		m_subtitle->adjustLines(Range::full(), newFirstLineTime, lastLineTime);
	}
}

void
Application::adjustToVideoPositionAnchorFirst()
{
	SubtitleLine *currentLine = m_linesWidget->currentLine();
	if(currentLine) {
		long firstLineTime = m_subtitle->firstLine()->showTime().toMillis();

		long oldCurrentLineTime = currentLine->showTime().toMillis();
		long oldDeltaTime = oldCurrentLineTime - firstLineTime;

		if(!oldDeltaTime)
			return;

		long newCurrentLineTime = videoPosition(true).toMillis();
		long newDeltaTime = newCurrentLineTime - firstLineTime;

		double scaleFactor = (double)newDeltaTime / oldDeltaTime;
		long shiftTime = (long)(firstLineTime - scaleFactor * firstLineTime);

		long newLastLineTime = (long)(shiftTime + m_subtitle->lastLine()->showTime().toMillis() * scaleFactor);

		if(newLastLineTime > Time::MaxMseconds) {
			if(KMessageBox::warningContinueCancel(m_mainWindow,
					i18n("Continuing would result in loss of timing information for some lines.\n"
						 "Are you sure you want to continue?")) != KMessageBox::Continue)
				return;
		}

		m_subtitle->adjustLines(Range::full(), firstLineTime, newLastLineTime);
	}
}

/// END ACTION HANDLERS

void
Application::updateTitle()
{
	if(m_subtitle) {
		if(m_translationMode) {
			static const QString titleBuilder(QStringLiteral("%1%2 | %3%4"));
			static const QString modified = QStringLiteral(" [") % i18n("modified") % QStringLiteral("]");

			m_mainWindow->setCaption(titleBuilder
									 .arg(m_subtitleUrl.isEmpty() ? i18n("Untitled") : m_subtitleFileName)
									 .arg(m_subtitle->isPrimaryDirty() ? modified : QString())
									 .arg(m_subtitleTrUrl.isEmpty() ? i18n("Untitled Translation") : m_subtitleTrFileName)
									 .arg(m_subtitle->isSecondaryDirty() ? modified : QString()), false);
		} else {
			m_mainWindow->setCaption(
						m_subtitleUrl.isEmpty() ? i18n("Untitled") : (m_subtitleUrl.isLocalFile() ? m_subtitleUrl.toLocalFile() : m_subtitleUrl.toString(QUrl::PreferLocalFile)),
						m_subtitle->isPrimaryDirty());
		}
	} else {
		m_mainWindow->setCaption(QString());
	}
}

void
Application::onWaveformDoubleClicked(Time time)
{
	if(m_player->state() == VideoPlayer::Stopped)
		m_player->play();

	m_playerWidget->pauseAfterPlayingLine(nullptr);
	m_player->seek(time.toSeconds());
}

void
Application::onWaveformMiddleMouseDown(Time time)
{
	if(m_player->state() == VideoPlayer::Stopped) {
		m_player->play();
		m_player->pause();
	} else if(m_player->state() != VideoPlayer::Paused) {
		m_player->pause();
	}

	m_playerWidget->pauseAfterPlayingLine(nullptr);
	m_player->seek(time.toSeconds());
}

void
Application::onWaveformMiddleMouseUp(Time /*time*/)
{
	m_player->play();
}

void
Application::onLineDoubleClicked(SubtitleLine *line)
{
	if(m_player->state() == VideoPlayer::Stopped)
		m_player->play();

	int mseconds = line->showTime().toMillis() - SCConfig::seekOffsetOnDoubleClick();
	m_playerWidget->pauseAfterPlayingLine(nullptr);
	m_player->seek(mseconds > 0 ? mseconds / 1000.0 : 0.0);

	if(m_player->state() != VideoPlayer::Playing && SCConfig::unpauseOnDoubleClick())
		m_player->play();

	m_mainWindow->m_waveformWidget->setScrollPosition(line->showTime().toMillis());
}

void
Application::onHighlightLine(SubtitleLine *line, bool primary, int firstIndex, int lastIndex)
{
	if(m_playerWidget->fullScreenMode()) {
		if(m_lastFoundLine != line) {
			m_lastFoundLine = line;

			m_playerWidget->pauseAfterPlayingLine(nullptr);
			m_player->seek(line->showTime().toSeconds());
		}
	} else {
		m_linesWidget->setCurrentLine(line, true);

		if(firstIndex >= 0 && lastIndex >= 0) {
			if(primary)
				m_curLineWidget->selectPrimaryText(firstIndex, lastIndex);
			else
				m_curLineWidget->selectTranslationText(firstIndex, lastIndex);
		}
	}
}

void
Application::onPlayingLineChanged(SubtitleLine *line)
{
	m_linesWidget->setPlayingLine(line);

	if(m_linkCurrentLineToPosition)
		m_linesWidget->setCurrentLine(line, true);
}

void
Application::onLinkCurrentLineToVideoToggled(bool value)
{
	if(m_linkCurrentLineToPosition != value) {
		m_linkCurrentLineToPosition = value;

		if(m_linkCurrentLineToPosition)
			m_linesWidget->setCurrentLine(m_playerWidget->playingLine(), true);
	}
}

void
Application::onPlayerFileOpened(const QString &filePath)
{
	m_recentVideosAction->addUrl(QUrl::fromLocalFile(filePath));
}

void
Application::onPlayerPlaying()
{
	QAction *playPauseAction = action(ACT_PLAY_PAUSE);
	playPauseAction->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-pause")));
	playPauseAction->setText(i18n("Pause"));
}

void
Application::onPlayerPaused()
{
	QAction *playPauseAction = action(ACT_PLAY_PAUSE);
	playPauseAction->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-start")));
	playPauseAction->setText(i18n("Play"));
}

void
Application::onPlayerStopped()
{
	QAction *playPauseAction = action(ACT_PLAY_PAUSE);
	playPauseAction->setIcon(QIcon::fromTheme(QStringLiteral("media-playback-start")));
	playPauseAction->setText(i18n("Play"));
}

void
Application::onPlayerTextStreamsChanged(const QStringList &textStreams)
{
	QAction *demuxTextStreamAction = (KSelectAction *)action(ACT_DEMUX_TEXT_STREAM);
	QMenu *menu = demuxTextStreamAction->menu();
	menu->clear();
	int i = 0;
	foreach(const QString &textStream, textStreams)
		menu->addAction(textStream)->setData(QVariant::fromValue<int>(i++));
	demuxTextStreamAction->setEnabled(i > 0);
}

void
Application::onPlayerAudioStreamsChanged(const QStringList &audioStreams)
{
	KSelectAction *activeAudioStreamAction = (KSelectAction *)action(ACT_SET_ACTIVE_AUDIO_STREAM);
	activeAudioStreamAction->setItems(audioStreams);

	QAction *speechImportStreamAction = (KSelectAction *)action(ACT_ASR_IMPORT_AUDIO_STREAM);
	QMenu *speechImportStreamActionMenu = speechImportStreamAction->menu();
	speechImportStreamActionMenu->clear();
	int i = 0;
	foreach(const QString &audioStream, audioStreams)
		speechImportStreamActionMenu->addAction(audioStream)->setData(QVariant::fromValue<int>(i++));
	speechImportStreamAction->setEnabled(i > 0);
}

void
Application::onPlayerActiveAudioStreamChanged(int audioStream)
{
	KSelectAction *activeAudioStreamAction = (KSelectAction *)action(ACT_SET_ACTIVE_AUDIO_STREAM);
	if(audioStream >= 0) {
		activeAudioStreamAction->setCurrentItem(audioStream);
		m_mainWindow->m_waveformWidget->setAudioStream(m_player->filePath(), audioStream);
	} else {
		m_mainWindow->m_waveformWidget->setNullAudioStream(m_player->duration() * 1000);
	}
}

void
Application::onPlayerMuteChanged(bool muted)
{
	KToggleAction *toggleMutedAction = (KToggleAction *)action(ACT_TOGGLE_MUTED);
	toggleMutedAction->setChecked(muted);
}

void
Application::updateActionTexts()
{
	const int shiftAmount = SCConfig::linesQuickShiftAmount();
	const int jumpLength = SCConfig::seekJumpLength();

	action(ACT_SEEK_BACKWARD)->setStatusTip(i18np("Seek backwards 1 second", "Seek backwards %1 seconds", jumpLength));
	action(ACT_SEEK_FORWARD)->setStatusTip(i18np("Seek forwards 1 second", "Seek forwards %1 seconds", jumpLength));

	QAction *shiftSelectedLinesFwdAction = action(ACT_SHIFT_SELECTED_LINES_FORWARDS);
	shiftSelectedLinesFwdAction->setText(i18np("Shift %2%1 millisecond", "Shift %2%1 milliseconds", shiftAmount, "+"));
	shiftSelectedLinesFwdAction->setStatusTip(i18np("Shift selected lines %2%1 millisecond", "Shift selected lines %2%1 milliseconds", shiftAmount, "+"));

	QAction *shiftSelectedLinesBwdAction = action(ACT_SHIFT_SELECTED_LINES_BACKWARDS);
	shiftSelectedLinesBwdAction->setText(i18np("Shift %2%1 millisecond", "Shift %2%1 milliseconds", shiftAmount, "-"));
	shiftSelectedLinesBwdAction->setStatusTip(i18np("Shift selected lines %2%1 millisecond", "Shift selected lines %2%1 milliseconds", shiftAmount, "-"));
}

void
Application::onConfigChanged()
{
	updateActionTexts();
}

