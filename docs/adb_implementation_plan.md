# ADB peripheral implementation plan (deprecated)

The in-tree ADB implementation has been retired. Future ADB work should be
tracked in the RP2040-focused **hootswitch** project (`/opt/adb/hootswitch`).

- Firmware no longer exposes CDC2 for ADB testing.
- ADB bring-up references should follow the hootswitch documentation and the
  Microchip AN591B application note (`/opt/adb/miscdocs/an591b.pdf`).
