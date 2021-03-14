#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <errno.h>
#include <limits.h>

#include <sys/ioctl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <net/if.h>

#include <arpa/inet.h>

#include <getopt.h>

#include "rtsp_server.h"
#include "capture.h"
#include "cb_utils.h"

#define nalu_sent_len         14000
//#define nalu_sent_len         1400
#define pcma_sent_len         1024
#define RTP_H264              96
#define MAX_CHAN              2
#define RTP_AUDIO             97
#define MAX_RTSP_CLIENT       4
#define RTSP_SERVER_PORT      554
#define RTSP_RECV_SIZE        1024
#define RTSP_MAX_VID          (640*1024)
#define RTSP_MAX_AUD          (15*1024)

#define AU_HEADER_SIZE        4
#define PARAM_STRING_MAX      100

static SAMPLE_VENC_GETSTREAM_PARA_S gs_stPara;
static pthread_t gs_VencPid;
static SAMPLE_VENC_GETSTREAM_PARA_S gs_stPara2;
static pthread_t gs_AencPid;

typedef unsigned short u_int16_t;
typedef unsigned char u_int8_t;
typedef u_int16_t portNumBits;
typedef u_int32_t netAddressBits;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif

typedef enum {
    RTSP_IDLE = 0,
    RTSP_CONNECTED = 1,
    RTSP_SENDING = 2,
} RTSP_STATUS;

typedef enum {
    RTP_UDP,
    RTP_TCP,
    RAW_UDP
} StreamingMode;

typedef struct {
    int index;
    int socket;
    int reqchn;
    int seqnum;
    int seqnum2;
    unsigned int tsvid;
    unsigned int tsaud;
    int status;
    int sessionid;
    int rtpport[2];
    int rtcpport[2];
    char IP[20];
    char urlPre[PARAM_STRING_MAX];
} RTSP_CLIENT;

//static bool flag = true;
RTP_FIXED_HEADER *rtp_hdr;
NALU_HEADER      *nalu_hdr;
FU_INDICATOR     *fu_ind;
FU_HEADER        *fu_hdr;
AU_HEADER        *au_hdr;

RTSP_CLIENT g_rtspClients[MAX_RTSP_CLIENT];

int g_nSendDataChn = -1;
pthread_mutex_t g_mutex;
pthread_cond_t  g_cond;
pthread_mutex_t g_sendmutex;

pthread_t g_SendDataThreadId = 0;

char g_rtp_playload[20];

int g_nframerate;
int g_naudiorate;
int exitok = 0;
int udpfd;
int udpfd2;

/******************************************************************************/

int buf_offset;
int buf_size;
int frame_header_size;
int data_offset;
int lowres_byte;
int highres_byte;

cb_input_buffer input_buffer;
int debug = 0;

char nalu[131072];
char pcma[1024];

unsigned char IDR4[]               = {0x65, 0xB8};
unsigned char NALx_START[]         = {0x00, 0x00, 0x00, 0x01};
unsigned char IDR4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
unsigned char IDR5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x26};
unsigned char PFR4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x41};
unsigned char PFR5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x02};
unsigned char SPS4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x67};
unsigned char SPS5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x42};
unsigned char PPS4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x68};
unsigned char PPS5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x44};
unsigned char VPS5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x40};

unsigned char SPS4_640X360[]       = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x14,
                                      0x96, 0x54, 0x05, 0x01, 0x7B, 0xCB, 0x37, 0x01,
                                      0x01, 0x01, 0x02};
// As above but without nalu header and with timing info at 20 fps
unsigned char SPS4_640X360_TI[]    = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x14,
                                      0x96, 0x54, 0x05, 0x01, 0x7B, 0xCB, 0x37, 0x01,
                                      0x01, 0x01, 0x40, 0x00, 0x00, 0x7D, 0x00, 0x00,
                                      0x13, 0x88, 0x21};
unsigned char SPS4_1920X1080[]     = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x20,
                                      0x96, 0x54, 0x03, 0xC0, 0x11, 0x2F, 0x2C, 0xDC,
                                      0x04, 0x04, 0x04, 0x08};
// As above but without nalu header and with timing info at 20 fps
unsigned char SPS4_1920X1080_TI[]  = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x20,
                                      0x96, 0x54, 0x03, 0xC0, 0x11, 0x2F, 0x2C, 0xDC,
                                      0x04, 0x04, 0x05, 0x00, 0x00, 0x03, 0x01, 0xF4,
                                      0x00, 0x00, 0x4E, 0x20, 0x84};
unsigned char VPS5_1920X1080[]     = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01,
                                      0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
                                      0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
                                      0x00, 0x7B, 0xAC, 0x09};
// As above without nalu prefix
unsigned char VPS5_1920X1080_N[]   = {0x40, 0x01, 0x0C, 0x01,
                                      0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
                                      0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
                                      0x00, 0x7B, 0xAC, 0x09};
// As above but with timing info at 20 fps
unsigned char VPS5_1920X1080_TI[]  = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01,
                                      0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
                                      0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
                                      0x00, 0x7B, 0xAC, 0x0C, 0x00, 0x00, 0x0F, 0xA4,
                                      0x00, 0x01, 0x38, 0x81, 0x40};

int model;
//int resolution;
int audio;
int port;
//int debug;

static char const* dateHeader()
{
    static char buf[200];
    time_t tt = time(NULL);
    strftime(buf, sizeof buf, "Date: %a, %b %d %Y %H:%M:%S GMT\r\n", gmtime(&tt));

    return buf;
}

static char *GetLocalIP(int sock)
{
    struct ifreq ifreq;
    struct sockaddr_in *sin;
    char * LocalIP = malloc(20);
    strcpy(ifreq.ifr_name,"wlan0");
    if (!(ioctl (sock, SIOCGIFADDR, &ifreq))) {
        sin = (struct sockaddr_in *) &ifreq.ifr_addr;
        sin->sin_family = AF_INET;
        strcpy(LocalIP, inet_ntoa(sin->sin_addr));
        //inet_ntop(AF_INET, &sin->sin_addr, LocalIP, 16);
    }
    printf("--------------------------------------------%s\n",LocalIP);
    return LocalIP;
}

char *strDupSize(char const *str)
{
    if (str == NULL) return NULL;
    size_t len = strlen(str) + 1;
    char* copy = malloc(len);

    return copy;
}

