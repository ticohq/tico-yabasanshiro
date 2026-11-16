/*  Copyright 2005 Guillaume Duhamel
	Copyright 2005-2006, 2013 Theo Berkau
	Copyright 2008 Filipe Azevedo <pasnox@gmail.com>

	This file is part of Yabause.

	Yabause is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Yabause is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Yabause; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
#include "UIYabause.h"
#include "debug.h"
#include "../Settings.h"
#include "../VolatileSettings.h"
#include "UISettings.h"
#include "UIBackupRam.h"
#include "UICheats.h"
#include "UICheatSearch.h"
#include "UIDebugSH2.h"
#include "UIDebugVDP1.h"
#include "UIDebugVDP2.h"
#include "UIDebugM68K.h"
#include "UIDebugSCUDSP.h"
#include "UIDebugSCSP.h"
#include "UIDebugSCSPChan.h"
#include "UIDebugSCSPDSP.h"
#include "UIMemoryEditor.h"
#include "UIMemoryTransfer.h"
#include "UIAbout.h"
#include "UISetupWizard.h"
#include "WebLoginWindow.h"
#include "UIReportList.h"
#include "../YabauseGL.h"
#include "../QtYabause.h"
#include "../CommonDialogs.h"
#include "../DebugMcpServer.h"

#include "PlayRecorder.h"

#include <chrono>
#include <cstdlib>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QTextEdit>
#include <QDockWidget>
#include <QImageWriter>
#include <QUrl>
#include <QDesktopServices>
#include <QDateTime>
#include <QWindow>
#include <QDebug>
#include <QRegularExpression>
#include <QResource>

#include <firebase/app.h>

#ifdef HAVE_VULKAN
//#include "vulkan/VIDVulkan.h"
#include "vulkan/VIDVulkanCInterface.h"
#endif
#include "QYabVulkanWidget.h"

#include "winsparkle.h"

extern "C" {
extern VideoInterface_struct *VIDCoreList[];
}

//#define USE_UNIFIED_TITLE_TOOLBAR

firebase::App* UIYabause::app = NULL;

void qAppendLog( const char* s )
{
	UIYabause* ui = QtYabause::mainWindow( false );
	
	if ( ui ) {
		ui->appendLog( s );
	}
	else {
		qWarning( "%s", s );
	}
}

UIYabause::UIYabause( QWidget* parent )
	: QMainWindow( parent )
{
	mInit = false;
   search.clear();
	searchType = 0;

	// setup dialog
	setupUi( this );
	toolBar->insertAction( aFileSettings, mFileSaveState->menuAction() );
	toolBar->insertAction( aFileSettings, mFileLoadState->menuAction() );
	toolBar->insertSeparator( aFileSettings );
	setAttribute( Qt::WA_DeleteOnClose );
#ifdef USE_UNIFIED_TITLE_TOOLBAR
	setUnifiedTitleAndToolBarOnMac( true );
#endif
	fSound->setParent( 0, Qt::Popup );
	fVideoDriver->setParent( 0, Qt::Popup );
	fSound->installEventFilter( this );
	fVideoDriver->installEventFilter( this );
	// Get Screen res list
	getSupportedResolutions();
	// fill combo driver
	cbVideoDriver->blockSignals( true );
	for ( int i = 0; VIDCoreList[i] != NULL; i++ )
		cbVideoDriver->addItem( VIDCoreList[i]->Name, VIDCoreList[i]->id );
	cbVideoDriver->blockSignals( false );
	// create glcontext
	

	// スタックウィジェットを作成
	mStackedWidget = new QStackedWidget(this);
	setCentralWidget(mStackedWidget);

	// FileSearchWidgetを作成
	mFileSearch = new FileSearchWidget(this);
	mStackedWidget->addWidget(mFileSearch);

	// YabauseGLを作成
	mYabauseGL = new YabauseGL(this);
	mStackedWidget->addWidget(mYabauseGL);

	mYabVulkanWidget = new QYabVulkanWidget();
	mYabVulkanWidget->setMinimumSize(800, 600);

	mStackedWidget->addWidget(mYabVulkanWidget);

	mStackedWidget->setCurrentWidget(mFileSearch);


	connect(mFileSearch, &FileSearchWidget::fileSelected,
		this, &UIYabause::handleFileSelected);

	connect(mFileSearch, &FileSearchWidget::viewBugReportsRequested,
		this, &UIYabause::handleViewBugReportsRequested);

	// create log widget
	teLog = new QTextEdit( this );
	teLog->setReadOnly( true );
	teLog->setWordWrapMode( QTextOption::NoWrap );
	teLog->setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOn );
	teLog->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOn );
	mLogDock = new QDockWidget( this );
	mLogDock->setWindowTitle( "Log" );
	mLogDock->setWidget( teLog );
	addDockWidget( Qt::BottomDockWidgetArea, mLogDock );
	mLogDock->setVisible( false );
	mCanLog = true;
	oldMouseX = oldMouseY = 0;
	mouseCaptured = false;

#ifndef SH2_TRACE
	aTraceLogging->setVisible(false);
#endif

	// create emulator thread
	mYabauseThread = new YabauseThread( this );

	// MCP debug server: menu toggle + optional --debug-mcp[=port] CLI switch.
	mDebugMcpServer = new DebugMcpServer(mYabauseThread, this);
	aDebugMcpServer = new QAction(QtYabause::translate("MCP Debug Server"), this);
	aDebugMcpServer->setCheckable(true);
	mViewDebug->addAction(aDebugMcpServer);
	connect(aDebugMcpServer, &QAction::toggled, this, [this](bool on) {
		Settings* s = QtYabause::settings();
		int port = s->value("Debug/McpServerPort", 9640).toInt();
		// Mirror the --debug-mcp CLI validation below: reject an out-of-range
		// (e.g. 0, meaning "ephemeral port") or corrupt stored value instead
		// of silently binding an unexpected port.
		if (port < 1 || port > 65535) port = 9640;
		if (on) {
			if (!mDebugMcpServer->start(port)) {
				CommonDialogs::information(
					QtYabause::translate("Failed to start MCP debug server (port busy?)"));
				aDebugMcpServer->setChecked(false);
				return;
			}
		} else {
			mDebugMcpServer->stop();
		}
		// Persist the enabled state only for menu-driven toggles. When the
		// server was enabled by the --debug-mcp CLI flag, keep it to that run
		// only so a later plain launch does not auto-start the server.
		if (!mMcpEnableFromCli)
			s->setValue("Debug/McpServerEnabled", on);
	});

	bool mcpEnable =
		QtYabause::settings()->value("Debug/McpServerEnabled", false).toBool();
	bool mcpFromCli = false;
	for (const QString& a : QCoreApplication::arguments()) {
		if (a == "--debug-mcp") { mcpEnable = true; mcpFromCli = true; }
		else if (a.startsWith("--debug-mcp=")) {
			mcpEnable = true;
			mcpFromCli = true;
			bool portOk = false;
			int parsedPort = a.mid(12).toInt(&portOk);
			// Ignore non-numeric or out-of-range values and keep the
			// stored/default port instead of silently binding an
			// ephemeral port.
			if (portOk && parsedPort >= 1 && parsedPort <= 65535) {
				QtYabause::settings()->setValue("Debug/McpServerPort", parsedPort);
			}
		}
	}
	if (mcpEnable) {
		// Guard the persist inside the toggled handler when the enable came
		// from the CLI flag (this run only, no settings write).
		mMcpEnableFromCli = mcpFromCli;
		aDebugMcpServer->setChecked(true);
		mMcpEnableFromCli = false;
	}

	// create hide mouse timer
	hideMouseTimer = new QTimer();
	// create mouse cursor timer
	mouseCursorTimer = new QTimer();
	// connections
	connect( mYabauseThread, SIGNAL( requestSize( const QSize& ) ), this, SLOT( sizeRequested( const QSize& ) ) );
	connect( mYabauseThread, SIGNAL( requestFullscreen( bool ) ), this, SLOT( fullscreenRequested( bool ) ) );
	connect( mYabauseThread, SIGNAL( requestVolumeChange( int ) ), this, SLOT( on_sVolume_valueChanged( int ) ) );
	connect( aViewLog, SIGNAL( toggled( bool ) ), mLogDock, SLOT( setVisible( bool ) ) );
	connect( mLogDock->toggleViewAction(), SIGNAL( toggled( bool ) ), aViewLog, SLOT( setChecked( bool ) ) );
	connect( mYabauseThread, SIGNAL( error( const QString&, bool ) ), this, SLOT( errorReceived( const QString&, bool ) ) );
	connect( mYabauseThread, SIGNAL( pause( bool ) ), this, SLOT( pause( bool ) ) );
	connect( mYabauseThread, SIGNAL( reset() ), this, SLOT( reset() ) );
	connect( hideMouseTimer, SIGNAL( timeout() ), this, SLOT( hideMouse() ));
	connect( mouseCursorTimer, SIGNAL( timeout() ), this, SLOT( cursorRestore() ));
	connect( mYabauseThread, SIGNAL( toggleEmulateMouse( bool ) ), this, SLOT( toggleEmulateMouse( bool ) ) );

	// A --vdp-dump run must not open the update dialog.
	if (!YabauseThread::vdpDumpMode())
		connect(this, SIGNAL(windowWasShown()), this, SLOT(initWinSparkle()),
			Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));

  //connect(this, SIGNAL(setStateFileLoaded(std::string)), this, SLOT(onStateFileLoaded(std::string)));

	// Load shortcuts
	VolatileSettings* vs = QtYabause::volatileSettings();
	QList<QAction *> actions = findChildren<QAction *>();
	foreach ( QAction* action, actions )
	{
		if (action->text().isEmpty())
			continue;

		QString text = vs->value(QString("Shortcuts/") + action->text(), "").toString();
		if (text.isEmpty())
			continue;
		action->setShortcut(text);
	}

	// retranslate widgets
	QtYabause::retranslateWidget( this );

	QList<QAction *> actionList = menubar->actions();
	for(int i = 0;i < actionList.size();i++) {
		addAction(actionList.at(i));
	}

	restoreGeometry( vs->value("General/Geometry" ).toByteArray() );
	//mYabauseGL->setMouseTracking(true);
	setMouseTracking(true);
	mouseXRatio = mouseYRatio = 1.0;
	emulateMouse = false;
	mouseSensitivity = vs->value( "Input/GunMouseSensitivity", 100 ).toInt();
	showMenuBarHeight = menubar->height();
	translations = QtYabause::getTranslationList();
	
	VIDSoftSetBilinear(QtYabause::settings()->value( "Video/Bilinear", false ).toBool());

	mIsCdIn = true;

  PlayRecorder * p = PlayRecorder::getInstance();
  using std::placeholders::_1;
  p->f_takeScreenshot = std::bind(&UIYabause::takeScreenshot, this, _1);

	QSettings settings(QtYabause::cloudSettingsIniPath(), QSettings::IniFormat);
	QString appId = settings.value("CloudService/app_id").toString();
	QString apiKey = settings.value("CloudService/api_key").toString();
	QString databaseUrl = settings.value("CloudService/database_url").toString();
	QString storageBucket = settings.value("CloudService/storage_bucket").toString();
	QString projectId = settings.value("CloudService/project_id").toString();

	// Firebase の初期化
	std::thread t([=] {
		firebase::AppOptions options;
		options.set_app_id(appId.toStdString().c_str());
		options.set_api_key(apiKey.toStdString().c_str());
		options.set_database_url(databaseUrl.toStdString().c_str());
		options.set_storage_bucket(storageBucket.toStdString().c_str());
		options.set_project_id(projectId.toStdString().c_str());
		app = firebase::App::Create(options);
	});
	t.detach();

}

UIYabause::~UIYabause()
{
	// Stop the MCP debug server before the emulation thread tears down.
	if (mDebugMcpServer) mDebugMcpServer->stop();
	win_sparkle_cleanup();
	mCanLog = false;
}

void UIYabause::handleFileSelected(const QString& filePath)
{
	qDebug() << "Selected file:" << filePath;

	VolatileSettings* vs = QtYabause::volatileSettings();
	const int currentCDCore = vs->value("General/CdRom").toInt();
	const QString currentCdRomISO = vs->value("General/CdRomISO").toString();

	QtYabause::settings()->setValue("Recents/ISOs", filePath);

	// Save it permanently
	QtYabause::settings()->setValue("General/CdRom", ISOCD.id);
	QtYabause::settings()->setValue("General/CdRomISO", filePath);
	QtYabause::settings()->setValue("General/PlaySSF", false);

	vs->setValue("autostart", false);
	vs->setValue("General/CdRom", ISOCD.id);
	vs->setValue("General/CdRomISO", filePath);
	vs->setValue("General/PlaySSF", false);

	int vidcoretype = vs->value("Video/VideoCore").toInt();
	if (vidcoretype == VIDCORE_VULKAN) {
		mStackedWidget->setCurrentWidget(mYabVulkanWidget);
		mYabVulkanWidget->show();

		mYabauseThread->pauseEmulation(false, true, [&]() {
			mYabVulkanWidget->setYabauseThread(mYabauseThread);
		});

		// The renderer only learns the widget size from a resize; without this
		// it keeps presenting into the size it started with and the rest of the
		// widget still shows the game browser underneath.
		mYabVulkanWidget->updateView();
		mYabVulkanWidget->update();
	}
	else {
		mStackedWidget->setCurrentWidget(mYabauseGL);
		mYabauseGL->setYabauseThread(mYabauseThread);
		mYabauseGL->makeCurrent();
		mYabauseThread->pauseEmulation(false, true);
		mYabauseGL->update();
	}

	refreshStatesActions();

}

void UIYabause::showEvent( QShowEvent* e )
{
	QMainWindow::showEvent( e );
	
	if ( !mInit )
	{
		LogStart();
		LogChangeOutput( DEBUG_CALLBACK, (char*)qAppendLog );
		VolatileSettings* vs = QtYabause::volatileSettings();

		//if ( vs->value( "View/Menubar" ).toInt() == BD_ALWAYSHIDE )
	//		menubar->hide();
		//if ( vs->value( "View/Toolbar" ).toInt() == BD_ALWAYSHIDE )
	//		toolBar->hide();
		if ( vs->value( "autostart" ).toBool() )
			aEmulationRun->trigger();
		aEmulationFrameSkipLimiter->setChecked( vs->value( "General/EnableFrameSkipLimiter" ).toBool() );
		aViewFPS->setChecked( vs->value( "General/ShowFPS" ).toBool() );
		mInit = true;
	}

	emit windowWasShown();

}

#include <cstdlib>
#include <cwchar>

std::string versionToScalar(const std::string& version) {
	std::stringstream ss(version);
	std::string segment;
	std::vector<int> parts;

	// バージョン文字列を "." で分割
	while (std::getline(ss, segment, '.')) {
		parts.push_back(std::stoi(segment));
	}

	// 各部分をスカラー値に変換
	std::ostringstream result;
	if (parts.size() > 0) {
		result << std::setw(3) << std::setfill('0') << std::setw(3) << parts[0]; // Majorバージョン（1桁）
	}
	if (parts.size() > 1) {
		result << std::setw(3) << std::setfill('0') << parts[1]; // Minorバージョン（3桁）
	}
	if (parts.size() > 2) {
		result << std::setw(3) << std::setfill('0') << parts[2]; // Patchバージョン（3桁）
	}

	result << "000"; // 手動

	return result.str();
}

// char* を wchar_t* に変換する関数
std::wstring charToWString(const char* str) {
	size_t len = std::strlen(str);
	std::wstring wstr(len, L'\0');
	std::mbstowcs(&wstr[0], str, len);
	return wstr;
}

void UIYabause::initWinSparkle()
{
	std::string scalar = versionToScalar(VERSION);
	std::wstring wScalarVersion = charToWString(scalar.c_str());
	win_sparkle_set_app_build_version(wScalarVersion.c_str());

	// Setup updates feed. This must be done before win_sparkle_init(), but
	// could be also, often more conveniently, done using a VERSIONINFO Windows
	// resource. See the "psdk" example and its .rc file for an example of that
	// (these calls wouldn't be needed then).
	win_sparkle_set_appcast_url("https://www.yabasanshiro.com/appcast.xml");
	std::wstring wVersion = charToWString(VERSION);
	win_sparkle_set_app_details(L"devMiyax", L"YabaSanshiro", wVersion.c_str());

	// Set DSA public key used to verify update's signature.
	// This is na example how to provide it from external source (i.e. from Qt
	// resource). See the "psdk" example and its .rc file for an example how to
	// provide the key using Windows resource.
	win_sparkle_set_dsa_pub_pem(reinterpret_cast<const char*>(QResource(":/pem/dsa_pub.pem").data()));

	win_sparkle_set_automatic_check_for_updates(1);
	win_sparkle_set_update_check_interval(3600*8);

	// Initialize the updater and possibly show some UI
	win_sparkle_init();
}

void UIYabause::checkForUpdates()
{
	win_sparkle_check_update_with_ui();
}

void UIYabause::on_actionCheck_for_updates_triggered() {
	win_sparkle_check_update_with_ui();
}

void UIYabause::closeEvent( QCloseEvent* e )
{
	// Stop the MCP debug server before the emulation thread tears down.
	if (mDebugMcpServer) mDebugMcpServer->stop();
	aEmulationPause->trigger();
	LogStop();

	if (isFullScreen())
		// Need to switch out of full screen or the geometry settings get saved
		fullscreenRequested( false );
	Settings* vs = QtYabause::settings();
	vs->setValue( "General/Geometry", saveGeometry() );
	vs->sync();

	QMainWindow::closeEvent( e );
}

void UIYabause::keyPressEvent( QKeyEvent* e )
{ 
	if (emulateMouse && mouseCaptured && e->key() == Qt::Key_Escape)
		mouseCaptured = false;
	else
		PerKeyDown( e->key() ); 

	//if (e->key() == Qt::Key_Alt) {
	//	toggleMenuAndToolBar();
	//}
}

void UIYabause::keyReleaseEvent( QKeyEvent* e )
{ PerKeyUp( e->key() ); }

void UIYabause::leaveEvent( QEvent* e )
{
	if (emulateMouse && mouseCaptured)
	{
		// lock cursor to center
		int midX = geometry().x()+(width()/2); // widget global x
		int midY = geometry().y()+menubar->height()+toolBar->height()+(height()/2); // widget global y

		QPoint newPos(midX, midY);
		this->cursor().setPos(newPos);
	}
}

void UIYabause::mousePressEvent( QMouseEvent* e )
{ 
	if (emulateMouse && !mouseCaptured)
	{
		this->setCursor(Qt::BlankCursor);
		mouseCaptured = true;
	}
	else
		PerKeyDown( (1 << 31) | e->button() );
}

void UIYabause::mouseReleaseEvent( QMouseEvent* e )
{ 
	PerKeyUp( (1 << 31) | e->button() );
}

void UIYabause::hideMouse()
{
	this->setCursor(Qt::BlankCursor);
	hideMouseTimer->stop();
}

void UIYabause::cursorRestore()
{
	this->setCursor(Qt::ArrowCursor);
	mouseCursorTimer->stop();
}

void UIYabause::mouseMoveEvent( QMouseEvent* e )
{ 
	int midX = geometry().x()+(width()/2); // widget global x
	int midY = geometry().y()+menubar->height()+toolBar->height()+(height()/2); // widget global y

	int x = (e->x()-(width()/2))*mouseXRatio;
	int y = ((menubar->height()+toolBar->height()+(height()/2))-e->y())*mouseYRatio;
	int minAdj = mouseSensitivity/100;

	// If minimum movement is less than x, wait until next pass to apply	
	if (abs(x) < minAdj) x = 0;
	if (abs(y) < minAdj) y = 0;

	if (mouseCaptured)
		PerAxisMove((1 << 30), x, y);

	VolatileSettings* vs = QtYabause::volatileSettings();

	if (!isFullScreen())
	{
		if (emulateMouse && mouseCaptured)
		{
			// lock cursor to center
			QPoint newPos(midX, midY);
			this->cursor().setPos(newPos);
			this->setCursor(Qt::BlankCursor);
			return;
		}
		else
			this->setCursor(Qt::ArrowCursor);
	}
	else
	{
		if (emulateMouse && mouseCaptured)
		{
			this->setCursor(Qt::BlankCursor);
			return;
		}
		else if (vs->value( "View/Menubar" ).toInt() == BD_SHOWONFSHOVER)
		{
//			if (e->y() < showMenuBarHeight)				menubar->show();
//			else
//				menubar->hide();
		}

		hideMouseTimer->start(3 * 1000);
		this->setCursor(Qt::ArrowCursor);
	}
}

void UIYabause::resizeEvent( QResizeEvent* event )
{
#if 0
  mYabauseGL->viewport_width_ = event->size().width();
  mYabauseGL->viewport_height_ = event->size().height();
  mYabauseGL->viewport_origin_x_ = 0;
  mYabauseGL->viewport_origin_y_ = 0;

	if (event->oldSize().width() != event->size().width()){
    	fixAspectRatio(event->size().width(), event->size().height());
		mYabauseGL->updateView( event->size() );
	}
#endif
	QMainWindow::resizeEvent( event );



}

void UIYabause::adjustHeight(int & height)
{
  // Compensate for menubar and toolbar
  //VolatileSettings* vs = QtYabause::volatileSettings();
  //if (vs->value("View/Menubar").toInt() != BD_ALWAYSHIDE)
  //  height += menubar->height();
  //if (vs->value("View/Toolbar").toInt() != BD_ALWAYSHIDE)
  //  height += toolBar->height();
}

void UIYabause::resizeIntegerScaling()
{
   if (!VIDCore || VIDCore->id != VIDCORE_SOFT)
      return;

   if (isFullScreen() || emulateMouse)
      return;

   VolatileSettings* vs = QtYabause::volatileSettings();

   if (!vs->value("Video/EnableIntegerPixelScaling").toBool())
      return;

   int multiplier = vs->value("Video/IntegerPixelScalingMultiplier").toInt();

   if (multiplier % 2 != 0)
      return;

   int vdp2width = 0;
   int vdp2height = 0;
   int vdp2interlace = 0;

   if (!VIDCore->GetNativeResolution)
      return;

   VIDCore->GetNativeResolution(&vdp2width, &vdp2height, &vdp2interlace);

   if (vdp2width == 0 || vdp2height == 0)
      return;

   int width = 0;
   int height = 0;

   if (vdp2width < 640)
      width = vdp2width * multiplier;
   else
      width = vdp2width * (multiplier / 2);

   if (!vdp2interlace)
      height = vdp2height * multiplier;
   else
      height = vdp2height * (multiplier / 2);

   //mYabauseGL->resize(width, height);

   adjustHeight(height);

   setMinimumSize(width, height);
   resize(width, height);
}

void UIYabause::swapBuffers()
{ 
   resizeIntegerScaling();

//#if (QT_VERSION >= QT_VERSION_CHECK(5, 0, 0))
    // QOpenGLContext complains if we swap on an non-exposed QWindow
//    if (!mYabauseGL || !mYabauseGL->windowHandle()->isExposed()){
//		printf("Not Exporsed\n");
//        return;
//	}
//#endif

	//mYabauseGL->update();
	//mYabauseGL->makeCurrent();
}

void UIYabause::appendLog( const char* s )
{
	if (! mCanLog)
	{
		qWarning( "%s", s );
		return;
	}

	lastErrorMessage = s;

	teLog->moveCursor( QTextCursor::End );
	teLog->append( s );

	VolatileSettings* vs = QtYabause::volatileSettings();
	if (( !mLogDock->isVisible( )) && ( vs->value( "View/LogWindow" ).toInt() == 1 )) {
		mLogDock->setVisible( true );
	}
}

bool UIYabause::eventFilter( QObject* o, QEvent* e )
{
	if ( e->type() == QEvent::Hide )
		setFocus();
	return QMainWindow::eventFilter( o, e );
}

void UIYabause::errorReceived( const QString& error, bool internal )
{
	if ( internal ) {
		appendLog( error.toLocal8Bit().constData() );
	}
	else {
		CommonDialogs::information( error + "\n" + lastErrorMessage );
	}
}

void UIYabause::sizeRequested( const QSize& s )
{
/*
	int heightOffset = toolBar->height()+menubar->height();
	int width, height;
	if (s.isNull())
	{
		return;
	}
	else
	{
		width=s.width();
		height=s.height();
	}

	mouseXRatio = 320.0 / (float)width * 2.0 * (float)mouseSensitivity / 100.0;
	mouseYRatio = 240.0 / (float)height * 2.0 * (float)mouseSensitivity / 100.0;

	// Compensate for menubar and toolbar
	VolatileSettings* vs = QtYabause::volatileSettings();
	if (vs->value( "View/Menubar" ).toInt() != BD_ALWAYSHIDE)
		height += menubar->height();
	if (vs->value( "View/Toolbar" ).toInt() != BD_ALWAYSHIDE)
		height += toolBar->height();

	resize( width, height ); 
*/
}

