#include "TagSidebar.h"

#include "Icons.h"
#include "TagColors.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QToolTip>
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

constexpr int kCollapsedWidth = 40;  // strip width when collapsed
constexpr int kExpandedWidth = 240;  // full sidebar width
// Display order of tag groups; unknown categories go last, alphabetically.
int categoryRank(const QString& category) {
    static const QStringList order = {
        QStringLiteral("artist"),  QStringLiteral("copyright"), QStringLiteral("character"),
        QStringLiteral("species"), QStringLiteral("model"),     QStringLiteral("general"),
        QStringLiteral("meta"),    QStringLiteral("usergroup"),
    };
    const int rank = order.indexOf(category.isEmpty() ? QStringLiteral("general") : category);
    return rank < 0 ? int(order.size()) : rank;
}

// Text budgets per row at expanded width.
constexpr int kTextBudgetRemovable = 150;
constexpr int kTextBudgetPlain = 180;

}  // namespace

TagSidebar::TagSidebar(QWidget* parent) : QWidget(parent) {
    buildUi();
    setCollapsed(true);
}

void TagSidebar::buildUi() {
    setObjectName(QStringLiteral("tagSidebar"));  // styled in Theme.cpp
    setAttribute(Qt::WA_StyledBackground);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(4);

    m_header = new QPushButton(QStringLiteral("Tags"), this);
    m_header->setObjectName(QStringLiteral("sidebarHeader"));
    m_header->setCursor(Qt::PointingHandCursor);
    connect(m_header, &QPushButton::clicked, this, [this]() { setCollapsed(!m_collapsed); });
    root->addWidget(m_header);

    // Collapsible body
    m_body = new QVBoxLayout();
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(4);

    m_tagList = new QListWidget(this);
    m_tagList->setObjectName(QStringLiteral("tagList"));
    m_tagList->installEventFilter(this);
    m_tagList->setSelectionMode(QAbstractItemView::NoSelection);
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
    m_addButton->setIcon(assetIcon(QStringLiteral("plus.svg")));

    // Stacked vertically: side-by-side the button squeezes the line edit down.
    auto* addBox = new QVBoxLayout();
    addBox->setContentsMargins(8, 4, 8, 0);
    addBox->setSpacing(6);
    addBox->addWidget(m_tagEdit);
    addBox->addWidget(m_addButton);
    m_body->addLayout(addBox);

    root->addLayout(m_body, 1);
    root->addStretch(0);  // keeps the header at the top while collapsed

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
    // Tag / md5 rows: click copies, double-click opens a search tab. The
    // first click of a double-click also copies, which is harmless.
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick) {
        auto* widget = qobject_cast<QWidget*>(watched);
        const auto it = widget ? m_tagLabels.constFind(widget) : m_tagLabels.constEnd();
        if (it != m_tagLabels.constEnd() &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                emit tagSearchRequested(it->search);
            } else if (QClipboard* clipboard = QGuiApplication::clipboard()) {
                clipboard->setText(it->copy);
                QToolTip::showText(QCursor::pos(),
                                   QStringLiteral("Copied \u201C%1\u201D").arg(it->copy),
                                   widget, {}, 1500);
            }
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
        const int ra = categoryRank(a.category);
        const int rb = categoryRank(b.category);
        if (ra != rb)
            return ra < rb;
        if (a.category != b.category)
            return a.category < b.category;
        return a.name < b.name;
    });

    auto groupOf = [](const SourceTag& tag) {
        return tag.category.isEmpty() ? QStringLiteral("general") : tag.category;
    };

    QString currentGroup;
    for (int i = 0; i < tags.size(); ++i) {
        const SourceTag& tag = tags.at(i);
        const QString group = groupOf(tag);

        // Group header: colored dot + CATEGORY + count.
        if (i == 0 || group != currentGroup) {
            currentGroup = group;
            int count = 0;
            for (int j = i; j < tags.size() && groupOf(tags.at(j)) == group; ++j) ++count;

            auto* headerItem = new QListWidgetItem(m_tagList);
            headerItem->setFlags(Qt::NoItemFlags);
            auto* header = new QLabel(m_tagList);
            header->setProperty("role", QStringLiteral("sectionHeader"));
            header->setTextFormat(Qt::RichText);
            header->setText(QStringLiteral("<span style='color:%1;'>&#9679;</span>&nbsp; %2"
                                           "&nbsp;&nbsp;<span style='font-weight:400;'>%3</span>")
                                .arg(categoryColorHtml(group), group.toUpper().toHtmlEscaped())
                                .arg(count));
            header->setContentsMargins(6, i == 0 ? 4 : 12, 6, 2);
            headerItem->setSizeHint(header->sizeHint());
            m_tagList->setItemWidget(headerItem, header);
        }

        const bool removable = m_editable && !(tag.category == QLatin1String("usergroup") &&
                                               tag.name == QLatin1String("favorite"));

        const QString text = tag.name;

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
        const LabelAction action{tag.name, tag.name};
        label->installEventFilter(this);
        m_tagLabels.insert(label, action);
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
        auto* item = new QListWidgetItem(QStringLiteral("No tags"), m_tagList);
        item->setFlags(Qt::NoItemFlags);
        item->setForeground(palette().placeholderText());
    }

    // File hash: copy it, or double-click to search md5:<hash>.
    if (!m_image.md5.isEmpty()) {
        auto* headerItem = new QListWidgetItem(m_tagList);
        headerItem->setFlags(Qt::NoItemFlags);
        auto* header = new QLabel(QStringLiteral("MD5"), m_tagList);
        header->setProperty("role", QStringLiteral("sectionHeader"));
        header->setContentsMargins(6, 12, 6, 2);
        headerItem->setSizeHint(header->sizeHint());
        m_tagList->setItemWidget(headerItem, header);

        auto* item = new QListWidgetItem(m_tagList);
        auto* label = new QLabel(m_tagList);
        label->setContentsMargins(6, 2, 6, 2);
        const QFontMetrics fm(label->font());
        label->setText(fm.elidedText(m_image.md5, Qt::ElideMiddle, kTextBudgetPlain));
        label->setToolTip(QStringLiteral("%1\nClick to copy, double-click to search")
                              .arg(m_image.md5));
        label->setCursor(Qt::PointingHandCursor);
        label->setProperty("role", QStringLiteral("hint"));
        label->installEventFilter(this);
        m_tagLabels.insert(label, {m_image.md5, QStringLiteral("md5:") + m_image.md5});
        item->setSizeHint(label->sizeHint());
        m_tagList->setItemWidget(item, label);
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

    m_header->setText(collapsed ? QString() : QStringLiteral("Tags"));
    m_header->setIcon(assetIcon(collapsed ? QStringLiteral("chevron-right.svg")
                                          : QStringLiteral("chevron-left.svg")));
    m_header->setToolTip(collapsed ? QStringLiteral("Show tags") : QStringLiteral("Hide tags"));

    setFixedWidth(collapsed ? kCollapsedWidth : kExpandedWidth);

    if (!collapsed && m_editable && m_tagEdit)
        m_tagEdit->setFocus();

    emit collapsedChanged(collapsed);
}

}  // namespace Mosaic
