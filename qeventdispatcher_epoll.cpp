/*****************************************************************************
** Copyright (C) 2011 Connected Table AB
**
** An epoll based event dispatcher for Qt.
**
** Notable work by Steffen Hansen and Tobias Nätterlund of KDAB
******************************************************************************/

#include <qplatformdefs.h>

#include <qcoreapplication.h>
#include <qpair.h>
#include <qsocketnotifier.h>
#include <qthread.h>
#include <qelapsedtimer.h>

#include "qeventdispatcher_epoll.h"
#include <private/qthread_p.h>
#include <private/qcoreapplication_p.h>
#include <private/qcore_unix_p.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>

#include <QDebug>

QT_BEGIN_NAMESPACE

/*****************************************************************************
 UNIX signal handling
 *****************************************************************************/

QEventDispatcherEpollPrivate::QEventDispatcherEpollPrivate()
{
    epollFD = epoll_create(16384);
//    qDebug() << "epollFD =" << epollFD;

    bool pipefail = false;

    // initialize the common parts of the event loop
    if (qt_safe_pipe(thread_pipe, O_NONBLOCK) == -1) {
        perror("QEventDispatcherEpollPrivate(): Unable to create thread pipe");
        pipefail = true;
    }

    if (pipefail)
        qFatal("QEventDispatcherEpollPrivate(): Can not continue without a thread pipe");

    epoll_event ev;
    memset(&ev,0,sizeof(ev));
    ev.data.fd = thread_pipe[0];
    ev.events = EPOLLIN;//| EPOLLOUT | EPOLLPRI;
    int rc = epoll_ctl(epollFD, EPOLL_CTL_ADD, thread_pipe[0], &ev);
    if( rc != 0 ) perror("QEventDispatcherEpollPrivate::doSelect(), epoll_ctl failed: ");

    interrupt = false;
}

QEventDispatcherEpollPrivate::~QEventDispatcherEpollPrivate()
{
    // cleanup the common parts of the event loop
    close(thread_pipe[0]);
    close(thread_pipe[1]);

    close(epollFD);

    // cleanup timers
    qDeleteAll(timerList);
}

#define EVENT_COUNT 100

