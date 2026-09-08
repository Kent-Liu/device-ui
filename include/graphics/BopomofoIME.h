#pragma once

#if defined(BOPOMOFO_IME_LVGL)

#include "graphics/bpmf/bpmf_engine.h"
#include "lvgl.h"
#include <functional>
#include <string>
#include <vector>

// Bopomofo input for the touch UI.
//
// The stock keyboard is reused rather than duplicated: the Bopomofo layout is
// installed as LV_KEYBOARD_MODE_USER_1 and the candidate list is the map's
// first row, so the panel keeps its size, its animation and its styling, and a
// keystroke costs one map rebuild instead of a second widget tree.
//
// While the Bopomofo map is up the keyboard's text area is cleared. LVGL's
// default handler inserts a button's own text into whatever text area the
// keyboard points at, which for a Bopomofo key is never what the user meant;
// with no text area it returns early and leaves every press to this class,
// which types through the text area pointer it kept for itself.
//
// Everything below the engine (composition, dictionary, candidate ordering) is
// shared verbatim with the physical-keyboard and joystick front ends; this file
// is only the LVGL half.
class BopomofoIME
{
  public:
    static BopomofoIME &instance();

    // Installs the Bopomofo map on the keyboard. Safe to call once the EEZ
    // screens exist.
    void init(lv_obj_t *keyboard);

    // The showKeyboard() / hideKeyboard() seams. attach() remembers where
    // committed text goes and puts the keyboard into the stored input mode.
    void attach(lv_obj_t *textarea);
    void detach();

    // The keyboard icon key, which switches between Latin and Bopomofo.
    void toggleMode();

    // True while the Bopomofo map is the one on screen, which is also the
    // condition under which the view must leave key handling to this class:
    // the button indices it switches on belong to the Latin map.
    bool active() const;

    // LV_EVENT_VALUE_CHANGED from the keyboard. Returns false when the press
    // was not ours, which only happens for the Latin map.
    bool handleButton();

    // Run when the user confirms with no composition in progress, i.e. the
    // Latin map's checkmark. The view owns what closing the keyboard means.
    void setConfirmCallback(std::function<void()> cb) { confirm_ = std::move(cb); }

  private:
    BopomofoIME() = default;

    enum class Cell : uint8_t {
        None,        // the composition read-out, which is not a button
        Candidate,   // data = index into the candidate list
        ExpandGrid,  // swap the key map for the full candidate grid
        GridPage,    // next page of the grid, wrapping at the end
        CloseGrid,   // back to the keys, composition untouched
        Symbol,      // data = ASCII key handed to the engine
        Text,        // data = index into extraText_, inserted as it stands
        Space,
        Backspace,
        Confirm,
        ToLatin
    };

    struct CellInfo {
        Cell    kind;
        uint8_t data;
    };

    void rebuildMap();
    void buildKeyMap();
    void buildCandidateGrid();
    void addButton(const std::string &text, uint32_t ctrl, Cell kind, uint8_t data);
    void newRow();
    void refreshCandidates();
    void commit(const std::string &word);
    void insertText(const char *utf8);
    void applyKeyboardMode();
    void loadPrefs();
    void storePrefs();

    bpmf::Engine engine_;
    lv_obj_t    *kb_ = nullptr;
    lv_obj_t    *ta_ = nullptr;

    bool chinese_    = true;  // stored preference, also the mode on screen
    bool storedMode_ = true;  // what the preference file holds, to write only on change
    // The candidate row holds three or four words at most, while the engine
    // offers up to MAX_CANDIDATES. The grid shows the rest of them on the same
    // widget: same geometry, same row count, so nothing moves or resizes when
    // it opens.
    bool grid_ = false;
    // Paged by how many words actually fitted, which depends on how long they
    // were - on the row and in the grid alike.
    int candFirst_ = 0;
    int candShown_ = 0;

    // The map handed to LVGL is a pointer that stays live, so the strings it
    // points at have to outlive the call: both vectors are rebuilt together and
    // the map is re-registered right after, never in between.
    std::vector<std::string>            cells_;
    std::vector<const char *>           map_;
    std::vector<lv_buttonmatrix_ctrl_t> ctrl_;
    std::vector<CellInfo>               info_;

    const lv_font_t      *latinFont_ = nullptr;
    std::function<void()> confirm_;
};

#endif // BOPOMOFO_IME_LVGL
