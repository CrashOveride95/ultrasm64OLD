# UltraSM64 (DEPRECATED)

# DUE TO THE RELEASE OF REFRESH 13, THIS REPO IS DEPRECATED. ALL CHANGES WILL BE PORTED TO REFRESH 13 IN A NEWER REPO
# PLEASE DON'T USE THIS REPO


- This repo contains a full decompilation of Super Mario 64 (J), (U), and (E) with minor exceptions in the audio subsystem.
- It has been edited to allow for the usage of the final "N64 OS" library, version ``2.0L``
- Support for Rumble Pak is included, but not all Shindou rumble events are here due to Shindou's state in Refresh 12.
- It has also been patched with someone2639's shiftable segments patch
- It has extremely WIP HVQM full motion video support
- Getting UNFLoader (flashcart USB library) to work with the game is in progress.

## FAQ

Q: Why in the hell are you bundling your own build of ``ld``?

A: Newer binutils (Like the one bundled with Ubuntu, 2.34) break linking with libultra builds due to local asm symbols.

This puts me at a crossroads of either touching leaked code and requiring GCC, or just using an older linker that works just fine.

I went with the latter.

Thanks to "someone2639 on soundcloud xd" for this hacky-ass idea

Q: Will this allow me to use Rumble/FlashRAM/Transfer Pak/microcode swapping/Other Cool N64 Features?

A: Theoretically, all yes.

## Installation help


Go read the original repo README.md
