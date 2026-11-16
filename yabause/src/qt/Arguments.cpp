#include "Arguments.h"
#include "VolatileSettings.h"
#include "QtYabause.h"

#include <iostream>
#include <iomanip>

#include <QApplication>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVectorIterator>

#include <stdlib.h>
#include <stdio.h>

extern "C" char *wavefilename;  // sndwav.c

namespace Arguments
{

	void autoframeskip(const QString& param);
	void autoload(const QString& param);
	void autostart(const QString& param);
	void binary(const QString& param);
	void bios(const QString& param);
	void cdrom(const QString& param);
	void fullscreen(const QString& param);
	void help(const QString& param);
	void iso(const QString& param);
	void nobios(const QString& param);
	void nosound(const QString& param);
	void version(const QString& param);
  void playRecord(const QString& param);

	struct Option
	{
		const char * shortname;
		const char * longname;
		const char * parameter;
		const char * description;
		unsigned short priority;
		void (*callback)(const QString& param);
	};

	static Option LAST_OPTION = { NULL, NULL, NULL, NULL, 0 };

	static Option availableOptions[] =
	{
		{ NULL,  "--autoframeskip=", "0|1", "Enable or disable auto frame skipping / limiting.",  2, autoframeskip },
		{ NULL,  "--autoload=", "<SAVESTATE>", "Automatically start emulation and load a save state.",1, autoload },
		{ "-a",  "--autostart", NULL,       "Automatically start emulation.",                      1, autostart },
		{ NULL,  "--binary=", "<FILE>[:ADDRESS]", "Use a binary file.",                           1, binary },
		{ "-b",  "--bios=", "<BIOS>",       "Choose a bios file.",                                3, bios },
		{ "-c",  "--cdrom=", "<CDROM>",     "Choose the cdrom device.",                           4, cdrom },
		{ "-f",  "--fullscreen", NULL,      "Start the emulator in fullscreen.",                  5, fullscreen },
    { "-p",  "--playrecord", "<DIR>",   "Play play record.",                  5, playRecord },
		{ "-h",  "--help", NULL,            "Show this help and exit.",                           0, help },
		{ "-i",  "--iso=", "<ISO>",         "Choose a dump file.",                                4, iso },
                { "-nb", "--no-bios", NULL,         "Use the emulated bios",                              3, nobios },
                { "-ns", "--no-sound", NULL,        "Turns sound off.",                                   6, nosound },
		{ "-v",  "--version", NULL,         "Show version and exit.",                             0, version },
		LAST_OPTION
	};

