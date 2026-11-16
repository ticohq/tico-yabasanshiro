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
#include "UISettings.h"
#include "../Settings.h"
#include "../CommonDialogs.h"
#include "UIPortManager.h"

#include <QDir>
#include <QList>
//#include <QDesktopWidget>

extern "C" {
extern M68K_struct* M68KCoreList[];
extern SH2Interface_struct* SH2CoreList[];
extern PerInterface_struct* PERCoreList[];
extern CDInterface* CDCoreList[];
extern SoundInterface_struct* SNDCoreList[];
extern VideoInterface_struct* VIDCoreList[];
extern OSD_struct* OSDCoreList[];
}

struct Item
{
	Item( const QString& i, const QString& n, bool e=true, bool s=true, bool z=false)
	{ id = i; Name = n; enableFlag = e; saveFlag = s; ipFlag = z; }
	
	QString id;
	QString Name;
	bool enableFlag;
	bool saveFlag;
	bool ipFlag;
};

typedef QList<Item> Items;

const Items mRegions = Items()
	<< Item( "Auto" , "Auto-detect" )
	<< Item( "J" , "Japan (NTSC)" )
	<< Item( "T", "Asia (NTSC)" )
	<< Item( "U", "North America (NTSC)" )
	<< Item( "B", "Central/South America (NTSC)" )
	<< Item( "K", "Korea (NTSC)" )
	<< Item( "A", "Asia (PAL)" )
	<< Item( "E", "Europe + others (PAL)" )
	<< Item( "L", "Central/South America (PAL)" );

const Items mCartridgeTypes = Items()
	<< Item( "0", "None", false, false )
	<< Item( "1", "Pro Action Replay", true, false )
	<< Item( "2", "4 Mbit Backup Ram", true, true )
	<< Item( "3", "8 Mbit Backup Ram", true, true )
	<< Item( "4", "16 Mbit Backup Ram", true, true )
	<< Item( "5", "32 Mbit Backup Ram", true, true )
	<< Item( "6", "8 Mbit Dram", false, false )
	<< Item( "7", "32 Mbit Dram", false, false )
	<< Item( "8", "Netlink", false, false, true )
	<< Item( "9", "16 Mbit ROM", true, false )
	<< Item( "10", "Japanese Modem", false, false, true );

const Items mPolygonGenerationMode = Items()
	<< Item("0", "Triangles using perspective correction")
	<< Item("1", "CPU Tessellation")
	<< Item("2", "GPU Tessellation")
	<< Item("3", "Compute Rasterizer (Experimental)");

const Items mResolutionMode = Items()
<< Item("0", "Native (native resolution of Window)")
<< Item("1", "4x")
<< Item("2", "2x")
<< Item("3", "Original")
<< Item("4", "720P")
<< Item("5", "1080P");

const Items mRbgResolutionMode = Items()
<< Item("0", "Original")
<< Item("1", "2x")
<< Item("5", "4x")
<< Item("2", "720P")
<< Item("3", "1080P")
<< Item("4", "Fit to Emulation Resolution");

// Output filter; the ids are the AAMODE values of ygl.h
const Items mFilterMode = Items()
<< Item("0", "None")
<< Item("1", "FXAA")
<< Item("2", "Scanline")
<< Item("3", "Bilinear")
<< Item("4", "CRT Hyllian")
<< Item("5", "CRT Lottes")
<< Item("6", "CRT NewPixie");

const Items mEmulationSpeed = Items()
<< Item("0", "1x")
<< Item("2", "2x")
<< Item("1", "Unlimited");

