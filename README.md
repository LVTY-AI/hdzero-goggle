# ripples' Custom HDZero Goggles Firmware

To install this firmware, head on over to the [releases](https://github.com/LVTY-AI/hdzero-goggle/releases) page and download the latest version for your goggles! I also have backups on [Google Drive](https://drive.google.com/drive/folders/1L3Wu2_ggWYJpjnqjVrTsP1es7GogYQT6?usp=share_link), as well as the recovery firmware files and VTX firmwares on there.

A couple of quick FAQ's I get very often:
**1. Does this have all of the latest changes from the official HDZero Github and do you have the fixes for DVR, Scroll Wheel, and Ghosting?**
Yes. In fact, I also have a bunch of other features pulled in from other forks and custom repos out there. My firmware has significant changes and fixes over the official HDZero Github. This means I have the DVR fix for the G2 and the ghosting fix for the BoxPros. The scroll wheel fixes are included, but this is mostly a hardware issue. I suggest you reach out to HDZero support to get your scroll wheel replaced if my firmware doesn't solve your issues.
**2. Is this firmware safe and will it brick my goggles or void the warranty?**
Yes, it's safe. Loads of people use it, myself included. No, it won't brick your goggles or void the warranty. This firmware is less likely to brick your goggles than the official firmware and actually has critical fixes that will extend your goggles' lifespan and prevent them from having issues and dying randomly. I test this firmware on my own goggles before it ever gets released.
**3. What is different about your firmware over the official HDZero firmware?**
My firmware has many new features, critical fixes, and general bug fixes that you won't find on the official firmware. The basic list is Auto HDZ/Analog, Analog and Auto Scan modes, separate image settings for analog and HDZero, RTC sync over backpack, Gradual low voltage alarm, Audio settings, Auto Bandwidth, fixed Headtracker and ELRS backpack. For a full list of features, check out the compact change log or the full release notes, both of which can be found [here](https://github.com/LVTY-AI/hdzero-goggle/releases)
**4. Which goggles are supported on your firmware?**
I currently support all of the HDZero branded goggles. This includes the G1, G2, BoxPro, and BoxPro+. Divimath products are NOT supported and using my firmware for those goggles will brick them. As a side note, the G2 is the only goggle I don't personally own, and while it is fully supported right now, it relies on a small group of testers to prove some features work and work out the bugs. Due to this, some features may arrive slower on the G2's, there may be a few more bugs, and for some future planned features, there will be no G2 support unless I acquire a set myself. Divimath goggles cannot be supported unless I get my hands on a set myself.
**5. How can I support you and this project?**
If you'd like to support the work I'm doing here (and help me get a G2 for testing), you can send a donation here: https://www.paypal.com/paypalme/ripleyjb
I also appreciate people who want to test for me with their G2's. Send me a message on Discord if you'd like to test for me. Additionally, I appreciate anyone who either wants to contribute to the project here on Github, has suggestions for the project, or simply reports bugs that you find. Spreading the word about this firmware is greatly appreciated as well, as many pilots simply don't know it's out there. Maybe you guys can convince HDZero to send me some goggles for testing and adopt this as the offical firmware!

As a perk for donating, your feature requests will get top priority and I'll give you early access to upcoming firmwares (there's some really cool stuff to come). I'll also have a special firmware version with some extra goodies as a thank you to those who donated $25 or more. Be sure to drop your Discord username/Github username in the donation description so I know who you are if you'd like access to priority feature requests, early access firmwares or the extra goodies firmware. It is not a requirement to donate. Firmware releases will continue to be free. Donations for this project help cover costs related to this project and will contribute to better support for all goggles users!

If you'd like to request a feature you'd like me to add, send me a message in Discord, leave it in the #feature-suggestions channel on the [HDZero Discord](https://discord.gg/kGsnEDMb2V), or open an issue on here, and I'll get to it when I can. This is a project I'm working on in my free time, so please set timeline expectations accordingly. For any other questions you may have, feel free to send me a message on Discord!

I hope you enjoy my custom firmware!

-ripples



## Environment Setup