	// Developer options, parsed apart from the table above (its priority slots
	// are all taken and "--vdp-dump=" is a prefix of the other two):
	//   --vdp-dump=PREFIX       replay PREFIX_{vdp2vram,..,vdp1regs}.bin without CPU
	//   --vdp-dump-out=FILE.png image written after the last frame
	//   --vdp-dump-frames=N     frames to replay (default 3)
	//   --polygon-mode=N        VDP1 polygon mode for this run
	//   --resolution-mode=N     resolution mode for this run
	//   --filter-mode=N         output filter (AAMODE) for this run
	//   --vdp-dump-size=WxH     size in pixels of the emulation view
	//   --vdp-dump-filters=a,b  filter of each replayed frame (switched at run time)
	//   --rotate-screen=0|1     "Rotate 90 degree" for this run
	//   --sound-wav=FILE        write the sound output to FILE (WAV, 44.1 kHz)
	//   --sound-hq=0|1          SCSP high-quality rendering for this run
	//   --sound-hq-chip-out=FILE  with --sound-hq=1: every sample, the chip's
	//                           own output and the high-quality output, to FILE
	//   --sound-hq-float-dsp=0|1|2  with --sound-hq=1: run the effect DSP in
	//                           floating point (2: fed with the chip's sends, for
	//                           checking the DSP arithmetic). Default: 1 while
	//                           high quality is on, as the user setting does
	//   --sound-hq-fx-out=FILE  with the float DSP: every sample, the effect
	//                           returns of the chip's DSP and of the float DSP
	//                           (raw float32: chip L, chip R, float L, float R)
	//   --sh2-core=N            SH2 core for this run (1 = debug interpreter)
	//   --scsp-main-mode=N      0 = sound CPU synced to the emulated time, 1 = realtime
	//   --sound-test-events=S,L,R  at emulated frame S save a state to memory,
	//                           at frame L load it back, at frame R reset (0 = never);
	//                           an optional ,K prints SCSP HQ key-on statistics at frame K
	// The dump run uses the OpenGL core, no disc, no sound and the HLE BIOS;
	// everything goes to the volatile settings so the ini is not changed.
	static void parseDeveloperOptions(const QStringList& arguments)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		bool dump = false;
		foreach (const QString& a, arguments)
		{
			if (a.startsWith("--vdp-dump-out="))
				vs->setValue("VdpDump/Out", a.mid((int)strlen("--vdp-dump-out=")));
			else if (a.startsWith("--vdp-dump-frames="))
				vs->setValue("VdpDump/Frames", a.mid((int)strlen("--vdp-dump-frames=")).toInt());
			else if (a.startsWith("--vdp-dump="))
			{
				vs->setValue("VdpDump/Prefix", a.mid((int)strlen("--vdp-dump=")));
				dump = true;
			}
			else if (a.startsWith("--polygon-mode="))
				vs->setValue("Video/polygon_generation_mode", a.mid((int)strlen("--polygon-mode=")).toInt());
			else if (a.startsWith("--resolution-mode="))
				vs->setValue("Video/resolution_mode", a.mid((int)strlen("--resolution-mode=")).toInt());
			else if (a.startsWith("--filter-mode="))
				vs->setValue("Video/filter_mode", a.mid((int)strlen("--filter-mode=")).toInt());
			else if (a.startsWith("--rotate-screen="))
				vs->setValue("Video/RotateScreen", a.mid((int)strlen("--rotate-screen=")).toInt() != 0);
			else if (a.startsWith("--vdp-dump-filters="))
				vs->setValue("VdpDump/Filters", a.mid((int)strlen("--vdp-dump-filters=")));
			else if (a.startsWith("--vdp-dump-size="))
				vs->setValue("VdpDump/Size", a.mid((int)strlen("--vdp-dump-size=")));
			else if (a.startsWith("--sound-wav="))
			{
				static QByteArray wavPath;
				wavPath = a.mid((int)strlen("--sound-wav=")).toLocal8Bit();
				wavefilename = wavPath.data();
				vs->setValue("Sound/SoundCore", SNDCORE_WAV);
			}
			else if (a.startsWith("--sound-hq="))
				vs->setValue("Sound/HighQuality", a.mid((int)strlen("--sound-hq=")).toInt() != 0);
			else if (a.startsWith("--sound-hq-float-dsp="))
				vs->setValue("Sound/HighQualityFloatDsp", a.mid((int)strlen("--sound-hq-float-dsp=")).toInt());
			else if (a.startsWith("--sound-hq-fx-out="))
				vs->setValue("Debug/SoundHqFxOut", a.mid((int)strlen("--sound-hq-fx-out=")));
			else if (a.startsWith("--sound-hq-chip-out="))
				vs->setValue("Debug/SoundHqChipOut", a.mid((int)strlen("--sound-hq-chip-out=")));
			else if (a.startsWith("--sh2-core="))
				vs->setValue("Advanced/SH2Interpreter", a.mid((int)strlen("--sh2-core=")).toInt());
			else if (a.startsWith("--scsp-main-mode="))
				vs->setValue("Sound/ScspMainMode", a.mid((int)strlen("--scsp-main-mode=")).toInt());
			else if (a.startsWith("--sound-test-events="))
				vs->setValue("Debug/SoundTestEvents", a.mid((int)strlen("--sound-test-events=")));
		}
		if (dump)
		{
			vs->setValue("autostart", true);
			vs->setValue("Video/VideoCore", VIDCORE_OGL);
			vs->setValue("Video/Fullscreen", false);
			vs->setValue("General/CdRom", CDCORE_DUMMY);
			vs->setValue("General/CdRomISO", QString());
			vs->setValue("Sound/SoundCore", SNDCORE_DUMMY);
			vs->setValue("General/EnableEmulatedBios", true);
			vs->setValue("General/RecordDir", QString());
		}
	}

	void parse()
	{
		parseDeveloperOptions(QApplication::arguments());
		QVector<Option *> choosenOptions(7);
		QVector<QString> params(7);

		QStringList arguments = QApplication::arguments();
		QStringListIterator argit(arguments);
		
		while(argit.hasNext())
		{
			QString argument = argit.next();
			Option * option = & * availableOptions;
			while(option->longname)
			{
				if (argument == option->shortname)
				{
					choosenOptions[option->priority] = option;
					if (option->parameter)
						params[option->priority] = argit.next();
				}
				if (argument.startsWith(option->longname))
				{
					choosenOptions[option->priority] = option;
					if (option->parameter)
						params[option->priority] = argument.mid((int)strlen(option->longname));
				}
				option++;
			}
		}
		
		for(int i = 0;i < 7;i++)
		{
			Option * option = choosenOptions[i];
			if (option)
				if (option->parameter)
					option->callback(params[i]);
				else
					option->callback(QString());
		}
	}

  void playRecord(const QString& param) {
    VolatileSettings * vs = QtYabause::volatileSettings();
    vs->setValue("General/RecordDir", param);
  }

	void autoframeskip(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("General/EnableFrameSkipLimiter", param);
	}

	void autoload(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("autostart", true);
		vs->setValue("autostart/load", true);
		vs->setValue("autostart/load/slot", param.toInt());
	}

	void autostart(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("autostart", true);
	}

	void binary(const QString& param)
	{
		QString filename;
		uint address = 0x06004000;
		QStringList parts = param.split(':');
		filename = parts[0];
		if (parts.size() > 1)
			address = parts[1].toUInt(NULL, 0);

		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("autostart", true);
		vs->setValue("autostart/binary", true);
		vs->setValue("autostart/binary/filename", filename);
		vs->setValue("autostart/binary/address", address);
	}

	void bios(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("General/Bios", param);
	}

	void cdrom(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("General/CdRom", CDCORE_ARCH);
		vs->setValue("General/CdRomISO", param);
	}

	void fullscreen(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("Video/Fullscreen", true);
	}

	void help(const QString& param)
	{
		std::cout << "Yabause:" << std::endl;
		
		Option * option = & * availableOptions;
		while(option->longname)
		{
			QString longandparam(option->longname);
			if (option->parameter)
				longandparam.append(option->parameter);

			if (option->shortname)
				std::cout << std::setw(5) << std::right << option->shortname << ", ";
			else
				std::cout << std::setw(7) << ' ';
			std::cout << std::setw(27) << std::left << longandparam.toLocal8Bit().constData()
				<< option->description
				<< std::endl;
			option++;
		}
		
		exit(0);
	}

	void iso(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue( "General/CdRom", CDCORE_ISO );
		vs->setValue( "General/CdRomISO", param );
	}

	void nobios(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("General/EnableEmulatedBios", true);
	}

	void nosound(const QString& param)
	{
		VolatileSettings * vs = QtYabause::volatileSettings();
		vs->setValue("Sound/SoundCore", SNDCORE_DUMMY);
	}

	void version(const QString& param)
	{
		std::cout << VERSION << std::endl;
		
		exit(0);
	}

}