int ParseRequestString(char const *reqStr,
                       unsigned    reqStrSize,
                       char       *resultCmdName,
                       unsigned    resultCmdNameMaxSize,
                       char       *resultURLPreSuffix,
                       unsigned    resultURLPreSuffixMaxSize,
                       char       *resultURLSuffix,
                       unsigned    resultURLSuffixMaxSize,
                       char       *resultCSeq,
                       unsigned    resultCSeqMaxSize)
{
    // This parser is currently rather dumb; it should be made smarter #####
    // Read everything up to the first space as the command name:
    int parseSucceeded = FALSE;
    unsigned i;
    for (i = 0; i < resultCmdNameMaxSize-1 && i < reqStrSize; ++i) {
        char c = reqStr[i];
        if (c == ' ' || c == '\t') {
            parseSucceeded = TRUE;
            break;
        }
        resultCmdName[i] = c;
    }
    resultCmdName[i] = '\0';
    if (!parseSucceeded)
        return FALSE;

    // Skip over the prefix of any "rtsp://" or "rtsp:/" URL that follows:
    unsigned j = i+1;
    while (j < reqStrSize && (reqStr[j] == ' ' || reqStr[j] == '\t'))
        ++j; // skip over any additional white space

    for (j = i+1; j < reqStrSize-8; ++j) {
        if ((reqStr[j] == 'r' || reqStr[j] == 'R')
                && (reqStr[j+1] == 't' || reqStr[j+1] == 'T')
                && (reqStr[j+2] == 's' || reqStr[j+2] == 'S')
                && (reqStr[j+3] == 'p' || reqStr[j+3] == 'P')
                && reqStr[j+4] == ':' && reqStr[j+5] == '/') {
            j += 6;
            if (reqStr[j] == '/') {
                // This is a "rtsp://" URL; skip over the host:port part that follows:
                ++j;
                while (j < reqStrSize && reqStr[j] != '/' && reqStr[j] != ' ') ++j;
            } else {
                // This is a "rtsp:/" URL; back up to the "/":
                --j;
            }
            i = j;
            break;
        }
    }

    // Look for the URL suffix (before the following "RTSP/"):
    parseSucceeded = FALSE;
    unsigned k;
    for (k = i+1; k < reqStrSize-5; ++k) {
        if (reqStr[k] == 'R' && reqStr[k+1] == 'T' &&
                reqStr[k+2] == 'S' && reqStr[k+3] == 'P' && reqStr[k+4] == '/') {
            while (--k >= i && reqStr[k] == ' ') {} // go back over all spaces before "RTSP/"
            unsigned k1 = k;
            while (k1 > i && reqStr[k1] != '/' && reqStr[k1] != ' ') --k1;
            // the URL suffix comes from [k1+1,k]

            // Copy "resultURLSuffix":
            if (k - k1 + 1 > resultURLSuffixMaxSize) return FALSE; // there's no room
            unsigned n = 0, k2 = k1+1;
            while (k2 <= k) resultURLSuffix[n++] = reqStr[k2++];
            resultURLSuffix[n] = '\0';

            // Also look for the URL 'pre-suffix' before this:
            unsigned k3 = --k1;
            while (k3 > i && reqStr[k3] != '/' && reqStr[k3] != ' ') --k3;
            // the URL pre-suffix comes from [k3+1,k1]

            // Copy "resultURLPreSuffix":
            if (k1 - k3 + 1 > resultURLPreSuffixMaxSize) return FALSE; // there's no room
            n = 0; k2 = k3+1;
            while (k2 <= k1) resultURLPreSuffix[n++] = reqStr[k2++];
            resultURLPreSuffix[n] = '\0';

            i = k + 7; // to go past " RTSP/"
            parseSucceeded = TRUE;
            break;
        }
    }
    if (!parseSucceeded) return FALSE;

    // Look for "CSeq:", skip whitespace,
    // then read everything up to the next \r or \n as 'CSeq':
    parseSucceeded = FALSE;
    for (j = i; j < reqStrSize-5; ++j) {
        if (reqStr[j] == 'C' && reqStr[j+1] == 'S' && reqStr[j+2] == 'e' &&
                reqStr[j+3] == 'q' && reqStr[j+4] == ':') {
            j += 5;
            unsigned n;
            while (j < reqStrSize && (reqStr[j] ==  ' ' || reqStr[j] == '\t')) ++j;
            for (n = 0; n < resultCSeqMaxSize-1 && j < reqStrSize; ++n,++j) {
                char c = reqStr[j];
                if (c == '\r' || c == '\n') {
                    parseSucceeded = TRUE;
                    break;
                }

                resultCSeq[n] = c;
            }
            resultCSeq[n] = '\0';
            break;
        }
    }
    if (!parseSucceeded)
        return FALSE;

    return TRUE;
}

int OptionAnswer(char *cseq, int sock)
{
    if (sock != 0) {
        char buf[1024];
        memset(buf,0,1024);
        char *pTemp = buf;
        pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sPublic: %s\r\n\r\n",
            cseq,dateHeader(),"OPTIONS,DESCRIBE,SETUP,PLAY,PAUSE,TEARDOWN");

        int reg = send(sock, buf,strlen(buf),0);
        if(reg <= 0) {
            return FALSE;
        } else {
            printf(">>>>>%s\n",buf);
        }
        return TRUE;
    }
    return FALSE;
}

int DescribeAnswer(char *cseq, int sock, char *urlSuffix, char *recvbuf)
{
    if (sock != 0) {
        char sdpMsg[1024];
        char buf[2048];
        memset(buf,0,2048);
        memset(sdpMsg,0,1024);
        char*localip;
        localip = GetLocalIP(sock);

        char *pTemp = buf;
        pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n",cseq);
        pTemp += sprintf(pTemp,"%s",dateHeader());
        pTemp += sprintf(pTemp,"Content-Type: application/sdp\r\n");

        //TODO °ÑÒ»Ð©¹Ì¶šÖµžÄÎª¶¯Ì¬Öµ
        char *pTemp2 = sdpMsg;
        pTemp2 += sprintf(pTemp2,"v=0\r\n");
        pTemp2 += sprintf(pTemp2,"o=StreamingServer 3331435948 1116907222000 IN IP4 %s\r\n",localip);
        pTemp2 += sprintf(pTemp2,"s=H.264\r\n");
        pTemp2 += sprintf(pTemp2,"c=IN IP4 0.0.0.0\r\n");
        pTemp2 += sprintf(pTemp2,"t=0 0\r\n");
        pTemp2 += sprintf(pTemp2,"a=control:*\r\n");

        /*H264 TrackID=0 RTP_PT 96*/
        pTemp2 += sprintf(pTemp2,"m=video 0 RTP/AVP 96\r\n");
        pTemp2 += sprintf(pTemp2,"a=control:trackID=0\r\n");
        pTemp2 += sprintf(pTemp2,"a=rtpmap:96 H264/90000\r\n");
        pTemp2 += sprintf(pTemp2,"a=fmtp:96 packetization-mode=1; sprop-parameter-sets=%s\r\n", "AAABBCCC");

        if (audio) {
            /*Audio TrackID=1 RTP_PT 97*/
            pTemp2 += sprintf(pTemp2,"m=audio 0 RTP/AVP 97\r\n");
            pTemp2 += sprintf(pTemp2,"a=control:trackID=1\r\n");
            if (strcmp(g_rtp_playload,"AAC")==0) {
                pTemp2 += sprintf(pTemp2,"a=rtpmap:97 MPEG4-GENERIC/%d/2\r\n",16000);
                pTemp2 += sprintf(pTemp2,"a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1410\r\n");
            } else if (strcmp(g_rtp_playload,"PCM")==0) {
                pTemp2 += sprintf(pTemp2,"a=rtpmap:97 L16/%d\r\n", 16000);
            } else if (strcmp(g_rtp_playload,"PCMA")==0) {
                pTemp2 += sprintf(pTemp2,"a=rtpmap:97 PCMA/%d\r\n", 16000);
            } else {
                pTemp2 += sprintf(pTemp2,"a=rtpmap:97 G726-32/%d/1\r\n",8000);
                pTemp2 += sprintf(pTemp2,"a=fmtp:97 packetization-mode=1\r\n");
            }
        }

        pTemp += sprintf(pTemp,"Content-length: %d\r\n", strlen(sdpMsg));
        pTemp += sprintf(pTemp,"Content-Base: rtsp://%s/%s/\r\n\r\n",localip,urlSuffix);

        //printf("mem ready\n");
        strcat(pTemp, sdpMsg);
        free(localip);
        //printf("Describe ready sent\n");
        int re = send(sock, buf, strlen(buf),0);
        if (re <= 0) {
            return FALSE;
        } else {
            printf(">>>>>%s\n",buf);
        }
    }

    return TRUE;
}

