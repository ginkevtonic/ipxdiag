@echo off
REM SLAVE.BAT -- run this on each slave/participant phone.
REM EDIT THE LINE BELOW: replace 192.168.1.100 with the master phone's
REM actual Wi-Fi IP address (Android Settings -> Wi-Fi -> tap network).
REM Also change Phone1 to a distinct name per phone if running more than one.

IPXNET CONNECT 192.168.1.68 1026
IPXDIAG Phone1
