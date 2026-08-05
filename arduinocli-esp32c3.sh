#!/bin/bash
# arduinocli-esp32c3.sh
#
# Usage:
#   ./arduinocli-esp32c3.sh                -> compile and flash
#   ./arduinocli-esp32c3.sh compile        -> compile only (no board needed)
#   ./arduinocli-esp32c3.sh reset          -> just reset the board (no flashing)
#   ./arduinocli-esp32c3.sh monitor        -> open the serial monitor
#   PORT=/dev/cu.usbmodemXXXX ./arduinocli-esp32c3.sh   -> force a specific port

# --- Board configuration ---
# Generic ESP32-C3 Dev Module profile:
#   - CDCOnBoot=cdc -> routes Serial to the native USB (/dev/cu.usbmodem*),
#     so Serial prints reach the monitor with no code changes.
FQBN="esp32:esp32:esp32c3:UploadSpeed=921600,CDCOnBoot=cdc"
BAUD=115200   # serial monitor baud rate

# --- Display ---
# ST7789V3 driven by the Adafruit ST7789 library; pins and panel size are
# defined in powerMeterDC.ino (SCK=4, MOSI=3, RST=2, DC=5, CS=6, BLK->3V3).
# TFT_eSPI is NOT used here: 2.5.43 writes raw SPI registers and crashes on
# ESP32-C3 with esp32 core 3.x (REG_SPI_BASE bug -> store access fault).

# --- Port detection ---
# The C3 port can change between reconnections, so detect it instead of
# hardcoding. Priority: native USB (usbmodem) and common bridges.
detect_port() {
    ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -n1
}
PORT="${PORT:-$(detect_port)}"

# --- esptool detection (for reset) ---
# Picks the most recent version installed by the core, without hardcoding it.
detect_esptool() {
    ls -d ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | sort -V | tail -n1
}

require_port() {
    if [ -z "$PORT" ]; then
        echo "No USB port found. Connect the ESP32-C3 board or export PORT=..."
        exit 1
    fi
}

case "${1:-upload}" in
    compile)
        echo "Compiling (no upload)..."
        arduino-cli compile --fqbn "$FQBN" --verbose .
        ;;
    reset)
        require_port
        ESPTOOL="$(detect_esptool)"
        if [ -z "$ESPTOOL" ]; then
            echo "esptool not found. Install/update the esp32 core."
            exit 1
        fi
        echo "Resetting ESP32-C3 on $PORT..."
        "$ESPTOOL" --chip esp32c3 --port "$PORT" run
        ;;
    monitor)
        require_port
        echo "Serial monitor on $PORT at $BAUD baud (Ctrl-C to exit)..."
        arduino-cli monitor -p "$PORT" -c baudrate=$BAUD
        ;;
    upload|*)
        require_port
        echo "Compiling and flashing on $PORT..."
        arduino-cli compile --fqbn "$FQBN" --upload --verbose -p "$PORT" .
        ;;
esac
