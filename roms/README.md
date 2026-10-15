# Macintosh ROM images

Put your own ROM dumps here (they are copyrighted and not part of this
repository). The emulator recognises them by their checksum (the first four
bytes), so only these files work:

| File | Machine | Size | Checksum |
|---|---|---|---|
| `plus.rom` | Macintosh Plus (ROM v1, v2 or v3) | 128 KB | 4D1EEEE1, 4D1EEAE1, 4D1F8172 |
| `se.rom` | Macintosh SE or SE FDHD | 256 KB | B2E362A8, B306E171 |
| `classic.rom` | Macintosh Classic | 512 KB | A49F9914 |

`idf.py flash` writes them to the `rom_plus`, `rom_se` and `rom_classic`
flash partitions. Any one is enough; choose that model on the setup screen.

Without flashing, the emulator also finds them in a `roms` folder on the SD
card, under any file name.
