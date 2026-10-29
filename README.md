# ESPBlox
Run a private vibeblox.online server on an ESP32!
### System Requirements:
- Any ESP32 with minimum 2MB of flash storage (recommended: Any ESP32 with PSRAM and minimum 4MB of flash storage)
- USB cable
- 2.4GHz Wi-Fi
### Setup (+v1.b3):
1. Unpack the zip.
2. Plug in your ESP32.
3. Run the flasher.bat file. (NOT espblox-flasher.bat)
4. Follow the instructions in the batch file.
5. After flash, the batch should open the UART console.
6. In the UART console, send:
   - config set wifi_ssid <your ssid>
   - config set wifi_password <your wifi password>
7. Send:
   - config get
  
   to learn the available keys and configure your world like so. (Note: A limit exists on max players based on the amount of PSRAM that exists)
If you closed batch and need the UART console, you can install Arduino IDE and open UART that way.
### Admin Panel
There is an admin panel at (url:port/admin). This URL lets you upload an existing world from another server, or reboot in and out of Safe Mode.
Set password with UART console:

config set admin_password <your_password>

### Connecting from the internet (+v1.b3.1)
To include your friends in the fun, you'll need a hostname with wss:// to join the game. Here's how to set one up with Cloudflare Workers:
1. Make a Cloudflare account and log into your dashboard.
2. Run the espblox-flasher.bat file. (NOT flasher.bat)
3. Input your public IP and your preferred name for your worker subdomain into the .bat.
4. Authorize the operation.
5. Forward the port you chose from your ESP via your router to port 27631.
