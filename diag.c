/* diag.c -- IPXDIAG: multi-node IPX network diagnostic suite
 *
 * Usage:
 *   IPXDIAG /COORD [name]     run as the coordinator (drives the tests)
 *   IPXDIAG [name]            run as a participant (waits, responds,
 *                              and executes tests when instructed to)
 *
 * Design summary (see the chat writeup for the full rationale):
 *  - Every node auto-responds to PING/BURST/SIM traffic addressed to it,
 *    no matter who sent it -- that's what lets any pair be tested.
 *  - The coordinator can either test itself against a participant, or
 *    instruct one participant to test against another (MSG_PAIRTEST_CMD)
 *    and relay the result back (MSG_RESULT). This is what covers the
 *    "phone vs phone" case even though your PC is running the menu.
 *  - Round-trip numbers never depend on clocks being synced across
 *    devices: PING RTT is measured on the requester using its own
 *    send/receive timestamps; BURST/SIM elapsed time is measured on
 *    the *receiver* between its first and last packet, using only its
 *    own clock.
 *
 * NOT compiled/tested here (no DOS/Watcom toolchain in this sandbox) --
 * build and debug against your actual DOSBox IPX setup. See README.md.
 *
 * Build (OpenWatcom, 16-bit real mode, small model):
 *   wcl -0 -bt=dos -ms diag.c ipx.c timer.c -fe=IPXDIAG.EXE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include "ipx.h"
#include "timer.h"
#include "protocol.h"

#define RX_POOL       4
#define PKT_HDR_LEN   ((int)(sizeof(DiagPkt) - DATA_CAP))

typedef struct {
    IPXAddress addr;
    char       name[NAME_LEN];
    int        valid;
} RosterEntry;

typedef struct {
    unsigned long sent, recv;
    unsigned long minMs, maxMs, sumMs;
    unsigned long elapsedMs;   /* receiver-measured span, for burst/sim */
} TestStats;

/* ---- globals ---- */
static IPXAddress  myAddr;
static char        myName[NAME_LEN];
static unsigned short mySocket = DIAG_SOCKET;
static int          isCoordinator = 0;
static unsigned char myIndex = 0xFF;

static RosterEntry  roster[MAX_NODES];
static int           nodeCount = 0;

static IPX_ECB       txEcb;
static IPX_Header     txHdr;
static DiagPkt        txPktBuf;

static IPX_ECB        rxEcb[RX_POOL];
static IPX_Header     rxHdr[RX_POOL];
static DiagPkt         rxPktBuf[RX_POOL];

/* "waiting for a specific reply" mechanism, reused by every test type */
static int            expectPending = 0;
static unsigned char  expectMsgType = 0;
static DiagPkt         expectPkt;
static int             expectGot = 0;

/* passive burst/sim receive tracking -- only one test assumed in flight
 * at a time; don't overlap tests against the same node. */
static unsigned long  passRecvCount = 0;
static unsigned long  passFirstMs = 0;
static IPXAddress      passFrom;

static FILE *logFile = NULL;

static unsigned short BE16(unsigned short x)
{
    return (unsigned short)(((x & 0xFF) << 8) | (x >> 8));
}

/* ---------------------------------------------------------------- */
static void PollReceive(void);   /* fwd decl */

