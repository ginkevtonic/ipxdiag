/* ipx.c -- see ipx.h for notes/caveats
 *
 * Rewritten to call INT 7Ah directly for every IPX function, instead of
 * resolving a far entry point via INT 2F/7A00 and calling through it.
 * The entry-point approach compiled fine but hung indefinitely on the
 * very first real call (IPX_OpenSocket) under DOSBox -- INT 7Ah is the
 * baseline interrupt-based interface every IPX-compatible stack must
 * support correctly (unlike the entry-point shortcut, which is more of
 * an optional optimization some stacks implement), so it's the safer
 * bet. Also sidesteps the far-pointer calling-convention issues that
 * caused most of the earlier build trouble.
 *
 * Earlier gotcha, still relevant: don't name a local variable `seg`
 * (or other x86 assembler reserved words) if you reference it inside
 * an _asm block -- Watcom's inline assembler reads `seg` as the SEG
 * operator, not your variable, producing a confusing "Operand is
 * expected" error rather than an "undeclared identifier" one.
 */

#include <string.h>
#include <dos.h>
#include "ipx.h"

static int haveIpx = 0;

int IPX_Detect(void)
{
    unsigned char installed;

    _asm {
        mov ax, 0x7A00
        int 0x2F
        mov installed, al
    }

    haveIpx = (installed == 0xFF);
    return haveIpx;
}

int IPX_OpenSocket(unsigned short *socketNum)
{
    unsigned short sn = *socketNum;
    unsigned char  al_result = 0xFF;

    /* BX=0 Open Socket. AL=socket type (0=unrestricted).
     * DX = socket number, BIG-ENDIAN, 0 = request dynamic assignment.
     * Returns AL=completion code, DX=assigned socket (still big-endian). */
    unsigned short dx_in = ((sn & 0xFF) << 8) | (sn >> 8);   /* host->BE */

    _asm {
        mov bx, 0
        mov al, 0
        mov dx, dx_in
        int 0x7A
        mov al_result, al
        mov dx_in, dx
    }

    if (al_result == 0) {
        /* dx_in came back big-endian; convert to host order */
        sn = ((dx_in & 0xFF) << 8) | (dx_in >> 8);
        *socketNum = sn;
        return 1;
    }
    return 0;
}

void IPX_CloseSocket(unsigned short socketNum)
{
    unsigned short dx_in = ((socketNum & 0xFF) << 8) | (socketNum >> 8);
    _asm {
        mov bx, 1
        mov dx, dx_in
        int 0x7A
    }
}

void IPX_GetLocalAddress(unsigned char netOut[4], unsigned char nodeOut[6])
{
    /* BX=9 Get Internetwork Address. ES:SI -> 10-byte buffer
     * (4 bytes network, 6 bytes node) to be filled in. */
    unsigned char buf[10];
    void far *bufptr = (void far *)buf;
    unsigned short bufSeg = FP_SEG(bufptr);
    unsigned short bufOff = FP_OFF(bufptr);

    _asm {
        mov bx, 9
        mov es, bufSeg
        mov si, bufOff
        int 0x7A
    }

    memcpy(netOut, buf, 4);
    memcpy(nodeOut, buf + 4, 6);
}

int IPX_GetLocalTarget(const unsigned char destNet[4],
                        const unsigned char destNode[6],
                        unsigned short destSocket,
                        unsigned char immOut[6])
{
    /* BX=2 Get Local Target. ES:SI -> 12-byte IPXAddress (net+node+socket,
     * socket BIG-ENDIAN), ES:DI (some impls: separate buffer) -> 6-byte
     * immediate address out, AX <- estimated ticks to destination.
     * We build the request buffer + a scratch out buffer contiguously. */
    unsigned char req[12];
    unsigned char out[6];
    void far *reqptr = (void far *)req;
    void far *outptr = (void far *)out;
    unsigned short reqseg = FP_SEG(reqptr), reqoff = FP_OFF(reqptr);
    unsigned short outseg = FP_SEG(outptr), outoff = FP_OFF(outptr);
    unsigned short beSocket = ((destSocket & 0xFF) << 8) | (destSocket >> 8);
    unsigned char al_result = 0xFF;

    memcpy(req, destNet, 4);
    memcpy(req + 4, destNode, 6);
    req[10] = (unsigned char)(beSocket >> 8);
    req[11] = (unsigned char)(beSocket & 0xFF);

    _asm {
        mov bx, 2
        mov es, reqseg
        mov si, reqoff
        push es
        push si
        mov es, outseg
        mov di, outoff
        int 0x7A
        mov al_result, al
        pop si
        pop es
    }

    if (al_result == 0) {
        memcpy(immOut, out, 6);
        return 1;
    }
    /* Fallback: many stacks (incl. DOSBox's) accept destNode itself as the
     * immediate address on a flat/local segment -- use it directly. */
    memcpy(immOut, destNode, 6);
    return 0;
}

void IPX_SendECB(IPX_ECB far *ecb)
{
    unsigned short ecbSeg = FP_SEG(ecb);
    unsigned short ecbOff = FP_OFF(ecb);
    _asm {
        mov bx, 3
        mov es, ecbSeg
        mov si, ecbOff
        int 0x7A
    }
}

void IPX_ListenECB(IPX_ECB far *ecb)
{
    unsigned short ecbSeg = FP_SEG(ecb);
    unsigned short ecbOff = FP_OFF(ecb);
    _asm {
        mov bx, 4
        mov es, ecbSeg
        mov si, ecbOff
        int 0x7A
    }
}