void ParseTransportHeader(char const* buf,
                          StreamingMode* streamingMode,
                         char** streamingModeString,
                         char** destinationAddressStr,
                         u_int8_t* destinationTTL,
                         portNumBits* clientRTPPortNum, // if UDP
                         portNumBits* clientRTCPPortNum, // if UDP
                         unsigned char* rtpChannelId, // if TCP
                         unsigned char* rtcpChannelId // if TCP
                         )
{
    // Initialize the result parameters to default values:
    *streamingMode = RTP_UDP;
    *streamingModeString = NULL;
    *destinationAddressStr = NULL;
    *destinationTTL = 255;
    *clientRTPPortNum = 0;
    *clientRTCPPortNum = 1;
    *rtpChannelId = *rtcpChannelId = 0xFF;

    portNumBits p1, p2;
    unsigned ttl, rtpCid, rtcpCid;

    // First, find "Transport:"
    while (1) {
        if (*buf == '\0')
            return; // not found
        if (strncasecmp(buf, "Transport: ", 11) == 0)
            break;
        ++buf;
    }

    // Then, run through each of the fields, looking for ones we handle:
    char const* fields = buf + 11;
    char* field = strDupSize(fields);
    while (sscanf(fields, "%[^;]", field) == 1) {
        if (strcmp(field, "RTP/AVP/TCP") == 0) {
            *streamingMode = RTP_TCP;
        } else if (strcmp(field, "RAW/RAW/UDP") == 0 ||
            strcmp(field, "MP2T/H2221/UDP") == 0) {
            *streamingMode = RAW_UDP;
            //*streamingModeString = strDup(field);
        } else if (strncasecmp(field, "destination=", 12) == 0) {
            //delete[] destinationAddressStr;
            free(destinationAddressStr);
            //destinationAddressStr = strDup(field+12);
        } else if (sscanf(field, "ttl%u", &ttl) == 1) {
            *destinationTTL = (u_int8_t)ttl;
        } else if (sscanf(field, "client_port=%hu-%hu", &p1, &p2) == 2) {
            *clientRTPPortNum = p1;
            *clientRTCPPortNum = p2;
        } else if (sscanf(field, "client_port=%hu", &p1) == 1) {
            *clientRTPPortNum = p1;
            *clientRTCPPortNum = (*streamingMode == RAW_UDP) ? 0 : p1 + 1;
        } else if (sscanf(field, "interleaved=%u-%u", &rtpCid, &rtcpCid) == 2) {
            *rtpChannelId = (unsigned char) rtpCid;
            *rtcpChannelId = (unsigned char) rtcpCid;
        }

        fields += strlen(field);
        while (*fields == ';')
            ++fields; // skip over separating ';' chars
        if (*fields == '\0' || *fields == '\r' || *fields == '\n')
            break;
    }
    free(field);
}


int SetupAnswer(char *cseq, int sock, int SessionId, int trackId,
                char *urlSuffix, char *recvbuf,
                int *rtpport, int *rtcpport)
{
    int serverrtpport;
    int serverrtcpport;

    if (sock != 0) {
        char buf[1024];
        memset(buf,0,1024);

        StreamingMode streamingMode;
        char* streamingModeString; // set when RAW_UDP streaming is specified
        char* clientsDestinationAddressStr;
        u_int8_t clientsDestinationTTL;
        portNumBits clientRTPPortNum, clientRTCPPortNum;
        unsigned char rtpChannelId, rtcpChannelId;
        ParseTransportHeader(recvbuf, &streamingMode, &streamingModeString,
            &clientsDestinationAddressStr, &clientsDestinationTTL,
            &clientRTPPortNum, &clientRTCPPortNum,
            &rtpChannelId, &rtcpChannelId);

        //Port clientRTPPort(clientRTPPortNum);
        //Port clientRTCPPort(clientRTCPPortNum);
        *rtpport = clientRTPPortNum;
        *rtcpport = clientRTCPPortNum;

        struct sockaddr_in s_addr;
        int addr_len = sizeof(s_addr);
        if (trackId == 0) {
            getsockname(udpfd, (struct sockaddr *) &s_addr, &addr_len);
        } else {
            getsockname(udpfd2, (struct sockaddr *) &s_addr, &addr_len);
        }
        serverrtpport = ntohs(s_addr.sin_port);
        serverrtcpport = serverrtpport + 10; // random address pointing to a non existent service

        char *pTemp = buf;
        char *localip;
        localip = GetLocalIP(sock);
        pTemp += sprintf(pTemp, "RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sTransport: RTP/AVP;unicast;destination=%s;client_port=%d-%d;server_port=%d-%d\r\nSession: %d\r\n\r\n",
            cseq,dateHeader(), localip,
            ntohs(htons(clientRTPPortNum)),
            ntohs(htons(clientRTCPPortNum)),
            ntohs(htons(serverrtpport)),
            ntohs(htons(serverrtcpport)),
            SessionId);

        free(localip);
        int reg = send(sock, buf, strlen(buf), 0);
        if (reg <= 0) {
            return FALSE;
        } else {
            printf(">>>>>%s", buf);
        }
        return TRUE;
    }
    return FALSE;
}

int PlayAnswer(char *cseq, int sock,int SessionId,char* urlPre,char* recvbuf)
{
    if (sock != 0) {
        char buf[1024];
        memset(buf,0,1024);
        char *pTemp = buf;
        char*localip;
        localip = GetLocalIP(sock);
        pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sRange: npt=0.000-\r\nSession: %d\r\nRTP-Info: url=rtsp://%s/%s;seq=0\r\n\r\n",
            cseq, dateHeader(), SessionId, localip, urlPre);

        free(localip);

        int reg = send(sock, buf, strlen(buf), 0);
        if (reg <= 0) {
            return FALSE;
        } else {
            printf(">>>>>%s",buf);
        }
        return TRUE;
    }
    return FALSE;
}

int PauseAnswer(char *cseq, int sock, char *recvbuf)
{
    if (sock != 0) {
        char buf[1024];
        memset(buf,0,1024);
        char *pTemp = buf;
        pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%s\r\n\r\n",
            cseq,dateHeader());

        int reg = send(sock, buf,strlen(buf),0);
        if(reg <= 0) {
            return FALSE;
        } else {
            printf(">>>>>%s",buf);
        }
        return TRUE;
    }
    return FALSE;
}

int TeardownAnswer(char *cseq, int sock, int SessionId, char *recvbuf)
{
    if (sock != 0) {
        char buf[1024];
        memset(buf,0,1024);
        char *pTemp = buf;
        pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sSession: %d\r\n\r\n",
            cseq,dateHeader(),SessionId);

        int reg = send(sock, buf,strlen(buf),0);
        if (reg <= 0) {
            return FALSE;
        } else {
            printf(">>>>>%s",buf);
        }
        return TRUE;
    }
    return FALSE;
}

