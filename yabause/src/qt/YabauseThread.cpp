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
#include "YabauseThread.h"
#include "Settings.h"
#include "VolatileSettings.h"
#include "ui/UIPortManager.h"
#include "ui/UIControllerSetting.h"
#include "ui/UIYabause.h"
#include "InputPortConfig.h"

#include "../peripheral.h"
#ifdef HAVE_LIBSDL
#include "../persdljoy.h"
#endif
#include "../vdp2.h"
#include "../vdpdump.h"

#ifdef HAVE_VULKAN
#include "vulkan/VIDVulkan.h"
#include "vulkan/VIDVulkanCInterface.h"
#endif

#include <QDateTime>
#include <QStringList>
#include <QMap>
#include <QDebug>
#include <cwchar>

#include "../vulkan/Renderer.h"
Renderer* _vulkanRenderer;


extern "C" {

  int YabauseThread_IsUseBios() {
    VolatileSettings* vs = QtYabause::volatileSettings();
    if (vs->value("General/EnableEmulatedBios", false).toBool()) {
      return -1;
    }
    else {
      return 0;
    }
    return -1;
  }

  const char * YabauseThread_getBackupPath() {
    yabauseinit_struct* conf = YabauseThread::getInstance()->yabauseConf();
    return conf->buppath;
  }

  void YabauseThread_setUseBios(int use) {

    VolatileSettings* vs = QtYabause::volatileSettings();
    if (use == 0) {
      vs->setValue("General/EnableEmulatedBios",true);
    }
    else {
      vs->setValue("General/EnableEmulatedBios", false);
    }

    yabauseinit_struct* conf = YabauseThread::getInstance()->yabauseConf();
    if (vs->value("General/EnableEmulatedBios", false).toBool())
      conf->biospath = strdup("");
    else
      conf->biospath = strdup(vs->value("General/Bios", conf->biospath).toString().toLatin1().constData());

  }

  void YabauseThread_setBackupPath( const char * buf) {
    VolatileSettings* vs = QtYabause::volatileSettings();
    vs->setValue("Memory/Path", buf);
    yabauseinit_struct* conf = YabauseThread::getInstance()->yabauseConf();
    conf->buppath = strdup(vs->value("Memory/Path", conf->buppath).toString().toLatin1().constData());
  }

  void YabauseThread_resetPlaymode() {
    VolatileSettings* vs = QtYabause::volatileSettings();
    vs->setValue("General/RecordDir", "");
    yabauseinit_struct* conf = YabauseThread::getInstance()->yabauseConf();
    conf->buppath = strdup(vs->value("General/RecordDir", conf->buppath).toString().toLatin1().constData());
  }

  void YabauseThread_coldBoot() {
    YabauseThread::getInstance()->pauseEmulation(false, true);
    YabauseThread::getInstance()->reset();
  }
}

YabauseThread * YabauseThread::instance = nullptr;

YabauseThread::YabauseThread( QObject* o ) : QObject(o)
{
	mPause = true;
	mTimerId = -1;
	mInit = -1;
	memset(&mYabauseConf, 0, sizeof(mYabauseConf));
	showFPS = false;
  instance = this;
	_vulkanRenderer = new Renderer();
}

YabauseThread::~YabauseThread()
{
	deInitEmulation();
	vkQueueWaitIdle(_vulkanRenderer->GetVulkanQueue());
	vkDeviceWaitIdle(_vulkanRenderer->GetVulkanDevice());
	delete _vulkanRenderer;
}

yabauseinit_struct* YabauseThread::yabauseConf()
{
	return &mYabauseConf;
}


void YabauseThread::resize(int w, int h) {

	VolatileSettings* vs = QtYabause::volatileSettings();
	VideoSetSetting(VDP_SETTING_ROTATE_SCREEN, vs->value("Video/RotateScreen", false).toBool());
	// The per-pixel VDP2 compositor is the only supported path in this port.
	// There is no runtime toggle back to the legacy compositor anywhere in
	// yabause/src/qt; the F11 switch lives in the Vulkan standalone build's
	// DebugUI, which this port does not build.
	VideoSetSetting(VDP_SETTING_VDP2_NEW_COMPOSITE, 1);
	int aspectRatio = QtYabause::volatileSettings()->value("Video/AspectRatio", 0).toInt();

	if(VIDCore)
		VIDCore->Resize(0, 0, w, h, 0, aspectRatio);
}

#ifdef HAVE_VULKAN

#if 0
void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
  VIDCore->Resize(0, 0, width, height, 0, 0);
}


