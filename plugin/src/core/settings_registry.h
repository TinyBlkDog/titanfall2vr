#pragma once

// ---------------------------------------------------------------------------
// THE SETTINGS REGISTRY. PLAN-VRMENU M2.
//
// IT IS ADDITIVE. It sits BESIDE ApplyValue's if-chain and does not replace,
// wrap or reorder one line of it. That if-chain is the arming path for the
// entire working stack, and every setting -- typed into the INI or dragged on a
// slider -- still goes through ApplyValue(name, float). A menu edit and an INI
// line therefore take LITERALLY the same path and cannot diverge, which is the
// property that makes the menu safe to add to a stack that took weeks to arm.
//
// WHERE THE TABLE LIVES, AND WHY IT IS NOT IN ITS OWN .cpp. The table is
// defined at the end of config.cpp, because most of these settings are stored
// in atomics in that file's anonymous namespace. Putting the getters there
// means each getter reads the EXACT atomic its setter writes -- one store per
// setting, and no possibility of the registry drifting out of step with the
// if-chain the way a parallel copy of the values would.
//
// A REGISTRY ENTRY IS NOT A PLACE TO PUT A CONTROL THAT HAS NO SETTING. The
// plan is explicit: a row with nothing behind it is a second store by another
// name. Anything without a real key and a real getter is OMITTED, and the
// omission is recorded, rather than stubbed.
// ---------------------------------------------------------------------------

enum class SettingWidget {
    Toggle,
    Slider,
    Combo,
    Numeric,
};

// Halo's convention, printed on the control itself for anything not live.
enum class SettingTier {
    Live,     // takes effect the moment it changes
    Restart,  // takes effect on the next launch
    Careful,  // changes something that can visibly break; never a bare drag
};

enum class SettingCategory {
    Home,
    View,
    HandsWeapon,
    Hud,
    Reticle,
    Body,
    Controls,
    Advanced,
    CategoryCount,
};

struct Setting {
    // THE INI KEY, and it is the setting's only identity. The label is for
    // people; this is what ApplyValue matches and what gets written to disk.
    const char* name;
    const char* label;
    // Written for the wearer, not for a developer.
    const char* tooltip;
    // Reads the SAME store the setter writes. Never a cached copy.
    float (*get)();
    // The slider's range spans the INI's accepted range, because the registry
    // carries both and they therefore cannot disagree -- Halo shipped a config
    // value that could not be reached from their menu.
    float minimum;
    float maximum;
    float defaultValue;
    SettingWidget widget;
    SettingTier tier;
    SettingCategory category;
    // Combo labels, nullptr-terminated; null for non-combos.
    const char* const* comboLabels;
};

int SettingCount();
const Setting* SettingAt(int index);
const Setting* FindSetting(const char* name);
const char* SettingCategoryName(SettingCategory category);
const char* SettingTierLabel(SettingTier tier);

// THE ONE WRITE PATH. Forwards to ApplyValue(name, value) and does nothing
// else. There is deliberately no second way to set a registered setting.
void SetSettingValue(const Setting& setting, float value);

// ---------------------------------------------------------------------------
// THE STARTUP SELF-CHECK. It is part of M2, not an optional extra.
//
// It feeds EVERY registry name through ApplyValue at that setting's CURRENT
// value -- a no-op for the value, but not for the question -- and counts how
// many of them fell through to "unknown setting". A registry naming a key the
// if-chain does not handle is a control that will silently do nothing, and this
// is the only thing that catches it before a wearer does.
//
// It also round-trips: after applying, the getter must read back what was
// applied. A getter pointing at a different store than its setter is the other
// way a control silently lies, and it looks identical from the headset.
//
// Returns the number of problems found; zero is the passing result.
unsigned RunSettingsRegistrySelfCheck();

// Writes one setting into the live ini, rewriting its existing line in place and
// leaving every comment untouched. Called for each committed edit, so a change
// made in the headset survives a restart.
bool PersistSettingToIni(const char* name, float value);

// THE POSITIVE CONTROL IS BUILT IN AND UNCONDITIONAL. The check always feeds one
// deliberately bogus name through the same path first, and fails loudly if that
// name is NOT reported. It is not an ini key: this check runs before the ini is
// parsed, so an ini-set control could never be active when it ran -- and a
// control you can forget to switch on is one you will forget to switch on. A
// sweep never seen to catch anything cannot be trusted when it reports clean.

// How many names ApplyValue has rejected as unknown, for the whole process.
// The self-check reads this either side of each apply; it is exposed because
// that counter is the instrument and a caller should be able to see it.
unsigned long long UnknownSettingCount();

// ---------------------------------------------------------------------------
// STAGED EDITS. The menu does not write through while it is open.
//
// The game is LIVE behind the panel -- there is no pause -- so a slider that
// applied on every frame of a drag would be changing the world underneath the
// wearer while they were still deciding. The wearer asked for edits to be held
// and applied on exit, and that is right for a second reason: several of these
// settings interact, and applying them one at a time mid-drag walks through
// combinations nobody chose.
//
// THIS IS NOT A SECOND STORE, and the distinction matters because the registry's
// whole safety argument is that there is exactly one. A staged value is an EDIT
// BUFFER: it is written only by the panel, read only by the panel, and its only
// exit is through SetSettingValue -> ApplyValue, the same call an INI line
// makes. Nothing else in the process can see it, and closing the panel either
// commits it or discards it. The moment it is committed it ceases to exist.
//
// Auto-commit on close for now. A confirmation step is the obvious next thing
// and CommitStagedSettings/DiscardStagedSettings are already separate so it can
// be added without moving anything.

// Returns the value the panel should DISPLAY: the staged edit if there is one,
// otherwise the live value from the setting's getter.
float StagedOrLiveValue(int index);
// Records an edit without applying it.
void StageSettingValue(int index, float value);
// How many settings are currently edited but not yet applied.
int StagedEditCount();
// Applies every staged edit through ApplyValue, in registry order, and clears
// the buffer. Returns the number applied.
int CommitStagedSettings();
// Throws the buffer away without applying anything.
void DiscardStagedSettings();

// ---------------------------------------------------------------------------
// A HARD RULE ON GETTERS, learned by crashing the game three times.
//
// A registry getter MUST be callable from the presenting thread with no XR lock
// held, and must not itself take a lock that the Present path may already own.
//
// xr.decouple's getter, IsXrDecoupled(), takes g_xrMutex. The panel was built
// inside Present's scoped_lock on that same mutex, so the getter re-locked a
// non-recursive std::mutex on the same thread -- undefined behaviour, and a hard
// crash on summon. The panel build has been moved outside the lock, which fixes
// it generally rather than for this one entry, but the rule still holds: prefer
// an atomic read. If a value genuinely needs a lock to read, it does not belong
// in this table until it has a lock-free accessor.
// ---------------------------------------------------------------------------