static void SendPkt(const IPXAddress *dest, DiagPkt *pkt)
{
    unsigned char imm[6];
    unsigned short wireLen;

    while (txEcb.inUseFlag != 0) {
        PollReceive();
    }

    if (pkt->dataLen > DATA_CAP) pkt->dataLen = DATA_CAP;
    wireLen = (unsigned short)(PKT_HDR_LEN + pkt->dataLen);

    memcpy(&txPktBuf, pkt, wireLen);
    strncpy(txPktBuf.name, myName, NAME_LEN - 1);
    txPktBuf.fromNode = myIndex;

    memset(&txHdr, 0, sizeof(txHdr));
    txHdr.checksum = 0xFFFF;
    txHdr.transportControl = 0;
    {
        int isBroadcast = (dest->node[0] == 0xFF && dest->node[1] == 0xFF &&
                            dest->node[2] == 0xFF && dest->node[3] == 0xFF &&
                            dest->node[4] == 0xFF && dest->node[5] == 0xFF);
        /* Broadcasts use packet type 20 (0x14), historically reserved for
         * NetBIOS broadcast traffic -- some IPX tunnels (possibly incl.
         * DOSBox's) only relay broadcasts of that specific type rather
         * than any arbitrary packet type. Unicast traffic keeps type 4
         * ("packet exchange"), which is already confirmed working. */
        txHdr.packetType = isBroadcast ? 20 : 4;
    }
    memcpy(txHdr.destNetwork, dest->network, 4);
    memcpy(txHdr.destNode, dest->node, 6);
    txHdr.destSocket = BE16(dest->socket);
    memcpy(txHdr.srcNetwork, myAddr.network, 4);
    memcpy(txHdr.srcNode, myAddr.node, 6);
    txHdr.srcSocket = BE16(mySocket);

    IPX_GetLocalTarget(dest->network, dest->node, dest->socket, imm);

    memset(&txEcb, 0, sizeof(txEcb));
    txEcb.socketNumber = BE16(mySocket);
    memcpy(txEcb.immediateAddress, imm, 6);
    txEcb.fragmentCount = 2;
    txEcb.fragAddress0 = (void far *)&txHdr;
    txEcb.fragSize0 = sizeof(IPX_Header);
    txEcb.fragAddress1 = (void far *)&txPktBuf;
    txEcb.fragSize1 = wireLen;

    IPX_SendECB(&txEcb);

    /* debug: wait briefly for the send to actually complete and report it */
    {
        unsigned long waitStart = Timer_Ms();
        while (txEcb.inUseFlag != 0 && (Timer_Ms() - waitStart) < 2000) {
            /* just spin -- don't call PollReceive here, we want to isolate
             * pure send completion without interference */
        }
        printf("TX: type=%d to node=%02X:%02X:%02X:%02X:%02X:%02X inUse=%d code=%02X\n",
               pkt->msgType,
               dest->node[0], dest->node[1], dest->node[2],
               dest->node[3], dest->node[4], dest->node[5],
               txEcb.inUseFlag, txEcb.completionCode);
        fflush(stdout);
    }
}

static void SendBroadcast(DiagPkt *pkt)
{
    IPXAddress bcast;
    memset(&bcast, 0, sizeof(bcast));
    memcpy(bcast.network, myAddr.network, 4);   /* use OUR assigned network,
                                                    not a hardcoded 0 -- some
                                                    tunnels won't route a
                                                    broadcast with network=0 */
    memset(bcast.node, 0xFF, 6);
    bcast.socket = DIAG_SOCKET;
    SendPkt(&bcast, pkt);
}

/* ---------------------------------------------------------------- */
static void AddToRoster(const IPXAddress *addr, const char *name)
{
    int i;
    for (i = 0; i < nodeCount; i++) {
        if (memcmp(roster[i].addr.node, addr->node, 6) == 0) return; /* dup */
    }
    if (nodeCount >= MAX_NODES) return;
    roster[nodeCount].addr = *addr;
    strncpy(roster[nodeCount].name, name, NAME_LEN - 1);
    roster[nodeCount].name[NAME_LEN - 1] = 0;
    roster[nodeCount].valid = 1;
    nodeCount++;
}

static void LogOpen(void)
{
    int isNew;
    FILE *probe = fopen("RESULTS.CSV", "r");
    isNew = (probe == NULL);
    if (probe) fclose(probe);

    logFile = fopen("RESULTS.CSV", "a");
    if (logFile && isNew) {
        fprintf(logFile,
            "Time,Test,Profile,From,To,Sent,Recv,LossPct,MinMs,AvgMs,MaxMs,ElapsedMs,ThroughputBps\n");
        fflush(logFile);
    }
}

static void LogRow(const char *test, const char *profile, const char *from,
                    const char *to, TestStats *s, unsigned long throughputBps)
{
    struct dostime_t t;
    double lossPct = s->sent ? (100.0 * (s->sent - s->recv) / s->sent) : 0.0;
    double avgMs = s->recv ? ((double)s->sumMs / s->recv) : 0.0;

    _dos_gettime(&t);
    if (!logFile) return;

    fprintf(logFile, "%02u:%02u:%02u,%s,%s,%s,%s,%lu,%lu,%.1f,%lu,%.1f,%lu,%lu,%lu\n",
            t.hour, t.minute, t.second, test, profile, from, to,
            s->sent, s->recv, lossPct, s->minMs, avgMs, s->maxMs,
            s->elapsedMs, throughputBps);
    fflush(logFile);

    printf("  sent=%lu recv=%lu loss=%.1f%% min/avg/max=%lu/%.1f/%lu ms elapsed=%lums thpt=%lu Bps\n",
           s->sent, s->recv, lossPct, s->minMs, avgMs, s->maxMs,
           s->elapsedMs, throughputBps);
}

