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

template <typename Predicate> bool wait_until(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

} // namespace onedrive::test::gui_support
