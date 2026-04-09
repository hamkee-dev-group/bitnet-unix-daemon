#ifdef __linux__

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "poller.h"
#include <sys/epoll.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

struct poller {
    int                  epfd;
    int                  max;
    struct epoll_event  *ev_buf;
    void               **udata;
    int                  udata_sz;
};

poller_t *
poller_create(int max_events)
{
    poller_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (p->epfd < 0) { free(p); return NULL; }

    p->max    = max_events;
    p->ev_buf = calloc((size_t)max_events, sizeof(struct epoll_event));

    p->udata_sz = 1024;
    p->udata    = calloc((size_t)p->udata_sz, sizeof(void *));

    if (!p->ev_buf || !p->udata) {
        close(p->epfd);
        free(p->ev_buf);
        free(p->udata);
        free(p);
        return NULL;
    }
    return p;
}

static int
ensure_udata(poller_t *p, int fd)
{
    if (fd >= p->udata_sz) {
        int ns = fd + 256;
        void **nu = realloc(p->udata, (size_t)ns * sizeof(void *));
        if (!nu) return -1;
        for (int i = p->udata_sz; i < ns; i++)
            nu[i] = NULL;
        p->udata    = nu;
        p->udata_sz = ns;
    }
    return 0;
}

static uint32_t
to_epoll(uint32_t ev)
{
    uint32_t e = 0;
    if (ev & POLLER_IN)  e |= EPOLLIN;
    if (ev & POLLER_OUT) e |= EPOLLOUT;
    return e;
}

int
poller_add(poller_t *p, int fd, uint32_t events, void *userdata)
{
    if (ensure_udata(p, fd) < 0)
        return -1;
    p->udata[fd] = userdata;

    struct epoll_event ev = { .events = to_epoll(events), .data.fd = fd };
    return epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &ev);
}

int
poller_mod(poller_t *p, int fd, uint32_t events)
{
    struct epoll_event ev = { .events = to_epoll(events), .data.fd = fd };
    return epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &ev);
}

int
poller_del(poller_t *p, int fd)
{
    if (fd < p->udata_sz)
        p->udata[fd] = NULL;
    return epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL);
}

int
poller_wait(poller_t *p, poller_event_t *events, int max, int timeout_ms)
{
    int n = max > p->max ? p->max : max;
    int ret = epoll_wait(p->epfd, p->ev_buf, n, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    for (int i = 0; i < ret; i++) {
        events[i].fd = p->ev_buf[i].data.fd;
        events[i].events = 0;
        events[i].userdata = NULL;
        if (p->ev_buf[i].events & EPOLLIN)  events[i].events |= POLLER_IN;
        if (p->ev_buf[i].events & EPOLLOUT) events[i].events |= POLLER_OUT;
        if (p->ev_buf[i].events & EPOLLERR) events[i].events |= POLLER_ERR;
        if (p->ev_buf[i].events & EPOLLHUP) events[i].events |= POLLER_HUP;
        int fd = events[i].fd;
        if (fd >= 0 && fd < p->udata_sz)
            events[i].userdata = p->udata[fd];
    }
    return ret;
}

void
poller_destroy(poller_t *p)
{
    if (!p) return;
    close(p->epfd);
    free(p->ev_buf);
    free(p->udata);
    free(p);
}

#endif
