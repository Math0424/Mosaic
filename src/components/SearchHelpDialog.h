#pragma once

#include <QDialog>

namespace Mosaic {

// Reference card for the search box: tag syntax, OR-groups, exclusions,
// metatags, sort orders and the "feed:NN" recommender ranking. Opened from
// the app (hamburger) menu.
class SearchHelpDialog : public QDialog {
    Q_OBJECT

   public:
    explicit SearchHelpDialog(QWidget* parent = nullptr);
};

}  // namespace Mosaic