void UIYabause::fixAspectRatio( int width , int height )
{
#if 0
  if (this->isFullScreen()) {
    mYabauseGL->viewport_width_ = QtYabause::volatileSettings()->value("Video/FullscreenWidth", "1920").toInt();
    mYabauseGL->viewport_height_ = QtYabause::volatileSettings()->value("Video/FullscreenHeight", "1080").toInt();
    mYabauseGL->viewport_origin_x_ = 0;
    mYabauseGL->viewport_origin_y_ = 0;
    return;
  }

  int aspectRatio = QtYabause::volatileSettings()->value("Video/AspectRatio", 0).toInt();
  switch (aspectRatio)
  {
  case 0:
  case 1:
  case 2:
  {
      int heightOffset = toolBar->height();
      heightOffset += menubar->height();

      VolatileSettings* vs = QtYabause::volatileSettings();
      if (vs->value("Video/RotateScreen").toBool()) {
        if (aspectRatio == 0 || aspectRatio == 1)
          height = 4 * ((float)width / 3);
        else if(aspectRatio == 2)
          height = 16 * ((float)width / 9);
      }
      else {
        if (aspectRatio == 0 || aspectRatio == 1)
          height = 3 * ((float)width / 4);
        else if (aspectRatio == 2)
          height = 9 * ((float)width / 16);
      }

      mouseYRatio = 240.0 / (float)height * 2.0 * (float)mouseSensitivity / 100.0;

      adjustHeight(height);
      mYabauseGL->viewport_height_ = height - heightOffset;
      setFixedHeight(height);
    break;
  }
  case 3:
      setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
      setMinimumSize(0, 0);
      break;
	}
#endif
}