bool YabauseThread::IsFullscreen(void)
{
  auto wnd = vulkanRenderer->getWindow()->getWindowHandle();
  return glfwGetWindowMonitor(wnd) != nullptr;
}


void YabauseThread::SetFullScreen(bool fullscreen)
{

  auto wnd = vulkanRenderer->getWindow()->getWindowHandle();
  
  if (IsFullscreen() == fullscreen)
    return;

  if (fullscreen)
  {
    // backup window position and window size
    glfwGetWindowPos(wnd, &_wndPos[0], &_wndPos[1]);
    glfwGetWindowSize(wnd, &_wndSize[0], &_wndSize[1]);

    // get resolution of monitor
    const GLFWvidmode * mode = glfwGetVideoMode(glfwGetPrimaryMonitor());

    // switch to full screen
    glfwSetWindowMonitor(wnd, _monitor, 0, 0, mode->width, mode->height, 0);
  }
  else
  {
    // restore last window size and position
    glfwSetWindowMonitor(wnd, nullptr, _wndPos[0], _wndPos[1], _wndSize[0], _wndSize[1], 0);
  }

}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
  YabauseThread * instance = YabauseThread::getInstance();

  if (action == GLFW_RELEASE) {

    if (key == GLFW_KEY_F4) {
      if (instance->IsFullscreen()) {
        instance->SetFullScreen(false);
      }
      else {
        instance->SetFullScreen(true);
      }
    }

  }
}
#endif

#endif


void YabauseThread::initEmulation()
{
	reloadSettings();

  VolatileSettings* vs = QtYabause::volatileSettings();
  int vidcoretype = vs->value("Video/VideoCore", mYabauseConf.vidcoretype).toInt();

  // Pick the rendering model for this session before YabauseInit():
  // the Vulkan core renders on the dedicated VDP thread (async), while
  // the OpenGL and software cores issue their draw calls on the
  // emulation thread (the Qt GL context never leaves it), so they must
  // run synchronous. The flag must not change until the next init.
  VdpSetAsyncRendering(vidcoretype == VIDCORE_VULKAN);

#ifdef HAVE_VULKAN	
  if (vidcoretype == VIDCORE_VULKAN) {
    //int width = vs->value("Video/WinWidth", 800).toInt();
    //int height = vs->value("Video/WinHeight", 600).toInt();

    //vulkanRenderer = new Renderer();
    //auto w = vulkanRenderer->OpenWindow(width, height, "[Yaba Sanshiro Vulkan] F4: Toggle full screen mode ", nullptr);
    //VIDVulkan::getInstance()->setRenderer(vulkanRenderer);
    //glfwSetFramebufferSizeCallback(w->getWindowHandle(), framebufferResizeCallback);
    //glfwSetKeyCallback(w->getWindowHandle(), key_callback);
    //_monitor = glfwGetPrimaryMonitor();
		VIDVulkan::getInstance()->setRenderer(_vulkanRenderer);
  }
#endif	
#ifndef USE_SCSP2
	// Set before YabauseInit(), so the SCSP thread never sees them change
	// under it (ScspInit() builds the renderer when the mode is on).
	// Sound/HighQuality is the user setting (sound page, or --sound-hq). It
	// turns on the float DSP too, unless --sound-hq-float-dsp picks a mode.
	{
		const int hq = vs->value("Sound/HighQuality", false).toBool() ? 1 : 0;
		ScspSetHighQualityFloatDsp(vs->value("Sound/HighQualityFloatDsp", hq).toInt());
		ScspSetHighQuality(hq);
	}
	{
		// --sound-hq-fx-out=FILE (developer): for every sample with the float
		// DSP on, the effect returns of the chip's DSP and of the float DSP,
		// raw float32 (chip L, chip R, float L, float R).
		static FILE * fxOut = NULL;
		const QString path = vs->value("Debug/SoundHqFxOut").toString();
		if (!path.isEmpty() && fxOut == NULL) {
			fxOut = fopen(path.toLocal8Bit().constData(), "wb");
			if (fxOut) {
				struct Hook {
					static void write(const float chip[2], const float flt[2]) {
						const float v[4] = { chip[0], chip[1], flt[0], flt[1] };
						fwrite(v, sizeof(v), 1, fxOut);
					}
				};
				scsp_hq_fx_output_hook = &Hook::write;
			}
		}
	}
	{
		// --sound-hq-chip-out=FILE (developer): for every sample the chip's own
		// output and the high-quality output that replaces it, raw 16-bit
		// (chip L, chip R, hq L, hq R) after a 44-byte blank header.
		static FILE * chipOut = NULL;
		const QString path = vs->value("Debug/SoundHqChipOut").toString();
		if (!path.isEmpty() && chipOut == NULL) {
			chipOut = fopen(path.toLocal8Bit().constData(), "wb");
			if (chipOut) {
				static const char header[44] = { 0 };
				fwrite(header, 1, sizeof(header), chipOut);
				struct Hook {
					static void write(s16 chip_l, s16 chip_r, s16 hq_l, s16 hq_r) {
						const s16 v[4] = { chip_l, chip_r, hq_l, hq_r };
						fwrite(v, sizeof(v), 1, chipOut);
					}
				};
				scsp_hq_chip_output_hook = &Hook::write;
			}
		}
	}
#endif
	mInit = YabauseInit( &mYabauseConf );
	if (vdpDumpMode()) {
		// With the HLE BIOS and no disc YabauseInit() reports -2 (no game)
		// after every subsystem is initialized; the dump needs nothing more.
		if (mInit == -2)
			mInit = 0;
		mVdpDumpPrefix = vs->value("VdpDump/Prefix").toString().toLocal8Bit();
		mVdpDumpFrames = vs->value("VdpDump/Frames", 3).toInt();
		if (mVdpDumpFrames < 1)
			mVdpDumpFrames = 3;
		mVdpDumpDone = 0;
		mVdpDumpError = false;
		printf("[VDPDUMP] init rc=%d polygon_mode=%d resolution_mode=%d filter_mode=%d frames=%d\n", mInit,
		       mYabauseConf.polygon_generation_mode, mYabauseConf.resolution_mode,
		       mYabauseConf.video_filter_type, mVdpDumpFrames);
		fflush(stdout);
	}
	SetOSDToggle(showFPS && !vdpDumpMode());
}


