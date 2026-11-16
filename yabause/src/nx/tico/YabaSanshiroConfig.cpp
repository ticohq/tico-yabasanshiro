/// @file YabaSanshiroConfig.cpp
/// @brief JSON configuration for YabaSanshiro Saturn emulator
///
/// Copyright 2025 tico

#include "YabaSanshiroConfig.h"
#include "TicoLogger.h"
#include <fstream>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Load / Unload / Save
// ─────────────────────────────────────────────────────────────────────────────

void YabaSanshiroConfig::Load()
{
    if (m_loaded)
        return;

    std::ifstream f(CONFIG_PATH);
    if (f.good())
    {
        m_config = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
        if (m_config.is_discarded())
        {
            LOG_ERROR("CONFIG", "[YabaSanshiroConfig] Corrupt config detected. Recreating...");
            f.close();
            remove(CONFIG_PATH);
            CreateDefaultConfig(CONFIG_PATH);
            std::ifstream f2(CONFIG_PATH);
            if (f2.good())
            {
                m_config = nlohmann::json::parse(f2, nullptr, false, true);
                if (m_config.is_discarded())
                    m_config = nlohmann::json::object();
            }
        }
        m_loaded = true;
        LOG_INFO("CONFIG", "[YabaSanshiroConfig] Loaded config from %s", CONFIG_PATH);
    }
    else
    {
        CreateDefaultConfig(CONFIG_PATH);
        std::ifstream f2(CONFIG_PATH);
        if (f2.good())
        {
            m_config = nlohmann::json::parse(f2, nullptr, false, true);
            if (m_config.is_discarded())
            {
                LOG_ERROR("CONFIG", "[YabaSanshiroConfig] Default config failed to parse!");
                m_config = nlohmann::json::object();
            }
            m_loaded = true;
            LOG_INFO("CONFIG", "[YabaSanshiroConfig] Created and loaded default config");
        }
        else
        {
            LOG_ERROR("CONFIG", "[YabaSanshiroConfig] Failed to create config at %s", CONFIG_PATH);
        }
    }
}

void YabaSanshiroConfig::Unload()
{
    if (m_loaded)
    {
        m_config.clear();
        m_loaded = false;
        LOG_INFO("CONFIG", "[YabaSanshiroConfig] Unloaded config");
    }
}

