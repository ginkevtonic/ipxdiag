/* ipx.h -- Minimal IPX API wrapper for OpenWatcom (16-bit real mode DOS)
 *
 * Implements the classic Novell far-call IPX API accessed via INT 2Fh/7Ah.
 * Register conventions follow the widely-documented Novell IPX spec
 * (see Ralf Brown's Interrupt List, INT 7Ah, for the authoritative
 * reference if anything here needs cross-checking against a real stack).
 *
 * NOTE: written for OpenWatcom's inline assembler in 16-bit real mode
 * (wcc -bt=dos, small/medium model). Not tested on real hardware/DOSBox --
 * verify ECB/ANSI struct packing and calling convention on your target
 * before trusting timing results.
 */

#ifndef IPX_H
#define IPX_H

#pragma pack(push, 1)

typedef struct {
    unsigned char network[4];   /* big-endian network number, 0 = local */
    unsigned char node[6];      /* 6-byte node address; all FF = broadcast */
    unsigned short socket;      /* BIG-ENDIAN socket number */
} IPXAddress;

/* Event Control Block -- must match Novell's ECB layout exactly */
typedef struct {
    unsigned long  link;                 /* reserved, zero it */
    void far       *ESRAddress;          /* NULL = no event service routine */
    unsigned char  inUseFlag;
    unsigned char  completionCode;
    unsigned short socketNumber;         /* BIG-ENDIAN */
    unsigned char  IPXWorkspace[4];
    unsigned char  driverWorkspace[12];
    unsigned char  immediateAddress[6];  /* physical/immediate node addr */
    unsigned short fragmentCount;
    void far       *fragAddress0;
    unsigned short fragSize0;
    void far       *fragAddress1;
    unsigned short fragSize1;
} IPX_ECB;

/* 30-byte IPX packet header -- must be fragment 0 of every send ECB */
typedef struct {
    unsigned short checksum;      /* 0xFFFF = checksum disabled */
    unsigned short length;        /* driver fills this in on send */
    unsigned char  transportControl;
    unsigned char  packetType;    /* use 4 = "packet exchange"/generic */
    unsigned char  destNetwork[4];
    unsigned char  destNode[6];
    unsigned short destSocket;    /* BIG-ENDIAN */
    unsigned char  srcNetwork[4];
    unsigned char  srcNode[6];
    unsigned short srcSocket;     /* BIG-ENDIAN */
} IPX_Header;

#pragma pack(pop)

/* completion codes we care about */
#define IPX_CC_SUCCESS        0x00
#define IPX_CC_CANCELLED      0xFC
#define IPX_CC_MALFORMED      0xFD
#define IPX_CC_NOT_DELIVERED  0xFE
#define IPX_CC_INCOMPLETE     0xFF   /* still pending -- not an error */

int  IPX_Detect(void);
int  IPX_OpenSocket(unsigned short *socketNum);   /* pass 0 for dynamic */
void IPX_CloseSocket(unsigned short socketNum);
void IPX_GetLocalAddress(unsigned char netOut[4], unsigned char nodeOut[6]);
int  IPX_GetLocalTarget(const unsigned char destNet[4],
                         const unsigned char destNode[6],
                         unsigned short destSocket,
                         unsigned char immOut[6]);

/* Fire-and-check-later async wrappers around SendPacket/ListenForPacket.
 * Caller owns the ECB + header + data buffer memory and must not move/free
 * it until ecb->inUseFlag == 0. */
void IPX_SendECB(IPX_ECB far *ecb);
void IPX_ListenECB(IPX_ECB far *ecb);

#endif