The firmware can either be built in a [devcontainer](https://containers.dev/) or natively on a linux machine.

Note: decompressing the repository in Windows system may damage some files and prevent correct builds.

### Devcontainer Setup

This repository supports the [vscode devcontainer](https://code.visualstudio.com/docs/devcontainers/containers) integration.
To get started, install docker, vscode and the devcontainer extension.
A [prompt](https://code.visualstudio.com/docs/devcontainers/create-dev-container#_add-configuration-files-to-a-repository) to reopen this repository in a container should appear.

### Native Setup

CMake is required for generating the build files.
A bash script is supplied to take care of the bootstrap process:

```
~/hdzero-goggle$ ./setup.sh
```

## Building Firmware

In either of the above scenarios the firmware can be built via make.
An appropiate vscode build task ships with this repository as well.

Compiling HDZero Goggles:
```
~/hdzero-goggle$ cd build_goggle
~/hdzero-goggle/build_goggle$ make clean all -j $(nproc)
```

The firmware is generated as hdzero-goggle/build_goggle/out/HDZERO_GOGGLE-77-206-1.1.2-ripples-<commit>.bin
Tagged releases omit the commit suffix.

Compiling HDZero BoxPro:
```
~/hdzero-goggle$ cd build_boxpro
~/hdzero-goggle/build_boxpro$ make clean all -j $(nproc)
```

The firmware is generated as hdzero-goggle/build_boxpro/out/HDZERO_BOXPRO-77-211-1.1.2-ripples-<commit>.bin
Tagged releases omit the commit suffix.

### Building the firmware using nix

The nix build system can be used to build the firmware on any linux system.  
Make sure that nix [is installed](https://nixos.org/download/), and the [flakes feature](https://wiki.nixos.org/wiki/Flakes) is enabled.  
No bootstrapping or installation of any tools is required.

Use this command to build the firmware

```shellSession
nix build .#goggle-app
```

After this succeeds, the firmware can be found under `./result` in the current directory.


## Loading the Firmware

Firmware can be either flashed via goggle menu or alternatively be executed via the SD Card with a custom development script.  An example of this development script is provided below.  The goggles automatically checks to see if the develop.sh script exists in the root of the SD Card and if found develop.sh is then executed.

The following files must be placed in the root of SD Card in this example. This script will then check to see if HDZGOGGLE binary has been found during bootup and if found then executed.

Otherwise, if the HDZGOOGLE binary is not detected, the goggles will continue to load the built-in executable which was previously flashed.

SD Card File Hierarchy:

```
/develop.sh
/HDZGOGGLE
```

Development script (develop.sh):

```
#!/bin/sh

# Load via SD Card if found
if [ -e /mnt/extsd/HDZGOGGLE ]; then
	/mnt/extsd/HDZGOGGLE &
else
	/mnt/app/app/HDZGOGGLE &
fi
```

## Building the Emulator

Goggle source code can be built natively on the host machine and used for debugging.

### Library required

Requires build-essential tools and SDL2 development libraries (libsdl2-dev for debian) to be already installed.

```
sudo apt-get install build-essential libsdl2-dev
```

### Build and Run

Emulator support for both Goggle and BoxPro is supported by setting the appropriate compilation switches.

```
~/hdzero-goggle$ mkdir build_emu
~/hdzero-goggle$ cd build_emu
~/hdzero-goggle/build_emu$ cmake .. -DEMULATOR_BUILD=ON -DCMAKE_BUILD_TYPE=Debug -DHDZ_GOGGLE=ON -DHDZ_BOXPRO=OFF -DHDZ_GOGGLE2=OFF
~/hdzero-goggle/build_emu$ make -j $(nproc)
~/hdzero-goggle/build_emu$ ./HDZGOGGLE
```

### Emulator Keys

`a` = right button press
`w` = wheel up
`s` = wheel down
`d` = wheel center press
Use `F11` to toggle full screen where applicable.

## Support and Developer Channels

Join the official Discord server here:

https://discord.gg/kGsnEDMb2V

Or the official Facebook group:

https://www.facebook.com/groups/hdzero
