#include <QApplication>
#include "Magnivo.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Magnivo");
    app.setOrganizationName("Magnivo");

    Magnivo mag;
    mag.show();

    int rc = app.exec();
    // Magnivo destructor resets MagSetFullscreenTransform(1.0)
    // so the screen is back to normal on X / Esc close.
    return rc;
}