UISettings::UISettings( QList <supportedRes_struct> *supportedResolutions, QList <translation_struct> *translations, QWidget* p )
	: QDialog( p )
{
	// setup dialog
	setupUi( this );

	QRect maxWinRect = QGuiApplication::primaryScreen()->availableGeometry();
	
	
	//leWinWidth->setValidator(new QIntValidator(0, maxWinRect.width(), leWinWidth));
	//leWinHeight->setValidator(new QIntValidator(0, maxWinRect.height(), leWinHeight));

	// IP�A�h���X�̐��K�\���p�^�[��
	QString ipNum = "(25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])";
	QRegularExpression ipRegex("^" + ipNum + "\\." + ipNum + "\\." + ipNum + "\\." + ipNum + "$");
	leCartridgeModemPort->setValidator(new QIntValidator(1, 65535, leCartridgeModemPort));

	cbPolygonGeneration->blockSignals(true);
	cbVideoCore->blockSignals(true);

	pmPort1->setPort( 1 );
	pmPort1->loadSettings();
	pmPort2->setPort( 2 );
	pmPort2->loadSettings();

	if ( p && !p->isFullScreen() )
	{
		setWindowFlags( Qt::Sheet );
	}

	// NOTE: CD-ROM drive enumeration (setupCdDrives) is deferred. It is only
	// needed for the real-CD-drive playback core (CDCORE_ARCH) and is populated
	// lazily from on_cbCdRom_currentIndexChanged / loadSettings when that core is
	// selected. Enumerating drives here blocked dialog construction for seconds.

	// load cores informations
	loadCores();

	supportedRes = *supportedResolutions;
	loadSupportedResolutions();

#ifdef HAVE_LIBMINI18N
	trans = *translations;
	loadTranslations();
#else
   lTranslation->hide();
	cbTranslation->hide();
#endif

	loadShortcuts();

	// load settings
	loadSettings();

	// connections
	foreach ( QToolButton* tb, findChildren<QToolButton*>() )
	{
		connect( tb, SIGNAL( clicked() ), this, SLOT( tbBrowse_clicked() ) );
	}

	// retranslate widgets
	QtYabause::retranslateWidget( this );

	cbVideoCore->blockSignals(false);
	cbPolygonGeneration->blockSignals(false);

}

void UISettings::requestFile( const QString& c, QLineEdit* e, const QString& filters )
{
	const QString s = CommonDialogs::getOpenFileName( e->text(), c, filters );
	if ( !s.isNull() )
		e->setText( s );
}

void UISettings::requestNewFile( const QString& c, QLineEdit* e, const QString& filters )
{
	const QString s = CommonDialogs::getSaveFileName( e->text(), c, filters );
	if ( !s.isNull() )
		e->setText( s );
}

void UISettings::requestFolder( const QString& c, QLineEdit* e )
{
	const QString s = CommonDialogs::getExistingDirectory( e->text(), c );
	if ( !s.isNull() )
		e->setText( s );
}

QStringList getCdDriveList()
{
	QStringList list;

#if defined Q_OS_WIN
	// Enumerate CD-ROM drives with GetLogicalDrives() directly. QDir::drives()
	// stats every drive root (GetFileAttributes on "X:\"), which blocks for
	// seconds per empty optical or disconnected network drive. GetDriveTypeW()
	// reports the drive type from cached info without touching the media, so it
	// stays fast.
	DWORD driveMask = GetLogicalDrives();
	for (int i = 0; i < 26; ++i)
	{
		if (!(driveMask & (1u << i)))
			continue;
		wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', 0 };
		if (GetDriveTypeW(root) == DRIVE_CDROM)
			list.append(QString("%1:/").arg(QChar(QLatin1Char('A' + i))));
	}
#elif defined Q_OS_LINUX
	FILE * f = fopen("/proc/sys/dev/cdrom/info", "r");
	char buffer[1024];
	char drive_name[10];
	char drive_path[255];

	if (f != NULL) {
		while (fgets(buffer, 1024, f) != NULL) {
			if (sscanf(buffer, "drive name:%s", drive_name) == 1) {
				sprintf(drive_path, "/dev/%s", drive_name);

				list.append(drive_path);
			}
		}
		fclose(f);
	}
#elif defined Q_OS_MAC
#endif
	return list;
}

void UISettings::setupCdDrives()
{
	QStringList list=getCdDriveList();
	foreach(QString string, list)
		cbCdDrive->addItem(string);
}

void UISettings::on_leBios_textChanged(const QString & text)
{
	if (QFileInfo(text).exists())
		cbEnableBiosEmulation->setEnabled(true);
	else
		cbEnableBiosEmulation->setEnabled(false);
}

void UISettings::tbBrowse_clicked()
{
	// get toolbutton sender
	QToolButton* tb = qobject_cast<QToolButton*>( sender() );
	
	if ( tb == tbBios )
		requestFile( QtYabause::translate( "Choose a bios file" ), leBios );
	else if ( tb == tbCdRom )
	{
		if ( cbCdRom->currentText().contains( "dummy", Qt::CaseInsensitive ) )
		{
			CommonDialogs::information( QtYabause::translate( "The dummies cores don't need configuration." ) );
			return;
		}
		else if ( cbCdRom->currentText().contains( "iso", Qt::CaseInsensitive ) )
			requestFile( QtYabause::translate( "Select your iso/cue/bin file" ), leCdRom, QtYabause::translate( "CD Images (*.iso *.cue *.bin *.mds *.ccd)" ) );
		else
			requestFolder( QtYabause::translate( "Choose a cdrom drive/mount point" ), leCdRom );
	}
	else if ( tb == tbSaveStates )
		requestFolder( QtYabause::translate( "Choose a folder to store save states" ), leSaveStates );
	else if ( tb == tbCartridge )
	{
		if (mCartridgeTypes[cbCartridge->currentIndex()].saveFlag)
			requestNewFile( QtYabause::translate( "Choose a cartridge file" ), leCartridge );
		else
			requestFile( QtYabause::translate( "Choose a cartridge file" ), leCartridge );		    
	}
	else if ( tb == tbMemory )
		requestNewFile( QtYabause::translate( "Choose a memory file" ), leMemory );
	else if ( tb == tbMpegROM )
		requestFile( QtYabause::translate( "Choose a mpeg rom" ), leMpegROM );
}

