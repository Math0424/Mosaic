#pragma once

#include <QDialog>

#include <functional>

class QCheckBox;
class QLabel;
class QListWidget;
class QSpinBox;

namespace Mosaic {

class AppState;
class WebServer;

// Tools > Web server: turns the local web app on/off, picks the port and
// lists the addresses to open it at (on a phone: the LAN link with key).
class WebServerDialog : public QDialog {
   public:
    // `apply` (re)starts or stops the server from the state; returns false
    // with an error message when it could not start.
    WebServerDialog(AppState& state, WebServer& server,
                    std::function<bool(QString*)> apply, QWidget* parent = nullptr);

   private:
    void applyChanges();
    void refresh(const QString& error = {});

    AppState& m_state;
    WebServer& m_server;
    std::function<bool(QString*)> m_apply;
    QCheckBox* m_enabled = nullptr;
    QSpinBox* m_port = nullptr;
    QLabel* m_status = nullptr;
    QListWidget* m_urls = nullptr;
};

}  // namespace Mosaic