int QEventDispatcherEpollPrivate::doSelect(QEventLoop::ProcessEventsFlags flags, timeval *timeout)
{
    Q_Q(QEventDispatcherEpoll);

//    qDebug() << "QEventDispatcherEpollPrivate::doSelect(" << flags << timeout << ")";

    // needed in QEventDispatcherUNIX::select()
    timerList.updateCurrentTime();

    int nsel = 0;
    epoll_event events[EVENT_COUNT];
    memset(events,0,sizeof(epoll_event)*EVENT_COUNT);
    int nevents = 0;
    do {
        if(timeout)
            nsel = epoll_wait(epollFD, events, EVENT_COUNT, timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
        else // timeout == NULL means wait indefinitely for select(), -1 does the same for epoll_wait()
            nsel = epoll_wait(epollFD, events, EVENT_COUNT, -1);
        //qDebug() << "nsel after epoll_wait =" << nsel;
    } while (nsel == -1 && (errno == EINTR || errno == EAGAIN));

    if (nsel == -1) {
        // shouldn't happen, so let's complain to stderr
        // and hope someone sends us a bug report
        perror("epoll_wait()");
    }

    // some other thread woke us up... consume the data on the thread pipe so that
    // select doesn't immediately return next time
    for(int i = 0; i < nsel; ++i) {
        if(events[i].data.fd == thread_pipe[0]) {
            //qDebug("Reading thread_pipe[0]");
            char c[16];
            while (::read(thread_pipe[0], c, sizeof(c)) > 0)
                ;
            if (!wakeUps.testAndSetRelease(1, 0)) {
                // hopefully, this is dead code
                qWarning("QEventDispatcherEpoll: internal error, wakeUps.testAndSetRelease(1, 0) failed!");
            }
            ++nevents;
            break;
        }
    }

    // activate socket notifiers
    if(! (flags & QEventLoop::ExcludeSocketNotifiers) && nsel > 0)
        return nevents + q->activateSocketNotifiers(nsel, events);
    else
        return nevents;
}

QEventDispatcherEpoll::QEventDispatcherEpoll(QObject *parent)
    : QAbstractEventDispatcher(*new QEventDispatcherEpollPrivate, parent)
{ }

QEventDispatcherEpoll::QEventDispatcherEpoll(QEventDispatcherEpollPrivate &dd, QObject *parent)
    : QAbstractEventDispatcher(dd, parent)
{ }

QEventDispatcherEpoll::~QEventDispatcherEpoll()
{
    Q_D(QEventDispatcherEpoll);
    d->threadData->eventDispatcher = 0;
}

/*!
    \internal
*/
void QEventDispatcherEpoll::registerTimer(int timerId, int interval, QObject *obj)
{
#ifndef QT_NO_DEBUG
    if (timerId < 1 || interval < 0 || !obj) {
        qWarning("QEventDispatcherEpoll::registerTimer: invalid arguments");
        return;
    } else if (obj->thread() != thread() || thread() != QThread::currentThread()) {
        qWarning("QObject::startTimer: timers cannot be started from another thread");
        return;
    }
#endif

    Q_D(QEventDispatcherEpoll);
    d->timerList.registerTimer(timerId, interval, obj);
}

/*!
    \internal
*/
bool QEventDispatcherEpoll::unregisterTimer(int timerId)
{
#ifndef QT_NO_DEBUG
    if (timerId < 1) {
        qWarning("QEventDispatcherEpoll::unregisterTimer: invalid argument");
        return false;
    } else if (thread() != QThread::currentThread()) {
        qWarning("QObject::killTimer: timers cannot be stopped from another thread");
        return false;
    }
#endif

    Q_D(QEventDispatcherEpoll);
    return d->timerList.unregisterTimer(timerId);
}

/*!
    \internal
*/
bool QEventDispatcherEpoll::unregisterTimers(QObject *object)
{
#ifndef QT_NO_DEBUG
    if (!object) {
        qWarning("QEventDispatcherEpoll::unregisterTimers: invalid argument");
        return false;
    } else if (object->thread() != thread() || thread() != QThread::currentThread()) {
        qWarning("QObject::killTimers: timers cannot be stopped from another thread");
        return false;
    }
#endif

    Q_D(QEventDispatcherEpoll);
    return d->timerList.unregisterTimers(object);
}

QList<QEventDispatcherEpoll::TimerInfo>
QEventDispatcherEpoll::registeredTimers(QObject *object) const
{
    if (!object) {
        qWarning("QEventDispatcherEpoll::registeredTimers: invalid argument");
        return QList<TimerInfo>();
    }

    Q_D(const QEventDispatcherEpoll);
    return d->timerList.registeredTimers(object);
}

/*****************************************************************************
 QEventDispatcher implementations for UNIX
 *****************************************************************************/

static int epoll_event_type_from_socknot_type( int type )
{
    switch(type) {
    case QSocketNotifier::Read: return EPOLLIN;
    case QSocketNotifier::Write: return EPOLLOUT;
    }
    return 0;
}

void QEventDispatcherEpoll::registerSocketNotifier(QSocketNotifier *notifier)
{
    Q_D(QEventDispatcherEpoll);

    //qDebug() << "###registering " << notifier << " fd: " << notifier->socket() << " type:" << notifier->type();

    Q_ASSERT(notifier);
    const int sockfd = notifier->socket();
    const int type = notifier->type();
#ifndef QT_NO_DEBUG
    if (sockfd < 0) {
        qWarning("QSocketNotifier: Internal error");
        return;
    } else if (notifier->thread() != thread()
               || thread() != QThread::currentThread()) {
        qWarning("QSocketNotifier: socket notifiers cannot be enabled from another thread");
        return;
    }
#endif

    int newevent = epoll_event_type_from_socknot_type(type);

    QEventDispatcherEpollPrivate::SocketNotifierList::iterator it = d->socketNotifiers.find(sockfd);
    if( it != d->socketNotifiers.end() ) {
        /* We are already monitoring this FD,
         * add it to the list and figure out
         * if we are monitoring the right events
         * already.
         */
        int events = 0;
        Q_FOREACH( QSocketNotifier* sn, *it ) {
            events |= epoll_event_type_from_socknot_type(sn->type());
        }
        (*it).push_back(notifier);

        if( (events | newevent) != events ) {
            /* We are not, so add the new event type to the set */
            epoll_event ep;
            memset(&ep,0,sizeof(ep));
            ep.events = events | newevent;
            ep.data.fd = sockfd;
            int rc = epoll_ctl(d->epollFD, EPOLL_CTL_MOD, sockfd, &ep);
            if (rc != 0) {
                if (errno == ENOENT) {
                    /* Huh, gone? Try adding it */
                    int rc = epoll_ctl(d->epollFD, EPOLL_CTL_ADD, sockfd, &ep);
                    if( rc != 0 ) perror("QEventDispatcherEpoll::registerSocketNotifier(), epoll_ctl ADD failed: ");
                } else
                    perror("QEventDispatcherEpoll::registerSocketNotifier(), epoll_ctl MOD failed: ");
            }
        }
    } else {
        d->socketNotifiers.insert(sockfd,QList<QSocketNotifier*>() << notifier);

        epoll_event ep;
        memset(&ep,0,sizeof(ep));
        ep.events = newevent;
        ep.data.fd = sockfd;

        int rc = epoll_ctl(d->epollFD, EPOLL_CTL_ADD, sockfd, &ep);
        if( rc != 0 ) perror("QEventDispatcherEpoll::registerSocketNotifier(), epoll_ctl ADD failed: ");
    }
}

void QEventDispatcherEpoll::unregisterSocketNotifier(QSocketNotifier *notifier)
{
    Q_D(QEventDispatcherEpoll);

    Q_ASSERT(notifier);
    const int sockfd = notifier->socket();
    //qDebug() << "###unregistering " << notifier << " fd: " << notifier->socket();
#ifndef QT_NO_DEBUG
    if (sockfd < 0) {
        qWarning("QSocketNotifier: Internal error");
        return;
    } else if (notifier->thread() != thread()
               || thread() != QThread::currentThread()) {
        qWarning("QSocketNotifier: socket notifiers cannot be disabled from another thread");
        return;
    }
#endif
    d->sn_pending_list.removeAll(notifier);

    QEventDispatcherEpollPrivate::SocketNotifierList::iterator it = d->socketNotifiers.find(sockfd);
    if( it != d->socketNotifiers.end() ) {
        (*it).removeOne(notifier);
        if( (*it).isEmpty() ) {
            epoll_event dummy;
            memset(&dummy,0,sizeof(dummy));
            int rc = epoll_ctl(d->epollFD, EPOLL_CTL_DEL, sockfd, &dummy);
            if( rc != 0 ) perror("QEventDispatcherEpoll::unregisterSocketNotifier(), epoll_ctl failed: ");
        } else {
            int events = 0;
            Q_FOREACH( QSocketNotifier* sn, *it ) {
                events |= epoll_event_type_from_socknot_type(sn->type());
            }
            epoll_event ep;
            memset(&ep,0,sizeof(ep));
            ep.events = events;
            ep.data.fd = sockfd;
            int rc = epoll_ctl(d->epollFD, EPOLL_CTL_MOD, sockfd, &ep);
            if( rc != 0 ) perror("QEventDispatcherEpoll::registerSocketNotifier(), epoll_ctl MOD failed: ");
        }
    }
}

int QEventDispatcherEpoll::activateTimers()
{
    Q_ASSERT(thread() == QThread::currentThread());
    Q_D(QEventDispatcherEpoll);
    return d->timerList.activateTimers();
}

int QEventDispatcherEpoll::activateSocketNotifiers(int nevents, epoll_event* events)
{
    //qDebug() << "QEventDispatcherEpoll::activateSocketNotifiers(" << nevents << events << ")";
    Q_D(QEventDispatcherEpoll);
    if(nevents <= 0)
        return 0;

    // activate entries
    int n_act = 0;
    QEvent event(QEvent::SockAct);
    for(int i = 0; i < nevents; ++i) {
        if(events[i].data.fd == d->thread_pipe[0] /*|| events[i].data.fd == d->thread_pipe[1]*/) {
            //qDebug() << "thread pipe!";
            //epoll_ctl(d->epollFD, EPOLL_CTL_DEL, events[i].data.fd, &events[i]);
            continue;
        }

        QEventDispatcherEpollPrivate::SocketNotifierList::iterator it = d->socketNotifiers.find(events[i].data.fd);
        if( it != d->socketNotifiers.end() ) {
            d->sn_pending_list = *it;
            while (!d->sn_pending_list.isEmpty()) {
                QSocketNotifier* notifier = d->sn_pending_list.takeLast();
                Q_ASSERT(notifier);

                QCoreApplication::sendEvent(notifier, &event);
            }
        }

        ++n_act;
    }

    return n_act;
}

bool QEventDispatcherEpoll::processEvents(QEventLoop::ProcessEventsFlags flags)
{
    //qDebug("QEventDispatcherEpoll::processEvents");
    Q_D(QEventDispatcherEpoll);
    d->interrupt = false;

    // we are awake, broadcast it
    Q_EMIT awake();
    QCoreApplicationPrivate::sendPostedEvents(0, 0, d->threadData);

    int nevents = 0;
    const bool canWait = (d->threadData->canWait
                          && !d->interrupt
                          && (flags & QEventLoop::WaitForMoreEvents));

    if (canWait)
        Q_EMIT aboutToBlock();

    if (!d->interrupt) {
        // return the maximum time we can wait for an event.
        timeval *tm = 0;
        timeval wait_tm = { 0l, 0l };
        if (!(flags & QEventLoop::X11ExcludeTimers)) {
            if (d->timerList.timerWait(wait_tm))
                tm = &wait_tm;
        }

        if (!canWait) {
            if (!tm)
                tm = &wait_tm;

            // no time to wait
            tm->tv_sec  = 0l;
            tm->tv_usec = 0l;
        }

        nevents = d->doSelect(flags, tm);

        // activate timers
        if (! (flags & QEventLoop::X11ExcludeTimers)) {
            nevents += activateTimers();
        }
    }
    //qDebug("Returning from processEvents");
    // return true if we handled events, false otherwise
    return (nevents > 0);
}

bool QEventDispatcherEpoll::hasPendingEvents()
{
    extern uint qGlobalPostedEventsCount(); // from qapplication.cpp
    return qGlobalPostedEventsCount();
}

void QEventDispatcherEpoll::wakeUp()
{
    Q_D(QEventDispatcherEpoll);
    if (d->wakeUps.testAndSetAcquire(0, 1)) {
        char c = 0;
        qt_safe_write( d->thread_pipe[1], &c, 1 );
    }
}

void QEventDispatcherEpoll::interrupt()
{
    Q_D(QEventDispatcherEpoll);
    d->interrupt = true;
    wakeUp();
}

void QEventDispatcherEpoll::flush()
{ }

QT_END_NAMESPACE
