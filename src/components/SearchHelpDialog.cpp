#include "SearchHelpDialog.h"

#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace Mosaic {

namespace {

// Keep in sync with LibrarySql::splitTagGroups / orderByClause and
// LibraryCatalog::search.
const char* const kHelpHtml = R"HTML(
<style>
  h2 { margin: 14px 0 4px 0; }
  td { padding: 2px 14px 2px 0; vertical-align: top; }
  code { font-family: Consolas, monospace; }
  p.note { margin: 2px 0 6px 0; }
</style>

<h2>Basics</h2>
<p class="note">Type tags separated by spaces. Every tag must match (AND).
Tags are case-insensitive; use underscores for spaces (<code>long_hair</code>).</p>
<table>
<tr><td><code>cat dog</code></td><td>Posts with <b>both</b> cat and dog</td></tr>
<tr><td><code>-dog</code></td><td>Exclude posts tagged dog</td></tr>
<tr><td><code>~cat ~dog</code></td><td>Posts with cat <b>or</b> dog</td></tr>
<tr><td><code>( ~cat ~dog ) ( ~red ~blue )</code></td><td>(cat or dog) and (red or blue)</td></tr>
<tr><td><code>cat*</code></td><td>Wildcard: any tag starting with "cat" (<code>*</code> works anywhere)</td></tr>
<tr><td><code>artist:name</code></td><td>Only match the tag within that category</td></tr>
</table>
<p class="note">Categories: <code>general</code>, <code>artist</code>, <code>character</code>,
<code>species</code>, <code>meta</code>, <code>model</code>, <code>usergroup</code>.</p>

<h2>Library metatags</h2>
<table>
<tr><td><code>usergroup:favorite</code></td><td>Your favorited posts</td></tr>
<tr><td><code>md5:&lt;hash&gt;</code></td><td>One exact file</td></tr>
<tr><td><code>id:&lt;number&gt;</code></td><td>Post id from the site it was saved from</td></tr>
</table>

<h2>Sorting (library)</h2>
<p class="note"><code>order:</code> and <code>sort:</code> are interchangeable; the last one in the
query wins. Without one, newest imports come first.</p>
<table>
<tr><td><code>order:imported</code></td><td>Newest imports first (default)</td></tr>
<tr><td><code>order:created</code></td><td>Newest posts by original upload date
  (also <code>date</code>, <code>created_at</code>)</td></tr>
<tr><td><code>order:random</code></td><td>Random order</td></tr>
<tr><td><code>order:id</code> / <code>order:id_desc</code></td><td>Site post id, ascending / descending</td></tr>
<tr><td><code>order:filesize</code> / <code>order:filesize_asc</code></td><td>Largest / smallest file first</td></tr>
<tr><td><code>order:mpixels</code> / <code>order:mpixels_asc</code></td><td>Highest / lowest resolution first</td></tr>
<tr><td><code>order:landscape</code> / <code>order:portrait</code></td><td>Wide / tall images first</td></tr>
</table>

<h2>For You ranking (library)</h2>
<p class="note"><code>feed:NN</code> orders the results with the For You feed's
recommender, the same model that learns from your likes, saves and skips.
<code>NN</code> runs from 0 to 100 and sets how much it sticks to your taste
versus exploring. It replaces any <code>order:</code> and works together with tag filters.</p>
<table>
<tr><td><code>feed:100</code></td><td>Purely what it predicts you like most. Same order every time.</td></tr>
<tr><td><code>feed:70</code></td><td>Mostly your taste, with some surprises mixed in</td></tr>
<tr><td><code>feed:50</code></td><td>Half taste, half exploration</td></tr>
<tr><td><code>feed:0</code></td><td>Full exploration: posts the model knows least about or has never
  seen you react to, shuffled differently on each search</td></tr>
<tr><td><code>cat feed:80</code></td><td>Your cat posts, best matches first</td></tr>
</table>
<p class="note">Ranking a library search doesn't train the model. Only reactions in the feed do.
Searching again (page 1) draws a fresh ranking; later pages keep the same order.</p>

<h2>Site tabs</h2>
<p class="note">Searches on a site tab go to that site, so its own syntax applies
(for example <code>rating:safe</code>, <code>score:&gt;50</code>, <code>order:score</code>).
Your tag blacklist from Settings is added to every search as exclusions.
<code>feed:</code> only works in the library.</p>
)HTML";

}  // namespace

SearchHelpDialog::SearchHelpDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Search help"));
    resize(640, 680);

    auto* layout = new QVBoxLayout(this);
    auto* browser = new QTextBrowser(this);
    browser->setOpenLinks(false);
    browser->setHtml(QString::fromUtf8(kHelpHtml));
    layout->addWidget(browser);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

}  // namespace Mosaic