void YabauseThread::deInitEmulation()
{
  VolatileSettings* vs = QtYabause::volatileSettings();
  int vidcoretype = vs->value("Video/VideoCore", mYabauseConf.vidcoretype).toInt();

#ifdef HAVE_VULKAN		
  if (vidcoretype == VIDCORE_VULKAN) {
   vkQueueWaitIdle(_vulkanRenderer->GetVulkanQueue());
    vkDeviceWaitIdle(_vulkanRenderer->GetVulkanDevice());
  }
#endif	

	YabauseDeInit();

	mInit = -1;
}

bool YabauseThread::pauseEmulation( bool pause, bool reset, std::function<void()> preInitcallback )
{

	VolatileSettings* vs = QtYabause::volatileSettings();
	int vidcoretype = vs->value("Video/VideoCore", mYabauseConf.vidcoretype).toInt();

#ifdef HAVE_VULKAN	
	if (vidcoretype == VIDCORE_VULKAN) {
		if (pause == true && (_vulkanRenderer == nullptr || _vulkanRenderer->GetVulkanDevice() == VK_NULL_HANDLE || _vulkanRenderer->getWindow() == nullptr) ) {
			return false;
		}
	}
#endif

	if ( mPause == pause && !reset ) {
		return true;
	}
	
	if ( mInit == 0 && reset ) {
		deInitEmulation();
	}

  if (preInitcallback != nullptr) {
    preInitcallback();
  }
	
	if ( mInit < 0 ) {
		initEmulation();
	}
	
	if ( mInit < 0 )
	{
		emit error( QtYabause::translate( "Can't initialize emulator!" ), false );
		return false;
	}
	
	mPause = pause;
	
	if ( mPause ) {
		ScspMuteAudio(SCSP_MUTE_SYSTEM);
		killTimer( mTimerId );
		mTimerId = -1;
	}
	else {
		ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
		mTimerId = startTimer( 0 );
	}
	
	

	if (vs->value("autostart").toBool())
	{
		if (vs->value("autostart/binary").toBool()) {
			MappedMemoryLoadExec(
				vs->value("autostart/binary/filename").toString().toLocal8Bit().constData(),
				vs->value("autostart/binary/address").toUInt());
		}
		else if (vs->value("autostart/load").toBool()) {
			YabLoadStateSlot( QtYabause::volatileSettings()->value( "General/SaveStates", getDataDirPath() ).toString().toLatin1().constData(), vs->value("autostart/load/slot").toInt() );
		}
		vs->setValue("autostart", false);
	}


	emit this->pause( mPause );
	
	return true;
}

bool YabauseThread::resetEmulation()
{
	if ( mInit < 0 ) {
		return false;
	}
	
	YabauseReset();
	
	emit reset();

	return true;
}

