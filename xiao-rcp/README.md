# xiao-rcp

Thread Radio Co-Processor firmware for the Seeed XIAO nRF52840 Plus. Exposes
the nRF52840's 802.15.4 radio to the ESP32-S3 over UART1 (460800 baud, HDLC
framing) so the hub can run OpenThread Border Router without a second MCU-side
radio.

Framework: **Zephyr** + **nRF Connect SDK**, building the upstream OpenThread
RCP sample.

Not yet implemented — this directory is a placeholder until the Zephyr project
is imported.
