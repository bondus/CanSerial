/*
 * Socket functions for CanSerial
 *
 *  Copyright (C) 2019 Eug Krashtan <eug.krashtan@gmail.com>
 *  This file may be distributed under the terms of the GNU GPLv3 license.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <pty.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "cansock.h"
#include "portnumber.h"


typedef struct {
    uint16_t port;
    canid_t canid;
    uint8_t can_uuid[CAN_UUID_SIZE];
    int pingcount;
    int active; // Is port open
    int watch; // inotify watch
} tPortId;

// p[0] unused and VportFd[0] is the CAN socket
typedef struct {
    tPortId *p;
    struct pollfd *VportFd;
    int portsize; // size of allocated VpostFd vector
    int portptr;
} tPorts;



#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define EVENT_BUF_LEN     ( 1024 * ( EVENT_SIZE + 16 ) )


static pthread_t RxTh;
static pthread_mutex_t lock; // protects variables below

static int threadexit = 0;
static tPorts ports;
static int sock;  // The CAN socket
static int pingptr = 0;
static int Inotify;


static void CanTtyName(tPortId *p, char *name, int maxlen) {
    // TODO: Name according to CAN bus
    snprintf(name, maxlen, "/tmp/ttyCAN0_%02x%02x%02x%02x%02x%02x",
             p->can_uuid[0],
             p->can_uuid[1],
             p->can_uuid[2],
             p->can_uuid[3],
             p->can_uuid[4],
             p->can_uuid[5]);
}

static void CanVportClose(tPortId *p) {
    int res;

    char fname[64];
    CanTtyName(p, fname, sizeof(fname));
    inotify_rm_watch(Inotify, p->watch);
    res = unlink(fname);
    if (res != 0) {
        perror(fname);
    }
}

static int CanVport(int portid, uint8_t *uuid)
{
    int res, i;
    int fd, sfd;
    struct termios ti;

    // check if port exists
    for(i=1; i<ports.portptr; i++) {
        if (ports.p[i].port == portid) {
            // Assign the same virtual port for re-initialized CAN
            printf("Device reset\n");
            return i;
        }
    }

    if (ports.portptr == ports.portsize) {
        // Increase allocated space
        ports.portsize *= 2;
        ports.p = realloc(ports.p, sizeof(tPortId) * ports.portsize);
        ports.VportFd = realloc(ports.VportFd, sizeof(struct pollfd) * ports.portsize);
        if (!ports.p || !ports.VportFd) {
            fprintf(stderr, "CreatePipe: realloc failed!\n");
            exit(1);
        }
    }

    tPortId *p = &ports.p[ports.portptr];

    // Assign packet handlers
    p->canid = 2*portid+PKT_ID_CTL_FILTER;
    memcpy(p->can_uuid, uuid, CAN_UUID_SIZE);
    p->port = portid;
    p->pingcount = PINGS_BEFORE_DISCONNECT;
    p->active = 0;
    ports.VportFd[ports.portptr].revents = 0;

    // allocate virtual port
    memset(&ti, 0, sizeof(ti));
    res = openpty(&fd, &sfd, NULL, &ti, NULL);
    if (res) {
        fprintf(stderr, "Error: openpty %d\n", res);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        fprintf(stderr, "Error: fcntl getfl %d\n", flags);
        return -1;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(sfd, F_SETFD, FD_CLOEXEC);

    char *tname = ttyname(sfd);

    // Create symlink to tty
    char fname[64];
    CanTtyName(p, fname, sizeof(fname));
    unlink(fname);
    printf("%s CANID %03x\n", fname, p->canid);

    res = symlink(tname, fname);
    if (res) {
        fprintf(stderr, "Error: symlink %d\n", res);
        return -1;
    }
    res = chmod(tname, 0666);
    if (res) {
        fprintf(stderr, "Error: chmod %d\n", res);
        return -1;
    }
    ports.VportFd[ports.portptr].fd = fd;
    ports.VportFd[ports.portptr].events = POLLIN;

    p->watch =
        inotify_add_watch(Inotify, fname, IN_OPEN|IN_CLOSE);

    ports.portptr++;
    return ports.portptr-1;
}

static int ConfigurePort(tCanFrame frame) {
    struct __attribute__((__packed__)) {
        uint16_t canid;
        uint8_t u[CAN_UUID_SIZE];
    } resp;
    // Generate packet id:
    // (port number*2) + ID offset
    int portid = PnGetNumber(frame.data);
    resp.canid = 2*portid+PKT_ID_CTL_FILTER;
    memcpy(resp.u, frame.data, CAN_UUID_SIZE);

    // allocate virtual port for client
    printf("UUID %02x:%02x:%02x:%02x:%02x:%02x  ",
           resp.u[0], resp.u[1], resp.u[2],
           resp.u[3], resp.u[4], resp.u[5]);
    CanVport(portid, resp.u);
    CanSockSend(PKT_ID_SET, sizeof(resp), (uint8_t *)&resp);
    return 0;
}

void *CanRxThread( void *ptr )
{
    int i;
    int ret;
    tCanFrame frame;
    uint8_t rxbuf[CAN_DATA_SIZE];
    char ev_buf[EVENT_BUF_LEN];

    pthread_mutex_lock(&lock);

    while (threadexit==0) {
        pthread_mutex_unlock(&lock);
        ret = poll(ports.VportFd, ports.portptr, 1000);
        pthread_mutex_lock(&lock);

        if (ret>0) { // one of fd's ready
            if (ports.VportFd[0].revents) {
                // TODO: Should check what event!
                if(read(ports.VportFd[0].fd, &frame, sizeof(struct can_frame)) >= 0) {
                    // Configure port

                    if (frame.can_id == PKT_ID_UUID_RESP) {
                        if (ConfigurePort(frame) < 0) {
                            fprintf(stderr, "Aborting...\n");
                            break;
                        }
                    } else {
                        for(i=1; i<ports.portptr; i++) {
                            if (ports.p[i].canid == (frame.can_id - 1)) {
                                if (frame.can_dlc > 0 && ports.p[i].active) {
                                    write(ports.VportFd[i].fd, frame.data, frame.can_dlc);
                                }
                                // refresh channel activity
                                ports.p[i].pingcount = PINGS_BEFORE_DISCONNECT;
                                break;
                            }
                        }
                        if ( i == ports.portptr) {
                            printf("An unknown node is using CAN ID 0x%x. Ask for UUID\n", frame.can_id);
                            // Looks like lost hanshake, try to re-init
                            uint16_t txaddr = frame.can_id - 1;
                            CanSockSend(PKT_ID_UUID, 2, (uint8_t*) &(txaddr));
                        }
                    }
                }
            } else {
                for(i=1; i<ports.portptr; i++) {
                    if (ports.VportFd[i].revents) {
                        ssize_t rl = read (ports.VportFd[i].fd, rxbuf, CAN_DATA_SIZE);
                        if(rl>0) {
                            for(int j=0;j<rl;j++) {
                                if(rxbuf[j] == 0x7E) // End of packet indicator
                                    ports.p[i].active = 1; // Now we can send responses
                            }
                            CanSockSend(ports.p[i].canid, rl, rxbuf);
                        }
                    }
                }
            }
        }
        ssize_t ev = read( Inotify, ev_buf, EVENT_BUF_LEN );
        if ( ev > 0 ) {
            for (char *p = ev_buf; p < ev_buf + ev; ) {
                struct inotify_event *event = (struct inotify_event *) p;
                for(i=1; i<ports.portptr; i++) {
                    if(ports.p[i].watch == event->wd) {
                        if ( event->mask & IN_OPEN ) {
                            ports.p[i].active = 1;
                            // Send reset to MCU
                            CanSockSend(PKT_ID_UUID, 2, (uint8_t*) &(ports.p[i].canid));
                        } else if ( event->mask & IN_CLOSE ) {
                            ports.p[i].active = 0;
                        }
                        break;
                    }
                }
                p += EVENT_SIZE + event->len;
            }
        }
    }

    // close and delete fd's
    for(i=1; i<ports.portptr; i++) {
        printf("close port %d\n", i);
        CanVportClose(&(ports.p[i]));
    }
    pthread_mutex_unlock(&lock);
}

void CanPing(void)
{

    pthread_mutex_lock(&lock);
    if(pingptr == 0) {
        // request for new port assign
        CanSockSend(PKT_ID_UUID, 0, NULL);
        pingptr++;
        pthread_mutex_unlock(&lock);
        return;
    }

    if(pingptr >= ports.portptr) {
        pingptr=0;
    } else {
        // Check if we have packets from remote
        if(ports.p[pingptr].pingcount == 0) {
            // Unlink dead port
            // ToDo: Dirty variant. Need to close /dev/pts first.
            CanVportClose(&(ports.p[pingptr]));
            for(int i= pingptr; i<ports.portptr-1; i++) {
                memcpy(&(ports.p[pingptr]), &(ports.p[pingptr+1]), sizeof(tPortId));
                memcpy(&(ports.VportFd[pingptr]), &(ports.VportFd[pingptr+1]), sizeof(struct pollfd));
            }
            ports.portptr--;
            pthread_mutex_unlock(&lock);
            return;
        }
        ports.p[pingptr].pingcount --;
        if(ports.p[pingptr].pingcount < 2) {
            // To reduce bus load we'll send pings only if necessary
            CanSockSend(ports.p[pingptr].canid, 0, NULL);
        }
        pingptr++;
    }
    
    pthread_mutex_unlock(&lock);
}

int CanSockInit(void)
{
    struct sockaddr_can addr;
    struct ifreq ifr;
    struct can_filter *rfilter;
    int retval;

    /* open socket */
    if ((sock = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
        return ENOTSOCK;
    }

    addr.can_family = AF_CAN;

    strcpy(ifr.ifr_name, "can0");
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        return SIOCGIFINDEX;
    }
    addr.can_ifindex = ifr.ifr_ifindex;


    /* enable TX blocking mode when linux can TX buffer is full
       https://rtime.felk.cvut.cz/can/socketcan-qdisc-final.pdf */
    int sndbuf = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof (sndbuf)) < 0)
        perror("setsockopt");

    /* Set filters */
    rfilter = malloc(sizeof(struct can_filter) * NUM_CAN_FILTERS);
    if (!rfilter) {
        return ENOMEM;
    }
    rfilter[0].can_id = PKT_ID_UUID_FILTER;
    rfilter[0].can_mask = PKT_ID_UUID_MASK;
    rfilter[1].can_id = PKT_ID_CTL_FILTER;
    rfilter[1].can_mask = PKT_ID_CTL_MASK;

    setsockopt(sock, SOL_CAN_RAW, CAN_RAW_FILTER,
               rfilter, NUM_CAN_FILTERS * sizeof(struct can_filter));
    free(rfilter);

    /* set timeout */
    struct timeval tv;
    tv.tv_sec = 1;  // TODO. hmm
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    int rcvbuf_size = 512;	
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        perror("setsockopt SO_RCVBUF");
        return 1;
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return EIO;
    }

    // Allocate ports
    ports.portsize = 8;
    ports.portptr = 1; // skip one position for CAN socket
    ports.p = malloc(sizeof(tPortId) * ports.portsize);
    ports.VportFd = malloc(sizeof(struct pollfd) * ports.portsize);
    memset(ports.VportFd, 0, sizeof(struct pollfd) * ports.portsize);
    if (!ports.p || !ports.VportFd) {
        fprintf(stderr, "malloc failed!\n");
        return ENOMEM;
    }
    ports.VportFd[0].fd = sock;
    ports.VportFd[0].events = POLLIN;

    // Inotify for port open/close
    Inotify = inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
    if (Inotify == -1) {
        fprintf(stderr, "unable to create inotify fd\n");
        return EINVAL;
    }

    
    retval = pthread_mutex_init(&lock, NULL);
    if (retval)
        return retval;
    // Create CAN RX thread
    retval = pthread_create( &RxTh, NULL, CanRxThread, NULL);
    if(retval)
        return retval;
    return 0;
}

void CanSockClose()
{
    threadexit = 1;
    close(sock);
    pthread_join( RxTh, NULL);
}

int CanSockSend(canid_t id, uint8_t len, uint8_t* data)
{
    tCanFrame frame;

    if (len>8){	
        fprintf(stderr, "CanSockSend(%d, %d) EINVAL\n", id, len); 
        return EINVAL;
    }
    
    frame.can_id = id;
    frame.can_dlc = len;
    if (len)
        memcpy(frame.data, data, len);
    size_t w =  write(sock, &frame, sizeof(frame));
    if (w != sizeof(frame)) {
        perror("CAN write");
        return EIO;
    }
    return 0;
}