// SDL hands out device indices in connection order, and a stored binding embeds
// the index it was recorded against. Replug a pad and that index moves, so the
// binding has to be retargeted at the device the port is actually configured
// for, which is persisted by GUID.
static u32 retargetToConfiguredDevice( const QString& deviceId, u32 key )
{
#ifdef HAVE_LIBSDL
	if ( !InputPortConfig::isPhysicalDevice( deviceId ) )
		return key;
	const int index = PERSDLJoyGetDeviceIndexForId( deviceId.toLatin1().constData() );
	if ( index < 0 )
	{
		// The configured device is not plugged in. Keeping the stored code would
		// leave it pointing at whatever device now occupies that index, so the
		// port would be driven by a pad it was never configured for. Leave the
		// button unbound instead - the port is simply inert until the device
		// comes back.
		return PERKEY_UNBOUND;
	}
	return PERSDLJoyRetargetKey( key, index );
#else
	Q_UNUSED( deviceId );
	return key;
#endif
}

// Every peripheral type now stores a physical device per port, so all of them
// have to resolve their bindings through it. Reading the device once per port
// keeps the settings lookup out of the per-button loops.
static QString configuredDeviceFor( Settings* settings, uint port, const QString& id )
{
	return InputPortConfig::configuredDevice( settings, port, id.toUInt() );
}

void YabauseThread::reloadControllers()
{
	PerPortReset();
	QtYabause::clearPadsBits();
	
	Settings* settings = QtYabause::settings();

	emit toggleEmulateMouse( false );

	for ( uint port = 1; port < 3; port++ )
	{
		settings->beginGroup( QString( "Input/Port/%1/Id" ).arg( port ) );
		QStringList ids = settings->childGroups();
		settings->endGroup();

    if (port == 1 && ids.size() == 0) {
      // First run: seed port 1 so the emulator is playable with no setup. The
      // mapping depends on what is plugged in, and it is written to the
      // settings file so the configuration dialog shows it as the initial
      // value and the user can edit it from there.
      PerPad_struct* padbits = PerPadAdd(&PORTDATA1);
      InputPortConfig::SdlDeviceSource source;
      const QMap<u8, u32> defaults = InputPortConfig::seedPort(settings, source, 1, 1);
      for (QMap<u8, u32>::const_iterator it = defaults.constBegin(); it != defaults.constEnd(); ++it)
        PerSetKey(it.value(), it.key(), padbits);
      return;
    }
		
		ids.sort();
		foreach ( const QString& id, ids )
		{
			uint type = settings->value( QString( UIPortManager::mSettingsType ).arg( port ).arg( id ) ).toUInt();
			// Read once here rather than per branch: every peripheral type has a
			// device selector now, so every branch below resolves through it.
			const QString deviceId = configuredDeviceFor( settings, port, id );

			switch ( type )
			{
				case PERPAD:
				{
					PerPad_struct* padbits = PerPadAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );
					
					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList padKeys = settings->childKeys();
					settings->endGroup();
					
          padKeys.sort();
          foreach(const QString& padKey, padKeys) {
            const QString key = settings->value(QString(UIPortManager::mSettingsKey).arg(port).arg(id).arg(type).arg(padKey)).toString();
            PerSetKey(retargetToConfiguredDevice(deviceId, key.toUInt()), padKey.toUInt(), padbits);
          }
					break;
				}
				case PERWHEEL:
            {
               PerAnalog_struct* analogbits = PerWheelAdd(port == 1 ? &PORTDATA1 : &PORTDATA2);

               settings->beginGroup(QString("Input/Port/%1/Id/%2/Controller/%3/Key").arg(port).arg(id).arg(type));
               QStringList analogKeys = settings->childKeys();
               settings->endGroup();

               analogKeys.sort();
               foreach(const QString& analogKey, analogKeys)
               {
                  const QString key = settings->value(QString(UIPortManager::mSettingsKey).arg(port).arg(id).arg(type).arg(analogKey)).toString();

                  PerSetKey(retargetToConfiguredDevice(deviceId, key.toUInt()), analogKey.toUInt(), analogbits);
               }
               break;
            }
            case PERMISSIONSTICK:
            {
               PerAnalog_struct* analogbits = PerMissionStickAdd(port == 1 ? &PORTDATA1 : &PORTDATA2);

               settings->beginGroup(QString("Input/Port/%1/Id/%2/Controller/%3/Key").arg(port).arg(id).arg(type));
               QStringList analogKeys = settings->childKeys();
               settings->endGroup();

               analogKeys.sort();
               foreach(const QString& analogKey, analogKeys)
               {
                  const QString key = settings->value(QString(UIPortManager::mSettingsKey).arg(port).arg(id).arg(type).arg(analogKey)).toString();

                  PerSetKey(retargetToConfiguredDevice(deviceId, key.toUInt()), analogKey.toUInt(), analogbits);
               }
               break;
            }
            case PERTWINSTICKS:
            {
               PerAnalog_struct* analogbits = PerTwinSticksAdd(port == 1 ? &PORTDATA1 : &PORTDATA2);

               settings->beginGroup(QString("Input/Port/%1/Id/%2/Controller/%3/Key").arg(port).arg(id).arg(type));
               QStringList analogKeys = settings->childKeys();
               settings->endGroup();

               analogKeys.sort();
               foreach(const QString& analogKey, analogKeys)
               {
                  const QString key = settings->value(QString(UIPortManager::mSettingsKey).arg(port).arg(id).arg(type).arg(analogKey)).toString();

                  PerSetKey(retargetToConfiguredDevice(deviceId, key.toUInt()), analogKey.toUInt(), analogbits);
               }
               break;
            }
				case PER3DPAD:
				{
					PerAnalog_struct* analogbits = Per3DPadAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );

					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList analogKeys = settings->childKeys();
					settings->endGroup();

					analogKeys.sort();
					foreach ( const QString& analogKey, analogKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( analogKey ) ).toString();

						PerSetKey( retargetToConfiguredDevice( deviceId, key.toUInt() ), analogKey.toUInt(), analogbits );
					}
					break;
				}
				case PERGUN:
				{
					PerGun_struct* gunbits = PerGunAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );
					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList gunKeys = settings->childKeys();
					settings->endGroup();

					gunKeys.sort();
					foreach ( const QString& gunKey, gunKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( gunKey ) ).toString();

						PerSetKey( retargetToConfiguredDevice( deviceId, key.toUInt() ), gunKey.toUInt(), gunbits );
					}
					break;
				}
				case PERKEYBOARD:
					QtYabause::mainWindow()->appendLog( "Keyboard controller type is not yet supported" );
					break;
				case PERMOUSE:
				{
					PerMouse_struct* mousebits = PerMouseAdd( port == 1 ? &PORTDATA1 : &PORTDATA2 );

					settings->beginGroup( QString( "Input/Port/%1/Id/%2/Controller/%3/Key" ).arg( port ).arg( id ).arg( type ) );
					QStringList mouseKeys = settings->childKeys();
					settings->endGroup();

					mouseKeys.sort();
					foreach ( const QString& mouseKey, mouseKeys )
					{
						const QString key = settings->value( QString( UIPortManager::mSettingsKey ).arg( port ).arg( id ).arg( type ).arg( mouseKey ) ).toString();

						PerSetKey( retargetToConfiguredDevice( deviceId, key.toUInt() ), mouseKey.toUInt(), mousebits );
					}

					emit toggleEmulateMouse( true );
					break;
				}
				case 0:
					// Unconnected
					break;
				default:
					QtYabause::mainWindow()->appendLog( "Invalid controller type" );
					break;
			}
		}
	}
}

