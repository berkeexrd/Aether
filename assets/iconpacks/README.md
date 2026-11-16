# Aether icon packs

An icon pack replaces the icons of applications in the taskbar, the Start menu and the Alt+Tab switcher.
A pack is a folder of PNG files (256x256, transparent background) named after the program:

    iconpacks\<Pack name>\chrome.png        for chrome.exe
    iconpacks\<Pack name>\code.png          for Code.exe
    iconpacks\<Pack name>\microsoft.windowscalculator.png     for a Store app (the part of its AppUserModelID before "_")

Names are matched without the extension and ignoring case. Apps a pack has no file for keep their own icon.

Put your own packs in `%APPDATA%\Aether\iconpacks\<Pack name>\` (Settings > Appearance > Icon pack > "Open folder"); they show up next to the bundled ones.

## Bundled packs

Aether Color, Aether Glass and Aether Mono were generated from two open icon sets:

* [Simple Icons](https://simpleicons.org) - CC0 1.0 (public domain). Brand colours come from the same data set.
* [Material Design Icons](https://pictogrammers.com/library/mdi/) - Pictogrammers Free License (Apache 2.0).

The logos are trademarks of their respective owners and are shown only to identify the matching application.
