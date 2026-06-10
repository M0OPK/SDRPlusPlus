# SDR++Brown Icom Panadapter Edition (fork), is not the original bloat-free SDR software

This is a fork of the SDR++Brown version of SDR++. So, a fork of a fork. Features added to this fork are as follows (and will be updated here)

## SDRPlay PPM support

The SDRPlay source has been missing this feature since the source was created. This has been added and is a hardware PPM setting. That is the value is sent to the SDRPlay API and the device implements it.

Changes to SmGui to support float entry (required for PPM entry)

## RigCtld client changes

This client now has support to specify offsets for the modes AM/FM/LSB/USB/CW. Some radios shift their IF window to ensure that the signal sits in the middle of the filtered area. As such the SDR needs to shift ensure the centre frequency remains where it was, when the mode is changed.

Mode changes in SDR++ are now propagated to the radio.

Implemented a two way sync mode. The poll time can be specified, however there is a limit of how often rigctld will attempt to poll the radio. There are some crashes when rigctl gets interfered with (if icom transceive mode is enabled for example and the dial is switched fast). I've worked on some of these, but there still a few cases that can crash it.

Changes to underlying rigctl library to support these changes

## Icom CIV Client (new module)

This new module is based on the rigctld client module. It's designed to work with most Icom radios providing the following features.

Configuration screen for serial port, baud rate, CIV address, Panadapter mode toggle, Panadapter IF and the same offsets that were added to rigctld.

Real time handling of changes from the radio (requires Transceive mode to be on). That is, as you turn the dial or change mode the change is reflected immediately in SDR++.

Propagate changes from SDR++ to frequency and mode to the radio.

Only commands for frequency/mode change are used and only transceive commands accepted. This should provide a wide range of compatibility.

Tested on Icom 7100.

## Dependencies

The serial library (https://github.com/bgeiser/async_comm forked from https://github.com/dpkoch/async_comm) requires boost installed to compile.

Ubuntu/Debian: apt install libboost-all-dev

Arch (yay): yay -Sy boost

Windows (minimal): vcpkg install boost-asio boost-bind boost-function

Windows (full and slow): vcpkg install boost

Macos: brew install boost

# Original readme from sdr++ brown

[Changelog](changelog.md)

**Please do not report bugs in this fork to original author. Use original application, it works better.**

**Report bugs in this fork on this page, in ISSUES.** 

Please see [upstream project page](https://github.com/AlexandreRouma/SDRPlusPlus) for the basic list of its features.

Last merge: 2025-06-11

Please see [brown fork page](https://sdrpp-brown.san.systems) for list of fork features.

WINDOWS INSTALL TROUBLESHOOTING: https://youtu.be/Q3CV5U-2IIU

## Thanks / Credits

Thanks and due respect to:
 
* original author, Alexandre Rouma, for his great [work](https://github.com/AlexandreRouma/SDRPlusPlus). Due credits go to all contributors in the upstream project. 
* MSHV author, LZ2HV, for his great [work](http://lz2hv.org/mshv).
* logmmse/python authors for their great [work](https://github.com/wilsonchingg/logmmse).
* OMLSA authors for their great [idea](https://github.com/yuzhouhe2000/OMLSA-IMCRA) and [implementation](https://github.com/xiaochunxin/OMLSA-MCRA).
* imgui-notify author for his great [work](https://github.com/patrickcjk/imgui-notify)
* implot author for his great [work](https://github.com/epezent/implot/)
* alexander-sholohov (github) for his work on soapy_sdr module.
* Cropinghigh / Indir for his [work](github.com/cropinghigh/sdrpp-vhfvoiceradio) on extra VHF modes.
* monolifed for his [pbkdf2 header-only implementation](https://github.com/monolifed/pbkdf2-hmac-sha256)  

## Feedback

Found an issue? Fork is worse than original? File an [issue](https://github.com/sannysanoff/SDRPlusPlusBrown/issues).

## Debugging reminders

* to debug in windows in virtualbox env, download mesa opengl32.dll from https://downloads.fdossena.com/Projects/Mesa3D/Builds/MesaForWindows-x64-20.1.8.7z
* make sure you put rtaudiod.dll in the build folder's root otherwise audio sink will not load.
* use system monitor to debug missing dlls while they fail to load.

## Local Android build:

* put into your ~/.gradle/gradle.properties this line: sdrKitRoot=/home/user/SDRPlusPlus/android-sdr-kit/sdr-kit
  * it can obtained + built from: https://github.com/AlexandreRouma/android-sdr-kit 
  * docker build --platform linux/amd64 -t android-sdr-kit  .
  * docker start android-sdr-kit    # it will exit
  * docker cp be03210da56a:/sdr-kit .    # will create directory with built binary libs, replace be03210da56a with id obtained from 'docker ps -a'
* use jdk11 for gradle in android studio. Android Studio -> Settings -> ... -> Gradle -> Gradle JDK . This is needed if you have various errors with java.io unaccessible fields.
* in case of invalid keystore error (should not happen with jdk11): 
  * you may create new keystore with current jdk version:
    ~/soft/jdk8/bin/keytool -genkey -v -keystore debug2.keystore -storepass android -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 -validity 10000
  * use this filename (debug2.keystore) in app/build.gradle along with passwords in the signingConfigs -> debug section.

Good luck.