void UIYabause::getSupportedResolutions()
{
#if defined Q_OS_WIN
	DEVMODE devMode;
	BOOL result = TRUE;
	DWORD currentSettings = 0;
	devMode.dmSize = sizeof(DEVMODE);

	supportedResolutions.clear();

	while (result)
	{
		result = EnumDisplaySettings(NULL, currentSettings, &devMode);
		if (result && devMode.dmBitsPerPel == 32)
		{
			supportedRes_struct res;
			res.width = devMode.dmPelsWidth;
			res.height = devMode.dmPelsHeight;
			res.bpp = devMode.dmBitsPerPel;
			res.freq = devMode.dmDisplayFrequency;

			supportedResolutions.append(res);
		}
		currentSettings++;
	}
#elif HAVE_LIBXRANDR
	ResolutionList list;
	supportedRes_struct res;

	list = ScreenGetResolutions();

	while(0 == ScreenNextResolution(list, &res))
		supportedResolutions.append(res);
#endif
}

int UIYabause::isResolutionValid( int width, int height, int bpp, int freq )
{
	for (int i = 0; i < supportedResolutions.count(); i++)
	{
		if (supportedResolutions[i].width == width &&
			supportedResolutions[i].height == height)
			return i;
	}

	return -1;
}

int UIYabause::findBestVideoFreq( int width, int height, int bpp, int videoFormat )
{
	// Try to use a frequency close to 60 hz for NTSC, 75 hz for PAL
	if (videoFormat == VIDEOFORMATTYPE_PAL && isResolutionValid( width, height, bpp, 75 ) > 0)
		return 75;
	else if (videoFormat == VIDEOFORMATTYPE_NTSC && isResolutionValid( width, height, bpp, 60 ) > 0)
		return 60;
	else
	{
		// Since we can't use the frequency we want, use the first one available
		int i=isResolutionValid( width, height, bpp, -1 );
		if (i < 0)
			return -1;
		return supportedResolutions[i].freq;
	}
}

void UIYabause::toggleFullscreen( int width, int height, bool f, int videoFormat )
{
#if 0
	// Make sure setting is valid
	if (f && isResolutionValid( width, height, -1, -1 ) < 0)
		return;

#if defined Q_OS_WIN
	if (f)
	{
		DEVMODE dmScreenSettings;
		memset (&dmScreenSettings, 0, sizeof (dmScreenSettings));

		int freq = findBestVideoFreq( width, height, 32, videoFormat );

		if (freq < 0)
			return;

    hwnd_ = (HWND)this->winId(); //FindWindow(0, 0);
    saved_window_info_.style = GetWindowLong(hwnd_, GWL_STYLE);
    saved_window_info_.ex_style = GetWindowLong(hwnd_, GWL_EXSTYLE);
    saved_window_info_.windowsize = this->size();
    saved_window_info_.windowspos = this->pos();

    //GetWindowRect(hwnd_, &saved_window_info_.window_rect);

		dmScreenSettings.dmSize = sizeof (dmScreenSettings);   
		dmScreenSettings.dmPelsWidth = width;
		dmScreenSettings.dmPelsHeight = height;    
		dmScreenSettings.dmBitsPerPel = 32;
		dmScreenSettings.dmDisplayFrequency = freq;
		dmScreenSettings.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
		ChangeDisplaySettings(&dmScreenSettings, CDS_FULLSCREEN);
	} 
  else {
    ChangeDisplaySettings(NULL, 0);
    //toolBar->show();
    //menubar->show();

    int title_height = (GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CXPADDEDBORDER));
    int title_width = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    SetWindowLong(hwnd_, GWL_STYLE, saved_window_info_.style);
    SetWindowLong(hwnd_, GWL_EXSTYLE, saved_window_info_.ex_style);
    saved_window_info_.windowspos.setX(saved_window_info_.windowspos.x()/* + title_width*/);
    saved_window_info_.windowspos.setY(saved_window_info_.windowspos.y()/* + title_height*/);
    this->move(saved_window_info_.windowspos);
    sizeRequested(saved_window_info_.windowsize);
  }
