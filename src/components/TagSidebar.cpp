#include "TagSidebar.h"

#include "Icons.h"
#include "TagColors.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QStringListModel>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace Mosaic {

namespace {

constexpr int kCollapsedWidth = 34;  // strip width when collapsed
constexpr int kExpandedWidth = 220;  // full sidebar width
// Text budgets per row at expanded width.
constexpr int kTextBudgetRemovable = 150;
constexpr int kTextBudgetPlain = 180;

}  // namespace

TagSidebar::TagSidebar(QWidget* parent) : QWidget(parent) {
    buildUi();
    setCollapsed(true);
}

void TagSidebar::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    m_header = new QPushButton(QStringLiteral("Tags"), this);
    m_header->setFlat(true);
    m_header->setStyleSheet(
        QStringLiteral("QPushButton { text-align: left; font-weight: bold; padding: 4px; }"));
    connect(m_header, &QPushButton::clicked, this, [this]() { setCollapsed(!m_collapsed); });
    root->addWidget(m_header);

    // Collapsible body
    m_body = new QVBoxLayout();
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(4);

    m_tagList = new QListWidget(this);
    m_tagList->installEventFilter(this);
    m_tagList->setAlternatingRowColors(true);
    m_tagList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_body->addWidget(m_tagList, 1);

    // Add-tag input (local images only)
    m_tagEdit = new QLineEdit(this);
    m_tagEdit->setPlaceholderText(QStringLiteral("Add tag (category:name for new tags)"));
    m_tagEdit->setClearButtonEnabled(true);

    m_completer = new QCompleter(this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setFilterMode(Qt::MatchStartsWith);
    m_completer->setMaxVisibleItems(12);
    m_tagEdit->setCompleter(m_completer);
    if (m_completer->popup())
        m_completer->popup()->setFocusPolicy(Qt::NoFocus);

    m_tagEdit->installEventFilter(this);

    m_addButton = new QPushButton(QStringLiteral("Add tag"), this);

    // Stacked vertically: side-by-side the button squeezes the line edit down.
    m_body->addWidget(m_tagEdit);
    m_body->addWidget(m_addButton);

    root->addLayout(m_body, 1);

    connect(m_addButton, &QPushButton::clicked, this, [this]() {
        const QString tag = m_tagEdit->text().trimmed();
        if (!tag.isEmpty())
            emit addTagRequested(tag);
        m_tagEdit->clear();
    });
    connect(m_tagEdit, &QLineEdit::returnPressed, this, [this]() {
        const QString tag = m_tagEdit->text().trimmed();
        if (!tag.isEmpty())
            emit addTagRequested(tag);
        m_tagEdit->clear();
    });
}