void UISettings::on_cbInput_currentIndexChanged( int id )
{
	PerInterface_struct* core = QtYabause::getPERCore( cbInput->itemData( id ).toInt() );

	// getPERCore() returns null for an id that is not in the list, so this has
	// to be checked before Init() is called through it, not after.
	if ( !core )
		core = QtYabause::getPERCore( QtYabause::defaultPERCore().id );
	Q_ASSERT( core );

	core->Init();

	pmPort1->setCore( core );
	pmPort2->setCore( core );
}

void UISettings::on_cbCdRom_currentIndexChanged( int id )
{
	CDInterface* core = QtYabause::getCDCore( cbCdRom->itemData( id ).toInt() );

	Q_ASSERT( core );

	switch (core->id)
	{
		case CDCORE_DUMMY:
			cbCdDrive->setVisible(false);
			leCdRom->setVisible(false);
			tbCdRom->setVisible(false);
			break;
		case CDCORE_ISO:
			cbCdDrive->setVisible(false);
			leCdRom->setVisible(true);
			tbCdRom->setVisible(true);
			break;
		case CDCORE_ARCH:
			// Populate the CD-drive list lazily: only the real-CD-drive core needs it.
			if (cbCdDrive->count() == 0)
				setupCdDrives();
			cbCdDrive->setVisible(true);
			leCdRom->setVisible(false);
			tbCdRom->setVisible(false);
			break;
		default: break;
	}
}

void UISettings::on_cbClockSync_stateChanged( int state )
{
	dteBaseTime->setVisible( state == Qt::Checked );
}

void UISettings::on_cbCartridge_currentIndexChanged( int id )
{
	leCartridge->setVisible(mCartridgeTypes[id].enableFlag);
	tbCartridge->setVisible(mCartridgeTypes[id].enableFlag);
	lCartridgeModemIP->setVisible(mCartridgeTypes[id].ipFlag);
	leCartridgeModemIP->setVisible(mCartridgeTypes[id].ipFlag);
	lCartridgeModemPort->setVisible(mCartridgeTypes[id].ipFlag);
	leCartridgeModemPort->setVisible(mCartridgeTypes[id].ipFlag);
}

#include <QProcess>

void UISettings::checkRestart(std::function<void()> cancelCallBack) {

	// �ċN�����m�F����_�C�A���O��\��
	QMessageBox::StandardButton reply = QMessageBox::question(
		this,
		QtYabause::translate("Restart Required"),
		QtYabause::translate("To apply this change, the application needs to restart.\nDo you want to restart now?"),
		QMessageBox::Yes | QMessageBox::Cancel
	);

	if (reply == QMessageBox::Yes) {
		// �ݒ��ۑ����� (���� saveVideoCoreId(id) ���g�p����Ɖ���)
		saveSettings();

		// �ċN������
		QString program = QCoreApplication::applicationFilePath();
		QStringList arguments = QCoreApplication::arguments();
		arguments.removeFirst(); // �A�v���̃p�X�͏��O
		QProcess::startDetached(program, arguments); // �V�����v���Z�X���J�n
		QCoreApplication::quit(); // ���݂̃A�v���P�[�V�������I��
	}
	else if (reply == QMessageBox::Cancel) {
		// �ݒ�����ɖ߂�
		//cbVideoCore->setCurrentIndex(currentId);
		cancelCallBack();
	}


}

void UISettings::on_cbPolygonGeneration_currentIndexChanged(int id) {

	Settings* s = QtYabause::settings();

	// ���݂̐ݒ�ID���擾�i���� getCurrentVideoCoreId() �Ō��݂̐ݒ���擾������̂Ƃ���j
	int currentId = cbPolygonGeneration->findData(s->value("Video/polygon_generation_mode", mPolygonGenerationMode.at(2).id).toInt());

	// �ݒ肪�ύX����Ă��Ȃ��ꍇ�͉������Ȃ�
	if (id == currentId) {
		return;
	}

	checkRestart([&] {
		cbPolygonGeneration->setCurrentIndex(currentId);
	});

}