#elif HAVE_LIBXRANDR
	if (f)
	{
		int i = isResolutionValid(width, height, 32, -1);
		ScreenChangeResolution(&supportedResolutions[i]);
	}
	else
	{
		ScreenRestoreResolution();
	}
#endif
#endif
}

void UIYabause::toggleMenuAndToolBar() {
	isAltPressed = !isAltPressed;

	if (isFullScreen()) {
		if (isAltPressed) {
			//menubar->show();
			//toolBar->show();
			menuBar()->show();
			for (QToolBar* toolBar : findChildren<QToolBar*>()) {
				toolBar->show();
			}
		}
		else {
			//menubar->hide();
			//toolBar->hide();
			menuBar()->hide();
			for (QToolBar* toolBar : findChildren<QToolBar*>()) {
				toolBar->hide();
			}
		}
	}
}

void UIYabause::fullscreenRequested( bool f )
{

	if (!f) {
		showNormal();
		isAltPressed = false;
		//toolBar->show();
		//menubar->show();
		menuBar()->show();
		for (QToolBar* toolBar : findChildren<QToolBar*>()) {
			toolBar->show();
		}
		restoreResolution();
	}
	else {
		isAltPressed = false;
	  originalGeometry = geometry();
		saveCurrentResolution();
		//toolBar->hide();
		//menubar->hide();
		menuBar()->hide();
		for (QToolBar* toolBar : findChildren<QToolBar*>()) {
			toolBar->hide();
		}
		VolatileSettings* vs = QtYabause::volatileSettings();
		setResolution(vs->value("Video/FullscreenWidth", "1920").toInt(), vs->value("Video/FullscreenHeight", "1080").toInt()); 
		showFullScreen();
	}

}

void UIYabause::saveCurrentResolution() {
	EnumDisplaySettings(nullptr, ENUM_CURRENT_SETTINGS, &originalMode);
}

void UIYabause::restoreResolution() {
	ChangeDisplaySettings(&originalMode, 0);
}

void UIYabause::setResolution(int width, int height) {

	QList<QScreen*> screens = QGuiApplication::screens();
	QScreen* targetScreen = screens[0];
	windowHandle()->setScreen(targetScreen);

	DEVMODE mode = originalMode;
	mode.dmPelsWidth = width;
	mode.dmPelsHeight = height;
	mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
	ChangeDisplaySettingsEx(targetScreen->name().toStdWString().c_str(), &mode, NULL, CDS_FULLSCREEN, NULL );
}


void UIYabause::refreshStatesActions()
{
	// reset save actions
	QRegularExpression saveStateRegex("^aFileSaveState\\d+$");
	for (QAction* a : findChildren<QAction*>()) {
		if (saveStateRegex.match(a->objectName()).hasMatch()) {
			if (a == aFileSaveStateAs)
				continue;
			int i = a->objectName().remove("aFileSaveState").toInt();
			a->setText(QString("%1 ... ").arg(i));
			a->setToolTip(a->text());
			a->setStatusTip(a->text());
			a->setData(i);
		}
	}

	// reset load actions
	QRegularExpression loadStateRegex("^aFileLoadState\\d+$");
	for (QAction* a : findChildren<QAction*>()) {
		if (loadStateRegex.match(a->objectName()).hasMatch()) {
			if (a == aFileLoadStateAs)
				continue;
			int i = a->objectName().remove("aFileLoadState").toInt();
			a->setText(QString("%1 ... ").arg(i));
			a->setToolTip(a->text());
			a->setStatusTip(a->text());
			a->setData(i);
			a->setEnabled(false);
		}
	}
	// get states files of this game
	const QString serial = QtYabause::getCurrentCdSerial();
	const QString mask = QString( "%1_*.yss" ).arg( serial );
	const QString statesPath = QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString();
	QRegularExpression rx(QString(mask).replace('*', "(\\d+)"));
	QDir d( statesPath );
	foreach ( const QFileInfo& fi, d.entryInfoList( QStringList( mask ), QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase ) )
	{
		QRegularExpressionMatch match = rx.match(fi.fileName());
		if (match.hasMatch())
		{
			int slot = match.captured(1).toInt();
			const QString caption = QString("%1 %2")
				.arg(slot)
				.arg(QLocale().toString(fi.lastModified(), QLocale::ShortFormat));
			// update save state action
			if ( QAction* a = findChild<QAction*>( QString( "aFileSaveState%1" ).arg( slot ) ) )
			{
				a->setText( caption );
				a->setToolTip( caption );
				a->setStatusTip( caption );
				// update load state action
				a = findChild<QAction*>( QString( "aFileLoadState%1" ).arg( slot ) );
				a->setText( caption );
				a->setToolTip( caption );
				a->setStatusTip( caption );
				a->setEnabled( true );
			}
		}
	}
}

// A boolean setting read back from the ini file is the string "true", and
// toInt() on that is 0 - passing it straight to VideoSetSetting() turns the
// feature off at the very moment the user switched it on. It only ever looked
// right because the next startup reads the same key with toBool().
//
// Takes a QVariant so it serves both callers: the settings dialog keeps its
// snapshot as QVariant, the wizard keeps its as QString.
static int settingAsBool( const QVariant& value )
{
	return value.toBool() ? 1 : 0;
}

void UIYabause::on_aFileSettings_triggered()
{
	Settings *s = (QtYabause::settings());
	QHash<QString, QVariant> hash;
	const QStringList keys = s->allKeys();
	Q_FOREACH(QString key, keys) {
		hash[key] = s->value(key);
	}

	YabauseLocker locker( mYabauseThread );
	if ( UISettings( &supportedResolutions, &translations, window() ).exec() )
	{
		VolatileSettings* vs = QtYabause::volatileSettings();
		aEmulationFrameSkipLimiter->setChecked( vs->value( "General/EnableFrameSkipLimiter" ).toBool() );
		aViewFPS->setChecked( vs->value( "General/ShowFPS" ).toBool() );
		mouseSensitivity = vs->value( "Input/GunMouseSensitivity" ).toInt();
#if 0
		if(isFullScreen())
		{
			if ( vs->value( "View/Menubar" ).toInt() == BD_HIDEFS || vs->value( "View/Menubar" ).toInt() == BD_ALWAYSHIDE )
				menubar->hide();
			else
				menubar->show();

			if ( vs->value( "View/Toolbar" ).toInt() == BD_HIDEFS || vs->value( "View/Toolbar" ).toInt() == BD_ALWAYSHIDE )
				toolBar->hide();
			else
				toolBar->show();
		}
		else
		{
			if ( vs->value( "View/Menubar" ).toInt() == BD_ALWAYSHIDE )
				menubar->hide();
			else
				menubar->show();

			if ( vs->value( "View/Toolbar" ).toInt() == BD_ALWAYSHIDE )
				toolBar->hide();
			else
				toolBar->show();
		}
#endif
		
		//only reset if bios, region, cart,  back up, mpeg, sh2, m68k are changed
		Settings *ss = (QtYabause::settings());
		QHash<QString, QVariant> newhash;
		const QStringList newkeys = ss->allKeys();
		Q_FOREACH(QString key, newkeys) {
			newhash[key] = ss->value(key);
		}
		if(newhash["General/Bios"]!=hash["General/Bios"] ||
			newhash["General/EnableEmulatedBios"]!=hash["General/EnableEmulatedBios"] ||
			newhash["Advanced/Region"]!=hash["Advanced/Region"] ||
			newhash["Cartridge/Type"]!=hash["Cartridge/Type"] ||
			newhash["Memory/Path"]!=hash["Memory/Path"] ||
			newhash["MpegROM/Path" ]!=hash["MpegROM/Path" ] ||
			newhash["Advanced/SH2Interpreter" ]!=hash["Advanced/SH2Interpreter" ] ||
         newhash["Advanced/68kCore"] != hash["Advanced/68kCore"] ||
			newhash["General/CdRom"]!=hash["General/CdRom"] ||
			newhash["General/CdRomISO"]!=hash["General/CdRomISO"] ||
			newhash["General/ClockSync"]!=hash["General/ClockSync"] ||
			newhash["General/FixedBaseTime"]!=hash["General/FixedBaseTime"] ||
      newhash["General/UseSh2Cache"] != hash["General/UseSh2Cache"]
		)
		{
			if ( mYabauseThread->pauseEmulation( true, true ) )
				refreshStatesActions();
			return;
		}
#ifdef HAVE_LIBMINI18N
		if(newhash["General/Translation"] != hash["General/Translation"])
		{
			mini18n_close();
			retranslateUi(this);
			if ( QtYabause::setTranslationFile() == -1 )
				qWarning( "Can't set translation file" );
			QtYabause::retranslateApplication();
		}
#endif
		if(newhash["Video/VideoCore"] != hash["Video/VideoCore"])
			on_cbVideoDriver_currentIndexChanged(newhash["Video/VideoCore"].toInt());
		
		if(newhash["General/ShowFPS"] != hash["General/ShowFPS"])
			SetOSDToggle(newhash["General/ShowFPS"].toBool());

		if (newhash["General/EnableMultiThreading"] != hash["General/EnableMultiThreading"] ||
			 newhash["General/NumThreads"] != hash["General/NumThreads"])
		{
			if (newhash["General/EnableMultiThreading"].toBool())
			{
				int num = newhash["General/NumThreads"].toInt() < 1 ? 1 : newhash["General/NumThreads"].toInt();
				VIDSoftSetVdp1ThreadEnable(num == 1 ? 0 : 1);
				VIDSoftSetNumLayerThreads(num);
				VIDSoftSetNumPriorityThreads(num);
			}
			else
			{
				VIDSoftSetVdp1ThreadEnable(0);
				VIDSoftSetNumLayerThreads(1);
				VIDSoftSetNumPriorityThreads(1);
			}
		}

		
		if (newhash["Sound/SoundCore"] != hash["Sound/SoundCore"])
			ScspChangeSoundCore(newhash["Sound/SoundCore"].toInt());

      if (newhash["Sound/NewScsp"].toBool() != hash["Sound/NewScsp"])
         scsp_set_use_new(newhash["Sound/NewScsp"].toInt());

#ifndef USE_SCSP2
		// High-quality sound: safe to switch while a game runs (the renderer is
		// built here, on this thread). The float DSP follows the setting.
		if (settingAsBool(newhash["Sound/HighQuality"]) != settingAsBool(hash["Sound/HighQuality"]))
		{
			const int hq = settingAsBool(newhash["Sound/HighQuality"]);
			if (hq)
			{
				ScspSetHighQualityFloatDsp(1);
				ScspSetHighQuality(1);
			}
			else
			{
				ScspSetHighQuality(0);
				ScspSetHighQualityFloatDsp(0);
			}
		}
#endif

		if (newhash["Video/WindowWidth"] != hash["Video/WindowWidth"] || newhash["Video/WindowHeight"] != hash["Video/WindowHeight"] ||
          newhash["View/Menubar"] != hash["View/Menubar"] || newhash["View/Toolbar"] != hash["View/Toolbar"] || 
          newhash["Input/GunMouseSensitivity"] != hash["Input/GunMouseSensitivity"])
        sizeRequested(QSize(newhash["Video/WindowWidth"].toInt(),newhash["Video/WindowHeight"].toInt()));
    fixAspectRatio(rect().width(), rect().height());

    if (newhash["Video/resolution_mode"] != hash["Video/resolution_mode"]) {
      VideoSetSetting(VDP_SETTING_RESOLUTION_MODE, newhash["Video/resolution_mode"].toInt());
    }

    if (newhash["Video/rbg_resolution_mode"] != hash["Video/rbg_resolution_mode"]) {
      VideoSetSetting(VDP_SETTING_RBG_RESOLUTION_MODE, newhash["Video/rbg_resolution_mode"].toInt());
    }

    if (newhash["Video/filter_mode"] != hash["Video/filter_mode"]) {
      VideoSetSetting(VDP_SETTING_FILTERMODE, newhash["Video/filter_mode"].toInt());
    }

	if (newhash["Video/UseComputeShader"] != hash["Video/UseComputeShader"]) {
		VideoSetSetting(VDP_SETTING_RBG_USE_COMPUTESHADER, settingAsBool(newhash["Video/UseComputeShader"]));
	}

  if (newhash["Video/polygon_generation_mode"] != hash["Video/polygon_generation_mode"]) {
    VideoSetSetting(VDP_SETTING_POLYGON_MODE, newhash["Video/polygon_generation_mode"].toInt());
  }

  if (newhash["General/EmulationSpeed"] != hash["General/EmulationSpeed"]) {
    VDP2SetFrameLimit(newhash["General/EmulationSpeed"].toInt());
  }

  

		if (newhash["Video/FullscreenWidth"] != hash["Video/FullscreenWidth"] || 
			newhash["Video/FullscreenHeight"] != hash["Video/FullscreenHeight"] ||
			newhash["Video/Fullscreen"] != hash["Video/Fullscreen"])
		{
			bool f = isFullScreen();
			if (f)
				fullscreenRequested( false );
			fullscreenRequested( f );
		}

		mYabauseThread->reloadControllers();
		refreshStatesActions();
	}
}

