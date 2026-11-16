#pragma once
/// @file YabaSanshiroConfig.h
/// @brief JSON configuration for YabaSanshiro Saturn emulator
/// Follows the FlycastConfig pattern but standalone (no libretro)
///
/// Copyright 2025 tico

#include <string>
#include <nlohmann/json.hpp>
#include "YabaSanshiroEnums.h"

class YabaSanshiroConfig
{
public:
    /// Load config from sdmc:/tico/config/cores/yabasanshiro.jsonc
    void Load();

    /// Unload config and free memory
    void Unload();

    /// Save current config back to disk
    void Save();

    /// Get a raw string value by key (empty string if not found)
    std::string GetValue(const std::string &key) const;

    /// Set a string value by key
    void SetValue(const std::string &key, const std::string &value);

    /// Check if config is loaded
    bool IsLoaded() const { return m_loaded; }

    // ── Typed getters (return int values ready for yabauseinit_struct) ──

    int GetVideoFormat() const;       // VIDEOFORMATTYPE_NTSC / _PAL
    int GetResolutionMode() const;    // RES_NATIVE .. RES_1080P
    int GetRBGResolutionMode() const; // RBG_RES_ORIGINAL .. RBG_RES_FIT_TO_EMULATION
    int GetVideoFilter() const;       // AA_NONE .. AA_BILNEAR_FILTER
    int GetPolygonMode() const;       // PERSPECTIVE_CORRECTION .. GPU_TESSERATION
    int GetFrameLimit() const;        // 0=60Hz, 1=No Limit, 2=120Hz
    int GetSCSPSync() const;          // 1-256

    // Boolean options → int (0/1)
    int GetFrameSkip() const;
    int GetShowFPS() const;
    int GetRotateScreen() const;
    int GetSH2Cache() const;
    int GetHighQualitySound() const; // 1=the high-quality SCSP output and float DSP
    int GetSkipLoad() const;
    int GetAsyncRendering() const; // 1=async VDP thread (better perf), 0=sync
    int GetRBGComputeShader() const; // 1=use compute shader for RBG (better perf), 0=legacy

    // Input options → int (0=Normal, 1=3D, 2=LightGun)
    int GetControllerMode(int player) const;

    // Cartridge type → int (CART_* from cs0.h)
    int GetCartridgeType() const;

private:
    nlohmann::json m_config;
    bool m_loaded = false;

    const char *GetConfigValue(const char *key, const char *defaultValue) const;
    void CreateDefaultConfig(const char *path);

    static constexpr const char *CONFIG_PATH =
#ifdef __SWITCH__
        "sdmc:/tico/config/cores/yabasanshiro.jsonc";
#else
        "tico/config/cores/yabasanshiro.jsonc";
#endif
};