bool TagSidebar::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (auto* widget = qobject_cast<QWidget*>(watched);
            widget && m_tagLabels.contains(widget)) {
            emit tagSearchRequested(m_tagLabels.value(widget));
            return true;
        }
    }
    // Don't propagate wheel at list edges (would switch posts in the viewer).
    if (watched == m_tagList && event->type() == QEvent::Wheel) {
        QScrollBar* bar = m_tagList->verticalScrollBar();
        const bool canScroll = bar && bar->maximum() > bar->minimum();
        if (!canScroll)
            return true;
        auto* wheel = static_cast<QWheelEvent*>(event);
        const bool atTop = bar->value() <= bar->minimum() && wheel->angleDelta().y() > 0;
        const bool atBottom = bar->value() >= bar->maximum() && wheel->angleDelta().y() < 0;
        if (atTop || atBottom)
            return true;
        return false;
    }
    if (watched == m_tagEdit) {
        if (event->type() == QEvent::MouseButtonPress) {
            m_tagEdit->setFocus();
            return false;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Tab) {
                QString completion;
                if (m_completer->popup() && m_completer->popup()->isVisible() &&
                    m_completer->popup()->currentIndex().isValid()) {
                    completion = m_completer->popup()->currentIndex().data().toString();
                }
                if (completion.isEmpty())
                    completion = m_completer->currentCompletion();
                if (completion.isEmpty() && m_completer->completionCount() == 1) {
                    completion = m_completer->completionModel()->index(0, 0).data().toString();
                }
                if (completion.isEmpty())
                    return false;
                m_tagEdit->setText(completion);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TagSidebar::setImage(const SourceImage& image, bool editable) {
    m_image = image;
    m_editable = editable;
    rebuildList();
    if (m_tagEdit)
        m_tagEdit->setVisible(!m_collapsed && m_editable);
    if (m_addButton)
        m_addButton->setVisible(!m_collapsed && m_editable);
}

void TagSidebar::rebuildList() {
    m_tagList->clear();
    m_tagLabels.clear();

    QList<SourceTag> tags = m_image.tags;
    std::sort(tags.begin(), tags.end(), [](const SourceTag& a, const SourceTag& b) {
        if (a.category != b.category) {
            return a.category < b.category;
        }
        return a.name < b.name;
    });

    for (const SourceTag& tag : tags) {
        const bool removable = m_editable && !(tag.category == QLatin1String("usergroup") &&
                                               tag.name == QLatin1String("favorite"));

        QString text = tag.name;
        if (!tag.category.isEmpty() && tag.category != QLatin1String("general")) {
            text += QStringLiteral("  [%1]").arg(tag.category);
        }

        auto* item = new QListWidgetItem(m_tagList);
        auto* row = new QWidget(m_tagList);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(6, 2, 6, 2);
        rowLayout->setSpacing(6);

        auto* label = new QLabel(row);
        const QFontMetrics fm(label->font());
        const int budget = removable ? kTextBudgetRemovable : kTextBudgetPlain;
        label->setText(fm.elidedText(text, Qt::ElideMiddle, budget));
        label->setToolTip(text);
        label->setStyleSheet(QStringLiteral("color: %1;").arg(categoryColor(tag.category).name()));
        label->setTextInteractionFlags(Qt::NoTextInteraction);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setCursor(Qt::PointingHandCursor);
        const QString tagName = tag.name;
        label->installEventFilter(this);
        m_tagLabels.insert(label, tagName);
        rowLayout->addWidget(label, 1);

        if (removable) {
            auto* removeButton = new QToolButton(row);
            removeButton->setIcon(assetIcon(QStringLiteral("close.svg")));
            removeButton->setToolTip(QStringLiteral("Remove tag"));
            removeButton->setAutoRaise(true);
            removeButton->setFixedSize(20, 20);
            removeButton->setCursor(Qt::PointingHandCursor);
            connect(removeButton, &QToolButton::clicked, this,
                    [this, tag]() { emit removeTagRequested(tag.name); });
            rowLayout->addWidget(removeButton);
        }

        item->setSizeHint(row->sizeHint());
        item->setToolTip(QStringLiteral("Category: %1").arg(tag.category));
        m_tagList->setItemWidget(item, row);
    }

    if (tags.isEmpty()) {
        auto* item = new QListWidgetItem(QStringLiteral("(no tags)"), m_tagList);
        item->setForeground(palette().placeholderText());
    }
}

void TagSidebar::refreshCompletions(const QStringList& tagNames) {
    m_completer->setModel(new QStringListModel(tagNames, m_completer));
}

bool TagSidebar::isCollapsed() const { return m_collapsed; }

void TagSidebar::setCollapsed(bool collapsed) {
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;

    m_tagList->setVisible(!collapsed);
    m_tagEdit->setVisible(!collapsed && m_editable);
    m_addButton->setVisible(!collapsed && m_editable);

    m_header->setText(collapsed ? QStringLiteral("\u25B8") : QStringLiteral("\u25BE Tags"));
    m_header->setToolTip(collapsed ? QStringLiteral("Show tags") : QStringLiteral("Hide tags"));

    setFixedWidth(collapsed ? kCollapsedWidth : kExpandedWidth);

    if (!collapsed && m_editable && m_tagEdit)
        m_tagEdit->setFocus();

    emit collapsedChanged(collapsed);
}

}  // namespace Mosaic
