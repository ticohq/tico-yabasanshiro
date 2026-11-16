/*  Copyright 2005 Guillaume Duhamel
	Copyright 2005-2006 Theo Berkau
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

#include <QApplication>
#include <QSurfaceFormat>

#include "QtYabause.h"
#include "Settings.h"
#include "VolatileSettings.h"
#include "ui/UIYabause.h"
#include "ui/UISetupWizard.h"
#include "YabauseThread.h"
#include <QScreen>
#include <QWindow>
#include <stdio.h>
#include "services/PreferenceManager.h"
#ifndef NO_CLI
#include "Arguments.h"
#endif


#include <crtdbg.h>
int main( int argc, char** argv )
{
	//HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
	//_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

	// The OpenGL renderer's context request belongs on the render widget, not
	// in QSurfaceFormat::setDefaultFormat(): the default format is inherited by
	// every surface Qt creates, including the widget backingstore composition
	// context. Drivers that cannot present from the requested context (Intel
	// UHD Graphics) then leave the whole main window black even when OpenGL is
	// never used for emulation. The format is requested in the YabauseGL
	// constructor instead.
	//
	// The swap interval, however, has to stay here. QOpenGLWidget renders into
	// an FBO and the visible buffer swap is performed by the top-level window's
	// context, which is built from this default format - setting it on the
	// widget alone leaves presentation locked to vsync, and the frame limiter
	// and fast-forward then have no effect. Asking for no version here is what
	// keeps this safe on the drivers above.
	{
		QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
		fmt.setSwapInterval( 0 );
		QSurfaceFormat::setDefaultFormat( fmt );
	}

	// create application
	QApplication app( argc, argv );
	// init application
	app.setApplicationName( QString( "Yaba Sanshiro 2 v%1 30th Anniversary Edition" ).arg( VERSION ) );
	// init settings
	Settings::setIniInformations();

	// Perform crash recovery for bug reproduction
	{
		PreferenceManager crashRecovery(nullptr);
		if (crashRecovery.hasCrashRecoverySnapshot()) {
			qWarning() << "Crash recovery snapshot found - restoring preferences";
			QMap<QString, QVariant> restored = crashRecovery.performCrashRecovery();
			if (!restored.isEmpty()) {
				qWarning() << "Restored" << restored.size() << "preferences from crash recovery";
			}
		}
	}

#ifdef HAVE_LIBMINI18N
	// set translation file
	if ( QtYabause::setTranslationFile() == -1 )
		qWarning( "Can't set translation file" );
#endif
#ifndef NO_CLI
	Arguments::parse();
#endif
	// First-launch onboarding. It runs before the main window exists, so a
	// video core change here needs no restart.
	const bool vdpDump = YabauseThread::vdpDumpMode();
	if ( !vdpDump && UISetupWizard::shouldRun() )
	{
		UISetupWizard wizard;
		wizard.exec();
	}

	// show main window
	QtYabause::mainWindow()->setWindowTitle( app.applicationName() );
	if ( vdpDump )
	{
		// A dump replay is a background developer run: put the window on the
		// first screen that is not the primary one (the primary only when it
		// is the sole screen) before it is first shown, and show it without
		// activation so it never takes focus.
		QWidget* w = QtYabause::mainWindow();
		QScreen* primary = QGuiApplication::primaryScreen();
		QScreen* chosen = primary;
		foreach ( QScreen* s, QGuiApplication::screens() )
		{
			if ( s != primary ) { chosen = s; break; }
		}
		const QRect area = chosen->availableGeometry();
		w->setAttribute( Qt::WA_ShowWithoutActivating );
		w->setWindowState( Qt::WindowNoState );
		w->resize( 1024, 768 );
		// --vdp-dump-size=WxH: pin the emulation view (the central widget,
		// which the GL widget fills) to W x H device pixels; the main window
		// grows around it.
		const QStringList dumpSize = QtYabause::volatileSettings()->value( "VdpDump/Size" ).toString().split( 'x' );
		if ( dumpSize.size() == 2 && dumpSize[0].toInt() > 0 && dumpSize[1].toInt() > 0 )
		{
			const qreal dpr = chosen->devicePixelRatio();
			QtYabause::mainWindow()->centralWidget()->setFixedSize(
				(int)( dumpSize[0].toInt() / dpr + 0.5 ), (int)( dumpSize[1].toInt() / dpr + 0.5 ) );
		}
		w->move( area.topLeft() + QPoint( 50, 50 ) );
		printf( "[WINPLACE] screens=%d chosen=%s origin=(%d,%d) primary=%s origin=(%d,%d) on_primary=%d\n",
		        (int)QGuiApplication::screens().size(), chosen->name().toLocal8Bit().constData(),
		        area.x(), area.y(), primary->name().toLocal8Bit().constData(),
		        primary->geometry().x(), primary->geometry().y(), chosen == primary ? 1 : 0 );
		w->show();
		QScreen* now = w->windowHandle() ? w->windowHandle()->screen() : nullptr;
		printf( "[WINPLACE] window pos=(%d,%d) screen=%s\n", w->x(), w->y(),
		        now ? now->name().toLocal8Bit().constData() : "(none)" );
		fflush( stdout );
	}
	else
		QtYabause::mainWindow()->show();
	// connection
	QObject::connect( &app, SIGNAL( lastWindowClosed() ), &app, SLOT( quit() ) );
	// exec application
	int i = app.exec();

	QtYabause::closeTranslation();
	return i;
}
