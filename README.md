# ESP32-C5 ITS Logger

<a href="esp32-c5-its.jpg"><img alt="esp32-c5-its" module src="esp32-c5-its.jpg" width="400px" align="right"/></a>

Logger for Intelligent Transort Systems (ITS) G5 / IEEE 802.11p / V2X / Car2X messages at 5.9GHz.

Inspired by the [OpenTrafficMap](https://opentrafficmap.org/) project to use an ESP32-C5 outside its specs to log ITS messages. While OpenTrafficMap focuses on stationary measurement points to visualize traffic on a map, this project's focus is recording while moving.

A mobile phone can be used as power supply and to record the data via USB. With the optional SD card module as mass storrage a power bank is sufficient for operation.

Logged are the raw messages, decoding has to be done with other tools e.g. Wireshark or CANalyzer.Car2x. [ESP32-C5-ITS-Log](#esp32-c5-its-log) can be used as log file converter for those tools. With the optional GPS module UTC time and the own position are recoreded as well.

When connecting the ESP to a Linux machine [ESP-C5-ITS-OTM](#esp32-c5-its-otm-open-traffic-map) can transmit the captured messages to Open Traffic Map.

Transmitting at 5.9GHz is illegal in most countries. As ITS messages are safety related and everybody should be safe, receiving and logging might be ok. Check yourself for your country and use it on your own risk.

## ESP32-C5-ITS-RX

ESP32-C5 firmware. Captures ITS messages and logs them to the Serial/JTAG USB (CDC/ACM) or an optional SD card.

The development environment is PlatformIO. Mandatory is only the ESP32-C5, optional components are listed below.
 1) Check/update the used GPIOs in ```src/esp32-c5-its.h```
 1) Select a envrionment matching your setup
 1) Compile
 1) Upload

Alternatively a CI build can be flashed: the GitHub Actions artifact contains ```<env>/firmware.factory.bin``` (bootloader, partition table and app) to be written at 0x0, e.g. ```esptool.py --chip esp32c5 write_flash 0x0 firmware.factory.bin```.

To enable 802.11p communication unofficial espidf framework functions are used, they might be removed in later versions from the IDE.

Logged are all received ITS messages and a timestamp/postion per second. If the optional GPS has a fix it's UTC time and position, else it's the ESP's system time.

### LED (optional)

The LED flashes for each received ITS message and timestamp/position update.

### RGB LED (optional)

The RGB LED shows dim continuous light to indicate timestamp/position status

| mode                | continuous color |
| ------------------- | -----------------|
| system time         | off              |
| GPS time            | dim blue         |
| GPS position        | dim red          |
| GPS time & position | dim green        |

The RGB LED flashes bright for each received ITS message and timestamp/position update.

| mode              | flash color  |
| ----------------- | ------------ |
| nothing connected | red          |
| USB connected     | yellow       |
| SD card logging   | blue         |
| USB & SD card     | green        |

### GPS (optional)

A GPS receiver can be connected for UTC time and position. The GPS module must provide a PPS signal.

| ESP32-C5 | GPS |
|----------|-----|
| GPIO 3   | PPS |
| GPIO 4   | RX  |
| GPIO 5   | TX  |

### SD Card (optional)

An SD card can be connected for logging data. Logging to SD card is started/stopped by pressing the ESP's boot button. The log filename is "log-timestamp.dat" with timestamp in seconds. If that file already exists, the first free name from "log-timestamp-1.dat" to "log-timestamp-99.dat" is used. An existing file is never overwritten; if all these names are taken, nothing is logged. Make sure to stop logging before switching off.

| ESP32-C5 | SD card |
|----------|---------|
| GPIO2    | MISO    |
| GPIO7    | MOSI    |
| GPIO6    | CLK     |
| GPIO10   | CS      |

## ESP32-C5-ITS-Log

Linux Converter for the recorded data, it converts to
- Wireshark PCAPNG: Generated file name is source name + ".pcapng" extension. Optional GPS data is added as a Beacon message with source address { 0xaa 0xaa 0xaa 0xaa 0xaa 0xaa }.
- CANoe/CANalyzer BLF: Generated file name is source name + ".blf" extension. Optional GPS data is added as GNSS system variable. BLF object times never step back: after a device restart within one USB log, ESP uptime continues from the latest object time until a GPS fix provides UTC time again. The PCAPNG file keeps the raw log times.

