/*****************************************************************************
** Copyright (C) 2011 Connected Table AB
**
** An epoll based event dispatcher for Qt.
**
** Notable work by Steffen Hansen and Tobias Nätterlund of KDAB
******************************************************************************/

#ifndef QEVENTDISPATCHER_EPOLL
#define QEVENTDISPATCHER_EPOLL

#include <QtCore/qabstracteventdispatcher.h>
#include <QtCore/qhash.h>
#include <QtCore/qlist.h>
#include <QtCore/qsocketnotifier.h>

#if QT_VERSION >= 0x050000
#include <private/qtimerinfo_unix_p.h>
#endif

struct epoll_event;

QT_BEGIN_NAMESPACE

#if QT_VERSION < 0x050000
// internal timer info
struct QTimerInfo {
    int id;           // - timer identifier
    timeval interval; // - timer interval
    timeval timeout;  // - when to sent event
    QObject *obj;     // - object to receive event
    QTimerInfo **activateRef; // - ref from activateTimers
};

class QTimerInfoList : public QList<QTimerInfo*>
{
#if ((_POSIX_MONOTONIC_CLOCK-0 <= 0) && !defined(Q_OS_MAC)) || defined(QT_BOOTSTRAPPED)
    timeval previousTime;
    clock_t previousTicks;
    int ticksPerSecond;
    int msPerTick;

    bool timeChanged(timeval *delta);
#endif

    // state variables used by activateTimers()
    QTimerInfo *firstTimerInfo;

public:
    QTimerInfoList();

    timeval currentTime;
    timeval updateCurrentTime();

    // must call updateCurrentTime() first!
    void repairTimersIfNeeded();

    bool timerWait(timeval &);
    void timerInsert(QTimerInfo *);
    void timerRepair(const timeval &);

    void registerTimer(int timerId, int interval, QObject *object);
    bool unregisterTimer(int timerId);
    bool unregisterTimers(QObject *object);
    QList<QPair<int, int> > registeredTimers(QObject *object) const;

    int activateTimers();
};

#endif

class QEventDispatcherEpollPrivate;

class Q_CORE_EXPORT QEventDispatcherEpoll : public QAbstractEventDispatcher
{
    Q_OBJECT
    Q_DECLARE_PRIVATE(QEventDispatcherEpoll)

public:
    explicit QEventDispatcherEpoll(QObject *parent = 0);
    ~QEventDispatcherEpoll();

    bool processEvents(QEventLoop::ProcessEventsFlags flags);
    bool hasPendingEvents();

    void registerSocketNotifier(QSocketNotifier *notifier);
    void unregisterSocketNotifier(QSocketNotifier *notifier);

#if QT_VERSION < 0x050000
    void registerTimer(int timerId, int interval, QObject *obj);
#else
    void registerTimer(int timerId, int interval, Qt::TimerType timerType, QObject *obj);
    int remainingTime(int timerId);
#endif
    bool unregisterTimer(int timerId);
    bool unregisterTimers(QObject *object);
    QList<TimerInfo> registeredTimers(QObject *object) const;

    void wakeUp();
    void interrupt();
    void flush();

protected:
    QScopedPointer<QEventDispatcherEpollPrivate> d_ptr;

//    void setSocketNotifierPending(QSocketNotifier *notifier);

    int activateTimers();
    int activateSocketNotifiers(int nevents, epoll_event* events);

};

class Q_CORE_EXPORT QEventDispatcherEpollPrivate
{
    Q_DECLARE_PUBLIC(QEventDispatcherEpoll)
    QEventDispatcherEpoll* const q_ptr;
public:
    QEventDispatcherEpollPrivate(QEventDispatcherEpoll* const q);
    ~QEventDispatcherEpollPrivate();

    int doSelect(QEventLoop::ProcessEventsFlags flags, timeval *timeout);

    int thread_pipe[2];

    // 3 socket notifier types - read, write and exception
    //QSockNotType sn_vec[3];

    QTimerInfoList timerList;

    typedef QHash<int,QList<QSocketNotifier*> > SocketNotifierList;

    SocketNotifierList socketNotifiers;

    // pending socket notifiers list
    QList<QSocketNotifier*> sn_pending_list;

    QAtomicInt wakeUps;
    bool interrupt;

    int epollFD;
};

QT_END_NAMESPACE

#endif // QEVENTDISPATCHER_EPOLL