void UIYabause::on_actionOpen_Tray_triggered()
{
	YabauseLocker locker(mYabauseThread);

	if (mIsCdIn){
		mYabauseThread->OpenTray();
		mIsCdIn = false;
	}
	else{
		const QString fn = CommonDialogs::getOpenFileName(QtYabause::volatileSettings()->value("Recents/ISOs").toString(), QtYabause::translate("Select your iso/cue/bin file"), QtYabause::translate("CD Images (*.chd *.iso *.cue *.bin *.mds *.ccd)"));
		if (!fn.isEmpty())
		{
			VolatileSettings* vs = QtYabause::volatileSettings();
			const int currentCDCore = vs->value("General/CdRom").toInt();
			const QString currentCdRomISO = vs->value("General/CdRomISO").toString();

			QtYabause::settings()->setValue("Recents/ISOs", fn);

			vs->setValue("autostart", false);
			vs->setValue("General/CdRom", ISOCD.id);
			vs->setValue("General/CdRomISO", fn);
			vs->setValue("General/PlaySSF", false);

			refreshStatesActions();

		}
		mYabauseThread->CloseTray();
		mIsCdIn = true;
	}
}

void UIYabause::on_actionGame_Browser_triggered() {
	YabauseLocker locker(mYabauseThread);

	if (mStackedWidget->currentWidget() == mFileSearch) {
		VolatileSettings* vs = QtYabause::volatileSettings();
		int vidcoretype = vs->value("Video/VideoCore").toInt();
		if (vidcoretype == VIDCORE_VULKAN) {
			mStackedWidget->setCurrentWidget(mYabVulkanWidget);
		}
		else {
			mStackedWidget->setCurrentWidget(mYabauseGL);
		}
	}
	else {
		mStackedWidget->setCurrentWidget(mFileSearch);
	}

}


void UIYabause::on_aFileOpenISO_triggered()
{
	YabauseLocker locker( mYabauseThread );
	const QString fn = CommonDialogs::getOpenFileName( QtYabause::volatileSettings()->value( "Recents/ISOs" ).toString(), QtYabause::translate( "Select your iso/cue/bin file" ), QtYabause::translate( "CD Images (*.chd *.iso *.cue *.bin *.mds *.ccd)" ) );
	if ( !fn.isEmpty() )
	{
		VolatileSettings* vs = QtYabause::volatileSettings();
		const int currentCDCore = vs->value( "General/CdRom" ).toInt();
		const QString currentCdRomISO = vs->value( "General/CdRomISO" ).toString();
		
		QtYabause::settings()->setValue( "Recents/ISOs", fn );

    // Save it permanently
    QtYabause::settings()->setValue("General/CdRom", ISOCD.id);
    QtYabause::settings()->setValue("General/CdRomISO", fn);
    QtYabause::settings()->setValue("General/PlaySSF", false);

		vs->setValue( "autostart", false );
		vs->setValue( "General/CdRom", ISOCD.id );
		vs->setValue( "General/CdRomISO", fn );
    vs->setValue("General/PlaySSF", false);

		int vidcoretype = vs->value("Video/VideoCore").toInt();
		if (vidcoretype == VIDCORE_VULKAN) {
			mStackedWidget->setCurrentWidget(mYabVulkanWidget);
			mYabVulkanWidget->show();
			mYabauseThread->pauseEmulation(false, true, [&]() {
				mYabVulkanWidget->setYabauseThread(mYabauseThread);
			});

			mYabVulkanWidget->updateView();
			mYabVulkanWidget->update();
		}
		else {
			mStackedWidget->setCurrentWidget(mYabauseGL);
			mYabauseGL->setYabauseThread(mYabauseThread);
			mYabauseGL->makeCurrent();
			mYabauseThread->pauseEmulation(false, true);
			mYabauseGL->update();
		}

		refreshStatesActions();

	}
}

void UIYabause::on_aFileOpenSSF_triggered()
{
   YabauseLocker locker(mYabauseThread);

   const QString fn = CommonDialogs::getOpenFileName(
      QtYabause::volatileSettings()->value("Recents/SSFs").toString(), 
      QtYabause::translate("Select your ssf file"), 
      QtYabause::translate("Sega Saturn Sound Format files (*.ssf *.minissf)"));

   if (!fn.isEmpty())
   {
      VolatileSettings* vs = QtYabause::volatileSettings();

      QtYabause::settings()->setValue("Recents/SSFs", fn);

      vs->setValue("autostart", false);
      vs->setValue("General/SSFPath", fn);
      vs->setValue("General/PlaySSF", true);

			int vidcoretype = vs->value("Video/VideoCore").toInt();
			if (vidcoretype == VIDCORE_VULKAN) {
				mStackedWidget->setCurrentWidget(mYabVulkanWidget);
				mYabVulkanWidget->show();
				mYabauseThread->pauseEmulation(false, true, [&]() {
					mYabVulkanWidget->setYabauseThread(mYabauseThread);
				});
				mYabVulkanWidget->updateView();
				mYabVulkanWidget->update();
			}
			else {
				mStackedWidget->setCurrentWidget(mYabauseGL);
				mYabauseGL->setYabauseThread(mYabauseThread);
				mYabauseGL->makeCurrent();
				mYabauseThread->pauseEmulation(false, true);
				mYabauseGL->update();
			}
			refreshStatesActions();
   }
}