static struct tm *localtime_qt(const QDateTime &input, struct tm *result)
{
	QDate date(input.date());
	result->tm_year = date.year() - 1900;
	result->tm_mon = date.month();
	result->tm_mday = date.day();
	result->tm_wday = date.dayOfWeek();
	result->tm_yday = date.dayOfYear();

	QTime time(input.time());
	result->tm_sec = time.second();
	result->tm_min = time.minute();
	result->tm_hour = time.hour();

	return result;
}

void YabauseThread::reloadClock()
{
	QString tmp;
	Settings* s = QtYabause::settings();

	if (mYabauseConf.basetime == 0)
		tmp = "";
	else
		tmp = QDateTime::fromSecsSinceEpoch(mYabauseConf.basetime).toString();

	// Clock sync
	mYabauseConf.clocksync = (int)s->value("General/ClockSync", mYabauseConf.clocksync).toBool();
	tmp = s->value("General/FixedBaseTime", tmp).toString();
	if (!tmp.isEmpty() && mYabauseConf.clocksync)
	{
		QDateTime date = QDateTime::fromString(tmp, Qt::ISODate);
		mYabauseConf.basetime = static_cast<long>(date.toSecsSinceEpoch());
	}
	else
	{
		mYabauseConf.basetime = 0;
	}
}

