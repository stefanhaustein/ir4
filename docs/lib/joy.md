# joy

[← Library Index](README.md) · Source: [lib/joy.ir4](../../lib/joy.ir4)

Joystick and gamepad input (optional override per platform)
Bitmask returned by joystate:
1 = Up, 2 = Down, 4 = Left, 8 = Right, 16 = Fire/Btn1, 32 = Btn2, 64 = Select, 128 = Start

## Functions

- **joycount** ( -> :i )

  Query number of hardware joystick/gamepad ports (0 = hardware joysticks unsupported)

- **joystate** ( port :i -> :i )

  Read current button bitmask for *port* (0..joycount-1 = hardware ports, -1 = keyboard-as-joystick; returns 0 if unsupported)

## Platforms

### agon

Agon Light keyboard-mapped joystick (via MOS sysvars inkey)

### c64

Commodore 64 joystick ports (port 0 = CIA1 Port 2 `$DC00`, port 1 = Port 1 `$DC01`, port -1 = keyboard)

### x16

Commander X16 KERNAL joystick (ports 0..4) + KEYHDL keyboard-as-joystick (port -1)
