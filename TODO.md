# esp32mac roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

1. **Trace the Classic’s ROM disk startup.** Drive Cmd–Option–X–O through the
   host ADB path at cold boot and log which key transitions the ROM observes.
   Compare ordinary boot with a known-good ROM-disk boot trace where available,
   then confirm the successful sequence with a physical keyboard.

2. **Establish a System 6 baseline.** Boot an appropriate System 6 release on
   the Plus, SE and Classic using the host harness. Record model and ROM
   versions, Finder startup and document save/reopen on a scratch image, then
   repeat representative boots on the supported boards.

3. **Measure disk I/O stalls in Finder.** Profile a large file copy through
   `components/macintosh/sony.c` while tracking missed frames and audio
   continuity. Verify the saved file bytes after remounting before evaluating
   changes to disk buffering.
