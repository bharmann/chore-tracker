// Firmware version shown in System Info and compared with GitHub releases.
// Builds from the Arduino IDE are "dev". The release workflow
// (.github/workflows/release.yml) overwrites this file with the tag (e.g.
// "v1.4.0") before compiling, so a release binary knows its own version.
#pragma once
#define FW_VERSION "dev"