void UISettings::on_cbVideoCore_currentIndexChanged(int id) {

	Settings* s = QtYabause::settings();

	// ���݂̐ݒ�ID���擾�i���� getCurrentVideoCoreId() �Ō��݂̐ݒ���擾������̂Ƃ���j
	int currentId = cbVideoCore->findData(s->value("Video/VideoCore", QtYabause::defaultVIDCore().id).toInt());

	// �ݒ肪�ύX����Ă��Ȃ��ꍇ�͉������Ȃ�
	if (id == currentId) {
		return;
	}

	checkRestart([&] {
		cbVideoCore->setCurrentIndex(currentId);
	});

#if 0
	// �ċN�����m�F����_�C�A���O��\��
	QMessageBox::StandardButton reply = QMessageBox::question(
		this,
		QtYabause::translate("Restart Required"),
		QtYabause::translate("To apply this change, the application needs to restart.\nDo you want to restart now?"),
		QMessageBox::Yes | QMessageBox::Cancel
	);

	if (reply == QMessageBox::Yes) {
		// �ݒ��ۑ����� (���� saveVideoCoreId(id) ���g�p����Ɖ���)
		saveSettings();

		// �ċN������
		QString program = QCoreApplication::applicationFilePath();
		QStringList arguments = QCoreApplication::arguments();
		arguments.removeFirst(); // �A�v���̃p�X�͏��O
		QProcess::startDetached(program, arguments); // �V�����v���Z�X���J�n
		QCoreApplication::quit(); // ���݂̃A�v���P�[�V�������I��
	}
	else if (reply == QMessageBox::Cancel) {
		// �ݒ�����ɖ߂�
		cbVideoCore->setCurrentIndex(currentId);
	}
#endif

}

void UISettings::loadCores()
{
	// CD Drivers
	for ( int i = 0; CDCoreList[i] != NULL; i++ )
		cbCdRom->addItem( QtYabause::translate( CDCoreList[i]->Name ), CDCoreList[i]->id );
	
	// VDI Drivers
	for ( int i = 0; VIDCoreList[i] != NULL; i++ )
		cbVideoCore->addItem( QtYabause::translate( VIDCoreList[i]->Name ), VIDCoreList[i]->id );

#if YAB_PORT_OSD
	// OSD Drivers
//	for ( int i = 0; OSDCoreList[i] != NULL; i++ )
//		cbOSDCore->addItem( QtYabause::translate( OSDCoreList[i]->Name ), OSDCoreList[i]->id );
#else
	delete cbOSDCore;
	delete lOSDCore;
#endif
	
  // EmulationSpeed
  foreach(const Item& it, mEmulationSpeed)
    cbEmulationSpeed->addItem(QtYabause::translate(it.Name), it.id);

	// Polygon Generation
	foreach(const Item& it, mPolygonGenerationMode)
		cbPolygonGeneration->addItem(QtYabause::translate(it.Name), it.id);

  // Resolution
  foreach(const Item& it, mResolutionMode)
    cbResolution->addItem(QtYabause::translate(it.Name), it.id);

  // RbgResolution
  foreach(const Item& it, mRbgResolutionMode)
    cbRBGREsolution->addItem(QtYabause::translate(it.Name), it.id);

  // Filter
  foreach(const Item& it, mFilterMode)
    cbFilter->addItem(QtYabause::translate(it.Name), it.id);

	// SND Drivers
	for ( int i = 0; SNDCoreList[i] != NULL; i++ )
		if ( SNDCoreList[i]->id != SNDCORE_WAV )  // developer core (--sound-wav=FILE)
			cbSoundCore->addItem( QtYabause::translate( SNDCoreList[i]->Name ), SNDCoreList[i]->id );
	
	// Cartridge Types
	foreach ( const Item& it, mCartridgeTypes )
		cbCartridge->addItem( QtYabause::translate( it.Name ), it.id );
	
	// Input Drivers. There is a single real core left, so the combo has nothing
	// to offer: hide it rather than show a one-entry list. It is still filled in
	// because the rest of the dialog reads the selected core from it.
	for ( int i = 0; PERCoreList[i] != NULL; i++ )
		cbInput->addItem( QtYabause::translate( PERCoreList[i]->Name ), PERCoreList[i]->id );
	cbInput->setVisible( false );
	
	// Regions
	foreach ( const Item& it, mRegions )
		cbRegion->addItem( QtYabause::translate( it.Name ), it.id );
	
	// SH2 Interpreters
	for ( int i = 0; SH2CoreList[i] != NULL; i++ )
		cbSH2Interpreter->addItem( QtYabause::translate( SH2CoreList[i]->Name ), SH2CoreList[i]->id );

   //68k cores
   for (int i = 0; M68KCoreList[i] != NULL; i++)
      cb68kCore->addItem(QtYabause::translate(M68KCoreList[i]->Name), M68KCoreList[i]->id);

  cbAspectRatio->addItem(QtYabause::translate("Original aspect ratio"), 0);
	cbAspectRatio->addItem( QtYabause::translate( "Fixed aspect ratio: 4:3" ), 1 );
	cbAspectRatio->addItem( QtYabause::translate( "Fixed aspect ratio: 16:9" ), 2 );
  cbAspectRatio->addItem(QtYabause::translate("Fit to window"), 3);
}

