# FamiCAS dual USB CDC bridge (`2PORTmonitor` / `2portserial`)

The RP2040 exposes two independent Windows COM ports so the current PIC debug
UART and FC debug/RAMGO traffic can never be mixed.

## CDC 0: transparent 921600-baud bridge

```text
PC <-> USB CDC 0 <-> UART0
                       GPIO12 TX
                       GPIO13 RX
                       921600 baud, 8N1
```

CDC 0 is a byte-for-byte binary bridge. It adds no prefixes, line endings, or
startup messages. It was originally used for `ROMWRITER_Host`, but the current
FamiCAS bench wiring replaces the former FTDI/COM7 PIC debug adapter:

```text
Pico GPIO12 TX -> PIC RF3 / UART1 RX
Pico GPIO13 RX <- PIC RF4 / UART1 TX
```

The observed Windows name was COM13. PIC-to-PC receive was reverified from the
PIC BOOT banner through the application startup log at 921600 baud. PC-to-PIC
transmit through this new wiring has not yet been rerun; use the existing PIC
UART1 binary PING when that direction is next required. The final PIC firmware
does not emit periodic debug text, so reset the PIC to prove the receive path.

To use this CDC for `ROMWRITER_Host` again, restore the physical GPIO12/13
writer wiring first. Changing only the Windows application is not sufficient.

## CDC 1: FC printf and half-duplex test

```text
FC $4016 bit 0 -> expansion connector -> GPIO9 (UART1 RX) -> USB CDC 1
                                                        300000 baud, 8N1
```

CDC 1 normally displays FC output in a terminal at 300000 baud. GPIO8 is not
configured as a UART TX pin; the only accepted PC-to-Pico input is the framed
RAM-and-GO upload described below.

The experimental reverse path behaves like a serial controller/keypad:

```text
expansion pin 12 P/S   -> Pico GPIO9  (FC async command, 300000 baud)
expansion pin 13 /D1   <- Pico GPIO8  (Pico reply at $4016 bit 1, LSB first)
expansion pin 14 CLOCK -> Pico GPIO10 (one pulse per FC $4016 read)
expansion pin 15 VCC   = 5 V           (do not connect to a Pico GPIO)
expansion pin 1  GND   <-> Pico GND
```

When the Pico receives the exact `HDX?\r\n` command on GPIO9, it presents this
fixed 16-byte reply on GPIO8:

```text
55 AA 00 FF 01 02 04 08 10 20 40 80 C3 3C 5A A5
```

The FC samples `$4016` bit 1 from /D1. The read creates a CLOCK pulse; the
Pico advances to the next bit on CLOCK's rising edge. This preserves GPIO9 as
the ordinary 300000-baud printf path between transactions.

Because expansion pin 13 is active-low `/D1`, GPIO8 drives the complement of
each logical reply bit. The value read by FC software is therefore the original
uncomplemented byte.

### FC internal-RAM-and-GO debug loader

CDC1 also accepts one binary frame from the PC:

```text
"FCRG" + length_u16_le + crc16_u16_le + payload
```

The CRC is CRC-16/CCITT-FALSE (polynomial `0x1021`, initial `0xFFFF`) and the
payload is limited to 1..1024 bytes. The Pico queues it once. When the FC sends
`RAM?\r\n`, the Pico returns the frame over DATA/CLOCK; after all bits have
been clocked, the queue is cleared so a console reset cannot re-run stale code.

The matching FC test ROM loads the bytes at `$0400-$07FF`, verifies CRC, prints
`RAMGO LEN=.... CRC=.... GO`, clears A/X/Y, and jumps to `$0400`. Zero page,
stack, and the proven UART routine at `$0300` remain intact. Payload code must
reset or jump back explicitly if it wants to return to the ROM monitor.

P/S and CLOCK are 5 V console outputs and must be level-shifted before GPIO9
and GPIO10 because RP2040 GPIOs are not 5 V tolerant. DATA runs in the opposite
direction; use an HCT-family 5 V buffer for a permanent connection. During the
bench test, disconnect any controller that also drives the same DATA line.

While CDC 1 is open, the Pico also emits a one-second diagnostic line such as
`[PICO] GPIO9_RX bytes=123 err=0 overflow=0 level=1 baud=300000 HDX=IDLE bit=128 done=1`.
This line is
generated inside the Pico, so it separates the USB CDC path from the FC-to-GPIO9
UART path. `bytes` counts bytes accepted by UART1 and `err` counts UART framing,
parity, break, or overrun indications.

## Entering BOOTSEL without the button

Open either CDC port at 1200 baud and close it. The firmware defers the request
out of the TinyUSB callback and then calls `reset_usb_boot()`, causing the Pico
to re-enumerate as the `RPI-RP2` drive. For example:

```powershell
$port = [System.IO.Ports.SerialPort]::new('COM13', 1200)
$port.Open()
$port.Close()
```

The first installation of a firmware containing this feature still requires
the BOOTSEL button. Subsequent UF2 updates do not.

## Windows port identification and cautions

Both ports belong to one USB device (`VID:PID CAFE:4002`) and therefore share
the same Pico USB serial number. CDC0 appears as interface `MI_00`; CDC1 appears
as `MI_02`. The observed numbers COM13/COM14 are not permanent.

- Open CDC0 at 921600 baud for the PIC bridge.
- Open CDC1 at 300000 baud for FC printf/RAMGO.
- The selected USB line coding changes the corresponding hardware UART baud.
- Selecting 1200 baud on either CDC requests BOOTSEL; do not select it by accident.
- Only one process can own each COM port. Close a terminal before a RAMGO uploader
  opens CDC1.
- CDC1 emits a Pico status line every second. It may appear between fragments of
  a long FC line; parsers must buffer to LF and handle this interleaving.
- If CDC1 remains closed while FC output continues, the finite Pico ring can
  overflow. A sequence gap immediately after reopening is not by itself an FC
  soft-UART failure.

## Build output

```text
build_famicas/2PORTmonitor.uf2
```

The hardware-verified UF2 was built from firmware commit
`bb908c08331ff1aec3190fcc9209e8df99f46069` and has SHA-256
`79E02378AB1E46E8E3253005D290F8EEEEE037D8A069A4C29EB3B9B1B7FDB45B`.

```text
PICO_BOARD:    pico
build type:    Release
Pico SDK:      2.2.0
Arm GCC:       14.2.1 / Arm GNU Toolchain 14.2.Rel1
CMake:         3.25.0
Ninja:         1.13.0
C standard:    C11
C++ standard:  C++17
```

A copy of the exact UF2 and a complete toolchain manifest are committed in the
FamiCAS repository at
`BINARY/2MB対応版/自己更新_20260806/PICO_2PORTmonitor.uf2` and its `README.md`.
