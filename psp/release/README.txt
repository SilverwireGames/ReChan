ReChan PSP Edition {{VERSION}}
==============================

Unofficial, free port of Jackie Chan Stuntmaster (PlayStation) to the PSP,
based on ReChan. This package does NOT contain any game files: the PSP
extracts what it needs from the image of your own disc.

REQUIREMENTS
- PSP-2000, PSP-3000 or PSP Go with a custom firmware or HEN that runs
  homebrew. Tested on a PSP-3000. The PSP-1000 does not have enough memory.
- An image of the US disc (NTSC-U, SLUS-00684), preferably a .bin.
  From a .bin/.cue pair copy only the .bin. An .iso with 2048-byte sectors
  does not keep the movies; the PAL release and PS1 EBOOT.PBP files do not
  work.
- About 1.3 GB free on the Memory Stick during the install, 600 MB after.

INSTALL
1. On the PSP, open Settings > USB Connection and connect the cable.
2. Copy the RECHAN folder from this package to PSP/GAME/ on the Memory Stick.
3. Copy the disc image to PSP/GAME/RECHAN/discimage/.
4. Leave USB mode and open Game > Memory Stick > Re:Chain PSP Edition.
5. On the "Game Assets" screen, choose Extract Assets. It takes a few
   minutes: keep the PSP plugged in and do not quit the game. If the
   extraction is interrupted, it starts again the next time the game opens.
6. Choose the language and the HUD style (Modern or Classic).

After the extraction, delete the image in discimage/ to free the space.
The folder can have another name (for example PSP/GAME/RECHANBETA).

UPDATING
Copy the RECHAN folder of the new version over the old one. The extracted
files, saves and settings stay in place.

KNOWN ISSUES
- Busy scenes may drop below 60 FPS now and then.
- The Classic style draws the menus with ReChan's font: the original font
  is not distributed.
- The English, Spanish, French, German and Italian subtitles were translated
  from the Portuguese ones and may not follow the audio word for word.
- The PSP Go is not tested yet.

Issues and suggestions: https://github.com/luckvice/ReChanMetal/issues

CREDITS
_AG (@gennariarmando)    - ReChan creator: reverse engineering, game
                           reimplementation and the PC version.
GNK (@GABRIELNNK)        - Portuguese translation, movie subtitles, loading
                           and saving from the menus and door collision
                           fixes.
Lucas Soares (@luckvice) - Metal version for macOS and the PSP Edition port.

LEGAL NOTICE
ReChan is an unofficial, free, non-commercial project. It is not affiliated
with the developers, publisher or rights holders of Jackie Chan Stuntmaster.
No original game files (disc images, executables, videos, music, voice
clips, textures, models or levels) are included. Jackie Chan Stuntmaster and
all related trademarks belong to their respective owners.
