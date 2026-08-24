@echo off
REM SLAVE.BAT -- run this on each client device.
REM EDIT THE IP BELOW: replace 192.168.1.100 with the server device's
REM actual Wi-Fi IP address (Android Settings -> Wi-Fi -> tap network).
REM Port 1026 must match whatever MASTER.BAT used for STARTSERVER.
REM Change Phone1 to a distinct name per device if running more than one.

IPXNET CONNECT 192.168.1.68 1026
IPXDIAG Phone1
