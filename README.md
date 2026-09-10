# SmartAndRelax

SmartAndRelax is based on the open-source project **WiFi remote for Bestway Lay-Z-SPA** by visualapproach and contributors and has been modified and extended for the SmartAndRelax project.

Original project:  
https://github.com/visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA

SmartAndRelax is distributed under the **GNU General Public License v3.0 (GPL-3.0)**.

The complete license text is available in the `LICENSE` file.

## Source code and release history

The source code for SmartAndRelax is provided in this repository.

This source release is **4.0.1**. Earlier versions are available through their corresponding release archives and Git tags.

For release-specific changes and dates, see `CHANGELOG.md`, the Git history and the corresponding GitHub releases. Additional copyright and attribution information is available in `NOTICE.md`.

## Build and installation

The PlatformIO project is located in `Code/`. Install PlatformIO and use the dependencies specified in `Code/platformio.ini`.

```sh
cd Code
pio run -e d1_mini_pro
pio run -e d1_mini_pro -t buildfs
```

The PlatformIO configuration invokes the included build scripts. The additional build helper introduced in 4.0.1 is `Code/patch_bearssl_timeout.py`.

To install a self-built image via USB, select the correct board and serial port. The standard PlatformIO targets are:

```sh
cd Code
pio run -e d1_mini_pro -t uploadfs
pio run -e d1_mini_pro -t upload
```

Use the appropriate environment for your ESP8266 board. Flashing or erasing a device may remove saved settings and credentials; keep a backup before modifying an existing device.

## Local configuration

An optional configuration example is provided at `Code/src/sar_private_config.example.h`. Local configuration belongs in `Code/src/sar_private_config.h`. Keep this file and any individual credentials or private keys outside the public repository.

## SmartAndRelax modifications

SmartAndRelax contains modifications and extensions to the original visualapproach project, including German localization and additional SmartAndRelax-specific functionality.

Existing copyright, authorship and license notices from the original project and included third-party components remain applicable.

## Hardware

The SmartAndRelax hardware is also based in substantial part on the original visualapproach project.

Fully configured SmartAndRelax hardware is available separately.
