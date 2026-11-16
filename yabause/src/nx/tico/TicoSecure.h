/// @file TicoSecure.h
/// @brief Sealing secrets on the SD card, shared by tico and its cores.
///
/// Header-only (C++17, libsodium). Cores vendor this one file unchanged.
///
/// A key is derived from three things: a secret given at build time
/// (TICO_SECRET_HEX, 64 hex digits, never committed), the console's serial
/// number, and a scope ("user:<id>", "core-session"). Data sealed with it is
/// XChaCha20-Poly1305: unreadable as text, tamper-evident, and useless on
/// another console.
///
/// What this does not do: every homebrew can read the SD card, and the build
/// secret ships inside the binaries, so a program written to attack tico can
/// still undo it. It keeps secrets out of plain files, shared logs, backups
/// and copied SD cards.
///
/// Cores receive what they need for one run through the session file
/// (ReadSession), sealed by tico right before the launch.
#pragma once

#include <sodium.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

// The secret comes from the build (-DTICO_SECRET_HEX=...) or from a
// git-ignored TicoSecret.h next to this file holding that one #define.
#if !defined(TICO_SECRET_HEX) && __has_include("TicoSecret.h")
#include "TicoSecret.h"
#endif
#ifndef TICO_SECRET_HEX
#define TICO_SECRET_HEX ""
#endif