void * RtspClientMsg(void *pParam)
{
    pthread_detach(pthread_self());

    int nRes;
    char pRecvBuf[RTSP_RECV_SIZE];
    RTSP_CLIENT * pClient = (RTSP_CLIENT*) pParam;

    memset(pRecvBuf, 0, sizeof(pRecvBuf));
    printf("RTSP:-----Create Client %s\n", pClient->IP);
    while (pClient->status != RTSP_IDLE) {
        nRes = recv(pClient->socket, pRecvBuf, RTSP_RECV_SIZE,0);
        //printf("-------------------%d\n",nRes);
        if (nRes < 1) {
            //usleep(1000);
            //printf("RTSP:Recv Error--- %d\n",nRes);
            //continue;
            g_rtspClients[pClient->index].status = RTSP_IDLE;
            g_rtspClients[pClient->index].seqnum = 0;
            g_rtspClients[pClient->index].seqnum2 = 0;
            g_rtspClients[pClient->index].tsvid = 0;
            g_rtspClients[pClient->index].tsaud = 0;
            close(pClient->socket);
            break;
        }
        char cmdName[PARAM_STRING_MAX];
        char urlPreSuffix[PARAM_STRING_MAX];
        char urlSuffix[PARAM_STRING_MAX];
        char cseq[PARAM_STRING_MAX];

        ParseRequestString(pRecvBuf, nRes, cmdName, sizeof(cmdName), urlPreSuffix, sizeof(urlPreSuffix),
            urlSuffix, sizeof(urlSuffix), cseq,sizeof(cseq));

        char *p = pRecvBuf;

        printf("<<<<<%s\n",p);

        if (strstr(cmdName, "OPTIONS")) {
            OptionAnswer(cseq, pClient->socket);
        } else if (strstr(cmdName, "DESCRIBE")) {
            DescribeAnswer(cseq, pClient->socket, urlSuffix, p);
        } else if (strstr(cmdName, "SETUP")) {
            int rtpport, rtcpport;
            int trackID = 0;
            sscanf(urlSuffix, "trackID=%u", &trackID);
            SetupAnswer(cseq, pClient->socket, pClient->sessionid, trackID, urlPreSuffix, p, &rtpport, &rtcpport);

            //printf("----------------------------------------------TrackId %d\n",trackID);
            if(trackID < 0 || trackID >= 2) trackID = 0;
            g_rtspClients[pClient->index].rtpport[trackID] = rtpport;
            g_rtspClients[pClient->index].rtcpport[trackID] = rtcpport;
            g_rtspClients[pClient->index].reqchn = atoi(urlPreSuffix);
            if (strlen(urlPreSuffix) < 100)
                strcpy(g_rtspClients[pClient->index].urlPre, urlPreSuffix);
        } else if (strstr(cmdName, "PLAY")) {
            PlayAnswer(cseq, pClient->socket, pClient->sessionid, g_rtspClients[pClient->index].urlPre, p);
            g_rtspClients[pClient->index].status = RTSP_SENDING;
            printf("Start Play\n");
        } else if (strstr(cmdName, "PAUSE")) {
            PauseAnswer(cseq, pClient->socket, p);
        } else if (strstr(cmdName, "TEARDOWN")) {
            TeardownAnswer(cseq, pClient->socket, pClient->sessionid, p);
            g_rtspClients[pClient->index].status = RTSP_IDLE;
            g_rtspClients[pClient->index].seqnum = 0;
            g_rtspClients[pClient->index].seqnum2 = 0;
            g_rtspClients[pClient->index].tsvid = 0;
            g_rtspClients[pClient->index].tsaud = 0;
            close(pClient->socket);
        }
        if (exitok) {
            exitok++;
            return NULL;
        }
    }
    printf("RTSP:-----Exit Client %s\n",pClient->IP);
    return NULL;
}

void * RtspServerListen(void*pParam)
{
    int s32Socket;
    struct sockaddr_in servaddr;
    int s32CSocket;
    int s32Rtn;
    int s32Socket_opt_value = 1;
    int nAddrLen;
    struct sockaddr_in addrAccept;

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(RTSP_SERVER_PORT);

    s32Socket = socket(AF_INET, SOCK_STREAM, 0);

    if (setsockopt(s32Socket, SOL_SOCKET, SO_REUSEADDR, &s32Socket_opt_value,sizeof(int)) == -1) {
        return (void *)(-1);
    }
    s32Rtn = bind(s32Socket, (struct sockaddr *) &servaddr, sizeof(struct sockaddr_in));
    if (s32Rtn < 0) {
        return (void *)(-2);
    }

    s32Rtn = listen(s32Socket, 50);
    if (s32Rtn < 0) {
         return (void *)(-2);
    }


    nAddrLen = sizeof(struct sockaddr_in);
    int nSessionId = 1000;
    while ((s32CSocket = accept(s32Socket, (struct sockaddr*)&addrAccept, &nAddrLen)) >= 0) {
        printf("<<<<RTSP Client %s Connected...\n", inet_ntoa(addrAccept.sin_addr));

        int nMaxBuf = 10 * 1024;
        if (setsockopt(s32CSocket, SOL_SOCKET, SO_SNDBUF, (char*)&nMaxBuf, sizeof(nMaxBuf)) == -1)
            printf("RTSP:!!!!!! Enlarge socket sending buffer error !!!!!!\n");
        int i;
        int bAdd=FALSE;
        for (i = 0; i < MAX_RTSP_CLIENT; i++) {
            if (g_rtspClients[i].status == RTSP_IDLE) {
                memset(&g_rtspClients[i],0,sizeof(RTSP_CLIENT));
                g_rtspClients[i].index = i;
                g_rtspClients[i].socket = s32CSocket;
                g_rtspClients[i].status = RTSP_CONNECTED ;//RTSP_SENDING;
                g_rtspClients[i].sessionid = nSessionId++;
                strcpy(g_rtspClients[i].IP,inet_ntoa(addrAccept.sin_addr));
                pthread_t threadIdlsn = 0;

                struct sched_param sched;
                sched.sched_priority = 1;
                //to return ACKecho
                pthread_create(&threadIdlsn, NULL, RtspClientMsg, &g_rtspClients[i]);
                pthread_setschedparam(threadIdlsn, SCHED_RR, &sched);

                bAdd = TRUE;
                break;
            }
        }
        if (bAdd == FALSE) {
            memset(&g_rtspClients[0], 0, sizeof(RTSP_CLIENT));
            g_rtspClients[0].index = 0;
            g_rtspClients[0].socket = s32CSocket;
            g_rtspClients[0].status = RTSP_CONNECTED ;//RTSP_SENDING;
            g_rtspClients[0].sessionid = nSessionId++;
            strcpy(g_rtspClients[0].IP, inet_ntoa(addrAccept.sin_addr));
            pthread_t threadIdlsn = 0;
            struct sched_param sched;
            sched.sched_priority = 1;
            //to return ACKecho
            pthread_create(&threadIdlsn, NULL, RtspClientMsg, &g_rtspClients[0]);
            pthread_setschedparam(threadIdlsn, SCHED_RR, &sched);
            bAdd = TRUE;
        }
        if (exitok) {
            exitok++;
            return NULL;
        }
    }
    if(s32CSocket < 0) {
       // printf("RTSP listening on port %d,accept err, %d\n", RTSP_SERVER_PORT, s32CSocket);
    }

    printf("----- INIT_RTSP_Listen() Exit !! \n");

    return NULL;
}

