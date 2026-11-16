#pragma once
/// @file YabaSanshiroEnums.h
/// @brief Typed enum options for YabaSanshiro Saturn emulator configuration
/// Mirrors the FlycastEnums.h pattern but maps to yabauseinit_struct fields
///
/// Copyright 2025 tico

#include <cstring>
#include <string>

namespace YabaSanshiro_Options
{

    // ─────────────────────────────────────────────────────────────────────────────
    // Boolean helper (shared by all on/off toggles)
    // ─────────────────────────────────────────────────────────────────────────────

    struct BoolOption
    {
        static constexpr const char *ValueTrue = "enabled";
        static constexpr const char *ValueFalse = "disabled";
        static constexpr const char *LabelTrue = "Enabled";
        static constexpr const char *LabelFalse = "Disabled";

        enum class Enum
        {
            Disabled,
            Enabled
        };

        static inline const char *GetValue(Enum e) { return e == Enum::Enabled ? ValueTrue : ValueFalse; }
        static inline const char *GetLabel(Enum e) { return e == Enum::Enabled ? LabelTrue : LabelFalse; }
        static inline Enum FromString(const char *v)
        {
            if (v && strcmp(v, ValueTrue) == 0)
                return Enum::Enabled;
            return Enum::Disabled;
        }
        static inline int ToInt(Enum e) { return e == Enum::Enabled ? 1 : 0; }
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // VIDEO FORMAT  (yabauseinit_struct::videoformattype)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace VideoFormat
    {
        constexpr const char *Key = "video_format";
        constexpr const char *Label = "Video Format";
        constexpr const char *Default = "NTSC";
        enum class Enum
        {
            NTSC = 0,
            PAL = 1
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::NTSC:
                return "NTSC";
            case Enum::PAL:
                return "PAL";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e) { return GetValue(e); }
        inline Enum FromString(const char *v)
        {
            if (v && strcmp(v, "PAL") == 0)
                return Enum::PAL;
            return Enum::NTSC;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // RESOLUTION MODE  (yabauseinit_struct::resolution_mode — RESOLUTION_MODE enum)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace ResolutionMode
    {
        constexpr const char *Key = "resolution_mode";
        constexpr const char *Label = "Internal Resolution";
        constexpr const char *Default = "Native";
        enum class Enum
        {
            Native = 0,
            x4 = 1,
            x2 = 2,
            Original = 3,
            _720P = 4,
            _1080P = 5
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::Native:
                return "Native";
            case Enum::x4:
                return "4x";
            case Enum::x2:
                return "2x";
            case Enum::Original:
                return "Original";
            case Enum::_720P:
                return "720P";
            case Enum::_1080P:
                return "1080P";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e)
        {
            switch (e)
            {
            case Enum::Native:
                return "Native (Window)";
            case Enum::x4:
                return "4x";
            case Enum::x2:
                return "2x";
            case Enum::Original:
                return "Original (Saturn)";
            case Enum::_720P:
                return "720P";
            case Enum::_1080P:
                return "1080P";
            }
            return "Native (Window)";
        }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::Native;
            if (strcmp(v, "4x") == 0)
                return Enum::x4;
            if (strcmp(v, "2x") == 0)
                return Enum::x2;
            if (strcmp(v, "Original") == 0)
                return Enum::Original;
            if (strcmp(v, "720P") == 0)
                return Enum::_720P;
            if (strcmp(v, "1080P") == 0)
                return Enum::_1080P;
            return Enum::Native;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // RBG RESOLUTION MODE  (yabauseinit_struct::rbg_resolution_mode)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace RBGResolutionMode
    {
        constexpr const char *Key = "rbg_resolution_mode";
        constexpr const char *Label = "RBG Resolution";
        constexpr const char *Default = "Original";
        enum class Enum
        {
            Original = 0,
            x2 = 1,
            _720P = 2,
            _1080P = 3,
            FitToEmulation = 4,
            x4 = 5 // RBG_RES_4x: appended after FitToEmulation in the core
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::Original:
                return "Original";
            case Enum::x2:
                return "2x";
            case Enum::_720P:
                return "720P";
            case Enum::_1080P:
                return "1080P";
            case Enum::FitToEmulation:
                return "Fit to Emulation";
            case Enum::x4:
                return "4x";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e) { return GetValue(e); }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::Original;
            if (strcmp(v, "2x") == 0)
                return Enum::x2;
            if (strcmp(v, "720P") == 0)
                return Enum::_720P;
            if (strcmp(v, "1080P") == 0)
                return Enum::_1080P;
            if (strcmp(v, "Fit to Emulation") == 0)
                return Enum::FitToEmulation;
            if (strcmp(v, "4x") == 0)
                return Enum::x4;
            return Enum::Original;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // VIDEO FILTER  (yabauseinit_struct::video_filter_type — AAMODE enum)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace VideoFilter
    {
        constexpr const char *Key = "video_filter";
        constexpr const char *Label = "Video Filter";
        constexpr const char *Default = "None";
        enum class Enum
        {
            None = 0,
            FXAA = 1,
            Scanline = 2,
            Bilinear = 3,
            // The CRT filters (ygl.h AA_CRT_*): the picture at the Saturn's own
            // resolution through one of RetroArch's CRT shaders
            CrtHyllian = 4,
            CrtLottes = 5,
            CrtNewPixie = 6
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::None:
                return "None";
            case Enum::FXAA:
                return "FXAA";
            case Enum::Scanline:
                return "Scanline";
            case Enum::Bilinear:
                return "Bilinear";
            case Enum::CrtHyllian:
                return "CRT Hyllian";
            case Enum::CrtLottes:
                return "CRT Lottes";
            case Enum::CrtNewPixie:
                return "CRT NewPixie";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e) { return GetValue(e); }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::None;
            if (strcmp(v, "FXAA") == 0)
                return Enum::FXAA;
            if (strcmp(v, "Scanline") == 0)
                return Enum::Scanline;
            if (strcmp(v, "Bilinear") == 0)
                return Enum::Bilinear;
            if (strcmp(v, "CRT Hyllian") == 0)
                return Enum::CrtHyllian;
            if (strcmp(v, "CRT Lottes") == 0)
                return Enum::CrtLottes;
            if (strcmp(v, "CRT NewPixie") == 0)
                return Enum::CrtNewPixie;
            return Enum::None;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // POLYGON MODE  (yabauseinit_struct::polygon_generation_mode — POLYGONMODE enum)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace PolygonMode
    {
        constexpr const char *Key = "polygon_mode";
        constexpr const char *Label = "Polygon Generation";
        constexpr const char *Default = "GPU Tessellation";
        enum class Enum
        {
            PerspectiveCorrection = 0,
            CPUTessellation = 1,
            GPUTessellation = 2,
            // Vulkan only: the OpenGL core falls back to GPU tessellation
            ComputeRasterizer = 3
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::PerspectiveCorrection:
                return "Perspective Correction";
            case Enum::CPUTessellation:
                return "CPU Tessellation";
            case Enum::GPUTessellation:
                return "GPU Tessellation";
            case Enum::ComputeRasterizer:
                return "Compute Rasterizer";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e) { return GetValue(e); }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::GPUTessellation;
            if (strcmp(v, "Perspective Correction") == 0)
                return Enum::PerspectiveCorrection;
            if (strcmp(v, "CPU Tessellation") == 0)
                return Enum::CPUTessellation;
            if (strcmp(v, "Compute Rasterizer") == 0)
                return Enum::ComputeRasterizer;
            return Enum::GPUTessellation;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // FRAME LIMIT  (yabauseinit_struct::framelimit)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace FrameLimit
    {
        constexpr const char *Key = "frame_limit";
        constexpr const char *Label = "Frame Limit";
        constexpr const char *Default = "60Hz";
        enum class Enum
        {
            _60Hz = 0,
            NoLimit = 1,
            _120Hz = 2
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::_60Hz:
                return "60Hz";
            case Enum::NoLimit:
                return "No Limit";
            case Enum::_120Hz:
                return "120Hz";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e)
        {
            switch (e)
            {
            case Enum::_60Hz:
                return "60Hz (1x Speed)";
            case Enum::NoLimit:
                return "No Limit (Unlimited)";
            case Enum::_120Hz:
                return "120Hz (2x Speed)";
            }
            return "60Hz (1x Speed)";
        }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::_60Hz;
            if (strcmp(v, "No Limit") == 0)
                return Enum::NoLimit;
            if (strcmp(v, "120Hz") == 0)
                return Enum::_120Hz;
            return Enum::_60Hz;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // DISPLAY MODE  (Integer scaling vs Display aspect-ratio)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace DisplayMode
    {
        constexpr const char *Key = "display_mode";
        constexpr const char *Label = "Display Mode";
        constexpr const char *Default = "Display";
        enum class Enum
        {
            Integer = 0,
            Display = 1
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::Integer:
                return "Integer";
            case Enum::Display:
                return "Display";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e) { return GetValue(e); }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::Display;
            if (strcmp(v, "Integer") == 0)
                return Enum::Integer;
            return Enum::Display;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // DISPLAY SIZE  (context-dependent on DisplayMode)
    //   Integer → 1x, 2x, 3x, 4x, Auto
    //   Display → Stretch, 4:3, 16:9, Original
    // ─────────────────────────────────────────────────────────────────────────────

    namespace DisplaySize
    {
        constexpr const char *Key = "display_size";
        constexpr const char *Label = "Size";
        constexpr const char *Default = "4:3";

        // Integer sizes  (used when DisplayMode == Integer)
        // Display sizes  (used when DisplayMode == Display)
        // Combined enum so both share one config key
        enum class Enum
        {
            // Display sizes
            Stretch = 0,
            _4_3 = 1,
            _16_9 = 2,
            Original = 3,
            // Integer sizes
            _1x = 4,
            _2x = 5,
            _3x = 6,
            _4x = 7,
            Auto = 8
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::Stretch:
                return "Stretch";
            case Enum::_4_3:
                return "4:3";
            case Enum::_16_9:
                return "16:9";
            case Enum::Original:
                return "Original";
            case Enum::_1x:
                return "1x";
            case Enum::_2x:
                return "2x";
            case Enum::_3x:
                return "3x";
            case Enum::_4x:
                return "4x";
            case Enum::Auto:
                return "Auto";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e) { return GetValue(e); }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::_4_3;
            if (strcmp(v, "Stretch") == 0)
                return Enum::Stretch;
            if (strcmp(v, "16:9") == 0)
                return Enum::_16_9;
            if (strcmp(v, "Original") == 0)
                return Enum::Original;
            if (strcmp(v, "1x") == 0)
                return Enum::_1x;
            if (strcmp(v, "2x") == 0)
                return Enum::_2x;
            if (strcmp(v, "3x") == 0)
                return Enum::_3x;
            if (strcmp(v, "4x") == 0)
                return Enum::_4x;
            if (strcmp(v, "Auto") == 0)
                return Enum::Auto;
            return Enum::_4_3;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }

        // Subsets for UI cycling
        static constexpr Enum IntegerSizes[] = {Enum::_1x, Enum::_2x, Enum::_3x, Enum::_4x, Enum::Auto};
        static constexpr int IntegerSizeCount = 5;
        static constexpr Enum IntegerDefault = Enum::Auto;

        static constexpr Enum DisplaySizes[] = {Enum::Stretch, Enum::_4_3, Enum::_16_9, Enum::Original};
        static constexpr int DisplaySizeCount = 4;
        static constexpr Enum DisplayDefault = Enum::_4_3;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Boolean options (simple on/off toggles)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace FrameSkip
    {
        constexpr const char *Key = "frame_skip";
        constexpr const char *Default = "enabled";
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    namespace ShowFPS
    {
        constexpr const char *Key = "show_fps";
        constexpr const char *Default = "disabled";
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    namespace RotateScreen
    {
        constexpr const char *Key = "rotate_screen";
        constexpr const char *Default = "disabled";
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    namespace SH2Cache
    {
        constexpr const char *Key = "sh2_cache";
        constexpr const char *Default = "enabled";
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    namespace SkipLoad
    {
        constexpr const char *Key = "skip_load";
        constexpr const char *Default = "disabled";
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    namespace AsyncRendering
    {
        constexpr const char *Key = "async_rendering";
        constexpr const char *Default = "disabled"; // Better performance, enabled by default
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    // The SCSP's output rendered again without the hardware's shortcuts
    // (sound_hq/scsp_hq.h), with its effects in floating point: what
    // Android's "High-quality sound" turns on. The chip itself is unchanged.
    namespace HighQualitySound
    {
        constexpr const char *Key = "high_quality_sound";
        constexpr const char *Default = "disabled";
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    namespace RBGComputeShader
    {
        constexpr const char *Key = "rbg_use_compute_shader";
        constexpr const char *Label = "RBG Compute Shader";
        constexpr const char *Default = "disabled"; // Better RBG layer performance
        using Enum = BoolOption::Enum;
        using Option = BoolOption;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // CONTROLLER MODE  (Standard Pad vs 3D Controller vs Light Gun)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace ControllerMode
    {
        constexpr const char *Key1 = "controller_1_mode";
        constexpr const char *Key2 = "controller_2_mode";
        constexpr const char *Label = "Controller Mode";
        constexpr const char *Default = "Normal";
        enum class Enum
        {
            Normal = 0,
            _3D = 1,
            LightGun = 2
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::Normal:
                return "Normal";
            case Enum::_3D:
                return "3D";
            case Enum::LightGun:
                return "LightGun";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e)
        {
            switch (e)
            {
            case Enum::Normal:
                return "Normal";
            case Enum::_3D:
                return "3D Pad";
            case Enum::LightGun:
                return "Light Gun";
            }
            return "Normal";
        }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::Normal;
            if (strcmp(v, "3D") == 0)
                return Enum::_3D;
            if (strcmp(v, "LightGun") == 0)
                return Enum::LightGun;
            return Enum::Normal;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // CARTRIDGE TYPE  (yabauseinit_struct::carttype — CART_* defines in cs0.h)
    // ─────────────────────────────────────────────────────────────────────────────

    namespace CartridgeType
    {
        constexpr const char *Key = "cartridge_type";
        constexpr const char *Label = "Cartridge Type";
        constexpr const char *Default = "None";
        // values are the core's CART_* ids (cs0.h). The backup RAM carts keep
        // their contents in the saves folder; Action Replay and the ROM cart
        // need a dump in the system folder (see TicoMain's PrepareCartridge).
        enum class Enum
        {
            None = 0,
            RAM_1MB = 6,
            RAM_4MB = 7,
            BackupRAM_4Mbit = 2,
            BackupRAM_8Mbit = 3,
            BackupRAM_16Mbit = 4,
            BackupRAM_32Mbit = 5,
            ActionReplay = 1,
            ROM_16Mbit = 9,
        };

        inline const char *GetValue(Enum e)
        {
            switch (e)
            {
            case Enum::None:
                return "None";
            case Enum::RAM_1MB:
                return "1MB RAM";
            case Enum::RAM_4MB:
                return "4MB RAM";
            case Enum::BackupRAM_4Mbit:
                return "4Mbit Backup RAM";
            case Enum::BackupRAM_8Mbit:
                return "8Mbit Backup RAM";
            case Enum::BackupRAM_16Mbit:
                return "16Mbit Backup RAM";
            case Enum::BackupRAM_32Mbit:
                return "32Mbit Backup RAM";
            case Enum::ActionReplay:
                return "Action Replay";
            case Enum::ROM_16Mbit:
                return "16Mbit ROM";
            }
            return Default;
        }
        inline const char *GetLabel(Enum e)
        {
            switch (e)
            {
            case Enum::None:
                return "None";
            case Enum::RAM_1MB:
                return "1MB RAM Cartridge";
            case Enum::RAM_4MB:
                return "4MB RAM Cartridge";
            case Enum::BackupRAM_4Mbit:
                return "4 Mbit Backup RAM Cartridge";
            case Enum::BackupRAM_8Mbit:
                return "8 Mbit Backup RAM Cartridge";
            case Enum::BackupRAM_16Mbit:
                return "16 Mbit Backup RAM Cartridge";
            case Enum::BackupRAM_32Mbit:
                return "32 Mbit Backup RAM Cartridge";
            case Enum::ActionReplay:
                return "Action Replay";
            case Enum::ROM_16Mbit:
                return "16 Mbit ROM Cartridge";
            }
            return "None";
        }
        inline Enum FromString(const char *v)
        {
            if (!v)
                return Enum::None;
            if (strcmp(v, "1MB RAM") == 0)
                return Enum::RAM_1MB;
            if (strcmp(v, "4MB RAM") == 0)
                return Enum::RAM_4MB;
            if (strcmp(v, "4Mbit Backup RAM") == 0)
                return Enum::BackupRAM_4Mbit;
            if (strcmp(v, "8Mbit Backup RAM") == 0)
                return Enum::BackupRAM_8Mbit;
            if (strcmp(v, "16Mbit Backup RAM") == 0)
                return Enum::BackupRAM_16Mbit;
            if (strcmp(v, "32Mbit Backup RAM") == 0)
                return Enum::BackupRAM_32Mbit;
            if (strcmp(v, "Action Replay") == 0)
                return Enum::ActionReplay;
            if (strcmp(v, "16Mbit ROM") == 0)
                return Enum::ROM_16Mbit;
            return Enum::None;
        }
        inline int ToInt(Enum e) { return static_cast<int>(e); }
    }

} // namespace YabaSanshiro_Options
