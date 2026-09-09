#include "graphics/BopomofoIME.h"

#if defined(BOPOMOFO_IME_LVGL)

#include "ui.h"
#include "util/FileLoader.h"
#include "util/ILog.h"

namespace
{

// Rows 1-4, columns 1-10, are the number, q, a and z rows of a physical Daqian
// keyboard, which is also the grid every Taiwanese phone keyboard shows. The
// labels are not written out here: they are read back from the layout table at
// build time, so a key can never end up captioned with a symbol the composer
// would not produce for it. ㄦ lives on '-', to the right of '0', and keeps
// that place at the end of the first row.
const char *const KEY_ROWS[] = {
    "1234567890-",
    "qwertyuiop",
    "asdfghjkl;",
    "zxcvbnm,./",
};

// Punctuation, addressed by index so that a button carries a byte rather than
// a pointer into a vector that is rebuilt on every keystroke.
const char *const TEXT_KEYS[] = {"，", "。", "？", "！"};

// Latin words would be wider than the keys they have to fit on, and the
// symbol font the keyboard inherits is not the one that carries these glyphs.
const char *const KEY_TO_LATIN = "英數";
const char *const KEY_SPACE    = "空白";
const char *const KEY_CONFIRM  = "送出";
const char *const KEY_MORE     = "▼";
const char *const KEY_COLLAPSE = "▲";
const char *const KEY_PREV     = "◀";
const char *const KEY_NEXT     = "▶";

// Width units. LVGL normalises each row on its own, so these only have to be
// consistent within a row.
constexpr uint8_t ROW_UNITS      = 11; // what a full Bopomofo row adds up to
constexpr uint8_t COMPOSE_UNITS  = 3;  // the read-out at the head of the candidate row
constexpr uint8_t MAX_CAND_UNITS = 4;  // a four-character word, the longest the dictionary holds

// The expanded grid. Five rows of candidates plus one control row comes to the
// same six rows the key map has, which is what keeps the keyboard from
// resizing its buttons when the grid opens. Five columns on a 320px-wide panel
// leaves a comfortable touch target; a four-character word is the only length
// that needs two of them at the font the keyboard runs.
constexpr uint8_t GRID_ROWS       = 5;
constexpr uint8_t GRID_UNITS      = 5;
constexpr size_t  GRID_WIDE_CHARS = 4;

// Enough to fill three pages of the grid, which is what the panel can show
// before the words are too rare to be worth paging to. The engine gathers with
// its own headroom and hands over the best of them, so asking for more costs a
// longer sort, not a longer walk. Candidates are addressed by an absolute index
// that has to survive in a byte, which is the ceiling this may not cross.
constexpr int MAX_CANDIDATES = GRID_ROWS * GRID_UNITS * 3;

size_t utf8CharLen(unsigned char c)
{
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

size_t utf8Count(const std::string &s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i += utf8CharLen((unsigned char)s[i]))
        n++;
    return n;
}

std::string utf8LastChar(const std::string &s)
{
    size_t start = 0;
    for (size_t i = 0; i < s.size(); i += utf8CharLen((unsigned char)s[i]))
        start = i;
    return s.substr(start);
}

// The preference record the physical-keyboard and joystick front ends already
// write, byte for byte: version, then the input mode in bit 0 and the layout in
// bit 1. This front end has no layout switch, so bit 1 is read and written back
// untouched - the same board can be reflashed between builds that do have one.
constexpr char    PREFS_PATH[]   = "L:/prefs/ime.dat";
constexpr uint8_t PREFS_VERSION  = 1;
constexpr uint8_t PREFS_CHINESE  = 0x01;

} // namespace

BopomofoIME &BopomofoIME::instance()
{
    static BopomofoIME ime;
    return ime;
}

void BopomofoIME::init(lv_obj_t *keyboard)
{
    kb_        = keyboard;
    latinFont_ = lv_obj_get_style_text_font(keyboard, LV_PART_MAIN);
    engine_.setMaxCandidates(MAX_CANDIDATES);
    loadPrefs();
    rebuildMap();
}

bool BopomofoIME::active() const
{
    return kb_ && chinese_ && lv_keyboard_get_mode(kb_) == LV_KEYBOARD_MODE_USER_1;
}

void BopomofoIME::attach(lv_obj_t *textarea)
{
    if (!kb_)
        return;
    ta_ = textarea;
    engine_.reset();
    grid_      = false;
    candFirst_ = 0;
    applyKeyboardMode();
}

void BopomofoIME::detach()
{
    engine_.reset();
    grid_      = false;
    candFirst_ = 0;
    storePrefs();
}

