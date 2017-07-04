Mylo PC remote power, reset, and serial server
==============================================

Mylo is developed for the ESP-12E ESP8266 and similar boards for the purpose
of remotely controlling the power and reset switches of a PC, monitoring the
system power state, and providing access to a serial console over TCP.  The
intention is to install an ESP8266-based board into a PC using a 3D printed
PCI-e or conventional PCI card with inlaid traces for Vaux (3.3v), 3.3v
power and ground.  Vaux connects to 3.3v power on the ESP8266 (ground to GND).
This provides the ESP8266 with uninterrupted power (ie. Mylo stays on even
when the PC is off).  The 3.3v line connects to GPIO12 in the sketch to sense
the system power state.

The ESP8266 UART only provides 3.3v TTL signaling, therefore to connect to the
PC serial ports, a converter chip such as the MAX3232 is required.  TX is on
GPIO15, RX on GPIO13 (after Serial.swap()).

Standard PC power and reset switches momentarily connect 3.3v/5v to ground to
generate a signal.  Use a multimeter to determine which of the two pins for
each switch is signal and which is ground.  Tap into the signal lines and
connect power signal to GPIO5, reset signal to GPIO4.  For good measure, a
2.2kOhm resistor can be used in series so avoid any significant current sink
into the ESP8266.

Upon receiving power, the ESP8266 will go into configuration mode.  This is
indicated by the single pulse heartbeat on the ESP-12E LED (use an LED on
GPIO16 if not provided on your board).  If the board provides a USB serial
adapter, you can also connect to it (115200 baud) for information.  In config
mode, the ESP8266 creates a Wifi access point with SSID "Mylo@ESP_xxxxxx"
where the trailing hex digits are specific to your ESP device.  Connect to
this network with passphrase "SetupMyMylo".  Once connected to the network,
the heartbeat should change to a double pulse.  Open a web browser and access
the configuration setup at http://192.168.4.1

The configuration will automatically load visible 2.4GHz SSID networks.
Select the one to connect to or use the manual/hidden option to enter an
unlisted network.  The refresh button at the bottom of the interface can be
used to rescan available access points.  Enter a password for the selected
network if it is not open.  The default DHCP name is based on the unique
chip ID of the ESP8266, change it if desired.

The web interface is enabled by default on port 80.  The web interface allows
basic power status and control of the power and reset switches remote.  This
interface can be disabled or the port changed here.  HTTP authentication is
not yet implemented.

MQTT is a lightweight message passing protocol allowing subscribing to and
publishing to topics hosted by a broker.  Enable if desired.  The host is
the broker system, either an IP address or resolvable hostname should work.
The publish and subscribed fields indicate the default topics the system will
publish to and subscribe to.  The Mylo will publish "ON"/"OFF" status on Myo
power on, system power state change, or request via subscription interface.
Subscription will respond to "ON", "OFF", "SHUTDOWN", "RESET, and "STATUS".
If necessary, provide a username, password, and change the MQTT server port.
TLS is not currently supported.

For both the web and MQTT interface, only the available power transitions are
allowed.  If the system power is OFF, the only valid transition is to ON.  This
is accomplished by a 0.5s press of the power button.  If the system is ON, the
power can be turned OFF by activating the power switch for 5s.  A soft SHUTDOWN
is initiated by activating the power switch for 0.5s.  The running OS is
required to poweroff the system completely, Mylo will do nothing more to
automatically reach a power off state.  RESET can only be activated while the
system is ON.

The final configuration is for the serial server.  By default this is enabled
and configured to 115200 baud rate with standard protocol parameters.  Adjust
as necessary or disable if the serial server is to be unused.  The default
port is 9999.  Serial data can be accessed using telnet to the configured port
or software like conserver to maintain a persistent connection.

When the desired configuration is created, use the 'Save & Reset' button to
store the configuration and restart the ESP8266.  No feedback is provided on
the web interface when the changes are committed, the reset should however be
evident by the heartbeat LED ceasing.  In server mode, the LED tracks the
power state of the host system.  The 'Erase' button can be used to clear the
EEPROM memory of the ESP8266.

If the Mylo is unable to connect to the selected network on restart, it will
return to configuration mode.  Configuration mode can also be signaled by
pulling GPIO14 to ground and resetting the ESP8266.  On the ESP-12E GPIO14
is immediately adjacent to GND allowing a jumper to be installed between the
pins to easily enable configuration mode.  Remove the jumper to allow normal
boot.  From a running Mylo server, accessing /erase.xml will also erase the
EEPROM configuration and restart the ESP8266 forcing it to enter config mode.

To program the ESP8266 use the Arduino IDE following the instructions here:

http://arduino-esp8266.readthedocs.io/en/latest/installing.html

Also enable FileSystem support following these directions:

http://arduino-esp8266.readthedocs.io/en/latest/filesystem.html

The MQTT "PubSubClient" library is also necessary.  This can be installed
via the Library Manager in the Arduino IDE.  (current version 2.6.0)

Mylo is developed with Arduino IDE version 1.8.3.

Board selection is NodeMCU 1.0 (ESP-12E Module), 160MHZ CPU, 4M (1M SPIFFS),
upload speed 921600 (optional for faster programming).

Upload the data files using "ESP8266 Sketch Data Upload" and the sketch
itself using the normal upload mechanism over USB serial.
