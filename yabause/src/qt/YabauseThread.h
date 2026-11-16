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
#ifndef YABAUSETHREAD_H
#define YABAUSETHREAD_H

#include <QThread>
#include <QSize>
#include <QMutexLocker>

#include "QtYabause.h"

struct GLFWmonitor;
class Renderer;

class YabauseThread : public QObject
{
	Q_OBJECT
	
public:
	YabauseThread( QObject* owner = 0 );
	virtual ~YabauseThread();
	
	yabauseinit_struct* yabauseConf();
	bool emulationRunning();
	bool emulationPaused();
	inline int init() const { return mInit; }

	void OpenTray();
	void CloseTray();

  static YabauseThread * getInstance() {
    return instance;
  }

  bool IsFullscreen(void);
  void SetFullScreen(bool fullscreen);
	void execEmulation();

	void resize(int w, int h);

	// --vdp-dump developer mode (see Arguments.cpp). execEmulation() replays
	// the dump instead of running the CPU; vdpDumpReady() turns true once the
	// requested number of frames has been rendered, and the widget then saves
	// the frame and quits.
	static bool vdpDumpMode();
	bool vdpDumpReady() const { return mVdpDumpError || mVdpDumpDone >= mVdpDumpFrames; }
	bool vdpDumpFailed() const { return mVdpDumpError; }


protected:
  static YabauseThread * instance;
	yabauseinit_struct mYabauseConf;
	bool showFPS;
	QMutex mMutex;
	bool mPause;
	int mTimerId;
	int mInit;
	QByteArray mVdpDumpPrefix;
	int mVdpDumpFrames = 3;
	int mVdpDumpDone = 0;
	bool mVdpDumpError = false;
	// --sound-test-events=S,L,R developer mode (see Arguments.cpp)
	int mSoundTestFrame = 0;
	void * mSoundTestState = nullptr;
	size_t mSoundTestStateSize = 0;
	void soundTestEvents();

	void initEmulation();
	void deInitEmulation();
	void resetYabauseConf();
	void timerEvent( QTimerEvent* );

  Renderer * vulkanRenderer;
  GLFWmonitor * _monitor;
  int _wndPos[2];
  int _wndSize[2];

public slots:
	bool pauseEmulation( bool pause, bool reset, std::function<void()> preInitcallback = nullptr);
	bool resetEmulation();
	void reloadControllers();
	void reloadClock();
	void reloadSettings();

signals:
	void requestSize( const QSize& size );
	void requestFullscreen( bool fullscreen );
	void requestVolumeChange( int volume );
	void error( const QString& error, bool internal = true );
	void pause( bool paused );
	void reset();
	void toggleEmulateMouse( bool enable );
};

#endif // YABAUSETHREAD_H
