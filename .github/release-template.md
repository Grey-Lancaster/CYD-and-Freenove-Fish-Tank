## Easiest: the web installer

### [➡ Open the Web Installer](https://grey-lancaster.github.io/CYD-and-Freenove-Fish-Tank/)

Use **Chrome or Edge on a computer**, plug the board in by USB, pick your board in the list and click Connect, then Flash. (Freenove boards: hold BOOT, tap RESET, release BOOT first; press RESET after flashing.)

| Installer choice | Build file | Display |
| --- | --- | --- |
| **Freenove 4.0"** | `freenove4` | Freenove FNK0104S, ESP32-S3, ST7796 480×320, capacitive touch |
| **Freenove 2.8"** | `freenove28` | Freenove FNK0104B/AB, ESP32-S3, ILI9341 320×240, capacitive touch, speaker |
| **CYD 2.8"** | `cyd28` | ESP32-2432S028 with an ILI9341 display (one USB port) |
| **CYD 2.8" 2-USB** | `cyd28usb2` | the same board with two USB ports and an ST7789 display (starts with red/blue swapped, as those panels need) |

Colours wrong on a CYD? You most likely picked the wrong CYD build; try the other one. Nothing is damaged.

![Fish Tank](https://raw.githubusercontent.com/Grey-Lancaster/CYD-and-Freenove-Fish-Tank/main/docs/img/screenshot-chest.png)

## Which file do I want?
| File | Use it to | WiFi and settings |
| --- | --- | --- |
| `Fish-Tank-<build>-factory.bin` | install on a new board (this is what the web installer flashes), or recover one | **wiped** (you set up WiFi again on the screen) |
| `Fish-Tank-<build>-update.bin` | update a tank that is already running | **kept** |

To apply an **update** file: open the tank's web page (`http://fishtank.local`; CYD: `http://cydfishtank.local`; Freenove 2.8": `http://fishtank28.local`), scroll to **Firmware update**, press **Open update page**, then choose the file for your board and let it upload; the tank restarts by itself. (Tanks running v1.6.5 or older have a plain file-upload box there instead of that button: choose the file in it.) From a computer: `espota.py -i <ip> -p 3232 -f Fish-Tank-<build>-update.bin -r`. Only use a file whose name contains your own build name.
