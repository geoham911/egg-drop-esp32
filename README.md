# ESP32 Egg Drop Controller

Firmware for an ESP32-C3 DevKitC-02 with a 128x64 SH1106 OLED and SG90 servo.
The OLED displays the Wi-Fi connection state and assigned IP address. Open that
IP address in a browser to use the **Trigger** and **Reset** controls.

## Configure

1. Copy `include/wifi_credentials.example.h` to
   `include/wifi_credentials.h`, then enter the SSID and WPA2 password for
   each network in `WIFI_NETWORKS`. The controller tries them in order and
   retries if it is disconnected. Leave unused entries empty. The local
   credentials header is ignored by Git and should never be committed.
2. Verify `I2C_SDA_PIN`, `I2C_SCL_PIN`, `SERVO_PIN`, `AUDIO_PIN`,
   `RGB_RING_DATA_PIN`, and the display address against your wiring.
3. Configure `SERVO_CLOSED_ANGLE` and `SERVO_OPEN_ANGLE` for the mechanism.
   If it moves in the wrong direction, set
   `REVERSE_SERVO_DIRECTION` to `true`. Adjust the open/closed angle constants
   to suit the mechanism; avoid forcing the servo against its mechanical stops.
4. Build and upload using the PlatformIO environment
   `esp32-c3-devkitc-02`.

## OLED troubleshooting

Open the Serial Monitor at 115200 baud after uploading. Startup checks for an
I2C device at `0x3C` or `0x3D` and reports the address it detects. If neither
address appears, check the module's SDA/SCL labels, the configured GPIO pins,
power, and the shared ground. If an address is detected but display setup
still fails, confirm that the module uses an SH1106 controller and is 128x64.

## Suggested wiring

| Module | ESP32-C3 DevKitC-02 |
| --- | --- |
| SH1106 SDA | GPIO 8 |
| SH1106 SCL | GPIO 9 |
| SH1106 VCC / GND | Match the display module's voltage requirements / GND |
| SG90 signal | GPIO 4 |
| Amplifier analog input | GPIO 5 |
| RGB ring DI | GPIO 6 |
| SG90 power | Regulated 5 V supply |
| RGB ring 5V / GND | External regulated 5 V / common GND |
| RGB ring D0 | Leave unconnected unless chaining another ring |

Power the servo and RGB ring from suitable external supplies, not from ESP32
GPIO pins. Connect their grounds, the amplifier ground, and ESP32 GND together.
GPIO 6 provides a 3.3 V data signal; a 5 V logic-level shifter is recommended
for reliable ring data. Add a 330-470 ohm resistor in series with the ring DI
line. GPIO 5 provides the amplifier tone signal; connect it only to an amplifier
input that accepts it, never directly to a speaker. Confirm the OLED module's
input voltage before connecting it.

## Browser controls

- `POST /trigger` starts a five-second countdown, moves the servo to the
  configured open angle for two seconds, then returns it to the closed/default
  angle automatically. The analog amplifier receives five short tones, one
  per countdown second: the first four are 800 Hz and the fifth is 1200 Hz. A
  red ring pixel rotates continuously, completing one revolution per second in
  sync with the countdown. While the servo is activated, the entire ring blinks
  green for two seconds. A new trigger request is rejected while a cycle is
  running.
- `POST /reset` cancels a countdown or active cycle and immediately returns the
  servo to its configured closed/default angle.
- `GET /status` reports the current sequence state for the browser interface.

The web server uses plain HTTP and has no authentication. Use it only on a
trusted local Wi-Fi network, and do not forward its port to the public internet.