int32_t VENC_Sent_V(char *buffer,int buflen)
{
    int is = 0;
    int nChanNum = 0;
    for (is = 0; is < MAX_RTSP_CLIENT; is++) {
        if (g_rtspClients[is].status != RTSP_SENDING) {
            continue;
        }
        int heart = g_rtspClients[is].seqnum % 1000;

        if(heart == 0) {
            printf("Heart[%d] %d\n", is, g_rtspClients[is].seqnum);
        }

        char* nalu_payload;
        int nAvFrmLen = 0;
        int nIsIFrm = 0;
        int nNaluType = 0;
        char sendbuf[nalu_sent_len+14];

        nChanNum = g_rtspClients[is].reqchn;
        if (nChanNum<0 || nChanNum>=MAX_CHAN ) {
            continue;
        }
        nAvFrmLen = buflen;
        //printf("%d\n",nAvFrmLen);
        //nAvFrmLen = vStreamInfo.dwSize ;//Streamlen
        struct sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port = htons(g_rtspClients[is].rtpport[0]);
        server.sin_addr.s_addr = inet_addr(g_rtspClients[is].IP);
        int bytes = 0;
        unsigned int ts_increase = 0;

        g_nframerate = FRAMERATE;
        ts_increase = (unsigned int)(90000.0 / g_nframerate);
        if (heart == 0)
            printf("FRC= %d, ts = %d\n",g_nframerate, ts_increase);
        rtp_hdr = (RTP_FIXED_HEADER*)&sendbuf[0];
        rtp_hdr->payload = RTP_H264;
        rtp_hdr->version = 2;
        rtp_hdr->marker  = 0;
        rtp_hdr->ssrc    = htonl(10);

        if(nAvFrmLen<=nalu_sent_len) {
            //printf("a");
            rtp_hdr->marker = 1;
            rtp_hdr->seq_no = htons(g_rtspClients[is].seqnum++);
            nalu_hdr = (NALU_HEADER*)&sendbuf[12];
            nalu_hdr->F = 0;
            nalu_hdr->NRI = nIsIFrm;
            nalu_hdr->TYPE = nNaluType;

            nalu_payload = &sendbuf[13];
            memcpy(nalu_payload,buffer,nAvFrmLen);
            g_rtspClients[is].tsvid += ts_increase;

            rtp_hdr->timestamp = htonl(g_rtspClients[is].tsvid);
            bytes = nAvFrmLen + 13 ;
            sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
        } else {
            //printf("b");
            int k = 0;
            int l = 0;
            int t = 0;

            k=nAvFrmLen/nalu_sent_len;
            l=nAvFrmLen%nalu_sent_len;

            g_rtspClients[is].tsvid += ts_increase;

            while(t<=k) {
                rtp_hdr->seq_no = htons(g_rtspClients[is].seqnum++);
                if(t==0) {
                    rtp_hdr->marker = 0;
                    fu_ind = (FU_INDICATOR*)&sendbuf[12];
                    fu_ind->F = 0;
                    fu_ind->NRI = nIsIFrm;
                    fu_ind->TYPE = 28;

                    fu_hdr = (FU_HEADER*)&sendbuf[13];
                    fu_hdr->E = 0;
                    fu_hdr->R = 0;
                    fu_hdr->S = 1;
                    fu_hdr->TYPE = nNaluType;

                    nalu_payload = &sendbuf[14];
                    memcpy(nalu_payload,buffer,nalu_sent_len);

                    bytes = nalu_sent_len + 14;
                    sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
                    t++;
                } else if(k==t) {
                    rtp_hdr->marker = 1;
                    fu_ind = (FU_INDICATOR*)&sendbuf[12];
                    fu_ind->F = 0 ;
                    fu_ind->NRI = nIsIFrm ;
                    fu_ind->TYPE = 28;

                    fu_hdr = (FU_HEADER*)&sendbuf[13];
                    fu_hdr->R = 0;
                    fu_hdr->S = 0;
                    fu_hdr->E = 1;
                    fu_hdr->TYPE = nNaluType;

                    nalu_payload = &sendbuf[14];
                    memcpy(nalu_payload,buffer+t*nalu_sent_len,l);

                    bytes = l+14;
                    sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
                    t++;
                } else if(t<k && t!=0) {
                    rtp_hdr->marker = 0;
                    fu_ind = (FU_INDICATOR*)&sendbuf[12];
                    fu_ind->F = 0;
                    fu_ind->NRI = nIsIFrm;
                    fu_ind->TYPE = 28;

                    fu_hdr = (FU_HEADER*)&sendbuf[13];
                    fu_hdr->R = 0;
                    fu_hdr->S = 0;
                    fu_hdr->E = 0;
                    fu_hdr->TYPE = nNaluType;

                    nalu_payload = &sendbuf[14];
                    memcpy(nalu_payload,buffer+t*nalu_sent_len,nalu_sent_len);

                    bytes = nalu_sent_len+14;
                    sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
                    t++;
                }
            }
        }
    }
    return 0;
}

int32_t VENC_Sent_A(char *buffer, int buflen)
{
    int is = 0;
    int nChanNum = 0;
    for (is = 0; is < MAX_RTSP_CLIENT; is++) {
        if (g_rtspClients[is].status != RTSP_SENDING) {
            continue;
        }
        int heart = g_rtspClients[is].seqnum2 % 1000;

        if(heart == 0) {
            printf("Heart[%d] %d\n", is, g_rtspClients[is].seqnum2);
        }

        char* pcma_payload;
        int nAvFrmLen = 0;
        int nIsIFrm = 0;
        char sendbuf[pcma_sent_len + 12];

        nChanNum = g_rtspClients[is].reqchn;
        if (nChanNum < 0 || nChanNum >= MAX_CHAN ) {
            continue;
        }
        nAvFrmLen = buflen;
        //printf("%d\n",nAvFrmLen);
        //nAvFrmLen = vStreamInfo.dwSize ;//Streamlen
        struct sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port = htons(g_rtspClients[is].rtpport[1]);
        server.sin_addr.s_addr = inet_addr(g_rtspClients[is].IP);
        int bytes = 0;
        unsigned int ts_increase = 0;

        g_naudiorate = OUT_AUDIO_FREQ;
        ts_increase = (unsigned int)(((buflen / 2) / AUDIO_CHANNELS) / g_naudiorate);
        if (heart == 0)
            printf("FRC= %d, ts = %d\n", g_nframerate, ts_increase);
        rtp_hdr = (RTP_FIXED_HEADER*) &sendbuf[0];
        rtp_hdr->payload = RTP_AUDIO;
        rtp_hdr->version = 2;
        rtp_hdr->marker  = 0;
        rtp_hdr->ssrc    = htonl(11);

        rtp_hdr->marker = 0;
        rtp_hdr->seq_no = htons(g_rtspClients[is].seqnum2++);

        pcma_payload = &sendbuf[12];
        memcpy(pcma_payload, buffer, nAvFrmLen);
        g_rtspClients[is].tsaud += ts_increase;

        rtp_hdr->timestamp = htonl(g_rtspClients[is].tsaud);
        bytes = nAvFrmLen + 12;
        sendto(udpfd2, sendbuf, bytes, 0, (struct sockaddr *) &server, sizeof(server));
    }
    return 0;
}

// DEBUG: to output stream
static ssize_t full_write(int fd, const void *buf, size_t len)
{
    ssize_t cc;
    ssize_t total;
    total = 0;
    while (len) {
        for (;;) {
            cc = write(fd, buf, len);
            if (cc >= 0 || EINTR != errno) {
                break;
            }
            errno = 0;
        }
        if (cc < 0) {
            if (total) {
                return total;
            }
            return cc;
        }
        total += cc;
        buf = ((const char *)buf) + cc;
        len -= cc;
    }
    return total;
}

