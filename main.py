import time
import can2040

# RX = GPIO2, TX = GPIO1, 500 kbit/s
can2040.start(rx=2, tx=1, bitrate=500_000)

counter = 0
last_tx = time.ticks_ms()

try:
    while True:
        # Empfangene Frames abholen
        msg = can2040.recv()
        while msg is not None:
            can_id, data, ext, rtr = msg
            print("RX id=0x%X ext=%s data=%s" % (can_id, ext, data.hex()))
            msg = can2040.recv()

        # Jede Sekunde ein Frame senden
        if time.ticks_diff(time.ticks_ms(), last_tx) >= 1000:
            last_tx = time.ticks_ms()
            ok = can2040.send(0x123, bytes([counter & 0xFF, 0xDE, 0xAD]))
            print("TX", "ok" if ok else "queue voll", can2040.stats())
            counter += 1

        time.sleep_ms(1)
finally:
    # Wird auch beim Abbrechen in Thonny ausgefuehrt: Bus freigeben
    can2040.stop()