void UIYabause::on_aFileOpenCDRom_triggered()
{
	YabauseLocker locker( mYabauseThread );
	QStringList list = getCdDriveList();
	int current = list.indexOf(QtYabause::volatileSettings()->value( "Recents/CDs").toString());
	QString fn = QInputDialog::getItem(this, QtYabause::translate("Open CD Rom"), 
													QtYabause::translate("Choose a cdrom drive/mount point") + ":",
													list, current, false);
	if (!fn.isEmpty())
	{
		VolatileSettings* vs = QtYabause::volatileSettings();
		const int currentCDCore = vs->value( "General/CdRom" ).toInt();
		const QString currentCdRomISO = vs->value( "General/CdRomISO" ).toString();

		QtYabause::settings()->setValue( "Recents/CDs", fn );

		vs->setValue( "autostart", false );
		vs->setValue( "General/CdRom", QtYabause::defaultCDCore().id );
		vs->setValue( "General/CdRomISO", fn );
      vs->setValue("General/PlaySSF", false);

			int vidcoretype = vs->value("Video/VideoCore").toInt();
			if (vidcoretype == VIDCORE_VULKAN) {
				mStackedWidget->setCurrentWidget(mYabVulkanWidget);
				mYabVulkanWidget->show();
				mYabauseThread->pauseEmulation(false, true, [&]() {
					mYabVulkanWidget->setYabauseThread(mYabauseThread);
				});
				mYabVulkanWidget->updateView();
				mYabVulkanWidget->update();
			}
			else {
				mStackedWidget->setCurrentWidget(mYabauseGL);
				mYabauseGL->setYabauseThread(mYabauseThread);
				mYabauseGL->makeCurrent();
				mYabauseThread->pauseEmulation(false, true);
				mYabauseGL->update();
			}

		refreshStatesActions();

	}
}

void UIYabause::on_mFileSaveState_triggered( QAction* a )
{
	if ( a == aFileSaveStateAs || a == actionTo_Cloud )
		return;
	YabauseLocker locker( mYabauseThread );
	if ( YabSaveStateSlot( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString().toLatin1().constData(), a->data().toInt() ) != 0 )
		CommonDialogs::information( QtYabause::translate( "Couldn't save state file" ) );
	else
		refreshStatesActions();
}

void UIYabause::on_mFileLoadState_triggered( QAction* a )
{
	if ( a == aFileLoadStateAs || a == actionFrom_Cloud )
		return;
	YabauseLocker locker( mYabauseThread );
	if ( YabLoadStateSlot( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString().toLatin1().constData(), a->data().toInt() ) != 0 )
		CommonDialogs::information( QtYabause::translate( "Couldn't load state file" ) );
}

void UIYabause::on_aFileSaveStateAs_triggered()
{
	YabauseLocker locker( mYabauseThread );
	const QString fn = CommonDialogs::getSaveFileName( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString(), QtYabause::translate( "Choose a file to save your state" ), QtYabause::translate( "Yabause Save State (*.yss)" ) );
	if ( fn.isNull() )
		return;
	if ( YabSaveState( fn.toLatin1().constData() ) != 0 )
		CommonDialogs::information( QtYabause::translate( "Couldn't save state file" ) );
}

void UIYabause::on_aFileLoadStateAs_triggered()
{
	YabauseLocker locker( mYabauseThread );
	const QString fn = CommonDialogs::getOpenFileName( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString(), QtYabause::translate( "Select a file to load your state" ), QtYabause::translate( "Yabause Save State (*.yss)" ) );
	if ( fn.isNull() )
		return;
	if ( YabLoadState( fn.toLatin1().constData() ) != 0 )
		CommonDialogs::information( QtYabause::translate( "Couldn't load state file" ) );
	else
		aEmulationRun->trigger();
}

void UIYabause::takeScreenshot(const char * fname) {
  // YABA_SHOT_PROF=1: report where the time in a screenshot goes. Capturing
  // one is reported to stall the emulator for about a second, which no single
  // step here should cost.
  const bool prof = (getenv("YABA_SHOT_PROF") != NULL);
  auto now = []() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  const double t_enter = now();
  YabauseLocker locker(mYabauseThread);
  const double t_lock = now();

	VolatileSettings* vs = QtYabause::volatileSettings();
	int vidcoretype = vs->value("Video/VideoCore").toInt();
	if (vidcoretype == VIDCORE_VULKAN) {
		if (!VIDCore || !VIDCore->GetScreenshot) return;
		void* buf = nullptr;
		int w = 0, h = 0;
		VIDCore->GetScreenshot(&buf, &w, &h);
		const double t_get = now();
		double t_alpha = t_get, t_mirror = t_get, t_write = t_get;
		if (buf && w > 0 && h > 0) {
			unsigned char* px = static_cast<unsigned char*>(buf);
			// Match the standalone yui.cpp behavior: force opaque alpha so PNG
			// readers don't show the rendered frame as semi-transparent.
			for (int i = 3; i < w * h * 4; i += 4) px[i] = 0xFF;
			t_alpha = now();
			// VIDVulkan::getScreenshot returns RGBA8 in bottom-up order
			// (default branch reads src[height-1-y] into dst[y]); QImage uses
			// top-down so flip vertically before writing.
			QImage img(static_cast<const uchar*>(buf), w, h, w * 4,
			           QImage::Format_RGBA8888);
			QImage flipped = img.mirrored(false, true);
			t_mirror = now();
			QImageWriter iw(fname);
			iw.write(flipped);
			t_write = now();
		}
		// Caller frees the malloc'd buffer (see VIDVulkan.cpp:7378).
		if (buf) free(buf);
		if (prof) {
			printf("[SHOT] lock=%.1f get=%.1f alpha=%.1f mirror=%.1f write=%.1f "
			       "total=%.1f ms (%dx%d)\n",
			       t_lock - t_enter, t_get - t_lock, t_alpha - t_get,
			       t_mirror - t_alpha, t_write - t_mirror, now() - t_enter, w, h);
			fflush(stdout);
		}
	}
	else {
		QImage screenshot = mYabauseGL->grabFramebuffer();
		QImageWriter iw(fname);
		iw.write(screenshot);
		if (prof) {
			printf("[SHOT/gl] total=%.1f ms\n", now() - t_enter);
			fflush(stdout);
		}
	}

}



void UIYabause::on_aFileAndroid_triggered() {
	on_actionAndroid_triggered();
}

void UIYabause::on_aFileiOS_triggered() {
	on_actioniOS_triggered();
}


void UIYabause::on_aFileScreenshot_triggered()
{
  // Recording-mode hotkey path: PlayRecorder timestamps a frame and calls
  // takeScreenshot() asynchronously. This works for both OpenGL and Vulkan
  // because takeScreenshot() now branches on VIDCORE_VULKAN.
  PlayRecorder * p = PlayRecorder::getInstance();
  if (p->getStatus() == 0) {
    p->takeShot();
    return;
  }

	// images filter that qt can write
	QStringList filters;
	foreach ( QByteArray ba, QImageWriter::supportedImageFormats() )
		if ( !filters.contains( ba, Qt::CaseInsensitive ) )
			filters << QString( ba ).toLower();
	for ( int i = 0; i < filters.count(); i++ )
		filters[i] = QtYabause::translate( "%1 Images (*.%2)" ).arg( filters[i].toUpper() ).arg( filters[i] );

#if defined(HAVE_LIBGL) && !defined(QT_OPENGL_ES_1) && !defined(QT_OPENGL_ES_2)
	if (VIDCore && VIDCore->id == VIDCORE_OGL) {
		glReadBuffer(GL_FRONT);
	}
#endif

	QImage screenshot;
	void* vkBuf = nullptr;  // owned by us when non-null (Vulkan path malloc)
	// take screenshot of gl view
	if (VIDCore && VIDCore->id == VIDCORE_OGL) {
		screenshot = mYabauseGL->grabFramebuffer();
	}
	else if (VIDCore && VIDCore->id == VIDCORE_VULKAN && VIDCore->GetScreenshot) {
		int w = 0, h = 0;
		VIDCore->GetScreenshot(&vkBuf, &w, &h);
		if (vkBuf && w > 0 && h > 0) {
			unsigned char* px = static_cast<unsigned char*>(vkBuf);
			for (int i = 3; i < w * h * 4; i += 4) px[i] = 0xFF;
			screenshot = QImage(static_cast<const uchar*>(vkBuf), w, h, w * 4,
			                    QImage::Format_RGBA8888).mirrored(false, true);
		}
	}


	YabauseLocker locker(mYabauseThread);

	// request a file to save to to user
	QString s = CommonDialogs::getSaveFileName( QString(), QtYabause::translate( "Choose a location for your screenshot" ), filters.join( ";;" ) );

	// if the user didn't provide a filename extension, we force it to png
	QFileInfo qfi( s );
	if ( qfi.suffix().isEmpty() )
		s += ".png";

	// write image if ok
	if ( !s.isEmpty() )
	{
		QImageWriter iw( s );
		if ( !iw.write( screenshot ))
		{
			CommonDialogs::information( QtYabause::translate( "An error occur while writing the screenshot: " + iw.errorString()) );
		}
	}
	if (vkBuf) free(vkBuf);
}

void UIYabause::on_aFileQuit_triggered()
{ close(); }

void UIYabause::on_aEmulationRun_triggered()
{
	VolatileSettings* vs = QtYabause::volatileSettings();
	int vidcoretype = vs->value("Video/VideoCore").toInt();
	if (vidcoretype == VIDCORE_VULKAN) {
		mStackedWidget->setCurrentWidget(mYabVulkanWidget);
    mYabVulkanWidget->setYabauseThread(mYabauseThread);
		mYabVulkanWidget->show();
		mYabVulkanWidget->updateView();
		mYabVulkanWidget->update();
	}
	else {
		mStackedWidget->setCurrentWidget(mYabauseGL);
		mYabauseGL->setYabauseThread(mYabauseThread);
		mYabauseGL->makeCurrent();
		mYabauseGL->update();
	}

	if ( mYabauseThread->emulationPaused() )
	{
		mYabauseThread->pauseEmulation( false, false );
		refreshStatesActions();
		if (isFullScreen())
			hideMouseTimer->start(3 * 1000);
	}
	
}

void UIYabause::on_actionRecord_triggered() {

  PlayRecorder * p = PlayRecorder::getInstance();
  if (p->getStatus() == -1) {
    using std::placeholders::_1;
    p->f_takeScreenshot = std::bind(&UIYabause::takeScreenshot, this, _1);
    p->startRocord();
    default_title = windowTitle();
    setWindowTitle("Recording");
    this->actionRecord->setText("Stop Record");

  }
  else if (p->getStatus() == 0) {
    p->stopRocord();
    setWindowTitle(default_title);
    this->actionRecord->setText("Record");
  }

}

