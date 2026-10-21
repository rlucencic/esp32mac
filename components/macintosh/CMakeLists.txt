# Compact Macintosh: Plus, SE, Classic (platform independent; host build in host/)
idf_component_register(SRCS "mac.c"
                            "mem.c"
                            "via.c"
                            "rtc.c"
                            "keyboard.c"
                            "adb.c"
                            "input.c"
                            "chips.c"
                            "sony.c"
                            "rom.c"
                            "video.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES musashi
                       PRIV_REQUIRES diskspace)

# Runs together with the CPU core in the emulation loop
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