namespace tico {
namespace secure {

namespace detail {

constexpr int Nibble(char c) {
  return (c >= '0' && c <= '9')   ? c - '0'
         : (c >= 'a' && c <= 'f') ? c - 'a' + 10
         : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                  : -1;
}

struct Secret {
  std::uint8_t bytes[32] = {};
  bool fromBuild = false;
};

// Evaluated by the compiler: the hex text never reaches the binary.
constexpr Secret Parse(const char *hex) {
  Secret secret{};
  std::size_t length = 0;
  while (hex[length])
    ++length;
  if (length != 64)
    return secret;
  for (int i = 0; i < 32; ++i) {
    const int hi = Nibble(hex[i * 2]), lo = Nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0)
      return Secret{};
    secret.bytes[i] = static_cast<std::uint8_t>(hi << 4 | lo);
  }
  secret.fromBuild = true;
  return secret;
}

inline const Secret &BuildSecret() {
  static constexpr Secret secret = Parse(TICO_SECRET_HEX);
  return secret;
}

// Development builds without a secret still work, with a fixed one.
constexpr std::uint8_t kDevSecret[32] = {
    0x74, 0x69, 0x63, 0x6f, 0x2d, 0x64, 0x65, 0x76, 0x2d, 0x6f, 0x6e, 0x6c, 0x79, 0x2d, 0x6e, 0x6f,
    0x74, 0x2d, 0x61, 0x2d, 0x72, 0x65, 0x61, 0x6c, 0x2d, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x21};

constexpr const char kMagic[4] = {'T', 'S', 'E', '1'};
constexpr const char kAd[] = "tico-secure-v1";

} // namespace detail

/// @brief False when this build fell back to the development secret.
inline bool HasBuildSecret() { return detail::BuildSecret().fromBuild; }

#ifdef __SWITCH__
namespace detail {
// libsodium's own generator reads /dev/urandom, which Horizon does not have:
// sodium_init() would abort. libnx's randomGet is seeded by the kernel.
inline const char *RandomName() { return "libnx"; }
inline std::uint32_t RandomU32() {
  std::uint32_t value;
  randomGet(&value, sizeof(value));
  return value;
}
inline void RandomStir() {}
inline void RandomBuf(void *buf, const std::size_t size) { randomGet(buf, size); }
inline int RandomClose() { return 0; }
} // namespace detail
#endif

inline bool Init() {
  static const bool ready = [] {
#ifdef __SWITCH__
    static randombytes_implementation random = {detail::RandomName, detail::RandomU32,
                                                detail::RandomStir, nullptr,
                                                detail::RandomBuf, detail::RandomClose};
    randombytes_set_implementation(&random);
#endif
    return sodium_init() >= 0;
  }();
  return ready;
}

namespace detail {
// How DeviceId() got its value, for the session log: the serial, or why not.
inline std::string &DeviceIdSource() {
  static std::string source;
  return source;
}
} // namespace detail

/// @brief Something only this console has. The serial on Switch.
inline std::string DeviceId() {
  static std::string id;
  if (!id.empty())
    return id;
  char why[64] = "not a Switch";
#ifdef __SWITCH__
  const Result init = setsysInitialize();
  if (R_SUCCEEDED(init)) {
    SetSysSerialNumber serial{};
    const Result got = setsysGetSerialNumber(&serial);
    if (R_SUCCEEDED(got))
      id.assign(serial.number, strnlen(serial.number, sizeof(serial.number)));
    else
      std::snprintf(why, sizeof(why), "setsysGetSerialNumber failed 0x%x", got);
    setsysExit();
    if (R_SUCCEEDED(got) && id.empty())
      std::snprintf(why, sizeof(why), "serial is empty");
  } else {
    std::snprintf(why, sizeof(why), "setsysInitialize failed 0x%x", init);
  }
#endif
  if (id.empty()) {
    id = "tico-desktop";
    detail::DeviceIdSource() = std::string("fallback (") + why + ")";
  } else {
    detail::DeviceIdSource() = "serial";
  }
  return id;
}

/// @brief The 32-byte key for @p scope on this console.
inline void Key(const std::string &scope, std::uint8_t out[32]) {
  const std::uint8_t *secret =
      HasBuildSecret() ? detail::BuildSecret().bytes : detail::kDevSecret;
  std::string message = DeviceId();
  message.push_back('\0');
  message += scope;
  crypto_generichash(out, 32, reinterpret_cast<const unsigned char *>(message.data()),
                     message.size(), secret, 32);
}

/// @brief Seal @p plain for @p scope: magic, nonce, ciphertext and tag.
inline std::vector<std::uint8_t> Seal(const std::string &scope, const std::string &plain) {
  std::vector<std::uint8_t> out;
  if (!Init())
    return out;
  std::uint8_t key[32];
  Key(scope, key);
  const std::size_t nonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
  out.resize(sizeof(detail::kMagic) + nonceSize + plain.size() +
             crypto_aead_xchacha20poly1305_ietf_ABYTES);
  std::memcpy(out.data(), detail::kMagic, sizeof(detail::kMagic));
  std::uint8_t *nonce = out.data() + sizeof(detail::kMagic);
  randombytes_buf(nonce, nonceSize);
  unsigned long long written = 0;
  crypto_aead_xchacha20poly1305_ietf_encrypt(
      nonce + nonceSize, &written, reinterpret_cast<const unsigned char *>(plain.data()),
      plain.size(), reinterpret_cast<const unsigned char *>(detail::kAd), sizeof(detail::kAd) - 1,
      nullptr, nonce, key);
  sodium_memzero(key, sizeof(key));
  out.resize(sizeof(detail::kMagic) + nonceSize + written);
  return out;
}

/// @brief Open what Seal made for @p scope. False when it was not sealed for
/// this scope on this console, or was changed.
inline bool Open(const std::string &scope, const std::uint8_t *data, std::size_t size,
                 std::string &plain) {
  const std::size_t nonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
  const std::size_t header = sizeof(detail::kMagic) + nonceSize;
  if (!Init() || size < header + crypto_aead_xchacha20poly1305_ietf_ABYTES ||
      std::memcmp(data, detail::kMagic, sizeof(detail::kMagic)) != 0)
    return false;
  std::uint8_t key[32];
  Key(scope, key);
  plain.resize(size - header);
  unsigned long long read = 0;
  const int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
      reinterpret_cast<unsigned char *>(&plain[0]), &read, nullptr, data + header, size - header,
      reinterpret_cast<const unsigned char *>(detail::kAd), sizeof(detail::kAd) - 1,
      data + sizeof(detail::kMagic), key);
  sodium_memzero(key, sizeof(key));
  if (rc != 0) {
    plain.clear();
    return false;
  }
  plain.resize(read);
  return true;
}

/// @name Text form, for a database column
/// @{
inline std::string SealText(const std::string &scope, const std::string &plain) {
  const std::vector<std::uint8_t> sealed = Seal(scope, plain);
  if (sealed.empty())
    return {};
  const int variant = sodium_base64_VARIANT_ORIGINAL;
  std::string text(sodium_base64_ENCODED_LEN(sealed.size(), variant), '\0');
  sodium_bin2base64(&text[0], text.size(), sealed.data(), sealed.size(), variant);
  text.resize(std::strlen(text.c_str()));
  return text;
}

