# epd/ - Waveshare's e-Paper driver

Copied unchanged from <https://github.com/waveshare/e-Paper> (`RaspberryPi_JetsonNano/c/lib`), where every file
carries the MIT licence notice you can read in its header. Only the 2.13" V4 panel and the lgpio backend are used
(`DEV_Config.c`, `dev_hardware_SPI.c`, `EPD_2in13_V4.c`); the other headers are here because `DEV_Config.h` includes them.
