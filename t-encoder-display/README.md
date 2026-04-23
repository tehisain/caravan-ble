# t-encoder-display

Firmware for the LilyGO **T-Encoder Pro** AMOLED + rotary encoder unit. Drives
the local UI (status gauges, levelling screen, settings menu) and accepts user
input (encoder rotation, button press, touch). Talks to the ESP32-S3 hub over a
JST UART link at 115200 baud — JSON status in, JSON events out. See the
**T-Encoder Pro Protocol** section of [`../README.md`](../README.md) for the
message schema.

Framework: **Arduino / ESP-IDF** with **LVGL** for graphics.

Not yet implemented — this directory is a placeholder until the UI project is
scaffolded.
