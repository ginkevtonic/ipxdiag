@echo off
REM MASTER.BAT -- run this on the master/coordinator phone.
REM Starts the IPX server, then runs IPXDIAG as the coordinator.
REM Change PC to whatever name you want this device to show up as.

IPXNET STARTSERVER 1026
IPXDIAG /COORD PC