/* ---------------------------------------------------------------- */
static int WaitFor(unsigned char msgType, unsigned long timeoutMs, DiagPkt *out)
{
    unsigned long start = Timer_Ms();
    expectMsgType = msgType;
    expectGot = 0;
    expectPending = 1;

    while (!expectGot && (Timer_Ms() - start) < timeoutMs) {
        PollReceive();
    }
    expectPending = 0;

    if (expectGot) {
        *out = expectPkt;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
static void RunPingTest(const IPXAddress *target, int count, TestStats *st)
{
    int i;
    memset(st, 0, sizeof(*st));
    st->minMs = 0xFFFFFFFFUL;

    for (i = 0; i < count; i++) {
        DiagPkt pkt, reply;
        unsigned long sendTime;

        memset(&pkt, 0, sizeof(pkt));
        pkt.msgType = MSG_PING;
        pkt.seqNum = (unsigned long)i;
        pkt.dataLen = 0;
        sendTime = Timer_Ms();
        pkt.timestamp = sendTime;
        SendPkt(target, &pkt);
        st->sent++;

        if (WaitFor(MSG_PONG, 2000, &reply)) {
            unsigned long rtt = Timer_Ms() - sendTime;
            st->recv++;
            if (rtt < st->minMs) st->minMs = rtt;
            if (rtt > st->maxMs) st->maxMs = rtt;
            st->sumMs += rtt;
        }

        /* small gap so we're measuring latency, not just saturating the link */
        { unsigned long t0 = Timer_Ms(); while (Timer_Ms() - t0 < 50) PollReceive(); }
    }
    if (st->minMs == 0xFFFFFFFFUL) st->minMs = 0;
}

static void RunBurstTest(const IPXAddress *target, unsigned short pktSize,
                          unsigned long count, TestStats *st)
{
    unsigned long i;
    DiagPkt pkt, report;
    unsigned long startSend;

    memset(st, 0, sizeof(*st));
    startSend = Timer_Ms();

    for (i = 0; i < count; i++) {
        memset(&pkt, 0, sizeof(pkt));
        pkt.msgType = MSG_BURST;
        pkt.seqNum = i;
        pkt.dataLen = (pktSize > PKT_HDR_LEN) ? (pktSize - PKT_HDR_LEN) : 0;
        pkt.timestamp = Timer_Ms();
        SendPkt(target, &pkt);
        st->sent++;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.msgType = MSG_BURST;
    pkt.seqNum = 0xFFFFFFFFUL;
    pkt.resultA = (long)count;
    SendPkt(target, &pkt);

    if (WaitFor(MSG_BURST_REPORT, 5000, &report)) {
        st->sent = (unsigned long)report.resultA;
        st->recv = (unsigned long)report.resultB;
        st->elapsedMs = (unsigned long)report.resultC;
    }
}

static void RunSimTest(const IPXAddress *target, int profileId, TestStats *st)
{
    const SimProfile *p = &SIM_PROFILES[profileId];
    unsigned long endTime = Timer_Ms() + ((unsigned long)p->durationSec * 1000UL);
    unsigned long seq = 0;
    DiagPkt pkt, ack, report;

    memset(st, 0, sizeof(*st));

    while (Timer_Ms() < endTime) {
        unsigned long tickStart = Timer_Ms();

        memset(&pkt, 0, sizeof(pkt));
        pkt.msgType = MSG_SIM;
        pkt.profile = (unsigned char)profileId;
        pkt.seqNum = seq++;
        pkt.dataLen = (p->packetSize > PKT_HDR_LEN) ? (p->packetSize - PKT_HDR_LEN) : 0;
        pkt.timestamp = tickStart;
        SendPkt(target, &pkt);
        st->sent++;

        if (p->lockstep) {
            /* real lockstep games stall the whole sim until the peer acks --
             * this measures exactly that stall. */
            WaitFor(MSG_SIM_ACK, 1000, &ack);
        } else {
            unsigned long elapsed = Timer_Ms() - tickStart;
            while (elapsed < p->intervalMs) {
                PollReceive();
                elapsed = Timer_Ms() - tickStart;
            }
        }
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.msgType = MSG_SIM;
    pkt.profile = (unsigned char)profileId;
    pkt.seqNum = 0xFFFFFFFFUL;
    pkt.resultA = (long)st->sent;
    SendPkt(target, &pkt);

    if (WaitFor(MSG_SIM_REPORT, 5000, &report)) {
        st->sent = (unsigned long)report.resultA;
        st->recv = (unsigned long)report.resultB;
        st->elapsedMs = (unsigned long)report.resultC;
    }
}

/* ---------------------------------------------------------------- */
static void ProcessPacket(DiagPkt *pkt, IPXAddress *from)
{
    if (expectPending && pkt->msgType == expectMsgType) {
        expectPkt = *pkt;
        expectGot = 1;
    }

    switch (pkt->msgType) {

    case MSG_DISCOVER:
        if (!isCoordinator) {
            DiagPkt reply;
            memset(&reply, 0, sizeof(reply));
            reply.msgType = MSG_HELLO;
            SendPkt(from, &reply);
            printf("[%s] discovered by coordinator, replied.\n", myName);
        }
        break;

    case MSG_HELLO:
        if (isCoordinator) AddToRoster(from, pkt->name);
        break;

    case MSG_PING: {
        DiagPkt reply;
        memset(&reply, 0, sizeof(reply));
        reply.msgType = MSG_PONG;
        reply.seqNum = pkt->seqNum;
        SendPkt(from, &reply);
        break;
    }

    case MSG_BURST:
        if (pkt->seqNum == 0xFFFFFFFFUL) {
            DiagPkt report;
            memset(&report, 0, sizeof(report));
            report.msgType = MSG_BURST_REPORT;
            report.resultA = pkt->resultA;
            report.resultB = (long)passRecvCount;
            report.resultC = (long)(Timer_Ms() - passFirstMs);
            SendPkt(from, &report);
            passRecvCount = 0; passFirstMs = 0;
        } else {
            if (passRecvCount == 0) { passFirstMs = Timer_Ms(); passFrom = *from; }
            passRecvCount++;
        }
        break;

    case MSG_SIM:
        if (pkt->seqNum == 0xFFFFFFFFUL) {
            DiagPkt report;
            memset(&report, 0, sizeof(report));
            report.msgType = MSG_SIM_REPORT;
            report.resultA = pkt->resultA;
            report.resultB = (long)passRecvCount;
            report.resultC = (long)(Timer_Ms() - passFirstMs);
            SendPkt(from, &report);
            passRecvCount = 0; passFirstMs = 0;
        } else {
            if (passRecvCount == 0) { passFirstMs = Timer_Ms(); passFrom = *from; }
            passRecvCount++;
            if (SIM_PROFILES[pkt->profile].lockstep) {
                DiagPkt ack;
                memset(&ack, 0, sizeof(ack));
                ack.msgType = MSG_SIM_ACK;
                ack.seqNum = pkt->seqNum;
                SendPkt(from, &ack);
            }
        }
        break;

    case MSG_PAIRTEST_CMD: {
        /* coordinator told us (a participant) to actively test someone else */
        IPXAddress target;
        TestStats st;
        DiagPkt result;
        char profLabel[NAME_LEN];
        const char *testName = (pkt->testType == TEST_PING) ? "PING" :
                                (pkt->testType == TEST_BURST) ? "BURST" : "SIM";

        memset(&target, 0, sizeof(target));
        memcpy(target.network, pkt->targetNet, 4);
        memcpy(target.node, pkt->targetNode, 6);
        target.socket = DIAG_SOCKET;

        printf("[%s] running %s test (as instructed by coordinator)...\n", myName, testName);

        if (pkt->testType == TEST_PING) {
            RunPingTest(&target, 20, &st);
        } else if (pkt->testType == TEST_BURST) {
            RunBurstTest(&target, 512, 200, &st);
        } else {
            RunSimTest(&target, pkt->profile, &st);
        }

        printf("[%s] %s test done: sent=%lu recv=%lu -- sending result to coordinator.\n",
               myName, testName, st.sent, st.recv);

        memset(&result, 0, sizeof(result));
        result.msgType = MSG_RESULT;
        result.testType = pkt->testType;
        result.profile = pkt->profile;
        result.resultA = (long)st.sent;
        result.resultB = (long)st.recv;
        result.resultC = (long)((st.elapsedMs) ? st.elapsedMs : st.sumMs);
        result.seqNum = st.minMs;         /* piggyback min/max in spare fields */
        result.timestamp = st.maxMs;
        SendPkt(from, &result);
        (void)profLabel;
        break;
    }

    default:
        break;
    }
}

static void PollReceive(void)
{
    int i;
    for (i = 0; i < RX_POOL; i++) {
        if (rxEcb[i].inUseFlag == 0 && rxEcb[i].completionCode != 0xAA) {
            /* a completed (or never-posted) ECB -- 0xAA is our "already
             * drained, needs repost" sentinel set below */
            if (rxEcb[i].completionCode == IPX_CC_SUCCESS) {
                IPXAddress from;
                memset(&from, 0, sizeof(from));
                memcpy(from.network, rxHdr[i].srcNetwork, 4);
                memcpy(from.node, rxHdr[i].srcNode, 6);
                from.socket = BE16(rxHdr[i].srcSocket);
                printf("RX: type=%d from node=%02X:%02X:%02X:%02X:%02X:%02X\n",
                       rxPktBuf[i].msgType,
                       from.node[0], from.node[1], from.node[2],
                       from.node[3], from.node[4], from.node[5]);
                fflush(stdout);
                ProcessPacket(&rxPktBuf[i], &from);
            } else if (rxEcb[i].completionCode != IPX_CC_INCOMPLETE) {
                printf("RX ECB[%d] completed with code=%02X (not success)\n",
                       i, rxEcb[i].completionCode);
                fflush(stdout);
            }
            rxEcb[i].completionCode = 0xAA;   /* mark drained */

            /* repost */
            memset(&rxEcb[i].link, 0, sizeof(rxEcb[i].link));
            rxEcb[i].inUseFlag = 0;
            rxEcb[i].socketNumber = BE16(mySocket);
            rxEcb[i].fragmentCount = 2;
            rxEcb[i].fragAddress0 = (void far *)&rxHdr[i];
            rxEcb[i].fragSize0 = sizeof(IPX_Header);
            rxEcb[i].fragAddress1 = (void far *)&rxPktBuf[i];
            rxEcb[i].fragSize1 = sizeof(DiagPkt);
            IPX_ListenECB(&rxEcb[i]);
        }
    }
}

/* ---------------------------------------------------------------- */
static void DoDiscovery(void)
{
    DiagPkt pkt;
    unsigned long start, lastSend;

    nodeCount = 0;
    memset(&pkt, 0, sizeof(pkt));
    pkt.msgType = MSG_DISCOVER;

    printf("Listening for participants (5 seconds)...\n");
    start = Timer_Ms();
    lastSend = 0;
    while (Timer_Ms() - start < 5000) {
        if (Timer_Ms() - lastSend >= 1000) {
            SendBroadcast(&pkt);
            lastSend = Timer_Ms();
        }
        PollReceive();
    }

    printf("Found %d participant(s):\n", nodeCount);
    { int i; for (i = 0; i < nodeCount; i++)
        printf("  [%d] %s\n", i + 1, roster[i].name); }
}

static void PrintTestMenu(void)
{
    printf("\n--- IPXDIAG coordinator (%s) ---\n", myName);
    printf("  [0] %s (this PC)\n", myName);
    { int i; for (i = 0; i < nodeCount; i++)
        printf("  [%d] %s\n", i + 1, roster[i].name); }
    printf("\n R = re-run discovery (broadcast -- currently not working on your network)\n");
    printf(" A = manually add a participant by address\n");
    printf(" P = ping test between two nodes\n");
    printf(" B = burst/throughput test between two nodes\n");
    printf(" S = simulated game-traffic test between two nodes\n");
    printf(" Q = quit\n> ");
}

static const IPXAddress *AddrOf(int idx)
{
    static IPXAddress self;
    if (idx == 0) { self = myAddr; return &self; }
    return &roster[idx - 1].addr;
}
static const char *NameOf(int idx)
{
    if (idx == 0) return myName;
    return roster[idx - 1].name;
}

static void RunCoordinatorPair(int fromIdx, int toIdx, int testType, int profileId)
{
    TestStats st;
    unsigned long throughputBps = 0;
    char profLabel[NAME_LEN];
    const char *testName = (testType == TEST_PING) ? "PING" :
                            (testType == TEST_BURST) ? "BURST" : "SIM";

    strcpy(profLabel, (testType == TEST_SIM) ? SIM_PROFILES[profileId].label : "-");

    if (fromIdx == 0) {
        /* coordinator itself is the active tester */
        if (testType == TEST_PING) RunPingTest(AddrOf(toIdx), 20, &st);
        else if (testType == TEST_BURST) RunBurstTest(AddrOf(toIdx), 512, 200, &st);
        else RunSimTest(AddrOf(toIdx), profileId, &st);
    } else {
        /* delegate: tell participant `fromIdx` to test participant `toIdx` */
        DiagPkt cmd, result;
        const IPXAddress *targetAddr = AddrOf(toIdx);
        unsigned long timeout;

        memset(&cmd, 0, sizeof(cmd));
        cmd.msgType = MSG_PAIRTEST_CMD;
        cmd.testType = (unsigned char)testType;
        cmd.profile = (unsigned char)profileId;
        memcpy(cmd.targetNet, targetAddr->network, 4);
        memcpy(cmd.targetNode, targetAddr->node, 6);
        SendPkt(AddrOf(fromIdx), &cmd);

        timeout = (testType == TEST_SIM) ?
            ((unsigned long)SIM_PROFILES[profileId].durationSec * 1000UL + 8000UL) : 15000UL;

        if (WaitFor(MSG_RESULT, timeout, &result)) {
            memset(&st, 0, sizeof(st));
            st.sent = (unsigned long)result.resultA;
            st.recv = (unsigned long)result.resultB;
            st.elapsedMs = (unsigned long)result.resultC;
            st.minMs = result.seqNum;
            st.maxMs = (unsigned long)result.timestamp;
            st.sumMs = 0; /* avg not meaningful for relayed burst/sim */
        } else {
            printf("  (timed out waiting for relayed result)\n");
            return;
        }
    }

    if (st.elapsedMs > 0 && testType != TEST_PING) {
        throughputBps = (unsigned long)((double)st.recv * 512.0 * 1000.0 / st.elapsedMs);
    }

    printf("%s test: %s -> %s (%s)\n", testName, NameOf(fromIdx), NameOf(toIdx), profLabel);
    LogRow(testName, profLabel, NameOf(fromIdx), NameOf(toIdx), &st, throughputBps);
}

static int ReadIndex(const char *prompt)
{
    char buf[8];
    printf("%s", prompt);
    if (!gets(buf)) return -1;
    return atoi(buf);
}

static void ManualAddParticipant(void)
{
    char nameBuf[NAME_LEN];
    char addrBuf[64];
    unsigned int net[4], node[6];
    IPXAddress addr;

    if (nodeCount >= MAX_NODES) { printf("Roster is full.\n"); return; }

    printf("Name for this node: ");
    if (!gets(nameBuf)) return;

    printf("Its address as printed on its own screen\n");
    printf("(format: NN:NN:NN:NN:NN:NN:NN:NN:NN:NN -- 4 net bytes then 6 node bytes): ");
    if (!gets(addrBuf)) return;

    if (sscanf(addrBuf, "%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x",
               &net[0], &net[1], &net[2], &net[3],
               &node[0], &node[1], &node[2], &node[3], &node[4], &node[5]) != 10) {
        printf("Couldn't parse that address -- expected 10 hex byte pairs separated by colons.\n");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.network[0] = (unsigned char)net[0]; addr.network[1] = (unsigned char)net[1];
    addr.network[2] = (unsigned char)net[2]; addr.network[3] = (unsigned char)net[3];
    addr.node[0] = (unsigned char)node[0]; addr.node[1] = (unsigned char)node[1];
    addr.node[2] = (unsigned char)node[2]; addr.node[3] = (unsigned char)node[3];
    addr.node[4] = (unsigned char)node[4]; addr.node[5] = (unsigned char)node[5];
    addr.socket = DIAG_SOCKET;

    AddToRoster(&addr, nameBuf);
    printf("Added '%s' as node [%d].\n", nameBuf, nodeCount);
}

static void CoordinatorLoop(void)
{
    DoDiscovery();

    for (;;) {
        int c;
        PrintTestMenu();
        c = getch();
        printf("%c\n", c);

        if (c == 'q' || c == 'Q') break;
        if (c == 'r' || c == 'R') { DoDiscovery(); continue; }
        if (c == 'a' || c == 'A') { ManualAddParticipant(); continue; }

        if (c == 'p' || c == 'P' || c == 'b' || c == 'B' || c == 's' || c == 'S') {
            int from, to, profileId = 0;
            int testType = (c == 'p' || c == 'P') ? TEST_PING :
                            (c == 'b' || c == 'B') ? TEST_BURST : TEST_SIM;

            from = ReadIndex("From node #: ");
            to = ReadIndex("To node #: ");
            if (from < 0 || to < 0 || from > nodeCount || to > nodeCount || from == to) {
                printf("Invalid selection.\n");
                continue;
            }
            if (testType == TEST_SIM) {
                int i;
                printf("Profiles:\n");
                for (i = 0; i < NUM_SIM_PROFILES; i++)
                    printf("  [%d] %s (%u bytes, %ums interval, %s)\n", i,
                           SIM_PROFILES[i].label, SIM_PROFILES[i].packetSize,
                           SIM_PROFILES[i].intervalMs,
                           SIM_PROFILES[i].lockstep ? "lockstep" : "streaming");
                profileId = ReadIndex("Profile #: ");
                if (profileId < 0 || profileId >= NUM_SIM_PROFILES) {
                    printf("Invalid profile.\n"); continue;
                }
            }
            RunCoordinatorPair(from, to, testType, profileId);
        }
    }
}

static void ParticipantLoop(void)
{
    unsigned long lastBeat = Timer_Ms();
    printf("IPXDIAG participant '%s' running. Press ESC to quit.\n", myName);
    printf("Waiting");
    fflush(stdout);
    for (;;) {
        PollReceive();
        if (Timer_Ms() - lastBeat > 5000) {
            printf(".");
            fflush(stdout);
            lastBeat = Timer_Ms();
        }
        if (kbhit() && getch() == 27) { printf("\nExiting.\n"); break; }
    }
}

/* ---------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int argOff = 1;

    strcpy(myName, "NODE");
    if (argc > 1 && (stricmp(argv[1], "/COORD") == 0 || stricmp(argv[1], "-coord") == 0)) {
        isCoordinator = 1;
        argOff = 2;
    }
    if (argc > argOff) strncpy(myName, argv[argOff], NAME_LEN - 1);
    if (isCoordinator) myIndex = 0;

    printf("chk0: starting up\n"); fflush(stdout);

    if (!IPX_Detect()) {
        printf("IPX not detected -- load IPXODI (or DOSBox's IPX support) first.\n");
        return 1;
    }
    printf("chk1: IPX detected\n"); fflush(stdout);

    if (!IPX_OpenSocket(&mySocket)) {
        printf("Could not open IPX socket %04X.\n", mySocket);
        return 1;
    }
    printf("chk2: socket %04X opened\n", mySocket); fflush(stdout);

    IPX_GetLocalAddress(myAddr.network, myAddr.node);
    myAddr.socket = mySocket;
    printf("chk3: got local address\n"); fflush(stdout);

    Timer_Init();
    printf("chk4: timer initialized\n"); fflush(stdout);

    { int i; for (i = 0; i < RX_POOL; i++) rxEcb[i].completionCode = 0xAA; }
    PollReceive();   /* posts all RX_POOL listens for the first time */
    printf("chk5: listens posted\n"); fflush(stdout);

    LogOpen();
    printf("chk6: log opened\n"); fflush(stdout);

    printf("IPXDIAG -- node '%s', %s\n", myName, isCoordinator ? "COORDINATOR" : "participant");
    printf("My IPX address -- net %02X:%02X:%02X:%02X  node %02X:%02X:%02X:%02X:%02X:%02X\n",
           myAddr.network[0], myAddr.network[1], myAddr.network[2], myAddr.network[3],
           myAddr.node[0], myAddr.node[1], myAddr.node[2],
           myAddr.node[3], myAddr.node[4], myAddr.node[5]);

    if (isCoordinator) CoordinatorLoop();
    else ParticipantLoop();

    if (logFile) fclose(logFile);
    IPX_CloseSocket(mySocket);
    Timer_Shutdown();
    return 0;
}
