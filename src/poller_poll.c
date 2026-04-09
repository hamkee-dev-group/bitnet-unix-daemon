#if defined(USE_POLL_BACKEND) || \
    (!defined(__linux__) && !defined(__OpenBSD__) && \
     !defined(__FreeBSD__) && !defined(__APPLE__))

#include "poller.h"
#include <poll.h>
#include <stdlib.h>
#include <errno.h>

struct poller {
    struct pollfd  *fds;
    void          **udata;
    int             count;
    int             cap;
};

poller_t *
poller_create(int max_events)
{
    poller_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->cap   = max_events > 0 ? max_events : 64;
    p->fds   = calloc((size_t)p->cap, sizeof(struct pollfd));
    p->udata = calloc((size_t)p->cap, sizeof(void *));
    if (!p->fds || !p->udata) {
        free(p->fds);
        free(p->udata);
        free(p);
        return NULL;
    }
    return p;
}

static int
find_slot(poller_t *p, int fd)
{
    for (int i = 0; i < p->count; i++)
        if (p->fds[i].fd == fd)
            return i;
    return -1;
}

int
poller_add(poller_t *p, int fd, uint32_t events, void *userdata)
{
    if (p->count >= p->cap) {
        int nc = p->cap * 2;
        struct pollfd *nf = realloc(p->fds, (size_t)nc * sizeof(struct pollfd));
        void **nu = realloc(p->udata, (size_t)nc * sizeof(void *));
        if (!nf || !nu) return -1;
        p->fds   = nf;
        p->udata = nu;
        p->cap   = nc;
    }
    int idx = p->count++;
    p->fds[idx].fd = fd;
    p->fds[idx].events = 0;
    p->fds[idx].revents = 0;
    if (events & POLLER_IN)  p->fds[idx].events |= POLLIN;
    if (events & POLLER_OUT) p->fds[idx].events |= POLLOUT;
    p->udata[idx] = userdata;
    return 0;
}

int
poller_mod(poller_t *p, int fd, uint32_t events)
{
    int idx = find_slot(p, fd);
    if (idx < 0) return -1;
    p->fds[idx].events = 0;
    if (events & POLLER_IN)  p->fds[idx].events |= POLLIN;
    if (events & POLLER_OUT) p->fds[idx].events |= POLLOUT;
    return 0;
}

int
poller_del(poller_t *p, int fd)
{
    int idx = find_slot(p, fd);
    if (idx < 0) return -1;
    p->count--;
    if (idx < p->count) {
        p->fds[idx]   = p->fds[p->count];
        p->udata[idx] = p->udata[p->count];
    }
    return 0;
}

int
poller_wait(poller_t *p, poller_event_t *events, int max, int timeout_ms)
{
    int ret = poll(p->fds, (unsigned long)p->count, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    int out = 0;
    for (int i = 0; i < p->count && out < max && out < ret; i++) {
        if (p->fds[i].revents == 0)
            continue;
        events[out].fd       = p->fds[i].fd;
        events[out].events   = 0;
        events[out].userdata = p->udata[i];
        if (p->fds[i].revents & POLLIN)  events[out].events |= POLLER_IN;
        if (p->fds[i].revents & POLLOUT) events[out].events |= POLLER_OUT;
        if (p->fds[i].revents & POLLERR) events[out].events |= POLLER_ERR;
        if (p->fds[i].revents & POLLHUP) events[out].events |= POLLER_HUP;
        out++;
    }
    return out;
}

void
poller_destroy(poller_t *p)
{
    if (!p) return;
    free(p->fds);
    free(p->udata);
    free(p);
}

#endif
