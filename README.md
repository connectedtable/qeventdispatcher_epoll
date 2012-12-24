An epoll() based event dispatcher for Qt.

Increased performance and lower cpu usage over the default select() based
dispatcher on certain types of applications, such as a server that handles
a lot of concurrent tcp connections.

Usage (Qt 4):
Simply include the header file and define an epoll event dispatcher in main
before creating the Qt application object.

```c++
int main(int argc, char** argv)
{
    QEventDispatcherEpoll epollDispatcher;
    QCoreApplication app(argc, argv);
    ...
    return app.exec();
}
```

Usage (Qt 5):

Simply include the header file and define an epoll event dispatcher in main
before creating the Qt application object.

```c++
int main(int argc, char** argv)
{
    QCoreApplication::setEventDispatcher(new QEventDispatcherEpoll);
    QCoreApplication app(argc, argv);
    ...
    return app.exec();
}
```

and add this line to `.pro` file:

```
QT += core-private
```

Version history:

2011-11-15: - Initial github release.

Copyright (C) 2011 Connected Table AB <info@connectedtable.com>