void UIYabause::on_actionPlay_triggered() {
  PlayRecorder * p = PlayRecorder::getInstance();
  if (p->getStatus() == -1) {
    using std::placeholders::_1;
    p->f_takeScreenshot = std::bind(&UIYabause::takeScreenshot, this, _1);
		QString s = QFileDialog::getExistingDirectory(
    this, 
    tr("Choose a location of your record"),
    NULL,
    QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);		

    if (!s.isEmpty()) {
      QByteArray ba = s.toLocal8Bit();
      p->startPlay(ba.data(), true, nullptr);
      default_title = windowTitle();
      setWindowTitle("Playing");
    }
  }
  else if (p->getStatus() == 1) {
    //p->stopRocord();
  }
    
}

void UIYabause::on_aEmulationPause_triggered()
{
	if ( !mYabauseThread->emulationPaused() )
		mYabauseThread->pauseEmulation( true, false );

	if (VIDCore && VIDCore->id == VIDCORE_VULKAN) {
    mYabVulkanWidget->update();
	}else{
		mYabauseGL->update();
	}
	
}

void UIYabause::on_aEmulationReset_triggered()
{ mYabauseThread->resetEmulation(); }

void UIYabause::on_aEmulationFrameSkipLimiter_toggled( bool toggled )
{
	Settings* vs = QtYabause::settings();
	vs->setValue( "General/EnableFrameSkipLimiter", toggled );
	vs->sync();

	if ( toggled )
		EnableAutoFrameSkip();
	else
		DisableAutoFrameSkip();
}

void UIYabause::on_aToolsBackupManager_triggered()
{
	YabauseLocker locker( mYabauseThread );
	if ( mYabauseThread->init() < 0 )
	{
		CommonDialogs::information( QtYabause::translate( "Yabause is not initialized, can't manage backup ram." ) );
		return;
	}
	UIBackupRam( this ).exec();
}

void UIYabause::on_aToolsCheatsList_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UICheats( this ).exec();
}

void UIYabause::on_aToolsCheatSearch_triggered()
{
   YabauseLocker locker( mYabauseThread );
   UICheatSearch cs(this, &search, searchType);
      
   cs.exec();

   search = *cs.getSearchVariables( &searchType);
}

void UIYabause::on_aToolsTransfer_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIMemoryTransfer( mYabauseThread, this ).exec();
}

void UIYabause::on_aViewFPS_triggered( bool toggled )
{
	Settings* vs = QtYabause::settings();
	vs->setValue( "General/ShowFPS", toggled );
	vs->sync();
	SetOSDToggle(toggled ? 1 : 0);
}

void UIYabause::on_aViewLayerVdp1_triggered()
{ ToggleVDP1(); }

void UIYabause::on_aViewLayerNBG0_triggered()
{ ToggleNBG0(); }

void UIYabause::on_aViewLayerNBG1_triggered()
{ ToggleNBG1(); }

void UIYabause::on_aViewLayerNBG2_triggered()
{ ToggleNBG2(); }

void UIYabause::on_aViewLayerNBG3_triggered()
{ ToggleNBG3(); }

void UIYabause::on_aViewLayerRBG0_triggered()
{ ToggleRBG0(); }

void UIYabause::on_aViewFullscreen_triggered( bool b )
{
	fullscreenRequested( b );
}

void UIYabause::breakpointHandlerMSH2(bool displayMessage)
{
	YabauseLocker locker( mYabauseThread );
	if (displayMessage)
		CommonDialogs::information( QtYabause::translate( "Breakpoint Reached" ) );
	UIDebugSH2( true, mYabauseThread, this ).exec();
}

void UIYabause::breakpointHandlerSSH2(bool displayMessage)
{
	YabauseLocker locker( mYabauseThread );
	if (displayMessage)
		CommonDialogs::information( QtYabause::translate( "Breakpoint Reached" ) );
	UIDebugSH2( false, mYabauseThread, this ).exec();
}

void UIYabause::breakpointHandlerM68K()
{
	YabauseLocker locker( mYabauseThread );
	CommonDialogs::information( QtYabause::translate( "Breakpoint Reached" ) );
	UIDebugM68K( mYabauseThread, this ).exec();
}

void UIYabause::breakpointHandlerSCUDSP()
{
	YabauseLocker locker( mYabauseThread );
	CommonDialogs::information( QtYabause::translate( "Breakpoint Reached" ) );
	UIDebugSCUDSP( mYabauseThread, this ).exec();
}

void UIYabause::breakpointHandlerSCSPDSP()
{
	YabauseLocker locker( mYabauseThread );
	CommonDialogs::information( QtYabause::translate( "Breakpoint Reached" ) );
	UIDebugSCSPDSP( mYabauseThread, this ).exec();
}

void UIYabause::on_aViewDebugMSH2_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIDebugSH2( true, mYabauseThread, this ).exec();
}

void UIYabause::on_aViewDebugSSH2_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIDebugSH2( false, mYabauseThread, this ).exec();
}

void UIYabause::on_aViewDebugVDP1_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIDebugVDP1( this ).exec();
}

void UIYabause::on_aViewDebugVDP2_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIDebugVDP2( this ).exec();
}

void UIYabause::on_aViewDebugM68K_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIDebugM68K( mYabauseThread, this ).exec();
}

void UIYabause::on_aViewDebugSCUDSP_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIDebugSCUDSP( mYabauseThread, this ).exec();
}

void UIYabause::on_aViewDebugSCSP_triggered()
{
	UIDebugSCSP *dialog = new UIDebugSCSP(this);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void UIYabause::on_aViewDebugSCSPChan_triggered()
{
   if (use_new_scsp) {
      UIDebugSCSPChan *dialog = new UIDebugSCSPChan(this);
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->show();
   } else {
      CommonDialogs::information( QtYabause::translate( "Only available with new scsp code(USE_NEW_SCSP=1)" ) );
   }
}

void UIYabause::on_aViewDebugSCSPDSP_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIDebugSCSPDSP( mYabauseThread, this ).exec();
}

void UIYabause::on_aViewDebugMemoryEditor_triggered()
{
	YabauseLocker locker( mYabauseThread );
	UIMemoryEditor( mYabauseThread, this ).exec();
}

void UIYabause::on_aTraceLogging_triggered( bool toggled )
{
#ifdef SH2_TRACE
	SH2SetInsTracing(toggled? 1 : 0);
#endif
	return;
}

void UIYabause::on_actionOpen_web_interface_triggered() {
  //QDesktopServices::openUrl(QUrl(actionOpen_web_interface->statusTip()));
  YabauseLocker locker( mYabauseThread );
  WebLoginWindow( window() ).exec();
}

void UIYabause::on_aHelpReport_triggered()
{
	QDesktopServices::openUrl(QUrl(aHelpReport->statusTip()));
}

void UIYabause::on_aHelpCompatibilityList_triggered()
{ QDesktopServices::openUrl( QUrl( aHelpCompatibilityList->statusTip() ) ); }

void UIYabause::on_aHelpAbout_triggered()
{
  YabauseLocker locker(mYabauseThread);
  UIAbout(window()).exec();
}


// Snapshot of every ini key, as a string.
//
// Never compare the two snapshots as QVariant: the values read before the
// wizard come back from the ini as QString, while the ones the wizard has just
// written are still ints in QSettings' cache, so QVariant equality would report
// a change that did not happen. The string form is stable in both directions,
// and it also distinguishes a key that did not exist before (empty) from one
// the wizard has just written.
static QHash<QString, QString> settingsSnapshot()
{
	QHash<QString, QString> snapshot;
	Settings* s = QtYabause::settings();
	const QStringList keys = s->allKeys();
	Q_FOREACH( const QString& key, keys )
		snapshot.insert( key, s->value( key ).toString() );
	return snapshot;
}

static bool settingChanged( const QHash<QString, QString>& before, const QHash<QString, QString>& after, const QString& key )
{
	return before.value( key ) != after.value( key );
}

void UIYabause::on_aHelpSetupWizard_triggered()
{
	// Every modal handler in this file pauses the emulation thread while its
	// dialog is up. A game must not keep running behind the wizard.
	YabauseLocker locker( mYabauseThread );

	// The wizard writes straight to the ini, so nothing is applied to the
	// running emulator unless this handler does it. on_aFileSettings_triggered()
	// does the same job for the settings dialog; the applicable subset is
	// duplicated here rather than factored out, because that function is
	// mostly settings-dialog specific (fullscreen, menu bar, translations,
	// software renderer threads) and pulling a shared helper out of it would
	// be a far larger and riskier change than the handful of calls below.
	const QHash<QString, QString> before = settingsSnapshot();

	UISetupWizard wizard( this );
	if ( wizard.exec() != QDialog::Accepted )
		return;

	const QHash<QString, QString> after = settingsSnapshot();

	// Controls. Called unconditionally rather than gated on an Input/ key
	// having changed: the device page polls PERSDLJoyRefreshDevices() once a
	// second while it is open, and any hot-plug there tears down and rebuilds
	// SDL's device table, renumbering indices that live PerPad bindings embed.
	// A user who replugs a pad while the wizard is up but only changes, say,
	// the graphics preset would otherwise leave that pad dead until restart,
	// because nothing under Input/ actually changed.
	mYabauseThread->reloadControllers();

	// Video settings the renderer picks up at once, applied exactly as
	// on_aFileSettings_triggered() applies them.
	if ( settingChanged( before, after, "Video/resolution_mode" ) )
		VideoSetSetting( VDP_SETTING_RESOLUTION_MODE, after.value( "Video/resolution_mode" ).toInt() );

	if ( settingChanged( before, after, "Video/rbg_resolution_mode" ) )
		VideoSetSetting( VDP_SETTING_RBG_RESOLUTION_MODE, after.value( "Video/rbg_resolution_mode" ).toInt() );

	if ( settingChanged( before, after, "Video/polygon_generation_mode" ) )
		VideoSetSetting( VDP_SETTING_POLYGON_MODE, after.value( "Video/polygon_generation_mode" ).toInt() );

	if ( settingChanged( before, after, "Video/UseComputeShader" ) )
		VideoSetSetting( VDP_SETTING_RBG_USE_COMPUTESHADER, settingAsBool( after.value( "Video/UseComputeShader" ) ) );

	if ( settingChanged( before, after, "Sound/SoundCore" ) )
		ScspChangeSoundCore( after.value( "Sound/SoundCore" ).toInt() );

	// Settings that are only read while the emulator is starting up. UISettings
	// routes edits to these through checkRestart(); the wizard writes them
	// directly, so it has to notice the change itself.
	//   Video/VideoCore                 - chosen in YabauseThread::initEmulation
	//   Video/polygon_generation_mode   - VideoSetSetting above only covers the
	//                                     part the running renderer can switch;
	//                                     Middle and High differ only here, so
	//                                     dropping it would silence the notice
	//                                     for the most likely change of all
	//   Sound/ScspSync, Sound/ScspMainMode - copied into yabauseinit_struct once
	QStringList restartKeys;
	restartKeys << "Video/VideoCore"
	            << "Video/polygon_generation_mode"
	            << "Sound/ScspSync"
	            << "Sound/ScspMainMode";

	bool needsRestart = false;
	Q_FOREACH( const QString& key, restartKeys )
	{
		if ( settingChanged( before, after, key ) )
			needsRestart = true;
	}

	if ( needsRestart )
	{
		CommonDialogs::information( QtYabause::translate(
			"Some of these settings only take effect when Yaba Sanshiro starts. Please restart it." ) );
	}

	// A different disc means the emulation has to come back up around it,
	// which is what on_aFileSettings_triggered() does for the same two keys.
	// Done last, after the notice, so that a user who changed both the disc
	// and a startup-only setting still gets told about the restart.
	if ( settingChanged( before, after, "General/CdRom" ) ||
	     settingChanged( before, after, "General/CdRomISO" ) )
	{
		if ( mYabauseThread->pauseEmulation( true, true ) )
			refreshStatesActions();
	}
}