void UISettings::loadSupportedResolutions()
{
	if (supportedRes.count() == 0)
	{
		lFullScreenResolution->setVisible(false);
		cbFullscreenResolution->setVisible(false);
		return;
	}

	// Load supported screen resolutions
	for (int i = 0; i < this->supportedRes.count(); i++)
	{
		QString text = QString("%1x%2").arg(QString::number(this->supportedRes[i].width), 
			QString::number (this->supportedRes[i].height));
		if (cbFullscreenResolution->findText(text) != -1)
			continue;
		cbFullscreenResolution->addItem(text, i);
	}
}

void UISettings::loadTranslations()
{
	cbTranslation->addItem(QString::fromUtf8(_("Use System Locale")), "");
	cbTranslation->addItem("English", "#");
	for (int i = 0; i < this->trans.count(); i++)
		cbTranslation->addItem(trans[i].name.left(1).toUpper()+trans[i].name.mid(1), trans[i].file);

}

void UISettings::loadShortcuts()
{	
	QList<QAction *> actions = parent()->findChildren<QAction *>();
	foreach ( QAction* action, actions )
	{
		if (action->text().isEmpty())
			continue;

		actionsList.append(action);
	}

	int row=0;
	twShortcuts->setRowCount(actionsList.count());
	foreach ( QAction* action, actionsList )
	{
		QTableWidgetItem *tblItem = new QTableWidgetItem(QString::fromUtf8(_(action->text().toUtf8())));
		tblItem->setFlags(tblItem->flags() ^ Qt::ItemIsEditable);
		twShortcuts->setItem(row, 0, tblItem);
		tblItem = new QTableWidgetItem(action->shortcut().toString());
		twShortcuts->setItem(row, 1, tblItem);
		row++;
	}
	QHeaderView *headerView = twShortcuts->horizontalHeader();
#if QT_VERSION >= 0x04FF00
	headerView->setSectionResizeMode(QHeaderView::Stretch);
	headerView->setSectionResizeMode(1, QHeaderView::Interactive);
#else
	headerView->setResizeMode(QHeaderView::Stretch);
	headerView->setResizeMode(1, QHeaderView::Interactive);
#endif
}

void UISettings::applyShortcuts()
{
	for (int row = 0; row < (int)actionsList.size(); ++row) 
	{
		QAction *action = actionsList[row];
		action->setShortcut(QKeySequence(twShortcuts->item(row, 1)->text()));
	}
}

