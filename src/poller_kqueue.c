#if defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__APPLE__)

#include "poller.h"
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

struct poller {
    int             kqfd;
    int             max;
    struct kevent  *ev_buf;
    void          **udata;
    int             udata_sz;
};

poller_t *
poller_create(int max_events)
{
    poller_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->kqfd = kqueue();
    if (p->kqfd < 0) { free(p); return NULL; }

    p->max    = max_events;
    p->ev_buf = calloc((size_t)max_events, sizeof(struct kevent));

    p->udata_sz = 1024;
    p->udata    = calloc((size_t)p->udata_sz, sizeof(void *));

    if (!p->ev_buf || !p->udata) {
        close(p->kqfd);
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

int
poller_add(poller_t *p, int fd, uint32_t events, void *userdata)
{
    if (ensure_udata(p, fd) < 0) return -1;
    p->udata[fd] = userdata;

    struct kevent kev[2];
    int n = 0;
    if (events & POLLER_IN)
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (events & POLLER_OUT)
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (n == 0) return 0;
    return kevent(p->kqfd, kev, n, NULL, 0, NULL);
}

int
poller_mod(poller_t *p, int fd, uint32_t events)
{
    struct kevent kev[4];
    int n = 0;
    EV_SET(&kev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(p->kqfd, kev, n, NULL, 0, NULL);

    n = 0;
    if (events & POLLER_IN)
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (events & POLLER_OUT)
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (n == 0) return 0;
    return kevent(p->kqfd, kev, n, NULL, 0, NULL);
}

int
poller_del(poller_t *p, int fd)
{
    if (fd < p->udata_sz)
        p->udata[fd] = NULL;
    struct kevent kev[2];
    EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(p->kqfd, kev, 2, NULL, 0, NULL);
    return 0;
}

int
poller_wait(poller_t *p, poller_event_t *events, int max, int timeout_ms)
{
    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int n = max > p->max ? p->max : max;
    int ret = kevent(p->kqfd, NULL, 0, p->ev_buf, n, tsp);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    for (int i = 0; i < ret; i++) {
        int fd = (int)p->ev_buf[i].ident;
        events[i].fd     = fd;
        events[i].events = 0;
        events[i].userdata = NULL;
        if (p->ev_buf[i].filter == EVFILT_READ)  events[i].events |= POLLER_IN;
        if (p->ev_buf[i].filter == EVFILT_WRITE) events[i].events |= POLLER_OUT;
        if (p->ev_buf[i].flags & EV_EOF)         events[i].events |= POLLER_HUP;
        if (p->ev_buf[i].flags & EV_ERROR)       events[i].events |= POLLER_ERR;
        if (fd >= 0 && fd < p->udata_sz)
            events[i].userdata = p->udata[fd];
    }
    return ret;
}

void
poller_destroy(poller_t *p)
{
    if (!p) return;
    close(p->kqfd);
    free(p->ev_buf);
    free(p->udata);
    free(p);
}

#endif