void *capture_video(void *ptr)
{
    pthread_detach(pthread_self());
    printf("RTSP:-----create video send thread\n");

    unsigned char *buf_idx_1, *buf_idx_2;
    unsigned char *buf_idx_w, *buf_idx_tmp;
    unsigned char *buf_idx_start = NULL;
    FILE *fFid;

    int frame_len = -1;
    int frame_res = -1;
    int frame_counter = -1;
    int frame_counter_last_valid_low = -1;
    int frame_counter_last_valid_high = -1;
    int frame_counter_invalid_low = 0;
    int frame_counter_invalid_high = 0;

    int i;
    int write_enable = 0;
    int sps_sync = 0;
    int nal_is_sps_or_vps5 = 0;

    SAMPLE_VENC_GETSTREAM_PARA_S *pstPara;
    pstPara = (SAMPLE_VENC_GETSTREAM_PARA_S*) ptr;

    udpfd = socket(AF_INET, SOCK_DGRAM, 0); // UDP
    printf("udp video socket ready\n");

    // Opening an existing file
    fFid = fopen(input_buffer.filename, "r");
    if ( fFid == NULL ) {
        fprintf(stderr, "%lld: could not open file %s\n", current_timestamp(), input_buffer.filename);
        exit(EXIT_FAILURE);
    }

    // Map file to memory
    input_buffer.buffer = (unsigned char*) mmap(NULL, input_buffer.size, PROT_READ, MAP_SHARED, fileno(fFid), 0);
    if (input_buffer.buffer == MAP_FAILED) {
        fprintf(stderr, "%lld: error mapping file %s\n", current_timestamp(), input_buffer.filename);
        fclose(fFid);
        exit(EXIT_FAILURE);
    }
    if (debug & 1) fprintf(stderr, "%lld: mapping file %s, size %d, to %08x\n", current_timestamp(), input_buffer.filename, input_buffer.size, (unsigned int) input_buffer.buffer);
    // Closing the file
    if (debug & 1) fprintf(stderr, "%lld: closing the file %s\n", current_timestamp(), input_buffer.filename);
    fclose(fFid) ;

    memcpy(&i, input_buffer.buffer + 16, sizeof(i));
    buf_idx_w = input_buffer.buffer + input_buffer.offset + i;
    buf_idx_1 = buf_idx_w;

    if (debug & 1) fprintf(stderr, "%lld: starting capture main loop\n", current_timestamp());

    // Infinite loop
    while (TRUE == pstPara->bThreadStart) {
        int32_t is, flag = 0;
        for (is = 0; is < MAX_RTSP_CLIENT; is++) {
            if (g_rtspClients[is].status == RTSP_SENDING) {
                flag = 1;
                break;
            }
        }
        if (flag == 0) {
            usleep(MILLIS_25);
            continue;
        }

        memcpy(&i, input_buffer.buffer + 16, sizeof(i));
        buf_idx_w = input_buffer.buffer + input_buffer.offset + i;
//        if (debug & 1) fprintf(stderr, "buf_idx_w: %08x\n", (unsigned int) buf_idx_w);
        buf_idx_tmp = cb_memmem(buf_idx_1, buf_idx_w - buf_idx_1, NALx_START, sizeof(NALx_START));
        if (buf_idx_tmp == NULL) {
            usleep(MILLIS_25);
            continue;
        } else {
            buf_idx_1 = buf_idx_tmp;
        }
//        if (debug & 1) fprintf(stderr, "found buf_idx_1: %08x\n", (unsigned int) buf_idx_1);

        buf_idx_tmp = cb_memmem(buf_idx_1 + 1, buf_idx_w - (buf_idx_1 + 1), NALx_START, sizeof(NALx_START));
        if (buf_idx_tmp == NULL) {
            usleep(MILLIS_25);
            continue;
        } else {
            buf_idx_2 = buf_idx_tmp;
        }
//        if (debug & 1) fprintf(stderr, "found buf_idx_2: %08x\n", (unsigned int) buf_idx_2);

        if ((write_enable) && (sps_sync)) {
            if (frame_res == RESOLUTION_HIGH) {
//                if (debug & 1) fprintf(stderr, "%lld: frame_len: %d - cb_current->size: %d\n", current_timestamp(), frame_len, cb_current->size);
                if (frame_len > sizeof(nalu)) {
                    fprintf(stderr, "%lld: frame size exceeds buffer size\n", current_timestamp());
                    sps_sync = 0;
                } else {
//                    input_buffer.read_index = buf_idx_start;
#ifdef SPS_TIMING_INFO
                    // Check if NALU is SPS
                    if (nal_is_sps_or_vps5 == 1) {
                        if (frame_res == RESOLUTION_LOW) {
                            frame_len = sizeof(SPS4_640X360_TI);
                        } else if (frame_res == RESOLUTION_HIGH) {
                            frame_len = sizeof(SPS4_1920X1080_TI);
                        }
                    } else if (nal_is_sps_or_vps5 == 3) {
                        frame_len = sizeof(VPS5_1920X1080_TI);
                    }
#endif
//                    if (debug & 1) fprintf(stderr, "%lld: frame_len: %d - frame_counter: %d - resolution: %d\n", current_timestamp(), frame_len, frame_counter, frame_res);
//                    if (debug & 1) fprintf(stderr, "%lld: frame_write_index: %d - cb_current->output_frame_size %d\n", current_timestamp(), cb_current->frame_write_index, cb_current->output_frame_size);
#ifdef SPS_TIMING_INFO
                    // Overwrite SPS or VPS with one that contains timing info at 20 fps
                    if (nal_is_sps_or_vps5 == 1) {
                        if (frame_res == RESOLUTION_LOW) {
                            memcpy(nalu, SPS4_640X360_TI, sizeof(SPS4_640X360_TI));
                        } else if (frame_res == RESOLUTION_HIGH) {
                            memcpy(nalu, SPS4_1920X1080_TI, sizeof(SPS4_1920X1080_TI));
                        }
                    } else if (nal_is_sps_or_vps5 == 3) {
                        memcpy(nalu, VPS5_1920X1080_TI, sizeof(VPS5_1920X1080_TI));
                    } else {
#endif
                        cb2s_memcpy(nalu, buf_idx_start, frame_len);
#ifdef SPS_TIMING_INFO
                    }
#endif
                    VENC_Sent_V(nalu, frame_len);
                }
            }
        }


        nal_is_sps_or_vps5 = 0;
        if (cb_memcmp(SPS4_START, buf_idx_1, sizeof(SPS4_START)) == 0) {
            nal_is_sps_or_vps5 = 1;
        } else if (cb_memcmp(SPS5_START, buf_idx_1, sizeof(SPS5_START)) == 0) {
            nal_is_sps_or_vps5 = 2;
        } else if (cb_memcmp(VPS5_START, buf_idx_1, sizeof(VPS5_START)) == 0) {
            nal_is_sps_or_vps5 = 3;
        }

        if ((nal_is_sps_or_vps5 == 1) || (nal_is_sps_or_vps5 == 2)) {
            // SPS frame
            write_enable = 1;
            sps_sync = 1;
            buf_idx_1 = cb_move(buf_idx_1, - (6 + frame_header_size));
            if (buf_idx_1[17 + data_offset] == lowres_byte) {
                frame_res = RESOLUTION_LOW;
            } else if (buf_idx_1[17 + data_offset] == highres_byte) {
                frame_res = RESOLUTION_HIGH;
            } else {
                frame_res = RESOLUTION_NONE;
            }
            cb2s_memcpy((unsigned char *) &frame_len, buf_idx_1, 4);
            frame_len -= 6;                                                              // -6 only for SPS
            frame_counter = (int) buf_idx_1[18 + data_offset] + (int) buf_idx_1[19 + data_offset] * 256;
            if ((frame_res == RESOLUTION_LOW) && ((frame_counter - frame_counter_last_valid_low > 20) ||
                        ((frame_counter < frame_counter_last_valid_low) && (frame_counter - frame_counter_last_valid_low > -65515)))) {

                if (debug & 1) fprintf(stderr, "%lld: incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                            current_timestamp(), frame_counter, frame_counter_last_valid_low);
                frame_counter_invalid_low++;
                // Check if sync is lost
                if (frame_counter_invalid_low > 40) {
                    if (debug & 1) fprintf(stderr, "%lld: sync lost\n", current_timestamp());
                    frame_counter_last_valid_low = frame_counter;
                    frame_counter_invalid_low = 0;
                } else {
                    write_enable = 0;
                }
            } else if ((frame_res == RESOLUTION_HIGH) && ((frame_counter - frame_counter_last_valid_high > 20) ||
                        ((frame_counter < frame_counter_last_valid_high) && (frame_counter - frame_counter_last_valid_high > -65515)))) {

                if (debug & 1) fprintf(stderr, "%lld: incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                            current_timestamp(), frame_counter, frame_counter_last_valid_high);
                frame_counter_invalid_high++;
                // Check if sync is lost
                if (frame_counter_invalid_high > 40) {
                    if (debug & 1) fprintf(stderr, "%lld: sync lost\n", current_timestamp());
                    frame_counter_last_valid_high = frame_counter;
                    frame_counter_invalid_high = 0;
                } else {
                    write_enable = 0;
                }
            } else {
                if (frame_res == RESOLUTION_LOW) {
                    frame_counter_invalid_low = 0;
                    frame_counter_last_valid_low = frame_counter;
                } else if (frame_res == RESOLUTION_HIGH) {
                    frame_counter_invalid_high = 0;
                    frame_counter_last_valid_high = frame_counter;
                } else {
                    write_enable = 0;
                }
            }
            if (debug & 1) fprintf(stderr, "%lld: SPS   detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                    current_timestamp(), frame_len, frame_counter,
                    (frame_res == RESOLUTION_LOW)? frame_counter_last_valid_low: frame_counter_last_valid_high, frame_res);
            buf_idx_1 = cb_move(buf_idx_1, 6 + frame_header_size);
            buf_idx_start = buf_idx_1;
        } else if ((cb_memcmp(PPS4_START, buf_idx_1, sizeof(PPS4_START)) == 0) ||
                        (cb_memcmp(PPS5_START, buf_idx_1, sizeof(PPS5_START)) == 0) ||
                        (cb_memcmp(VPS5_START, buf_idx_1, sizeof(VPS5_START)) == 0) ||
                        (cb_memcmp(IDR4_START, buf_idx_1, sizeof(IDR4_START)) == 0) ||
                        (cb_memcmp(IDR5_START, buf_idx_1, sizeof(IDR5_START)) == 0) ||
                        (cb_memcmp(PFR4_START, buf_idx_1, sizeof(PFR4_START)) == 0) ||
                        (cb_memcmp(PFR5_START, buf_idx_1, sizeof(PFR5_START)) == 0)) {
            // PPS, IDR and PFR frames
            write_enable = 1;
            buf_idx_1 = cb_move(buf_idx_1, -frame_header_size);
            if (buf_idx_1[17 + data_offset] == lowres_byte) {
                frame_res = RESOLUTION_LOW;
            } else if (buf_idx_1[17 + data_offset] == highres_byte) {
                frame_res = RESOLUTION_HIGH;
            } else {
                frame_res = RESOLUTION_NONE;
            }
            cb2s_memcpy((unsigned char *) &frame_len, buf_idx_1, 4);
            frame_counter = (int) buf_idx_1[18 + data_offset] + (int) buf_idx_1[19 + data_offset] * 256;
            if ((frame_res == RESOLUTION_LOW) && ((frame_counter - frame_counter_last_valid_low > 20) ||
                        ((frame_counter < frame_counter_last_valid_low) && (frame_counter - frame_counter_last_valid_low > -65515)))) {

                if (debug & 1) fprintf(stderr, "%lld: incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                            current_timestamp(), frame_counter, frame_counter_last_valid_low);
                frame_counter_invalid_low++;
                // Check if sync is lost
                if (frame_counter_invalid_low > 40) {
                    if (debug & 1) fprintf(stderr, "%lld: sync lost\n", current_timestamp());
                    frame_counter_last_valid_low = frame_counter;
                    frame_counter_invalid_low = 0;
                } else {
                    write_enable = 0;
                }
            } else if ((frame_res == RESOLUTION_HIGH) && ((frame_counter - frame_counter_last_valid_high > 20) ||
                        ((frame_counter < frame_counter_last_valid_high) && (frame_counter - frame_counter_last_valid_high > -65515)))) {

                if (debug & 1) fprintf(stderr, "%lld: incorrect frame counter - frame_counter: %d - frame_counter_last_valid: %d\n",
                            current_timestamp(), frame_counter, frame_counter_last_valid_high);
                frame_counter_invalid_high++;
                // Check if sync is lost
                if (frame_counter_invalid_high > 40) {
                    if (debug & 1) fprintf(stderr, "%lld: sync lost\n", current_timestamp());
                    frame_counter_last_valid_high = frame_counter;
                    frame_counter_invalid_high = 0;
                } else {
                    write_enable = 0;
                }
            } else {
                if (frame_res == RESOLUTION_LOW) {
                    frame_counter_invalid_low = 0;
                    frame_counter_last_valid_low = frame_counter;
                } else if (frame_res == RESOLUTION_HIGH) {
                    frame_counter_invalid_high = 0;
                    frame_counter_last_valid_high = frame_counter;
                } else {
                    write_enable = 0;
                }
            }
            if (debug & 1) fprintf(stderr, "%lld: frame detected - frame_len: %d - frame_counter: %d - frame_counter_last_valid: %d - resolution: %d\n",
                    current_timestamp(), frame_len, frame_counter,
                    (frame_res == RESOLUTION_LOW)? frame_counter_last_valid_low: frame_counter_last_valid_high, frame_res);
            buf_idx_1 = cb_move(buf_idx_1, frame_header_size);
            buf_idx_start = buf_idx_1;
        } else {
            write_enable = 0;
        }

        buf_idx_1 = buf_idx_2;
    }

    // Unreacheable path

    // Unmap file from memory
    if (munmap(input_buffer.buffer, input_buffer.size) == -1) {
        fprintf(stderr, "%lld: error munmapping file\n", current_timestamp());
    } else {
        if (debug & 1) fprintf(stderr, "%lld: unmapping file %s, size %d, from %08x\n", current_timestamp(), BUFFER_FILE, input_buffer.size, (unsigned int) input_buffer.buffer);
    }

    return NULL;
}

void *capture_audio(void *ptr)
{
    pthread_detach(pthread_self());
    printf("RTSP:-----create audio send thread\n");

    SAMPLE_VENC_GETSTREAM_PARA_S *pstPara;
    pstPara = (SAMPLE_VENC_GETSTREAM_PARA_S*) ptr;

    udpfd2 = socket(AF_INET, SOCK_DGRAM, 0); // UDP
    printf("udp audio socket ready\n");

    FILE* fFid = fopen(AUDIO_FIFO_FILE, "r");
    if (fFid == NULL)
    {
        printf("Unable to open audio fifo file %s, audio not available\n", AUDIO_FIFO_FILE);
        return NULL;
    }

    int flags = fcntl(fileno(fFid), F_GETFL, 0);
    if (flags == -1) {
        fclose(fFid);
        printf("Unable to get blocking flags on %s, audio not available\n", AUDIO_FIFO_FILE);
        return NULL;
    };

    // Set non blocking
    flags |= O_NONBLOCK;
    if (fcntl(fileno(fFid), F_SETFL, flags) != 0) {
        fclose(fFid);
        printf("Unable to set non blocking fifo on %s, audio not available\n", AUDIO_FIFO_FILE);
        return NULL;
    };

    // Clean fifo content
    unsigned char null[4];
    while (fread(null, 1, sizeof(null), fFid) > 0) {}

    // Set blocking
    flags &= ~O_NONBLOCK;
    if (fcntl(fileno(fFid), F_SETFL, flags) != 0) {
        fclose(fFid);
        printf("Unable to set blocking fifo on %s, audio not available\n", AUDIO_FIFO_FILE);
        return NULL;
    };

    // The input format is: 8 Khz, 16 bit,  mono
    // The output format is: 16 Khz, 16 bit,  mono
    while (TRUE == pstPara->bThreadStart) {
        fread(pcma, 1, pcma_sent_len / 2, fFid);
        // Double and swap:
        // - transform 8 KHz in 16 KHz duplicating the samples
        // - swap bytes endianness
        int i;
        for (i = (pcma_sent_len / 2) - 2; i >= 0; i = i - 2) {
            pcma[(i + 1) * 2 + 1] = pcma[i];
            pcma[(i + 1) * 2] = pcma[i + 1];
            pcma[i * 2 + 1] = pcma[(i + 1) * 2 + 1];
            pcma[i * 2] = pcma[(i + 1) * 2];
        }
        VENC_Sent_A(pcma, pcma_sent_len);
    }

    fclose(fFid);
}

int32_t thread_get_picture(void)
{
    struct sched_param schedvenc;
    schedvenc.sched_priority = 10;
    //to get stream
    gs_stPara.bThreadStart = TRUE;
    printf("RTSP:-----tmp_view_get_picture\n");

    pthread_create(&gs_VencPid, NULL, (void *) capture_video, (void *) &gs_stPara);
    pthread_setschedparam(gs_VencPid, SCHED_RR, &schedvenc);

    return 0;
}

int32_t thread_get_audio(void)
{
    struct sched_param schedaenc;
    schedaenc.sched_priority = 10;
    //to get stream
    gs_stPara2.bThreadStart = TRUE;
    printf("RTSP:-----tmp_view_get_audio\n");

    pthread_create(&gs_AencPid, NULL, (void *) capture_audio, (void *) &gs_stPara2);
    pthread_setschedparam(gs_AencPid, SCHED_RR, &schedaenc);

    return 0;
}

void InitRtspServer()
{
    pthread_t threadId = 0;
    memset(g_rtp_playload,0,sizeof(g_rtp_playload));
    strcpy(g_rtp_playload,"PCM");
    pthread_mutex_init(&g_sendmutex,NULL);
    pthread_mutex_init(&g_mutex,NULL);
    pthread_cond_init(&g_cond,NULL);
    memset(g_rtspClients,0,sizeof(RTSP_CLIENT)*MAX_RTSP_CLIENT);

    struct sched_param thdsched;
    thdsched.sched_priority = 2;
    //to listen visiting
    pthread_create(&threadId, NULL, RtspServerListen, NULL);
    pthread_setschedparam(threadId,SCHED_RR,&thdsched);
    printf("RTSP:-----Init Rtsp server\n");

    int32_t s32Ret;
    s32Ret = thread_get_picture();
    if (audio) {
        s32Ret = thread_get_audio();
    }
}

int loop()
{
    while (1) {
        usleep(10000);
    }
    return 0;
}

void print_usage(char *progname)
{
    fprintf(stderr, "\nUsage: %s [-r RES] [-p PORT] [-d]\n\n", progname);
    fprintf(stderr, "\t-m MODEL, --model MODEL\n");
    fprintf(stderr, "\t\tset model: y21ga, r30gb or h52ga (default y21ga)\n");
//    fprintf(stderr, "\t-r RES,   --resolution RES\n");
//    fprintf(stderr, "\t\tset resolution: low, high or both (default high)\n");
    fprintf(stderr, "\t-a AUDIO, --audio AUDIO\n");
    fprintf(stderr, "\t\tset audio: yes, no (default yes)\n");
    fprintf(stderr, "\t-p PORT,  --port PORT\n");
    fprintf(stderr, "\t\tset TCP port (default 554)\n");
//    fprintf(stderr, "\t-d DEBUG, --debug DEBUG\n");
//    fprintf(stderr, "\t\t0 none, 1 grabber, 2 rtsp library or 3 both\n");
    fprintf(stderr, "\t-h,       --help\n");
    fprintf(stderr, "\t\tprint this help\n");
}

int main(int argc, char* argv[])
{
    int c;
    char *endptr;

    // Setting default
    model = Y21GA;
//    resolution = RESOLUTION_HIGH;
    audio = 1;
    port = 554;
//    debug = 0;

    while (1) {
        static struct option long_options[] =
        {
            {"model",  required_argument, 0, 'm'},
//            {"resolution",  required_argument, 0, 'r'},
            {"audio",  required_argument, 0, 'a'},
            {"port",  required_argument, 0, 'p'},
//            {"debug",  required_argument, 0, 'd'},
            {"help",  no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

//        c = getopt_long (argc, argv, "m:r:a:p:d:h",
        c = getopt_long (argc, argv, "m:a:p:h",
                         long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c) {
        case 'm':
            if (strcasecmp("y21ga", optarg) == 0) {
                model = Y21GA;
            } else if (strcasecmp("r30gb", optarg) == 0) {
                model = R30GB;
            } else if (strcasecmp("h52ga", optarg) == 0) {
                model = H52GA;
            }
            break;

/*        case 'r':
            if (strcasecmp("low", optarg) == 0) {
                resolution = RESOLUTION_LOW;
            } else if (strcasecmp("high", optarg) == 0) {
                resolution = RESOLUTION_HIGH;
            } else if (strcasecmp("both", optarg) == 0) {
                resolution = RESOLUTION_BOTH;
            }
            break;*/

        case 'a':
            if (strcasecmp("no", optarg) == 0) {
                audio = 0;
            } else if (strcasecmp("yes", optarg) == 0) {
                audio = 1;
            }
            break;

        case 'p':
            errno = 0;    /* To distinguish success/failure after call */
            port = strtol(optarg, &endptr, 10);

            /* Check for various possible errors */
            if ((errno == ERANGE && (port == LONG_MAX || port == LONG_MIN)) || (errno != 0 && port == 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            if (endptr == optarg) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            break;

/*        case 'd':
            errno = 0;    // To distinguish success/failure after call
            debug = strtol(optarg, &endptr, 10);

            // Check for various possible errors
            if ((errno == ERANGE && (debug == LONG_MAX || debug == LONG_MIN)) || (errno != 0 && debug == 0)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            if (endptr == optarg) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            if ((debug < 0) || (debug > 3)) {
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
            fprintf (stderr, "debug on, level %d\n", debug);
            break;*/

        case 'h':
            print_usage(argv[0]);
            return -1;
            break;

        case '?':
            /* getopt_long already printed an error message. */
            break;

        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    printf("model: %s\n", ((model==Y21GA)?"y21ga":((model==R30GB)?"r30gb":"h52ga")));
    printf("audio: %s\n", ((audio==1)?"yes":"no"));
    printf("port:  %d\n", port);

    if (model == Y21GA) {
        buf_offset = BUF_OFFSET_Y21GA;
        buf_size = BUF_SIZE_Y21GA;
        frame_header_size = FRAME_HEADER_SIZE_Y21GA;
        data_offset = DATA_OFFSET_Y21GA;
        lowres_byte = LOWRES_BYTE_Y21GA;
        highres_byte = HIGHRES_BYTE_Y21GA;
    } else if (model == R30GB) {
        buf_offset = BUF_OFFSET_R30GB;
        buf_size = BUF_SIZE_R30GB;
        frame_header_size = FRAME_HEADER_SIZE_R30GB;
        data_offset = DATA_OFFSET_R30GB;
        lowres_byte = LOWRES_BYTE_R30GB;
        highres_byte = HIGHRES_BYTE_R30GB;
    } else if (model == H52GA) {
        buf_offset = BUF_OFFSET_H52GA;
        buf_size = BUF_SIZE_H52GA;
        frame_header_size = FRAME_HEADER_SIZE_H52GA;
        data_offset = DATA_OFFSET_H52GA;
        lowres_byte = LOWRES_BYTE_H52GA;
        highres_byte = HIGHRES_BYTE_H52GA;
    }

    // Fill input and output buffer struct
    strcpy(input_buffer.filename, BUFFER_FILE);
    input_buffer.size = buf_size;
    input_buffer.offset = buf_offset;

    InitRtspServer();
    loop();

    return 0;
}