void UISettings::loadSettings()
{
	// get settings pointer
	Settings* s = QtYabause::settings();

	// general
	leBios->setText( s->value( "General/Bios" ).toString() );
	cbEnableBiosEmulation->setChecked( s->value( "General/EnableEmulatedBios" ).toBool() );
	cbCdRom->setCurrentIndex( cbCdRom->findData( s->value( "General/CdRom", QtYabause::defaultCDCore().id ).toInt() ) );
	leCdRom->setText( s->value( "General/CdRomISO" ).toString() );
	if (s->value( "General/CdRom", QtYabause::defaultCDCore().id ).toInt() == CDCORE_ARCH) {
		if (cbCdDrive->count() == 0)
			setupCdDrives();
		cbCdDrive->setCurrentIndex(leCdRom->text().isEmpty() ? 0 : cbCdDrive->findText(leCdRom->text()));
	}

	leSaveStates->setText( s->value( "General/SaveStates", getDataDirPath() ).toString() );
#ifdef HAVE_LIBMINI18N
	int i;
	if ((i=cbTranslation->findData(s->value( "General/Translation" ).toString())) != -1)
		cbTranslation->setCurrentIndex(i);
	else
		cbTranslation->setCurrentIndex(0);
#endif
	cbEnableFrameSkipLimiter->setChecked( s->value( "General/EnableFrameSkipLimiter" ).toBool() );
	cbShowFPS->setChecked( s->value( "General/ShowFPS" ).toBool() );
	cbAutostart->setChecked( s->value( "autostart" ).toBool() );

  cbEmulationSpeed->setCurrentIndex(cbEmulationSpeed->findData(s->value("General/EmulationSpeed", mEmulationSpeed.at(0).id).toInt()));

	bool clocksync = s->value( "General/ClockSync" ).toBool();
	cbClockSync->setChecked( clocksync );	 
	dteBaseTime->setVisible( clocksync );

	QString dt = s->value( "General/FixedBaseTime" ).toString();
	if (!dt.isEmpty())		
		dteBaseTime->setDateTime( QDateTime::fromString( dt,Qt::ISODate) );
	else
		dteBaseTime->setDateTime( QDateTime(QDate(1998, 1, 1), QTime(12, 0, 0)) );

//	int numThreads = QThread::idealThreadCount();	
//	cbEnableMultiThreading->setChecked(s->value( "General/EnableMultiThreading", numThreads <= 1 ? false : true ).toBool());
//	sbNumberOfThreads->setValue(s->value( "General/NumThreads", numThreads < 0 ? 1 : numThreads ).toInt());

	// video
	cbVideoCore->setCurrentIndex( cbVideoCore->findData( s->value( "Video/VideoCore", QtYabause::defaultVIDCore().id ).toInt() ) );
//#if YAB_PORT_OSD
//	cbOSDCore->setCurrentIndex( cbOSDCore->findData( s->value( "Video/OSDCore", QtYabause::defaultOSDCore().id ).toInt() ) );
//#endif

	cbAspectRatio->setCurrentIndex( s->value( "Video/AspectRatio", 0 ).toInt() );
	//leWinWidth->setText( s->value( "Video/WindowWidth", s->value( "Video/Width", 640 ) ).toString() );
	//leWinHeight->setText( s->value( "Video/WindowHeight", s->value( "Video/Height", 480 ) ).toString() );
	QString text = QString("%1x%2").arg(s->value( "Video/FullscreenWidth", s->value( "Video/Width", 1920 ) ).toString(),
										s->value( "Video/FullscreenHeight", s->value( "Video/Height", 1080 ) ).toString());	
	cbFullscreenResolution->setCurrentIndex(cbFullscreenResolution->findText(text));
//	cbBilinear->setChecked( s->value( "Video/Bilinear", false ).toBool() );
	cbFullscreen->setChecked( s->value( "Video/Fullscreen", false ).toBool() );
	cbPolygonGeneration->setCurrentIndex(cbPolygonGeneration->findData(s->value("Video/polygon_generation_mode", mPolygonGenerationMode.at(2).id).toInt()));
  cbResolution->setCurrentIndex(cbResolution->findData(s->value("Video/resolution_mode", mResolutionMode.at(0).id).toInt()));
  cbRBGREsolution->setCurrentIndex(cbRBGREsolution->findData(s->value("Video/rbg_resolution_mode", mRbgResolutionMode.at(5).id).toInt()));
  cbFilter->setCurrentIndex(cbFilter->findData(s->value("Video/filter_mode", mFilterMode.at(0).id).toInt()));
//   cbEnableIntegerPixelScaling->setChecked(s->value("Video/EnableIntegerPixelScaling", false).toBool());
//   sbIntegerPixelScalingMultiplier->setValue(s->value("Video/IntegerPixelScalingMultiplier", 2).toInt());
   cbRotateScreen->setChecked(s->value("Video/RotateScreen").toBool());
   cbUseComputeShader->setChecked(s->value("Video/UseComputeShader",1).toBool());

   cbSh2Cache->setChecked(s->value("General/UseSh2Cache", true).toBool());

	// sound
	// An ini written before the DirectSound backend was removed still names a
	// core that is no longer in the list; findData() then returns -1 and saving
	// the dialog would write that back as the selected core.
	int soundCoreIndex = cbSoundCore->findData( s->value( "Sound/SoundCore", QtYabause::defaultSNDCore().id ).toInt() );
	if ( soundCoreIndex < 0 )
		soundCoreIndex = cbSoundCore->findData( QtYabause::defaultSNDCore().id );
	cbSoundCore->setCurrentIndex( soundCoreIndex );
//   cbNewScsp->setChecked(s->value("Sound/NewScsp", true).toBool());
   spinBox_scs_sync_count->setValue(s->value("Sound/ScspSync", 16).toInt());
   cbTimeMode->setCurrentIndex(s->value("Sound/ScspMainMode", 0).toInt());
   // Only the new SCSP renders it; this port always runs the new SCSP.
   cbSoundHighQuality->setChecked(s->value("Sound/HighQuality", false).toBool());

	// cartridge/memory
	cbCartridge->setCurrentIndex( cbCartridge->findData( s->value( "Cartridge/Type", mCartridgeTypes.at( 0 ).id ).toInt() ) );
	leCartridge->setText( s->value( "Cartridge/Path" ).toString() );
	leCartridgeModemIP->setText( s->value( "Cartridge/ModemIP", QString("127.0.0.1") ).toString() );
	leCartridgeModemPort->setText( s->value( "Cartridge/ModemPort", QString("1337") ).toString() );
	leMemory->setText( s->value( "Memory/Path", getDataDirPath().append( "/bkram.bin" ) ).toString() );
	leMpegROM->setText( s->value( "MpegROM/Path" ).toString() );
  checkBox_extended_internal_backup->setChecked(s->value("Memory/ExtendMemory", true).toBool());

  
  

	// input
	// An ini written before the DirectInput core was removed still names it;
	// findData() then returns -1 and saving the dialog would write that back.
	int perCoreIndex = cbInput->findData( s->value( "Input/PerCore", QtYabause::defaultPERCore().id ).toInt() );
	if ( perCoreIndex < 0 )
		perCoreIndex = cbInput->findData( QtYabause::defaultPERCore().id );
	cbInput->setCurrentIndex( perCoreIndex );
	sGunMouseSensitivity->setValue(s->value( "Input/GunMouseSensitivity", 100).toInt() );
	
	// advanced
	cbRegion->setCurrentIndex( cbRegion->findData( s->value( "Advanced/Region", mRegions.at( 0 ).id ).toString() ) );
	cbSH2Interpreter->setCurrentIndex( cbSH2Interpreter->findData( s->value( "Advanced/SH2Interpreter", QtYabause::defaultSH2Core().id ).toInt() ) );
   cb68kCore->setCurrentIndex(cb68kCore->findData(s->value("Advanced/68kCore", QtYabause::default68kCore().id).toInt()));

#if 0
	// view
	bgShowMenubar->setId( rbMenubarNever, BD_NEVERHIDE );
	bgShowMenubar->setId( rbMenubarFullscreen, BD_HIDEFS );
	bgShowMenubar->setId( rbMenubarAlways, BD_ALWAYSHIDE );
	bgShowMenubar->setId( rbMenubarFullscreenHover, BD_SHOWONFSHOVER );
	bgShowMenubar->button( s->value( "View/Menubar", BD_SHOWONFSHOVER ).toInt() )->setChecked( true );

	bgShowToolbar->setId( rbToolbarNever, BD_NEVERHIDE );
	bgShowToolbar->setId( rbToolbarFullscreen, BD_HIDEFS );
	bgShowToolbar->setId( rbToolbarAlways, BD_ALWAYSHIDE );
	bgShowToolbar->button( s->value( "View/Toolbar", BD_HIDEFS ).toInt() )->setChecked( true );

	bgShowLogWindow->setId( rbLogWindowNever, 0 );
	bgShowLogWindow->setId( rbLogWindowMessage, 1 );
	bgShowLogWindow->button( s->value( "View/LogWindow", 0 ).toInt() )->setChecked( true );
#endif
}

