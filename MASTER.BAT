@echo off
REM MASTER.BAT -- run this on the server/host device.
REM Starts the IPX tunnel server on a specific port (must match SLAVE.BAT).
REM Change the name after /COORD (or drop /COORD entirely if this device
REM should be a participant instead -- see the notes at the bottom).

IPXNET STARTSERVER 1026
IPXDIAG /COORD PC

REM NOTE: broadcast discovery (the "R" menu option) does not work over
REM this tunnel in our testing -- use "A" to manually add participants
REM by address instead, regardless of which device runs as server vs
REM client, or which device runs the IPXDIAG coordinator role.