void BopomofoIME::toggleMode()
{
    if (!kb_)
        return;
    chinese_ = !chinese_;
    engine_.reset();
    grid_      = false;
    candFirst_ = 0;
    applyKeyboardMode();
}

void BopomofoIME::applyKeyboardMode()
{
    if (!kb_)
        return;

    if (chinese_) {
        // With no text area attached, LVGL's own handler stops before it can
        // type a Bopomofo symbol into the message; everything is left to
        // handleButton(), which types through ta_.
        lv_keyboard_set_textarea(kb_, nullptr);
        // The keyboard inherits LVGL's built-in Montserrat, which has no Han
        // characters and no fallback. The generated face does.
        lv_obj_set_style_text_font(kb_, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
        rebuildMap();
        lv_keyboard_set_mode(kb_, LV_KEYBOARD_MODE_USER_1);
    } else {
        if (latinFont_)
            lv_obj_set_style_text_font(kb_, latinFont_, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_keyboard_set_mode(kb_, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(kb_, ta_);
    }
}

void BopomofoIME::refreshCandidates()
{
    if (engine_.composing())
        engine_.refresh();
    else
        engine_.clearCandidates();
    candFirst_ = 0;
}

void BopomofoIME::insertText(const char *utf8)
{
    if (ta_)
        lv_textarea_add_text(ta_, utf8);
}

void BopomofoIME::commit(const std::string &word)
{
    if (word.empty())
        return;
    insertText(word.c_str());
    grid_ = false;

    // Offer what usually follows the character just typed. The list this
    // produces is not a composition, so the composition read-out stays empty
    // and Backspace goes back to editing the message.
    engine_.predictAfter(utf8LastChar(word));
    candFirst_ = 0;
}

bool BopomofoIME::handleButton()
{
    if (!active())
        return false;

    uint32_t id = lv_buttonmatrix_get_selected_button(kb_);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE || id >= info_.size())
        return false;

    const CellInfo cell = info_[id];
    switch (cell.kind) {
    case Cell::None:
        return true;

    case Cell::Candidate: {
        const bool  wasPrediction = engine_.isPrediction();
        std::string word          = engine_.select(cell.data);
        if (word.empty())
            return true;
        // A predicted word starts with the character that produced it, so that
        // character has to give way to the whole word.
        if (wasPrediction && ta_)
            lv_textarea_delete_char(ta_);
        commit(word);
        break;
    }

    case Cell::ExpandGrid:
        grid_       = true;
        candFirst_  = 0;
        page_       = 0;
        pageStarts_ = {0};
        break;

    case Cell::GridPage:
        if (cell.data == 0) {
            if (page_ > 0)
                candFirst_ = pageStarts_[--page_];
        } else if (candFirst_ + candShown_ < (int)engine_.candidates().size()) {
            if (page_ + 1 == (int)pageStarts_.size())
                pageStarts_.push_back(candFirst_ + candShown_);
            candFirst_ = pageStarts_[++page_];
        } else {
            // Past the last page, back to the first: the way out of the end of
            // the list without a second press.
            page_      = 0;
            candFirst_ = 0;
        }
        break;

    case Cell::CloseGrid:
        grid_      = false;
        candFirst_ = 0;
        page_      = 0;
        break;

    case Cell::Symbol:
        engine_.addKey((char)cell.data);
        refreshCandidates();
        break;

    case Cell::Text:
        if (engine_.composing() && engine_.hasCandidates()) {
            std::string word = engine_.select(0);
            if (!word.empty())
                commit(word);
        }
        engine_.clearCandidates();
        insertText(TEXT_KEYS[cell.data]);
        candFirst_ = 0;
        break;

    case Cell::Space:
        if (engine_.composing()) {
            if (engine_.hasCandidates()) {
                std::string word = engine_.select(0);
                commit(word);
            } else {
                engine_.addSpace(); // first tone, which carries no mark
                refreshCandidates();
            }
        } else {
            engine_.clearCandidates();
            insertText(" ");
            candFirst_ = 0;
        }
        break;

    case Cell::Backspace:
        if (engine_.composing()) {
            engine_.backspace();
            refreshCandidates();
        } else {
            engine_.clearCandidates();
            if (ta_)
                lv_textarea_delete_char(ta_);
            candFirst_ = 0;
        }
        break;

    case Cell::Confirm:
        if (engine_.composing() && engine_.hasCandidates()) {
            std::string word = engine_.select(0);
            commit(word);
            break;
        }
        engine_.reset();
        candFirst_ = 0;
        rebuildMap();
        // The Latin keyboard's checkmark reaches the text area through LVGL's
        // own handler, which is out of the way here, so the event is sent by
        // hand: this is what actually sends a message.
        if (ta_)
            lv_obj_send_event(ta_, LV_EVENT_READY, nullptr);
        if (confirm_)
            confirm_();
        return true;

    case Cell::ToLatin:
        toggleMode();
        return true;
    }

    rebuildMap();
    return true;
}

// The control word is built by OR-ing LVGL's flags with a width, which in C++
// is an int; the cast back to the enum is where that lands.
void BopomofoIME::addButton(const std::string &text, uint32_t ctrl, Cell kind, uint8_t data)
{
    cells_.push_back(text);
    ctrl_.push_back((lv_buttonmatrix_ctrl_t)ctrl);
    info_.push_back({kind, data});
}

// Row breaks live in the map but not in the control or info arrays, which is
// what makes an info_ index the same thing as a button id.
void BopomofoIME::newRow()
{
    cells_.push_back("\n");
}

void BopomofoIME::rebuildMap()
{
    cells_.clear();
    map_.clear();
    ctrl_.clear();
    info_.clear();
    cells_.reserve(64);
    map_.reserve(72);

    if (grid_)
        buildCandidateGrid();
    else
        buildKeyMap();

    // The map LVGL keeps is an array of pointers into cells_, so it has to be
    // rebuilt after the last push_back and handed over immediately.
    for (const std::string &c : cells_)
        map_.push_back(c.c_str());
    map_.push_back("");

    lv_keyboard_set_map(kb_, LV_KEYBOARD_MODE_USER_1, map_.data(), ctrl_.data());
}

void BopomofoIME::buildKeyMap()
{
    // ── candidate row ────────────────────────────────────────────
    // Always present, empty or not: LVGL divides the keyboard's height by the
    // number of rows, so a row that comes and goes would resize every key under
    // it on the keystroke that produced the first candidate.
    const std::vector<std::string> &cands = engine_.candidates();
    const std::string               comp  = engine_.composingText();
    candShown_ = 0;
    {
        addButton(comp.empty() ? " " : comp, LV_BUTTONMATRIX_CTRL_DISABLED | COMPOSE_UNITS, Cell::None, 0);

        // The row always starts at the top of the list; paging through the rest
        // is the grid's job, and candFirst_ is the grid's page start alone.
        int units = ROW_UNITS - COMPOSE_UNITS - 1; // one unit held back for the grid key
        int shown = 0;
        while (shown < (int)cands.size() && units > 0) {
            const std::string &c = cands[shown];
            uint8_t w = (uint8_t)utf8Count(c);
            if (w < 1)
                w = 1;
            if (w > MAX_CAND_UNITS)
                w = MAX_CAND_UNITS;
            if (w > units)
                break;
            addButton(c, LV_BUTTONMATRIX_CTRL_POPOVER | w, Cell::Candidate, (uint8_t)shown);
            units -= w;
            shown++;
        }
        // Nothing fitted, which only happens when the first candidate is wider
        // than the row: show it anyway rather than an empty bar.
        if (shown == 0 && !cands.empty()) {
            addButton(cands[0], MAX_CAND_UNITS, Cell::Candidate, 0);
            shown = 1;
        }
        candShown_ = shown;
        // Whatever did not fit is reachable through the grid, so the key is
        // there exactly when the row is showing less than the whole list.
        if (shown < (int)cands.size())
            addButton(KEY_MORE, 1, Cell::ExpandGrid, 0);
        else if (shown == 0)
            addButton(" ", LV_BUTTONMATRIX_CTRL_DISABLED | (ROW_UNITS - COMPOSE_UNITS), Cell::None, 0);
        newRow();
    }

    // ── Bopomofo grid ────────────────────────────────────────────
    // Column 11 of rows 2-4 carries the keys that have no Bopomofo symbol;
    // row 1 already holds eleven of them, ㄦ included.
    for (size_t row = 0; row < sizeof(KEY_ROWS) / sizeof(KEY_ROWS[0]); row++) {
        for (const char *k = KEY_ROWS[row]; *k; k++) {
            const bpmf::Symbol *sym = engine_.mapKey(*k);
            if (!sym)
                continue;
            addButton(sym->utf8, LV_BUTTONMATRIX_CTRL_POPOVER | 1, Cell::Symbol, (uint8_t)*k);
        }
        if (row == 1)
            addButton("刪", 1, Cell::Backspace, 0);
        else if (row == 2)
            addButton(TEXT_KEYS[0], 1, Cell::Text, 0);
        else if (row == 3)
            addButton(TEXT_KEYS[1], 1, Cell::Text, 1);
        newRow();
    }

    // ── control row ──────────────────────────────────────────────
    addButton(KEY_TO_LATIN, 2, Cell::ToLatin, 0);
    addButton(KEY_SPACE, 5, Cell::Space, 0);
    addButton(TEXT_KEYS[2], 1, Cell::Text, 2);
    addButton(TEXT_KEYS[3], 1, Cell::Text, 3);
    addButton(KEY_CONFIRM, 2, Cell::Confirm, 0);
}

// ── candidate grid ───────────────────────────────────────────────
// The expanded picker is the keyboard itself with a different map, not a
// second widget laid over it: the keyboard's own position depends on where the
// text area ended up, so an overlay would have to chase it, and a panel of its
// own would have to repeat the styling, the font and the event wiring for no
// gain. Selecting from it commits and closes, which is what the user came for;
// the collapse key leaves the composition exactly as it was.
void BopomofoIME::buildCandidateGrid()
{
    const std::vector<std::string> &cands = engine_.candidates();

    int idx = candFirst_;
    if (idx >= (int)cands.size())
        idx = candFirst_ = 0;

    int shown = 0;
    for (uint8_t row = 0; row < GRID_ROWS; row++) {
        int units = GRID_UNITS;
        while (idx + shown < (int)cands.size() && units > 0) {
            const std::string &c = cands[idx + shown];
            const uint8_t      w = utf8Count(c) >= GRID_WIDE_CHARS ? 2 : 1;
            if (w > units)
                break;
            addButton(c, LV_BUTTONMATRIX_CTRL_POPOVER | w, Cell::Candidate, (uint8_t)(idx + shown));
            units -= w;
            shown++;
        }
        // A short row is padded rather than left to LVGL, which would stretch
        // the few buttons on it to the full width and break the column grid.
        if (units > 0)
            addButton(" ", LV_BUTTONMATRIX_CTRL_DISABLED | (uint8_t)units, Cell::None, 0);
        newRow();
    }
    candShown_ = shown;

    const std::string comp      = engine_.composingText();
    const bool        morePages = (idx + shown) < (int)cands.size();
    addButton(comp.empty() ? " " : comp, LV_BUTTONMATRIX_CTRL_DISABLED | 2, Cell::None, 0);
    addButton(KEY_PREV, (page_ > 0 ? 0u : LV_BUTTONMATRIX_CTRL_DISABLED) | 1u, Cell::GridPage, 0);
    // Forward wraps to the first page, so the end of the list is one press from
    // the start rather than a walk back through every page.
    addButton(KEY_NEXT, (morePages || page_ > 0 ? 0u : LV_BUTTONMATRIX_CTRL_DISABLED) | 1u, Cell::GridPage, 1);
    addButton(KEY_COLLAPSE, 1, Cell::CloseGrid, 0);
}

void BopomofoIME::loadPrefs()
{
    lv_fs_file_t f;
    if (lv_fs_open(&f, PREFS_PATH, LV_FS_MODE_RD) != LV_FS_RES_OK)
        return;
    uint8_t  buf[2] = {0, 0};
    uint32_t read   = 0;
    if (lv_fs_read(&f, buf, sizeof(buf), &read) == LV_FS_RES_OK && read == sizeof(buf) && buf[0] == PREFS_VERSION)
        chinese_ = (buf[1] & PREFS_CHINESE) != 0;
    lv_fs_close(&f);
    storedMode_ = chinese_;
}

void BopomofoIME::storePrefs()
{
    if (chinese_ == storedMode_)
        return;

    // Read first so that the layout bit, which this front end has no way of
    // setting, survives being written back.
    uint8_t      buf[2] = {PREFS_VERSION, 0};
    lv_fs_file_t f;
    if (lv_fs_open(&f, PREFS_PATH, LV_FS_MODE_RD) == LV_FS_RES_OK) {
        uint32_t read = 0;
        lv_fs_read(&f, buf, sizeof(buf), &read);
        lv_fs_close(&f);
        buf[0] = PREFS_VERSION;
    }
    buf[1] = (uint8_t)((buf[1] & ~PREFS_CHINESE) | (chinese_ ? PREFS_CHINESE : 0));

    if (lv_fs_open(&f, PREFS_PATH, LV_FS_MODE_WR) != LV_FS_RES_OK) {
        ILOG_WARN("IME prefs not written");
        return;
    }
    uint32_t written = 0;
    lv_fs_write(&f, buf, sizeof(buf), &written);
    lv_fs_close(&f);
    storedMode_ = chinese_;
}

#endif // BOPOMOFO_IME_LVGL