### Dependencies
 - [fpcap](https://github.com/fpcap/fpcap)
 - [Technica-Engineering vector_blf](https://github.com/Technica-Engineering/vector_blf)

### Compile
Download and compile fpcap and vector_blf libraries, then run `make VECTOR_BLF=<vector_blf install prefix> FPCAP=<fpcap source dir with build/>`.

### Usage
```txt
usage: log-cvt [-p|-b] [-u] <log-file> ...
options:
  -p    pcapng file
  -b    vector blf file
  -u    source is usb log with magic bytes (default sd log w/o magic bytes)
without -p or -b only the record counts are printed
exit status 1 if a file cannot be opened, read, converted or written
or contains no log records, else 0
```

### Recording a USB log
The USB data is binary. Record it with the port in raw mode, otherwise the tty layer changes or drops bytes and `cat` may stop early. Don't use a terminal program (screen, minicom, `pio device monitor`) for recording.
```txt
# Linux
( stty raw -echo -hupcl; exec cat ) < /dev/ttyACM0 > usb.log
# macOS (with only one ESP connected)
( stty raw -echo -hupcl; exec cat ) < /dev/cu.usbmodem* > usb.log
# alternative with socat
socat -u /dev/ttyACM0,rawer CREATE:usb.log
```
Stop the recording with Ctrl-C and convert it with `log-cvt -p -u usb.log`.

- A plain open of the port as above does not reset the ESP. Opening it with DTR low does, e.g. pyserial with `dtr=False` or `pio device monitor` with `monitor_dtr = 0`. Own programs should leave DTR and RTS as they are after open (pyserial default).
- Only one program at a time may read the port, with two each gets only parts of the stream. ESP32-C5-ITS-OTM resets the ESP when it starts.
- After a reset the ROM and bootloader print text before the first record. Readers must resync on the magic bytes, as `log-cvt -u` does.
- Version and Info are sent only once after boot, a recording started later may not contain them.
- Records produced while no program has the port open are lost or arrive late, without any marker for the gap. Start the recording first and keep it running.
- Linux: ModemManager may probe the port. To exclude the ESP add a udev rule, e.g. `/etc/udev/rules.d/99-esp32-c5-its.rules` with `ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", ENV{ID_MM_DEVICE_IGNORE}="1"`, and replug the ESP.

## ESP32-C5-ITS-OTM (Open Traffic Map)

Linux client for Open Traffic Map. Connect the ESP and send captured messages to Open Traffic Map.

### Dependencies
 - Mosquitto MQTT
```txt
apt install libmosquitto-dev libmosquittopp-dev libssl-dev
```
### Compile
```txt
make
```
Extra compiler and linker flags can be passed, e.g. Homebrew paths on macOS: ```make CPPFLAGS="-I$(brew --prefix)/include -I$(brew --prefix openssl@3)/include" LDFLAGS="-L$(brew --prefix)/lib"```
### Usage
For a your individual OTM node id use option ```-mi```. The last 6 digits of the Linux machine id (/etc/machine-id) will be appended for uniqueness. If no ```-mt``` is given the topic path will be updated accordingly. E.g. ```-mi myNode``` will produce id ```myNode-abcdef``` and topic ```its/myNode-abcdef/packet```. The bridge also publishes a retained ```online```/```offline``` status on the same topic with ```/packet``` replaced by ```/status``` (here ```its/myNode-abcdef/status```); ```offline``` is also its MQTT last will.

```txt
usage: esp32-c5-its-otm [option] ...
options
  -t|--tty <device>         tty device (default /dev/ttyACM0)
  -mh|--mqtt-host <host>    mqtt host (default cits1.opentrafficmap.org)
  -mp|--mqtt-port <port>    mqtt port (default 8883)
  -mi|--mqtt-id <id>        mqtt id (default esp32-c5-its)
  -mt|--mqtt-topic <topic>  mqtt topic (default its/esp32-c5-its/packet)
  --tls                     tls (default)
  --tls-unsecure            tls but no verification
  --tls-off                 no tls
  --cafile <file>           tls ca file (default first readable of /etc/ssl/certs/ca-certificates.crt, /etc/pki/tls/certs/ca-bundle.crt, /etc/ssl/ca-bundle.pem, /etc/ssl/cert.pem)
```

### Running as a service
The ttyACM number of the receiver can change when it is unplugged and plugged in again. For unattended operation use its stable path in ```/dev/serial/by-id/``` (```ls /dev/serial/by-id/``` shows it) instead:

```txt
-t /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<serial>-if00
```

The bridge resets the receiver each time it opens the port, so every start or restart of the bridge also restarts the receiver. Run only one bridge per receiver and do not open its port with other programs while the bridge is running. With several receivers on one host give each bridge its own ```-mi```, otherwise they all use the same MQTT id and topic.

Example systemd unit ```/etc/systemd/system/esp32-c5-its-otm.service```, with the bridge copied to ```/usr/local/bin```. ```<user>``` needs access to the tty, on most distributions by being a member of the ```dialout``` group (```sudo usermod -aG dialout <user>```).

```ini
[Unit]
Description=ESP32-C5 ITS to Open Traffic Map bridge
Wants=network-online.target
After=network-online.target

[Service]
User=<user>
ExecStart=/usr/local/bin/esp32-c5-its-otm -t /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<serial>-if00 -mi <node>
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```Restart=always``` restarts the bridge whenever it exits, e.g. when the receiver or the network is not ready yet at boot, and ```RestartSec=10``` keeps it from resetting the receiver in a tight loop. Enable and start it with ```sudo systemctl enable --now esp32-c5-its-otm```, show its output with ```journalctl -u esp32-c5-its-otm```.

## Log File Format

Records are byte-packed (no padding) and integers are stored little endian (ESP32 native). The struct below is an overview, the reference is [esp32-c5-its-rx/include/log-data.h](esp32-c5-its-rx/include/log-data.h).

In a USB log each record is magic + header + body, with the magic bytes `AA 55 55 AA`. In an SD card log each record is header + body, without magic. The body is always `body_size` bytes long, so a decoder advances by `body_size` after the header, also for unknown record types. In a USB log a decoder resynchronises by searching for the next magic, e.g. if the capture does not start at a record boundary or bytes were lost.

The header is 12 bytes (Python `struct.Struct('<BBHq')`), offsets are from the start of the header:

| offset | size | type   | field        |
| ------ | ---- | ------ | ------------ |
| 0      | 1    | uint8  | pkt_type     |
| 1      | 1    | uint8  | reserved     |
| 2      | 2    | uint16 | body_size    |
| 4      | 8    | int64  | timestamp_us |

```c

struct
{
  uint32_t magic;          // 0xAA5555AA -- only in usb log, missing in sd card log
  struct
  {
    uint8_t  pkt_type;     // 0 Time / 1 ITS / 2 GPS / 254 Info / 255 Version
    uint8_t  reserved;     // 0
    uint16_t body_size;    // body size  (Time 0, ITS 1..2000, GPS 24, Version 4, Info variable)
    int64_t  timestamp_us; // UTC time in µs after GPS fix, else ESP system time in µs
  } header ;

  union
  {
    struct
    {
      uint8_t payload[body_size];
    } its;
    struct
    {
      int32_t  latitude;    // °, * 1e7
      int32_t  longitude;   // °, * 1e7
      int32_t  altitudeWGS; // m, * 1e3 (mm)
      int32_t  altitudeMSL; // m, * 1e3 (mm)
      int32_t  speed;       // m/s, * 1e2 (cm/s)
      int32_t  heading;     // °, * 1e5
    } gps;                  // 24 bytes (Python struct.Struct('<6i'))
    struct
    {
      uint8_t logVersion;   // 1
      uint8_t prgVerMaj;
      uint8_t prgVerMin;
      uint8_t prgVerRev;
    } version;
    struct
    {
      char    text[body_size];
    } info;
  } body;
}
```