void YabauseThread::reloadSettings()
{
	//QMutexLocker l( &mMutex );
	// get settings pointer
	VolatileSettings* vs = QtYabause::volatileSettings();
	
	// reset yabause conf
	resetYabauseConf();

	// read & apply settings
   mYabauseConf.m68kcoretype = vs->value("Advanced/68kCore", mYabauseConf.m68kcoretype).toInt();
	mYabauseConf.percoretype = vs->value( "Input/PerCore", mYabauseConf.percoretype ).toInt();
	// Same self-healing as the sound core below: the DirectInput peripheral core
	// was removed, so an ini written before that still says PerCore=2. PerInit()
	// returns -1 for an id it cannot find, which fails the whole YabauseInit().
	if ( !QtYabause::getPERCore( mYabauseConf.percoretype ) )
		mYabauseConf.percoretype = QtYabause::defaultPERCore().id;
	mYabauseConf.sh2coretype = vs->value( "Advanced/SH2Interpreter", mYabauseConf.sh2coretype ).toInt();
	mYabauseConf.vidcoretype = vs->value( "Video/VideoCore", mYabauseConf.vidcoretype ).toInt();

  // Automatically set OSDCore based on VideoCore
	switch (mYabauseConf.vidcoretype) {
  case VIDCORE_SOFT:
		mYabauseConf.osdcoretype = OSDCORE_SOFT;
    break;
  case VIDCORE_OGL:
		mYabauseConf.osdcoretype = OSDCORE_NANOVG;
    break;
#ifdef HAVE_VULKAN
  case VIDCORE_VULKAN:
		mYabauseConf.osdcoretype = OSDCORE_NANOVG_VULKAN;
    break;
#endif
  default:
		mYabauseConf.osdcoretype = OSDCORE_DUMMY;
    break;

	}
	mYabauseConf.sndcoretype = vs->value( "Sound/SoundCore", mYabauseConf.sndcoretype ).toInt();
	// A settings file can name a sound core this build no longer has - the
	// DirectSound backend was removed, so every ini written before that still
	// says SoundCore=2. Passing an unknown id to ScspInit() fails the whole
	// YabauseInit(), so validate it here and fall back to the default instead.
	if ( !QtYabause::getSNDCore( mYabauseConf.sndcoretype ) )
		mYabauseConf.sndcoretype = QtYabause::defaultSNDCore().id;
	mYabauseConf.cdcoretype = vs->value( "General/CdRom", mYabauseConf.cdcoretype ).toInt();
	mYabauseConf.carttype = vs->value( "Cartridge/Type", mYabauseConf.carttype ).toInt();
	const QString r = vs->value( "Advanced/Region", mYabauseConf.regionid ).toString();
	if ( r.isEmpty() || r == "Auto" )
		mYabauseConf.regionid = 0;
	else
	{
		switch ( r[0].toLatin1() )
		{
			case 'J': mYabauseConf.regionid = 1; break;
			case 'T': mYabauseConf.regionid = 2; break;
			case 'U': mYabauseConf.regionid = 4; break;
			case 'B': mYabauseConf.regionid = 5; break;
			case 'K': mYabauseConf.regionid = 6; break;
			case 'A': mYabauseConf.regionid = 0xA; break;
			case 'E': mYabauseConf.regionid = 0xC; break;
			case 'L': mYabauseConf.regionid = 0xD; break;
		}
	}
	if (vs->value("General/EnableEmulatedBios", false).toBool())
		mYabauseConf.biospath = strdup( "" );
	else
		mYabauseConf.biospath = strdup( vs->value( "General/Bios", mYabauseConf.biospath ).toString().toUtf8().constData() );

  mYabauseConf.framelimit = vs->value("General/EmulationSpeed", mYabauseConf.framelimit).toInt();
	mYabauseConf.cdpath = strdup( vs->value( "General/CdRomISO", mYabauseConf.cdpath ).toString().toUtf8().constData() );
   mYabauseConf.ssfpath = strdup(vs->value("General/SSFPath", mYabauseConf.ssfpath).toString().toUtf8().constData());
   mYabauseConf.play_ssf = vs->value("General/PlaySSF", false).toBool();
   showFPS = vs->value( "General/ShowFPS", false ).toBool();
	mYabauseConf.usethreads = (int)vs->value( "General/EnableMultiThreading", mYabauseConf.usethreads ).toBool();
	mYabauseConf.numthreads = vs->value( "General/NumThreads", mYabauseConf.numthreads ).toInt();
	mYabauseConf.buppath = strdup( vs->value( "Memory/Path", mYabauseConf.buppath ).toString().toUtf8().constData() );
	mYabauseConf.mpegpath = strdup( vs->value( "MpegROM/Path", mYabauseConf.mpegpath ).toString().toUtf8().constData() );
  if (vs->value("Memory/ExtendMemory", true).toBool()) {
    mYabauseConf.extend_backup = 1;
  }else {
    mYabauseConf.extend_backup = 0;
  }
	mYabauseConf.cartpath = strdup( vs->value( "Cartridge/Path", mYabauseConf.cartpath ).toString().toUtf8().constData() );
	mYabauseConf.modemip = strdup( vs->value( "Cartridge/ModemIP", mYabauseConf.modemip ).toString().toUtf8().constData() );
	mYabauseConf.modemport = strdup( vs->value( "Cartridge/ModemPort", mYabauseConf.modemport ).toString().toUtf8().constData() );
	// The video format is decided by SMPC from the detected region (see
	// smpc.c), so there is nothing for the user to choose here.
	mYabauseConf.videoformattype = VIDEOFORMATTYPE_NTSC;
   mYabauseConf.use_new_scsp = (int)vs->value("Sound/NewScsp", mYabauseConf.use_new_scsp).toBool();

	// Output filter (AAMODE in ygl.h): None, FXAA, scanline, bilinear and the
	// CRT shaders. The "Filter" combo box of the video settings page stores it.
	mYabauseConf.video_filter_type = vs->value("Video/filter_mode", 0).toInt();
	mYabauseConf.polygon_generation_mode = vs->value("Video/polygon_generation_mode", mYabauseConf.polygon_generation_mode).toInt();
  mYabauseConf.resolution_mode = vs->value("Video/resolution_mode", mYabauseConf.resolution_mode).toInt();
  mYabauseConf.rbg_resolution_mode = vs->value("Video/rbg_resolution_mode", mYabauseConf.rbg_resolution_mode).toInt();
  mYabauseConf.rbg_use_compute_shader = vs->value("Video/UseComputeShader", mYabauseConf.rbg_use_compute_shader).toBool();

	emit requestSize( QSize( vs->value( "Video/WinWidth", 0 ).toInt(), vs->value( "Video/WinHeight", 0 ).toInt() ) );
	emit requestFullscreen( vs->value( "Video/Fullscreen", false ).toBool() );
	emit requestVolumeChange( vs->value( "Sound/Volume", 100 ).toInt() );


  mYabauseConf.rotate_screen = vs->value("Video/RotateScreen", false).toBool() ;
  mYabauseConf.scsp_sync_count_per_frame = vs->value("Sound/ScspSync", 1).toInt();
  mYabauseConf.scsp_main_mode = vs->value("Sound/ScspMainMode", 1).toInt();

  mYabauseConf.playRecordPath = strdup(vs->value("General/RecordDir", mYabauseConf.playRecordPath).toString().toLatin1().constData());


  mYabauseConf.use_sh2_cache = vs->value("General/UseSh2Cache", true).toBool()?1:0 ;

	reloadClock();
	reloadControllers();
}

