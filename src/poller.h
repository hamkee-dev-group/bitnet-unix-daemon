#ifndef BITNETD_POLLER_H
#define BITNETD_POLLER_H

#include <stdint.h>

#define POLLER_IN  0x01
#define POLLER_OUT 0x02
#define POLLER_ERR 0x04
#define POLLER_HUP 0x08

typedef struct {
    int       fd;
    uint32_t  events;
    void     *userdata;
} poller_event_t;

typedef struct poller poller_t;

poller_t *poller_create(int max_events);
int       poller_add(poller_t *p, int fd, uint32_t events, void *userdata);
int       poller_mod(poller_t *p, int fd, uint32_t events);
int       poller_del(poller_t *p, int fd);
int       poller_wait(poller_t *p, poller_event_t *events, int max,
                      int timeout_ms);
void      poller_destroy(poller_t *p);

#endif
