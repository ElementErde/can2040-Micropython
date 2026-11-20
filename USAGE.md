# Usage

Detailed usage guide for the `can2040` MicroPython module.

## Quick start

```python
import can2040

can2040.start(rx=2, tx=1, bitrate=500_000)

can2040.send(0x123, bytes([1, 2, 3]))   # send a message

msg = can2040.recv()                    # None if nothing is waiting
if msg:
    can_id, data, extended, rtr = msg

can2040.stop()                          # switch CAN off and release the bus
```

## Commands

### `can2040.start(rx, tx, bitrate=500000, pio=0)`
Starts CAN.
- `rx`, `tx`: GPIO numbers for receiving and transmitting.
- `bitrate`: bits per second. can2040 supports up to 1 Mbit/s. All nodes on the bus must use the same value.
- `pio`: PIO block (0 or 1). Only change it if you use the other block yourself.

### `can2040.send(id, data=None, extended=False, rtr=False)`
Puts a message into the transmit queue.
- `id`: 11-bit ID (or 29-bit with `extended=True`).
- `data`: up to 8 bytes (`bytes` or `bytearray`).
- Returns `True` if the message was accepted and `False` if the queue was full.
  `True` does not mean the message was already acknowledged. Check `stats()` for that.

### `can2040.recv()`
Fetches the next received message from the buffer (128 slots).
Returns `None` or a tuple `(id, data, extended, rtr)`.

### `can2040.stats()`
Returns `(acknowledged sent, errors, dropped received messages)`.
The first value only increases when another node acknowledges the message (ACK).

### `can2040.stop()`
Switches CAN off and sets the TX pin high (bus idle). Call it at the end of your program,
ideally in a `try/finally` block. Otherwise can2040 keeps running after you abort a script in Thonny.

## Important notes

- **TX pin after power-up:** RP2040 pins have a weak pull-down after startup. The TX pin is then low,
  and the transceiver holds the bus dominant (CANH–CANL difference about 2 V) until CAN is started. Fixes:
  - Put a `boot.py` on the Pico (for TX = GPIO1):
    ```python
    from machine import Pin
    Pin(1, Pin.OUT, value=1)
    ```
  - Or add a 10 kΩ resistor between the TX line and 3V3.
- **Dropped messages:** Received messages are stored in a buffer of 128 slots. Call `recv()` regularly.
  The third value of `stats()` shows whether messages were dropped.
- **Timing:** Long interrupt-disabled phases, such as writing to flash, can cost messages under high bus load.
- **Restarting with different pins:** Call `stop()` first. The old TX pin stays high afterwards.
- **Acknowledgement:** At least one other node with the same bitrate must be on the bus. Without an
  ACK the sender keeps retransmitting.

## Files

| File | Content |
|------|---------|
| `usermod/modcan2040.c` | MicroPython wrapper around can2040 |
| `usermod/micropython.cmake` | Integration into the MicroPython build |
| `.github/workflows/build.yml` | Cloud build of the firmware |
| `example.py` | Commented example program |
| `main.py` | Short test program |

## License

The wrapper includes can2040, which is licensed under the GNU GPL v3 (please verify in the
[upstream repository](https://github.com/KevinOConnor/can2040)). Firmware built with it is therefore
subject to the same terms. MicroPython itself is MIT-licensed.
