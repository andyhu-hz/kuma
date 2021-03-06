/* Copyright (c) 2014, Fengping Bao <jamol@live.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "IOPoll.h"
#include "Notifier.h"
#include "util/kmtrace.h"

#include <sys/epoll.h>

KUMA_NS_BEGIN

#define MAX_EPOLL_FDS   5000
#define MAX_EVENT_NUM   500

class EPoll : public IOPoll
{
public:
    EPoll();
    ~EPoll();

    bool init();
    KMError registerFd(SOCKET_FD fd, uint32_t events, IOCallback cb);
    KMError unregisterFd(SOCKET_FD fd);
    KMError updateFd(SOCKET_FD fd, uint32_t events);
    KMError wait(uint32_t wait_time_ms);
    void notify();
    PollType getType() const { return PollType::EPOLL; }
    bool isLevelTriggered() const { return false; }

private:
    uint32_t get_events(uint32_t kuma_events);
    uint32_t get_kuma_events(uint32_t events);

private:
    typedef std::map<int, IOCallback> IOCallbackMap;

    int             epoll_fd_;
    NotifierPtr     notifier_ { std::move(Notifier::createNotifier()) };
};

EPoll::EPoll()
: epoll_fd_(INVALID_FD)
{

}

EPoll::~EPoll()
{
    if(INVALID_FD != epoll_fd_) {
        close(epoll_fd_);
        epoll_fd_ = INVALID_FD;
    }
}

bool EPoll::init()
{
    epoll_fd_ = epoll_create(MAX_EPOLL_FDS);
    if(INVALID_FD == epoll_fd_) {
        return false;
    }
    if (!notifier_->ready()) {
        if(!notifier_->init()) {
            return false;
        }
        IOCallback cb ([this](uint32_t ev) { notifier_->onEvent(ev); });
        registerFd(notifier_->getReadFD(), KUMA_EV_READ|KUMA_EV_ERROR, std::move(cb));
    }
    return true;
}

uint32_t EPoll::get_events(uint32_t kuma_events)
{
    uint32_t ev = EPOLLET;//EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLET;
    if(kuma_events & KUMA_EV_READ) {
        ev |= EPOLLIN;
    }
    if(kuma_events & KUMA_EV_WRITE) {
        ev |= EPOLLOUT;
    }
    if(kuma_events & KUMA_EV_ERROR) {
        ev |= EPOLLERR | EPOLLHUP;
    }
    return ev;
}

uint32_t EPoll::get_kuma_events(uint32_t events)
{
    uint32_t ev = 0;
    if(events & EPOLLIN) {
        ev |= KUMA_EV_READ;
    }
    if(events & EPOLLOUT) {
        ev |= KUMA_EV_WRITE;
    }
    if(events & (EPOLLERR | EPOLLHUP)) {
        ev |= KUMA_EV_ERROR;
    }
    return ev;
}

KMError EPoll::registerFd(SOCKET_FD fd, uint32_t events, IOCallback cb)
{
    if (fd < 0) {
        return KMError::INVALID_PARAM;
    }
    resizePollItems(fd);
    int epoll_op = EPOLL_CTL_ADD;
    if (INVALID_FD != poll_items_[fd].fd) {
        epoll_op = EPOLL_CTL_MOD;
    }
    poll_items_[fd].fd = fd;
    poll_items_[fd].events = events;
    poll_items_[fd].cb = std::move(cb);
    struct epoll_event evt = {0};
    evt.data.ptr = (void*)(long)fd;
    evt.events = get_events(events);//EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLET;
    if(epoll_ctl(epoll_fd_, epoll_op, fd, &evt) < 0) {
        KUMA_ERRTRACE("EPoll::registerFd error, fd=" << fd << ", ev=" << evt.events << ", errno=" << errno);
        return KMError::FAILED;
    }
    KUMA_INFOTRACE("EPoll::registerFd, fd=" << fd << ", ev=" << evt.events);

    return KMError::NOERR;
}

KMError EPoll::unregisterFd(SOCKET_FD fd)
{
    int max_fd = int(poll_items_.size() - 1);
    KUMA_INFOTRACE("EPoll::unregisterFd, fd="<<fd<<", max_fd="<<max_fd);
    if (fd < 0 || fd > max_fd) {
        KUMA_WARNTRACE("EPoll::unregisterFd, failed, max_fd=" << max_fd);
        return KMError::INVALID_PARAM;
    }
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL);
    if(fd < max_fd) {
        poll_items_[fd].reset();
    } else if (fd == max_fd) {
        poll_items_.pop_back();
    }
    return KMError::NOERR;
}

KMError EPoll::updateFd(SOCKET_FD fd, uint32_t events)
{
    if(fd < 0 || fd >= poll_items_.size() || INVALID_FD == poll_items_[fd].fd) {
        return KMError::FAILED;
    }
    struct epoll_event evt = {0};
    evt.data.ptr = (void*)(long)fd;
    evt.events = get_events(events);
    if(epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &evt) < 0) {
        KUMA_ERRTRACE("EPoll::updateFd error, fd="<<fd<<", errno="<<errno);
        return KMError::FAILED;
    }
    poll_items_[fd].events = events;
    return KMError::NOERR;
}

KMError EPoll::wait(uint32_t wait_ms)
{
    struct epoll_event events[MAX_EVENT_NUM];
    int nfds = epoll_wait(epoll_fd_, events, MAX_EVENT_NUM , wait_ms);
    if (nfds < 0) {
        if(errno != EINTR) {
            KUMA_ERRTRACE("EPoll::wait, errno="<<errno);
        }
        KUMA_INFOTRACE("EPoll::wait, nfds="<<nfds<<", errno="<<errno);
    } else {
        for (int i=0; i<nfds; ++i) {
            SOCKET_FD fd = (SOCKET_FD)(long)events[i].data.ptr;
            if(fd < poll_items_.size()) {
                IOCallback &cb = poll_items_[fd].cb;
                if(cb) cb(get_kuma_events(events[i].events));
            }
        }
    }
    return KMError::NOERR;
}

void EPoll::notify()
{
    notifier_->notify();
}

IOPoll* createEPoll() {
    return new EPoll();
}

KUMA_NS_END