bool YabauseThread::emulationRunning()
{
	//QMutexLocker l( &mMutex );
	return mTimerId != -1 && !mPause;
}

bool YabauseThread::emulationPaused()
{
	//QMutexLocker l( &mMutex );
	return mPause;
}

void YabauseThread::OpenTray(){
	Cs2ForceOpenTray();
}

void YabauseThread::CloseTray(){

	VolatileSettings* vs = QtYabause::volatileSettings();
	mYabauseConf.cdcoretype = vs->value("General/CdRom", mYabauseConf.cdcoretype).toInt();
	mYabauseConf.cdpath = strdup(vs->value("General/CdRomISO", mYabauseConf.cdpath).toString().toUtf8().constData());

	Cs2ForceCloseTray(mYabauseConf.cdcoretype, mYabauseConf.cdpath);
}

void YabauseThread::resetYabauseConf()
{
	// free structure
	memset( &mYabauseConf, 0, sizeof( yabauseinit_struct ) );
	// fill default structure
  mYabauseConf.m68kcoretype = QtYabause::default68kCore().id;
	mYabauseConf.percoretype = QtYabause::defaultPERCore().id;
  mYabauseConf.sh2coretype = QtYabause::defaultCpuCore()->id;
	mYabauseConf.vidcoretype = QtYabause::defaultVIDCore().id;
	mYabauseConf.sndcoretype = QtYabause::defaultSNDCore().id;
	mYabauseConf.cdcoretype = QtYabause::defaultCDCore().id;
	mYabauseConf.carttype = CART_NONE;
	mYabauseConf.regionid = 0;
	mYabauseConf.biospath = 0;
	mYabauseConf.cdpath = 0;
	mYabauseConf.mpegpath = 0;
	mYabauseConf.cartpath = 0;
	mYabauseConf.videoformattype = VIDEOFORMATTYPE_NTSC;
	mYabauseConf.skip_load = 0;
	int numThreads = QThread::idealThreadCount();	
	mYabauseConf.usethreads = numThreads <= 1 ? 0 : 1;
	mYabauseConf.numthreads = numThreads < 0 ? 1 : numThreads;
	mYabauseConf.video_filter_type = 0;
	mYabauseConf.polygon_generation_mode = 0;
  mYabauseConf.resolution_mode = 0;
  mYabauseConf.framelimit = 0;
  mYabauseConf.rbg_resolution_mode = 0;
  mYabauseConf.rbg_use_compute_shader = 0;
  mYabauseConf.rotate_screen = 0;
  mYabauseConf.scsp_sync_count_per_frame = 1;
  mYabauseConf.scsp_main_mode = 1;
  mYabauseConf.use_new_scsp = 1;
  mYabauseConf.buppath = strdup(getDataDirPath().append("/bkram.bin").toLatin1().constData());
  mYabauseConf.playRecordPath = NULL;
}