void UISettings::saveSettings()
{
	// get settings pointer
	Settings* s = QtYabause::settings();

	// general
	s->setValue( "General/Bios", leBios->text() );
	s->setValue( "General/EnableEmulatedBios", cbEnableBiosEmulation->isChecked() );
	s->setValue( "General/CdRom", cbCdRom->itemData( cbCdRom->currentIndex() ).toInt() );
	CDInterface* core = QtYabause::getCDCore( cbCdRom->itemData( cbCdRom->currentIndex() ).toInt() );
	if ( core->id == CDCORE_ARCH )
		s->setValue( "General/CdRomISO", cbCdDrive->currentText() );
	else
		s->setValue( "General/CdRomISO", leCdRom->text() );
	s->setValue( "General/SaveStates", leSaveStates->text() );
#ifdef HAVE_LIBMINI18N
	s->setValue( "General/Translation", cbTranslation->itemData(cbTranslation->currentIndex()).toString() );
#endif
	s->setValue( "General/EnableFrameSkipLimiter", cbEnableFrameSkipLimiter->isChecked() );
	s->setValue( "General/ShowFPS", cbShowFPS->isChecked() );
	s->setValue( "autostart", cbAutostart->isChecked() );

  s->setValue("General/EmulationSpeed", cbEmulationSpeed->itemData(cbEmulationSpeed->currentIndex()).toInt());


	// video
	s->setValue( "Video/VideoCore", cbVideoCore->itemData( cbVideoCore->currentIndex() ).toInt() );
//#if YAB_PORT_OSD
//	s->setValue( "Video/OSDCore", cbOSDCore->itemData( cbOSDCore->currentIndex() ).toInt() );
//#endif
	// Move Outdated window/fullscreen keys
	s->remove("Video/Width");
	s->remove("Video/Height");

	// Save new version of keys
	//s->setValue( "Video/WindowWidth", leWinWidth->text() );
	//s->setValue( "Video/WindowHeight", leWinHeight->text() );
	s->setValue( "Video/AspectRatio", cbAspectRatio->currentIndex() );

	if (supportedRes.count() > 0)
	{
		supportedRes_struct res = supportedRes[cbFullscreenResolution->itemData(cbFullscreenResolution->currentIndex()).toInt()];
		s->setValue( "Video/FullscreenWidth", res.width );
		s->setValue( "Video/FullscreenHeight", res.height );
	}

	s->setValue( "Video/Fullscreen", cbFullscreen->isChecked() );
//	s->setValue( "Video/Bilinear", cbBilinear->isChecked() );
	s->setValue( "Video/polygon_generation_mode", cbPolygonGeneration->itemData(cbPolygonGeneration->currentIndex()).toInt());
  s->setValue("Video/resolution_mode", cbResolution->itemData(cbResolution->currentIndex()).toInt());
  s->setValue("Video/rbg_resolution_mode", cbRBGREsolution->itemData(cbRBGREsolution->currentIndex()).toInt());
  s->setValue("Video/filter_mode", cbFilter->itemData(cbFilter->currentIndex()).toInt());
  s->setValue("Video/UseComputeShader", cbUseComputeShader->isChecked());
//   s->setValue("Video/EnableIntegerPixelScaling", cbEnableIntegerPixelScaling->isChecked());
//   s->setValue("Video/IntegerPixelScalingMultiplier", sbIntegerPixelScalingMultiplier->value());

	s->setValue( "General/ClockSync", cbClockSync->isChecked() );
	s->setValue( "General/FixedBaseTime", dteBaseTime->dateTime().toString(Qt::ISODate));

//	s->setValue( "General/EnableMultiThreading", cbEnableMultiThreading->isChecked() );
//	s->setValue( "General/NumThreads", sbNumberOfThreads->value());
  s->setValue("Video/RotateScreen", cbRotateScreen->isChecked());

  s->setValue("General/UseSh2Cache", cbSh2Cache->isChecked());
  
  

  s->setValue("Sound/ScspSync", spinBox_scs_sync_count->value() );
  s->setValue("Sound/ScspMainMode", cbTimeMode->currentIndex() );
  s->setValue("Sound/HighQuality", cbSoundHighQuality->isChecked() );

	// sound
	s->setValue( "Sound/SoundCore", cbSoundCore->itemData( cbSoundCore->currentIndex() ).toInt() );
  s->setValue( "Sound/NewScsp", true);

	// cartridge/memory
	s->setValue( "Cartridge/Type", cbCartridge->itemData( cbCartridge->currentIndex() ).toInt() );
	s->setValue( "Cartridge/Path", leCartridge->text() );
	s->setValue( "Cartridge/ModemIP", leCartridgeModemIP->text() );
	s->setValue( "Cartridge/ModemPort", leCartridgeModemPort->text() );
	s->setValue( "Memory/Path", leMemory->text() );
	s->setValue( "MpegROM/Path", leMpegROM->text() );
  s->setValue("Memory/ExtendMemory", checkBox_extended_internal_backup->isChecked());

	// input
	s->setValue( "Input/PerCore", cbInput->itemData( cbInput->currentIndex() ).toInt() );	
	s->setValue( "Input/GunMouseSensitivity", sGunMouseSensitivity->value() );
	
	// advanced
	s->setValue( "Advanced/Region", cbRegion->itemData( cbRegion->currentIndex() ).toString() );
	s->setValue( "Advanced/SH2Interpreter", cbSH2Interpreter->itemData( cbSH2Interpreter->currentIndex() ).toInt() );
   s->setValue("Advanced/68kCore", cb68kCore->itemData(cb68kCore->currentIndex()).toInt());

	// view
	//s->setValue( "View/Menubar", bgShowMenubar->checkedId() );
	//s->setValue( "View/Toolbar", bgShowToolbar->checkedId() );
	//s->setValue( "View/LogWindow", bgShowLogWindow->checkedId() );

	// shortcuts
	applyShortcuts();
	s->beginGroup("Shortcuts");
	QList<QAction *> actions = parent()->findChildren<QAction *>();
	foreach ( QAction* action, actions )
	{
		if (action->text().isEmpty())
			continue;

		QString accelText = QString(action->shortcut().toString());
		s->setValue(action->text(), accelText);
	}
	s->endGroup();
}

void UISettings::accept()
{
	saveSettings();
	QDialog::accept();
}
