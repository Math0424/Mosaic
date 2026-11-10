#include "WebServerDialog.h"

#include "WebServer.h"
#include "../core/AppState.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

namespace Mosaic {

WebServerDialog::WebServerDialog(AppState& state, WebServer& server,
                                 std::function<bool(QString*)> apply, QWidget* parent)
    : QDialog(parent), m_state(state), m_server(server), m_apply(std::move(apply)) {
    setWindowTitle(QStringLiteral("Web server"));
    resize(520, 380);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        QStringLiteral("Browse the For You feed and your library from a phone or browser. "
                       "The page runs on this PC, so Mosaic has to stay open."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form = new QFormLayout();
    m_enabled = new QCheckBox(QStringLiteral("Serve on this network"), this);
    m_enabled->setChecked(m_state.webServerEnabled());
    form->addRow(m_enabled);
    m_port = new QSpinBox(this);
    m_port->setRange(1024, 65535);
    m_port->setValue(m_state.webServerPort());
    form->addRow(QStringLiteral("Port:"), m_port);
    root->addLayout(form);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    m_urls = new QListWidget(this);
    m_urls->setToolTip(QStringLiteral("Double-click to open, select and Copy to share"));
    connect(m_urls, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem* item) {
        QDesktopServices::openUrl(QUrl(item->text()));
    });
    root->addWidget(m_urls, 1);

    auto* note = new QLabel(
        QStringLiteral("<small>Links for other devices include an access key: anyone on your "
                       "network who has the link can see your library and feed. Windows may ask "
                       "to let Mosaic through the firewall the first time.</small>"),
        this);
    note->setWordWrap(true);
    root->addWidget(note);

    auto* buttons = new QHBoxLayout();
    auto* copyButton = new QPushButton(QStringLiteral("Copy link"), this);
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        QListWidgetItem* item = m_urls->currentItem();
        if (!item && m_urls->count() > 1)
            item = m_urls->item(1);  // first LAN link: what a phone needs
        if (!item && m_urls->count() > 0)
            item = m_urls->item(0);
        if (item)
            QApplication::clipboard()->setText(item->text());
    });
    auto* newKeyButton = new QPushButton(QStringLiteral("New access key"), this);
    newKeyButton->setToolTip(QStringLiteral("Signs out every other device"));
    connect(newKeyButton, &QPushButton::clicked, this, [this]() {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("New access key"),
            QStringLiteral("Make a new access key? Devices using the old link must open the "
                           "new one."));
        if (answer != QMessageBox::Yes)
            return;
        m_state.setWebServerToken(WebServer::generateToken());
        applyChanges();
    });
    auto* applyButton = new QPushButton(QStringLiteral("Apply"), this);
    connect(applyButton, &QPushButton::clicked, this, &WebServerDialog::applyChanges);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(copyButton);
    buttons->addWidget(newKeyButton);
    buttons->addStretch(1);
    buttons->addWidget(applyButton);
    buttons->addWidget(closeButton);
    root->addLayout(buttons);

    connect(m_enabled, &QCheckBox::toggled, this, &WebServerDialog::applyChanges);

    refresh();
}

void WebServerDialog::applyChanges() {
    m_state.setWebServerEnabled(m_enabled->isChecked());
    m_state.setWebServerPort(m_port->value());
    QString error;
    const bool ok = m_apply(&error);
    refresh(ok ? QString() : error);
}

void WebServerDialog::refresh(const QString& error) {
    m_urls->clear();
    if (!error.isEmpty()) {
        m_status->setText(QStringLiteral("<b>Could not start:</b> %1").arg(error.toHtmlEscaped()));
        return;
    }
    if (!m_server.isRunning()) {
        m_status->setText(QStringLiteral("Stopped."));
        return;
    }
    const QStringList urls = m_server.urls();
    m_status->setText(
        urls.size() > 1
            ? QStringLiteral("Running on port %1. Open a link below; on a phone use one of the "
                             "network addresses (same Wi-Fi).")
                  .arg(m_server.port())
            : QStringLiteral("Running on port %1, but no network address was found - only this "
                             "PC can connect.")
                  .arg(m_server.port()));
    m_urls->addItems(urls);
}

}  // namespace Mosaic
