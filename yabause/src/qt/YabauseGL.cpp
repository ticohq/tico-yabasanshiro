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
#include "YabauseGL.h"
#include "QtYabause.h"
#include "VolatileSettings.h"
#include <QWindow>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <YabauseThread.h>
#include <QCoreApplication>
#include <QImage>
#include <QTimer>
#include <stdio.h>


YabauseGL::YabauseGL( QWidget* p )
  : QOpenGLWidget(p)
{
	// Request the context on this widget only. Setting it as the
	// application-wide default format would also apply it to Qt's backingstore
	// composition context, which some drivers cannot present from - that turned
	// the whole window black on Intel UHD Graphics.
	//
	// Deliberately no setVersion()/setProfile() here. WGL and GLX return
	// exactly the version that is asked for, while this renderer needs the
	// driver's *maximum* compatibility context: its desktop shaders are GLSL
	// 400 and 430, and it resolves entry points that only exist past 4.3.
	// NVIDIA happens to hand back 4.6 whatever is asked for, which is why
	// asking for 3.2 went unnoticed; Intel honours the request literally, so
	// a 3.2 request yields GLSL 1.50 (no shader compiles) and even a 4.3
	// request leaves entry points unresolved - both crash during startup with
	// a call through a null function pointer. Leaving the version unset makes
	// the driver hand back its best compatibility context (4.6 on Intel UHD).
	QSurfaceFormat fmt = format();
	fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
	fmt.setSwapInterval(0);
	fmt.setDepthBufferSize(24);
	fmt.setStencilBufferSize(8);
	setFormat(fmt);

	setFocusPolicy( Qt::StrongFocus );
	setUpdateBehavior(QOpenGLWidget::PartialUpdate);

	if ( p ) {
		p->setFocusPolicy( Qt::StrongFocus );
		setFocusProxy( p );
	}
  viewport_width_ = 0;
  viewport_height_ = 0;
  viewport_origin_x_ = 0;
  viewport_origin_y_ = 0;
	pYabauseThread = nullptr;
}



void YabauseGL::showEvent( QShowEvent* e )
{
	// hack for clearing the the gl context
	QOpenGLWidget::showEvent( e );
	QSize s = size();
	resize( 0, 0 );
	resize( s );
}

void YabauseGL::initializeGL() {
	printf("YabauseGL::initializeGL");
	initializeOpenGLFunctions();
}

void YabauseGL::resizeGL( int w, int h )
{ 


	updateView( QSize(w, h) );
}

void YabauseGL::paintGL() {
	// Drawing
	//printf("YabauseGL::paintGL");

	if(pYabauseThread) pYabauseThread->execEmulation();

	// --vdp-dump: once the requested frames are rendered, save the widget
	// framebuffer and quit. grabFramebuffer() repaints through this function,
	// so it runs from the event loop and the replay is already finished then
	// (execEmulation() draws nothing more), leaving the last frame in place.
	static bool dumpSaveQueued = false;
	if (pYabauseThread && YabauseThread::vdpDumpMode() && pYabauseThread->vdpDumpReady()) {
		if (!dumpSaveQueued) {
			dumpSaveQueued = true;
			QTimer::singleShot(0, this, [this]() {
				int rc = 1;
				const QString out = QtYabause::volatileSettings()->value("VdpDump/Out").toString();
				if (!pYabauseThread->vdpDumpFailed() && !out.isEmpty()) {
					QImage img = grabFramebuffer();
					if (!img.isNull() && img.save(out)) {
						printf("[VDPDUMP] wrote %s (%dx%d)\n", out.toLocal8Bit().constData(),
						       img.width(), img.height());
						rc = 0;
					} else {
						printf("[VDPDUMP] failed to write %s\n", out.toLocal8Bit().constData());
					}
				}
				fflush(stdout);
				QCoreApplication::exit(rc);
			});
		}
		return;
	}
	update();
}

void YabauseGL::updateView( const QSize& s )
{
	qreal pixelRatio = this->devicePixelRatio();
	const QSize size = s.isValid() ? s : this->size();
	int logicalWidth = size.width() * pixelRatio;
	int logicalHeight = size.height() * pixelRatio;

	if (VIDCore && VIDCore->id == VIDCORE_OGL) {
		VolatileSettings* vs = QtYabause::volatileSettings();
		VideoSetSetting(VDP_SETTING_ROTATE_SCREEN, vs->value("Video/RotateScreen", false).toBool());
		int aspectRatio = QtYabause::volatileSettings()->value("Video/AspectRatio", 0).toInt();

		int full = 0;
		if (fullscreen) {
			full = 1;
		}
		else {
			full = 0;
		}

		
		viewport_width_ = logicalWidth;
		viewport_height_ = logicalHeight;
		glViewport( 0, 0, logicalWidth, logicalHeight);
  
    VIDCore->Resize(viewport_origin_x_, viewport_origin_y_, viewport_width_, viewport_height_, 1, aspectRatio);
  }
}


extern "C"{
	int YuiRevokeOGLOnThisThread(){
		// Todo: needs to imp for async rendering
		return 0;
	}

	int YuiUseOGLOnThisThread(){
		// Todo: needs to imp for async rendering
		return 0;
	}
}