void YabaSanshiroConfig::Save()
{
    if (!m_loaded)
        return;

    std::ofstream out(CONFIG_PATH);
    if (out.good())
    {
        out << m_config.dump(4);
        out.close();
        LOG_INFO("CONFIG", "[YabaSanshiroConfig] Saved config to %s", CONFIG_PATH);
    }
    else
    {
        LOG_ERROR("CONFIG", "[YabaSanshiroConfig] Failed to save config to %s", CONFIG_PATH);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic accessors
// ─────────────────────────────────────────────────────────────────────────────

const char *YabaSanshiroConfig::GetConfigValue(const char *key, const char *defaultValue) const
{
    if (m_config.contains(key) && m_config[key].is_string())
    {
        return m_config[key].get_ptr<const std::string *>()->c_str();
    }
    return defaultValue;
}

std::string YabaSanshiroConfig::GetValue(const std::string &key) const
{
    return GetConfigValue(key.c_str(), "");
}

void YabaSanshiroConfig::SetValue(const std::string &key, const std::string &value)
{
    m_config[key] = value;
}

// ─────────────────────────────────────────────────────────────────────────────
// Typed getters → int values for yabauseinit_struct
// ─────────────────────────────────────────────────────────────────────────────

using namespace YabaSanshiro_Options;

int YabaSanshiroConfig::GetVideoFormat() const
{
    const char *v = GetConfigValue(VideoFormat::Key, VideoFormat::Default);
    return VideoFormat::ToInt(VideoFormat::FromString(v));
}

int YabaSanshiroConfig::GetResolutionMode() const
{
    const char *v = GetConfigValue(ResolutionMode::Key, ResolutionMode::Default);
    return ResolutionMode::ToInt(ResolutionMode::FromString(v));
}

int YabaSanshiroConfig::GetRBGResolutionMode() const
{
    const char *v = GetConfigValue(RBGResolutionMode::Key, RBGResolutionMode::Default);
    return RBGResolutionMode::ToInt(RBGResolutionMode::FromString(v));
}

int YabaSanshiroConfig::GetVideoFilter() const
{
    const char *v = GetConfigValue(VideoFilter::Key, VideoFilter::Default);
    return VideoFilter::ToInt(VideoFilter::FromString(v));
}

int YabaSanshiroConfig::GetPolygonMode() const
{
    const char *v = GetConfigValue(PolygonMode::Key, PolygonMode::Default);
    return PolygonMode::ToInt(PolygonMode::FromString(v));
}

int YabaSanshiroConfig::GetFrameLimit() const
{
    const char *v = GetConfigValue(FrameLimit::Key, FrameLimit::Default);
    return FrameLimit::ToInt(FrameLimit::FromString(v));
}

int YabaSanshiroConfig::GetSCSPSync() const
{
    // a number, or its text as the tico settings store it
    int val = 4; // Android's default
    if (m_config.contains("scsp_sync"))
    {
        const auto &v = m_config["scsp_sync"];
        if (v.is_number_integer())
            val = v.get<int>();
        else if (v.is_string())
            val = atoi(v.get<std::string>().c_str());
    }
    if (val < 1)
        val = 1;
    if (val > 255)
        val = 255;
    return val;
}

int YabaSanshiroConfig::GetFrameSkip() const
{
    const char *v = GetConfigValue(FrameSkip::Key, FrameSkip::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetShowFPS() const
{
    const char *v = GetConfigValue(ShowFPS::Key, ShowFPS::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetRotateScreen() const
{
    const char *v = GetConfigValue(RotateScreen::Key, RotateScreen::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetSH2Cache() const
{
    const char *v = GetConfigValue(SH2Cache::Key, SH2Cache::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetSkipLoad() const
{
    const char *v = GetConfigValue(SkipLoad::Key, SkipLoad::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetAsyncRendering() const
{
    const char *v = GetConfigValue(AsyncRendering::Key, AsyncRendering::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetHighQualitySound() const
{
    const char *v = GetConfigValue(HighQualitySound::Key, HighQualitySound::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetRBGComputeShader() const
{
    const char *v = GetConfigValue(RBGComputeShader::Key, RBGComputeShader::Default);
    return BoolOption::ToInt(BoolOption::FromString(v));
}

int YabaSanshiroConfig::GetControllerMode(int player) const
{
    const char *key = (player == 0) ? ControllerMode::Key1 : ControllerMode::Key2;
    const char *v = GetConfigValue(key, ControllerMode::Default);
    return ControllerMode::ToInt(ControllerMode::FromString(v));
}

int YabaSanshiroConfig::GetCartridgeType() const
{
    const char *v = GetConfigValue(CartridgeType::Key, CartridgeType::Default);
    return CartridgeType::ToInt(CartridgeType::FromString(v));
}

// ─────────────────────────────────────────────────────────────────────────────
// Default config creation
// ─────────────────────────────────────────────────────────────────────────────

void YabaSanshiroConfig::CreateDefaultConfig(const char *configPath)
{
    LOG_INFO("CONFIG", "[YabaSanshiroConfig] Creating default config at %s", configPath);

    // Ensure parent directories exist
#ifdef __SWITCH__
    mkdir("sdmc:/tico", 0777);
    mkdir("sdmc:/tico/config", 0777);
    mkdir("sdmc:/tico/config/cores", 0777);
#else
    mkdir("tico", 0777);
    mkdir("tico/config", 0777);
    mkdir("tico/config/cores", 0777);
#endif

    std::ofstream out(configPath);
    if (out.good())
    {
        out << R"json({
    // ── Video ──
    "video_format": "NTSC",
    "resolution_mode": "Native",
    "rbg_resolution_mode": "Original",
    "video_filter": "None",
    "polygon_mode": "GPU Tessellation",
    "rotate_screen": "disabled",

    // ── Performance ──
    "frame_limit": "60Hz",
    "frame_skip": "enabled",
    "show_fps": "disabled",

    // ── Audio ──
    "scsp_sync": 1,
    "high_quality_sound": "disabled",

    // ── Rendering ──
    "rbg_use_compute_shader": "enabled",

    // ── Display (in-game) ──
    "display_mode": "Display",
    "display_size": "4:3",

    // ── Input ──
    "controller_1_mode": "Normal",
    "controller_2_mode": "Normal",

    // ── Emulation ──
    "sh2_cache": "enabled",
    "skip_load": "disabled",

    // ── Cartridge ──
    "cartridge_type": "None"
})json";
        out.close();
    }
    else
    {
        LOG_ERROR("CONFIG", "[YabaSanshiroConfig] Failed to write default config to %s", configPath);
    }
}