inline bool OpenText(const std::string &scope, const std::string &text, std::string &plain) {
  if (!Init())
    return false;
  std::vector<std::uint8_t> sealed(text.size());
  std::size_t length = 0;
  if (sodium_base642bin(sealed.data(), sealed.size(), text.c_str(), text.size(), nullptr, &length,
                        nullptr, sodium_base64_VARIANT_ORIGINAL) != 0)
    return false;
  return Open(scope, sealed.data(), length, plain);
}
/// @}

/// @name Core session
/// What a core needs for one run (who plays, their RetroAchievements token),
/// as JSON text. tico writes it right before every launch; a core reads it
/// at start. Nothing else of the user's is handed to cores.
/// @{
inline const char *SessionPath() {
#ifdef __SWITCH__
  return "sdmc:/tico/run/session.bin";
#else
  return "tico/run/session.bin";
#endif
}

/// @brief One line in sdmc:/tico/run/session.log, written even by release
/// builds: whether tico wrote the session and whether the core opened it.
/// It names no secret and no serial: only a short fingerprint of the key, so
/// tico's line and the core's can be compared.
inline void SessionLog(const char *side, const std::string &what) {
#ifdef __SWITCH__
  const char *path = "sdmc:/tico/run/session.log";
#else
  const char *path = "tico/run/session.log";
#endif
  // Kept small: started over once it passes 64 KB
  FILE *file = std::fopen(path, "ab");
  if (file && std::ftell(file) > 64 * 1024) {
    std::fclose(file);
    file = std::fopen(path, "wb");
  }
  if (!file)
    return;
  std::uint8_t probe[32] = {};
  if (Init())
    Key("session-log-fingerprint", probe);
  char when[32] = "";
  const std::time_t now = std::time(nullptr);
  if (const std::tm *local = std::localtime(&now))
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", local);
  std::fprintf(file, "%s %s: %s | device id: %s, key %02x%02x%02x%02x, build secret: %s\n", when,
               side, what.c_str(), detail::DeviceIdSource().c_str(), probe[0], probe[1], probe[2],
               probe[3], HasBuildSecret() ? "yes" : "NO (development key)");
  sodium_memzero(probe, sizeof(probe));
  std::fclose(file);
}

inline bool WriteSession(const std::string &json) {
  const std::vector<std::uint8_t> sealed = Seal("core-session", json);
  if (sealed.empty()) {
    SessionLog("tico", "write FAILED: could not seal (libsodium did not start)");
    return false;
  }
  const std::string path = SessionPath();
  const std::string temp = path + ".tmp";
  FILE *file = std::fopen(temp.c_str(), "wb");
  if (!file) {
    SessionLog("tico", "write FAILED: cannot create " + temp + " (" + std::strerror(errno) + ")");
    return false;
  }
  const bool ok = std::fwrite(sealed.data(), 1, sealed.size(), file) == sealed.size();
  std::fclose(file);
  if (!ok) {
    SessionLog("tico", "write FAILED: short write to " + temp);
    std::remove(temp.c_str());
    return false;
  }
  std::remove(path.c_str());
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    SessionLog("tico", "write FAILED: cannot rename to " + path + " (" + std::strerror(errno) + ")");
    return false;
  }
  SessionLog("tico", "wrote session, " + std::to_string(sealed.size()) + " bytes");
  return true;
}

/// @brief The session tico wrote, or false (none, another console, changed).
inline bool ReadSession(std::string &json) {
  FILE *file = std::fopen(SessionPath(), "rb");
  if (!file) {
    SessionLog("core", std::string("no session: cannot open ") + SessionPath() + " (" +
                           std::strerror(errno) + ")");
    return false;
  }
  std::vector<std::uint8_t> sealed;
  std::uint8_t chunk[1024];
  std::size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
    sealed.insert(sealed.end(), chunk, chunk + n);
  std::fclose(file);
  const bool opened = Open("core-session", sealed.data(), sealed.size(), json);
  const bool magic = sealed.size() >= sizeof(detail::kMagic) &&
                     std::memcmp(sealed.data(), detail::kMagic, sizeof(detail::kMagic)) == 0;
  SessionLog("core", std::to_string(sealed.size()) + " bytes, " +
                         (opened  ? std::string("opened")
                          : magic ? std::string("CANNOT OPEN: sealed with another key")
                                  : std::string("CANNOT OPEN: not a session file")));
  return opened;
}
/// @}

} // namespace secure
} // namespace tico
