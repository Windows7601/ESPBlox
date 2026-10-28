# ESPBlox
Run a private vibeblox.online server on an ESP32!
### System Requirements:
- Any ESP32 with minimum 2MB of flash storage (recommended: Any ESP32 with PSRAM and minimum 4MB of flash storage)
- USB cable
- 2.4GHz Wi-Fi
- Arduino IDE
### Setup (+v1.b2):
1. Unpack the zip.
2. Open the unpacked folder in Arduino IDE.
3. Plug in your ESP32.
4. Pick your flash size in Tools, and set Partition Type to Custom.
5. Flash the file, making sure the CSV is called partitions.csv and is unedited.
6. After flash, open the UART console.
7. In the UART console, send:
   - config set wifi_ssid <your ssid>
   - config set wifi_password <your wifi password>
8. Send:
   - config set
  
   to learn the available keys and configure your world like so. (Note: A limit exists on max players based on the amount of PSRAM that exists)
