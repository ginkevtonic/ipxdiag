/* protocol.h -- IPXDIAG wire protocol */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#define DIAG_SOCKET   0x5050    /* host byte order; converted when sent */
#define MAX_NODES     8
#define NAME_LEN      16
#define DATA_CAP      400       /* keeps whole DiagPkt well under 546 bytes */

#define MSG_DISCOVER      1   /* coordinator -> broadcast */
#define MSG_HELLO         2   /* participant -> coordinator, reply to DISCOVER */
#define MSG_PING          3   /* latency test request */
#define MSG_PONG          4   /* latency test reply */
#define MSG_BURST         5   /* throughput test data packet */
#define MSG_BURST_REPORT  6   /* receiver -> sender: loss/throughput summary */
#define MSG_SIM           7   /* simulated game-traffic packet */
#define MSG_SIM_ACK       8   /* ack, used only by lockstep-style profiles */
#define MSG_SIM_REPORT    9   /* receiver -> sender: sim summary */
#define MSG_PAIRTEST_CMD  10  /* coordinator -> participant: "test node X" */
#define MSG_RESULT        11  /* participant -> coordinator: final numbers */
#define MSG_DONE          12  /* coordinator -> all: test run complete */

#define TEST_PING   1
#define TEST_BURST  2
#define TEST_SIM    3

#pragma pack(push, 1)
typedef struct {
    unsigned char  msgType;
    unsigned char  fromNode;      /* roster index, 0xFF = coordinator/unknown */
    unsigned char  toNode;        /* target roster index for PAIRTEST_CMD */
    unsigned char  testType;      /* TEST_PING / TEST_BURST / TEST_SIM */
    unsigned char  profile;       /* sim profile id, see below */
    unsigned char  targetNet[4];  /* filled by coordinator for PAIRTEST_CMD */
    unsigned char  targetNode[6];
    unsigned long  seqNum;
    unsigned long  timestamp;     /* sender's Timer_Ms() at send */
    unsigned short dataLen;
    long           resultA;       /* generic result slots, meaning depends */
    long           resultB;       /* on msgType (see diag.c) */
    long           resultC;
    char           name[NAME_LEN];
    unsigned char  data[DATA_CAP];
} DiagPkt;
#pragma pack(pop)

/* Game-traffic simulation profiles: packet size + send interval + duration.
 * lockstep=1 means sender waits for MSG_SIM_ACK before sending the next
 * packet (models Magic Carpet/Syndicate Wars-style synchronised sim ticks);
 * lockstep=0 streams at the fixed interval regardless of acks (models
 * Doom/Duke3D-style continuous state broadcast). Tune sizes/intervals to
 * match real captures if you have them -- these are reasonable starting
 * guesses based on each game's known netcode style, not measured traces. */
typedef struct {
    const char *label;
    unsigned short packetSize;
    unsigned short intervalMs;
    unsigned short durationSec;
    unsigned char  lockstep;
} SimProfile;

#define NUM_SIM_PROFILES 4
static const SimProfile SIM_PROFILES[NUM_SIM_PROFILES] = {
    /* label                  size  interval  duration  lockstep */
    { "Doom/Duke3D-style",     34,      28,      15,       0 },
    { "MagicCarpet-lockstep",  96,      50,      15,       1 },
    { "SyndicateWars-lockstep",128,     40,      15,       1 },
    { "Carmageddon-fast",      64,      20,      15,       0 },
};

#endif