void YabauseThread::timerEvent( QTimerEvent* )
{
	//mPause = true;
	//mRunning = true;
	//while ( mRunning )
	{
		//if ( !mPause )
		//	PERCore->HandleEvents();
		//else
			//msleep( 25 );
		//sleep( 0 );
	}
}


bool YabauseThread::vdpDumpMode() {
	// The prefix is set once from the command line before the window exists;
	// cache it so paintGL does not query the settings every frame.
	static const bool mode =
		!QtYabause::volatileSettings()->value("VdpDump/Prefix").toString().isEmpty();
	return mode;
}

void YabauseThread::execEmulation() {
	if (mPause)
		return;
	if (!mVdpDumpPrefix.isEmpty()) {
		// Developer mode: replay the dump every frame; no CPU runs.
		if (vdpDumpReady())
			return;
		// --vdp-dump-filters=a,b,c: the filter of frame 1, 2, 3 (the last one
		// stays), switched at run time the way the settings dialog does.
		const QStringList filters = QtYabause::volatileSettings()->value("VdpDump/Filters").toString().split(',', Qt::SkipEmptyParts);
		if (!filters.isEmpty()) {
			const int f = filters.at(qMin(mVdpDumpDone, (int)filters.size() - 1)).toInt();
			VideoSetSetting(VDP_SETTING_FILTERMODE, f);
			printf("[VDPDUMP] frame %d filter %d\n", mVdpDumpDone + 1, f);
		}
		if (VdpDumpRunFrame(mVdpDumpPrefix.constData()) != 0) {
			printf("[VDPDUMP] cannot load dump %s\n", mVdpDumpPrefix.constData());
			mVdpDumpError = true;
		} else {
			++mVdpDumpDone;
		}
		fflush(stdout);
		return;
	}
	PERCore->HandleEvents();
	soundTestEvents();
}

// Developer mode for checking the sound output across a state load and a
// reset: --sound-test-events=S,L,R saves a state to memory after emulated
// frame S, loads it back after frame L and resets after frame R (0 = never).
// An optional fourth value K prints the SCSP high-quality key-on statistics
// after frame K.
void YabauseThread::soundTestEvents()
{
	static const QStringList ev =
		QtYabause::volatileSettings()->value("Debug/SoundTestEvents").toString().split(',');
	if (ev.size() != 3 && ev.size() != 4)
		return;
	++mSoundTestFrame;
	const int f = mSoundTestFrame;
	if (f == ev[0].toInt()) {
		free(mSoundTestState);
		mSoundTestState = nullptr;
		mSoundTestStateSize = 0;
		const int rc = YabSaveStateBuffer(&mSoundTestState, &mSoundTestStateSize);
		printf("[SNDTEST] frame %d save rc=%d size=%u\n", f, rc, (unsigned)mSoundTestStateSize);
	}
	if (f == ev[1].toInt() && mSoundTestState) {
		const int rc = YabLoadStateBuffer(mSoundTestState, mSoundTestStateSize);
		printf("[SNDTEST] frame %d load rc=%d\n", f, rc);
	}
	if (f == ev[2].toInt()) {
		YabauseReset();
		printf("[SNDTEST] frame %d reset\n", f);
	}
#ifndef USE_SCSP2
	if (ev.size() == 4 && f == ev[3].toInt()) {
		u32 calls[32], taps[32], zero[32];
		ScspHqGetKeyOnStats(calls, taps, zero);
		for (int i = 0; i < 32; i++)
			printf("[SNDTEST] frame %d slot %2d keyon() %u, tap key_on %u, op2 sample_counter==0 %u\n",
			       f, i, calls[i], taps[i], zero[i]);
		printf("[SNDTEST] frame %d float DSP seeds %u\n", f, ScspHqGetFloatDspSeedCount());
	}
#endif
	fflush(stdout);
}