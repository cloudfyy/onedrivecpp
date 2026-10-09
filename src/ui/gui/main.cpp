#include <QApplication>
#include <QLabel>
#include <QMainWindow>

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};

    QMainWindow window;
    window.setWindowTitle("OneDrive C++");
    window.resize(800, 600);

    auto* status = new QLabel(
        "The desktop interface is ready for account and synchronization "
        "features."
    );
    status->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(status);

    window.show();
    return application.exec();
}