void UIYabause::on_aSound_triggered()
{
	// show volume widget	
	sVolume->setValue(QtYabause::volatileSettings()->value( "Sound/Volume").toInt());
	QWidget* ab = toolBar->widgetForAction( aSound );
	fSound->move( ab->mapToGlobal( ab->rect().bottomLeft() ) );
	fSound->show();
}

void UIYabause::on_aVideoDriver_triggered()
{
	// set current core the selected one in the combo list
	if ( VIDCore )
	{
		cbVideoDriver->blockSignals( true );
		for ( int i = 0; VIDCoreList[i] != NULL; i++ )
		{
			if ( VIDCoreList[i]->id == VIDCore->id )
			{
				cbVideoDriver->setCurrentIndex( cbVideoDriver->findData( VIDCore->id ) );
				break;
			}
		}
		cbVideoDriver->blockSignals( false );
	}
	//  show video core widget
	QWidget* ab = toolBar->widgetForAction( aVideoDriver );
	fVideoDriver->move( ab->mapToGlobal( ab->rect().bottomLeft() ) );
	fVideoDriver->show();
}

void UIYabause::on_cbSound_toggled( bool toggled )
{
	if ( toggled )
		ScspUnMuteAudio(SCSP_MUTE_USER);
	else
		ScspMuteAudio(SCSP_MUTE_USER);
	cbSound->setIcon( QIcon( toggled ? ":/actions/sound.png" : ":/actions/mute.png" ) );
}

void UIYabause::on_sVolume_valueChanged( int value )
{ 
	ScspSetVolume( value ); 
	Settings* vs = QtYabause::settings();
	vs->setValue("Sound/Volume", value );
}

void UIYabause::on_cbVideoDriver_currentIndexChanged( int id )
{
	VideoInterface_struct* core = QtYabause::getVDICore( cbVideoDriver->itemData( id ).toInt() );
	if ( core )
	{
		if (VideoChangeCore(core->id) == 0) {
			//mYabauseGL->updateView();
		}
	}
}

void UIYabause::pause( bool paused )
{
	aEmulationRun->setEnabled( paused );
	aEmulationPause->setEnabled( !paused );
	aEmulationReset->setEnabled( !paused );
	if (VIDCore && VIDCore->id == VIDCORE_OGL) {
		mYabauseGL->updateView();
		mYabauseGL->update();
	}else	if (VIDCore && VIDCore->id == VIDCORE_VULKAN) {
		mYabVulkanWidget->updateView();
		mYabVulkanWidget->update();
	}
}

void UIYabause::reset()
{
	if (VIDCore && VIDCore->id == VIDCORE_OGL) {
		mYabauseGL->updateView();
		mYabauseGL->update();
	}else	if (VIDCore && VIDCore->id == VIDCORE_VULKAN) {
		mYabVulkanWidget->updateView();
		mYabVulkanWidget->update();
	}
}

void UIYabause::toggleEmulateMouse( bool enable )
{
	emulateMouse = enable;
}

#include <firebase/app.h>
#include <firebase/auth.h>
#include <firebase/database.h>
#include <firebase/storage/metadata.h>

using firebase::Future;
using firebase::database::DataSnapshot;
using firebase::storage::Metadata;
using firebase::storage::Storage;

#include <zlib.h>
const int CHUNK = 16384;

#include <iostream>
#include <fstream>
#include <cstdio>

void UIYabause::on_actionAndroid_triggered() {
	QUrl url("https://play.google.com/store/apps/details?id=org.devmiyax.yabasanshioro2.pro");
	QDesktopServices::openUrl(url);
}

void UIYabause::on_actioniOS_triggered() {
	QUrl url("https://apps.apple.com/jp/app/yaba-sanshiro-2/id1549144351");
	QDesktopServices::openUrl(url);
}


void UIYabause::on_actionTo_Cloud_triggered()
{
  YabauseLocker locker(mYabauseThread);
  firebase::auth::Auth *auth = firebase::auth::Auth::GetAuth(UIYabause::getFirebaseApp());
  firebase::auth::User user = auth->current_user();
  if (!user.is_valid()) {
    return;
  }

  QString datapath = getDataDirPath();
  std::string sdatapath = datapath.toStdString();
  sdatapath += "/current_state_save.bin";

  const char* gamecode = Cs2GetCurrentGmaecode();

  if (YabSaveCompressedState(sdatapath.c_str()) == -1) {
    return;
  }
  
  Storage *storage = Storage::GetInstance(UIYabause::getFirebaseApp(), "gs://uoyabause.appspot.com");
  StorageReference storage_ref = storage->GetReference();
  StorageReference base = storage_ref.Child(user.uid());
  StorageReference backup = base.Child("state");
  StorageReference fileref;
  fileref = backup.Child(gamecode);

  Future<Metadata> future = fileref.PutFile(sdatapath.c_str());
  future.OnCompletion(
    [](const firebase::Future<Metadata> &result, void *user_data) {
      
      UIYabause *self = (UIYabause *)user_data;

      if (result.status() == firebase::kFutureStatusComplete)
      {
        if (result.error() == firebase::storage::kErrorNone)
        {

        }
        else {
          std::cout << "Failed: " << result.error() << " " << result.error_message() << std::endl;
        }
      }
      QString datapath = getDataDirPath();
      std::string sdatapath = datapath.toStdString();
      sdatapath += "/current_state_save.bin";
      std::remove(sdatapath.c_str());
    },
    this);
}

void UIYabause::on_actionFrom_Cloud_triggered()
{
  firebase::auth::Auth *auth = firebase::auth::Auth::GetAuth(UIYabause::getFirebaseApp());
  firebase::auth::User user = auth->current_user();
  if (!user.is_valid()) {
    return;
  }

  QString datapath = getDataDirPath();
  std::string sdatapath = datapath.toStdString();
  sdatapath += "/current_state_load.bin";

  const char* gamecode = Cs2GetCurrentGmaecode();

  Storage *storage = Storage::GetInstance(UIYabause::getFirebaseApp(), "gs://uoyabause.appspot.com");
  StorageReference storage_ref = storage->GetReference();
  StorageReference base = storage_ref.Child(user.uid());
  StorageReference backup = base.Child("state");
  StorageReference fileref;
  fileref = backup.Child(gamecode);

  Future<size_t> future = fileref.GetFile(sdatapath.c_str());
  future.OnCompletion(
    [](const firebase::Future<size_t > &result, void *user_data) {
      UIYabause *self = (UIYabause *)user_data;
      if (result.status() == firebase::kFutureStatusComplete)
      {
        if (result.error() == firebase::storage::kErrorNone)
        {
           //emit self->onStateFileLoaded("cloudstate.bin");
           QMetaObject::invokeMethod(self, "onStateFileLoaded", Qt::QueuedConnection);
        }
        else {
          std::cout << "Failed: " << result.error() << " " << result.error_message() << std::endl;
        }
      }
  },
  this);
}

void UIYabause::onStateFileLoaded() {

  QString datapath = getDataDirPath();
  std::string sdatapath = datapath.toStdString();
  sdatapath += "/current_state_load.bin";

  YabauseLocker locker(mYabauseThread);
  if (YabLoadCompressedState(sdatapath.c_str()) != 0)
    CommonDialogs::information(QtYabause::translate("Couldn't load state file"));
  else
    aEmulationRun->trigger();

  std::remove(sdatapath.c_str());

}

void UIYabause::handleViewBugReportsRequested(const QString& productNumber, const QString& gameTitle)
{
	qDebug() << "Opening bug reports dialog for" << productNumber << gameTitle;

	// Check if Firebase app is initialized
	if (!app) {
		qCritical() << "Firebase App is not initialized yet!";
		QMessageBox::warning(this, "Error", "Firebase is still initializing. Please try again in a moment.");
		return;
	}

	qDebug() << "Firebase App is initialized, creating UIReportList";

	// Create and show bug reports dialog
	UIReportList* reportListDialog = new UIReportList(app, this);
	reportListDialog->setGameInfo(productNumber, gameTitle);
	reportListDialog->setAttribute(Qt::WA_DeleteOnClose); // Auto-delete when closed
	reportListDialog->show();
}
