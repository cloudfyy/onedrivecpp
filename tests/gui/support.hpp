#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <stdexcept>

namespace onedrive::test::gui_support {

inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

template <typename Predicate>
bool wait_until(
    Predicate predicate, qint64 timeout_ms = 5000, unsigned long sleep_ms = 1
) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        QThread::msleep(sleep_ms);
    }
    return predicate();
}

} // namespace onedrive::test::gui_support
