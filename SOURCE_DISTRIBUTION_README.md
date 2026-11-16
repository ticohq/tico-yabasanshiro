# YabaSanshiro 1.20.55 - Source Distribution (GPL)

This archive is the corresponding source for YabaSanshiro 1.20.55,
provided to comply with the GNU General Public License.

## Sanitized / redacted for security

This is a public source distribution, so **user-specific credentials**
(Firebase configuration, API keys, OAuth client identifiers) are NOT
functional source of the program. They have been removed from the values.
The files themselves are **kept** so the project still builds -- their
structure, keys, package names, and resource names are intact and only the
secret VALUES are replaced with the placeholder `REPLACE_WITH_YOUR_OWN_VALUE` (or a
valid-format dummy where the build tool parses the value).

Config files sanitized (kept so the build still works; secret values replaced):
- `yabause/src/android/app/google-services.json`
- `yabause/src/android/app/src/test/resources/google-services.json`
- `yabause/src/android/app/src/main/res/values/local_security.xml`
- `yabause/src/ios/uoyabause/uoyabause/GoogleService-Info-Debug.plist`

Files with a secret value redacted to a placeholder:
- `yabause/src/qt/GetPin.cpp`
- `yabause/src/ios/uoyabause/uoyabause/Info.plist`
- `yabause/src/ios/uoyabause/uoyabause/Info-lite.plist`
- `yabause/src/ios/uoyabause/uoyabause/Info.plist`
- `yabause/src/ios/uoyabause/uoyabause/Info-lite.plist`
- `yabause/src/android/app/src/test/java/org/uoyabause/android/DiscordWebhookTest.kt`

Agent tooling/configuration (`.claude`, `.agents`, etc.), instruction files
(`CLAUDE.md`, `AGENTS.md`), internal docs (`docs/`), release/agent skills
(`skills/`), developer tools (`tools/`), the Windows regression-test template
(`win_template/`), and CI / build-wrapper files (`.gitlab-ci.yml`,
`buildwin.bat`, `docker-compose.yml`), store listing text and store deployment
tooling (`release_notes/`, fastlane lanes, screenshot generators) and internal
planning notes are also omitted; they are not part of the emulator program. The core build system (CMake / Gradle / Xcode project)
is kept, so the source still builds.

## Building from this archive

The project builds as-is, but cloud-connected features need real credentials.
Replace the placeholder values with your own:

- Android: put your Firebase values into `google-services.json` (structure is
  already present) and your keys/tokens into
  `app/src/main/res/values/local_security.xml` (resource names are already
  present; replace the placeholder values).
- iOS: put your Firebase values into `GoogleService-Info-Debug.plist` and set
  your reversed OAuth client id in `Info.plist` / `Info-lite.plist`.
- Windows (Qt): set your own API key where `YOUR_API_KEY_HERE` appears
  in `yabause/src/qt/GetPin.cpp`.

RetroAchievements uses your own account (username / token entered at
runtime); no credentials are embedded and nothing is required at build time.

The full development history is available in the official repository.
