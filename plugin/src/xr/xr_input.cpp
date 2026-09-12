#include "xr_input.h"
#include "diagnostics.h"

#include <cstdio>
#include <cstring>
#include <vector>

// Storage for the published state. Identity bases, so a consumer that reads
// before the first sync gets a no-op rotation rather than a zero matrix, which
// collapses geometry instead of leaving it alone.
extern "C" volatile float g_controllerAimBasis[kHandCount][9] = {
    {1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}};
extern "C" volatile float g_controllerAimPositionMetres[kHandCount][3] = {{0, 0, 0}, {0, 0, 0}};
extern "C" volatile float g_controllerGripBasis[kHandCount][9] = {
    {1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}};
extern "C" volatile float g_controllerGripPositionMetres[kHandCount][3] = {{0, 0, 0}, {0, 0, 0}};
extern "C" volatile std::uint32_t g_controllerGeneration[kHandCount] = {0, 0};
extern "C" volatile std::uint8_t g_controllerPoseFlags[kHandCount] = {0, 0};
extern "C" volatile std::uint8_t g_controllerThumbstickClick[kHandCount] = {0, 0};
extern "C" volatile std::uint8_t g_controllerButtons[kHandCount] = {0, 0};
extern "C" volatile float g_controllerThumbstick[kHandCount][2] = {{0, 0}, {0, 0}};
extern "C" volatile float g_controllerTrigger[kHandCount] = {0.0f, 0.0f};
// SQUEEZE (the grip button), as a value rather than a click: the Touch profile
// reports it analogue, and a digital latch wants its own threshold rather than
// the runtime's. Ordnance is the left grip, tactical the right.
extern "C" volatile float g_controllerSqueeze[kHandCount] = {0.0f, 0.0f};
// The MENU button. Only the LEFT controller has one on the Touch profile -- the
// right hand's equivalent is reserved by the system for the Oculus dash and is
// not deliverable to an application, so there is one flag and not two.
extern "C" volatile std::uint8_t g_controllerMenu = 0;
// The capacitive thumbrest, per hand. Part of the core Touch profile, so no
// extension is involved; hardware without the pad (the PFD MR controllers)
// leaves the action inactive, which reads 0 forever and simply means the
// d-pad's thumbrest modifier never fires there.
extern "C" volatile std::uint8_t g_controllerThumbrestTouch[kHandCount] = {0, 0};
extern "C" volatile std::uint64_t g_controllerSequence = 0;

namespace {

const char* const kHandName[kHandCount] = {"left", "right"};
const char* const kHandRootPath[kHandCount] = {"/user/hand/left", "/user/hand/right"};

bool g_enabled = true;

XrInstance g_instance = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_aimPoseAction = XR_NULL_HANDLE;
XrAction g_gripPoseAction = XR_NULL_HANDLE;
XrAction g_thumbstickClickAction = XR_NULL_HANDLE;
XrAction g_thumbstickAction = XR_NULL_HANDLE;
XrAction g_triggerAction = XR_NULL_HANDLE;
XrAction g_squeezeAction = XR_NULL_HANDLE;
XrAction g_hapticAction = XR_NULL_HANDLE;
XrAction g_menuAction = XR_NULL_HANDLE;
XrAction g_thumbrestAction = XR_NULL_HANDLE;
// Face buttons. On Touch the left hand has x/y and the right has a/b, so one
// action per POSITION rather than per name: "primary" is X or A, "secondary" is
// Y or B. Reload is the right hand's secondary.
XrAction g_primaryButtonAction = XR_NULL_HANDLE;
XrAction g_secondaryButtonAction = XR_NULL_HANDLE;
XrPath g_handSubaction[kHandCount] = {XR_NULL_PATH, XR_NULL_PATH};
XrSpace g_aimSpace[kHandCount] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrSpace g_gripSpace[kHandCount] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
bool g_actionsReady = false;
bool g_attached = false;

// Logging state. Everything here exists so that a NULL result explains itself:
// "no controller poses in the log" has at least four distinct causes and the log
// has to say which one, per the operating rule that a null result is worthless
// until the log confirms the instrument was working.
XrPath g_reportedProfile[kHandCount] = {XR_NULL_PATH, XR_NULL_PATH};
bool g_haveReportedProfile[kHandCount] = {false, false};
std::uint8_t g_reportedFlags[kHandCount] = {0xFF, 0xFF};   // 0xFF: nothing reported yet
XrResult g_reportedSyncResult = XR_SUCCESS;
bool g_haveReportedSyncResult = false;
bool g_reportedFirstPose = false;
int g_reportedHandednessSign = 0;
unsigned g_frame = 0;

// BUTTONS ARE REPORTED ON THE EDGE, NOT ON THE SAMPLE.
//
// The first version of this instrument printed the thumbstick, click and trigger
// only inside the once-a-second pose sample, and the L3+R3 chord check sat behind
// the same early return. A press held for a fraction of a second therefore had
// about a one-in-eighty-four chance of appearing at all, and the first run duly
// reported click=0 trigger=0 throughout a test that included pressing both. That
// is the "check the instrument itself" rule: the zeros were a property of the
// sampling, not of the hardware, and reading them as a result about the bindings
// would have sent the next run looking for a binding bug that was never there.
//
// So every state change is logged the frame it happens, and the ACTIVE flags are
// logged too -- because isActive=0 (the action is not bound to anything) and
// isActive=1 with nothing pressed are the two answers this has to separate, and
// they produce identical published values.
constexpr std::uint8_t kButtonClickActive = 1u;
constexpr std::uint8_t kButtonStickActive = 2u;
constexpr std::uint8_t kButtonTriggerActive = 4u;
std::uint8_t g_reportedButtonActive[kHandCount] = {0xFF, 0xFF};
std::uint8_t g_reportedClick[kHandCount] = {0xFF, 0xFF};
int g_reportedTriggerStep[kHandCount] = {-1, -1};
int g_reportedStickStep[kHandCount] = {-1, -1};
int g_reportedChord = -1;
// Hysteresis, so an analogue axis resting near its threshold cannot log a line a
// frame. Engage high, release low.
constexpr float kTriggerEngage = 0.6f;
constexpr float kTriggerRelease = 0.3f;
constexpr float kStickEngage = 0.4f;
constexpr float kStickRelease = 0.2f;

int SteppedWithHysteresis(float value, int previous, float engage, float release) {
    if (previous == 1) return value > release ? 1 : 0;
    return value > engage ? 1 : 0;
}
// One sample line per hand per second at 84 Hz, and one idle line every five.
// The submit breakdown already writes two lines a second; this is deliberately
// the same order of magnitude rather than a per-frame trace.
constexpr unsigned kSampleEveryFrames = 84;
constexpr unsigned kIdleEveryFrames = 84 * 5;

bool Ok(XrResult result, const char* action) {
    if (XR_SUCCEEDED(result)) return true;
    char line[200]{};
    std::snprintf(line, sizeof(line), "[TF2VR] controllers: %s failed: %d.\n", action, result);
    Tf2VrLog(line);
    return false;
}

XrPath ToPath(const char* text) {
    XrPath path = XR_NULL_PATH;
    if (!XR_SUCCEEDED(xrStringToPath(g_instance, text, &path))) {
        char line[200]{};
        std::snprintf(line, sizeof(line), "[TF2VR] controllers: could not resolve path '%s'.\n", text);
        Tf2VrLog(line);
        return XR_NULL_PATH;
    }
    return path;
}

// XR_NULL_PATH is the answer that matters here: it means the runtime has not
// matched our suggested bindings to any live device, which is the difference
// between "no controllers are switched on" and "our bindings were rejected".
const char* DescribePath(XrPath path, char* buffer, std::uint32_t size) {
    if (path == XR_NULL_PATH) return "NONE (XR_NULL_PATH)";
    std::uint32_t used = 0;
    if (!XR_SUCCEEDED(xrPathToString(g_instance, path, size, &used, buffer))) {
        std::snprintf(buffer, size, "<unreadable path>");
    }
    return buffer;
}

bool CreateAction(XrActionType type, const char* name, const char* localized, XrAction& out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
    std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localized);
    // One action per concept, two subaction paths. The alternative -- a separate
    // action per hand -- doubles the binding table and makes it possible for the
    // two hands to be bound inconsistently.
    info.countSubactionPaths = kHandCount;
    info.subactionPaths = g_handSubaction;
    return Ok(xrCreateAction(g_actionSet, &info, &out), name);
}

struct Suggestion {
    XrAction action;
    const char* pathFormat;   // one "%s", replaced with "left" / "right"
    // Face buttons are not named the same on both hands -- Touch gives the left
    // x/y and the right a/b -- so a single "%s" format cannot express them.
    // When these are set they are used verbatim instead.
    const char* leftPath = nullptr;
    const char* rightPath = nullptr;
};

// xrSuggestInteractionProfileBindings is ATOMIC: one unsupported path and the
// whole profile's bindings are rejected, silently leaving actions unbound. So
// each profile gets only paths it is known to define, and the result of each
// suggestion is logged separately.
bool SuggestProfile(const char* profile, const Suggestion* items, size_t count) {
    const XrPath profilePath = ToPath(profile);
    if (profilePath == XR_NULL_PATH) return false;
    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(count * kHandCount);
    for (size_t index = 0; index < count; ++index) {
        if (items[index].action == XR_NULL_HANDLE) continue;
        for (int hand = 0; hand < kHandCount; ++hand) {
            char path[128]{};
            const char* explicitPath = (hand == 0) ? items[index].leftPath : items[index].rightPath;
            if (explicitPath) {
                std::snprintf(path, sizeof(path), "%s", explicitPath);
            } else if (items[index].pathFormat) {
                std::snprintf(path, sizeof(path), items[index].pathFormat, kHandName[hand]);
            } else {
                continue;   // this action is not bound on this hand
            }
            const XrPath resolved = ToPath(path);
            if (resolved == XR_NULL_PATH) return false;
            bindings.push_back({items[index].action, resolved});
        }
    }
    if (bindings.empty()) return false;
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profilePath;
    suggested.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    const XrResult result = xrSuggestInteractionProfileBindings(g_instance, &suggested);
    char line[280]{};
    std::snprintf(line, sizeof(line),
        "[TF2VR] controllers: %u bindings suggested for %s -> %s.\n",
        suggested.countSuggestedBindings, profile,
        XR_SUCCEEDED(result) ? "accepted" : "REJECTED (those actions stay unbound on this device)");
    Tf2VrLog(line);
    return XR_SUCCEEDED(result);
}

struct PoseSample {
    bool active = false;      // the action is bound to a device the runtime has
    bool tracked = false;     // ...and that device's pose is currently valid
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
};

PoseSample ReadPose(XrAction action, XrSpace space, int hand, XrSpace baseSpace, XrTime displayTime) {
    PoseSample sample;
    if (action == XR_NULL_HANDLE || space == XR_NULL_HANDLE) return sample;
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = g_handSubaction[hand];
    XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
    if (!XR_SUCCEEDED(xrGetActionStatePose(g_session, &get, &state))) return sample;
    sample.active = state.isActive != XR_FALSE;
    if (!sample.active) return sample;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(space, baseSpace, displayTime, &location))) return sample;
    // Both flags, and both TRACKING flags rather than only VALID: a runtime
    // reports the last known pose as "valid" after tracking is lost, so valid
    // alone would publish a stale pose as a live one.
    constexpr XrSpaceLocationFlags wanted =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if ((location.locationFlags & wanted) != wanted) return sample;
    sample.tracked = true;
    sample.pose = location.pose;
    return sample;
}

void PublishPose(const XrPosef& pose, volatile float* basis9, volatile float* position3) {
    float basis[9]{};
    XrQuaternionToSourceBasis(pose.orientation, basis);
    for (int index = 0; index < 9; ++index) basis9[index] = basis[index];
    position3[0] = pose.position.x;
    position3[1] = pose.position.y;
    position3[2] = pose.position.z;
}

}  // namespace

void SetControllerInputEnabled(bool enabled) {
    g_enabled = enabled;
    Tf2VrLog(enabled
        ? "[TF2VR] controller input enabled; poses will be read and logged, and nothing else.\n"
        : "[TF2VR] controller input DISABLED; no action set is created and no pose is read.\n");
}

bool IsControllerInputEnabled() { return g_enabled; }

bool XrInputCreateActions(XrInstance instance) {
    if (!g_enabled) return false;
    if (instance == XR_NULL_HANDLE) return false;
    // Actions and action sets belong to the INSTANCE, so they survive a session
    // being ended and begun again. If the instance itself changed, the old
    // handles died with it and the bookkeeping has to start clean -- destroying
    // them would be a use-after-free.
    if (g_actionsReady && g_instance == instance) return true;
    if (g_instance != instance) {
        g_actionSet = XR_NULL_HANDLE;
        g_aimPoseAction = g_gripPoseAction = XR_NULL_HANDLE;
        g_thumbstickClickAction = g_thumbstickAction = g_triggerAction = XR_NULL_HANDLE;
        for (int hand = 0; hand < kHandCount; ++hand) {
            g_aimSpace[hand] = g_gripSpace[hand] = XR_NULL_HANDLE;
            g_haveReportedProfile[hand] = false;
            g_reportedFlags[hand] = 0xFF;
        }
        g_actionsReady = false;
        g_attached = false;
        g_reportedFirstPose = false;
        g_haveReportedSyncResult = false;
        g_reportedHandednessSign = 0;
    }
    g_instance = instance;

    for (int hand = 0; hand < kHandCount; ++hand) {
        g_handSubaction[hand] = ToPath(kHandRootPath[hand]);
        if (g_handSubaction[hand] == XR_NULL_PATH) return false;
    }

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
    std::snprintf(setInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Gameplay");
    setInfo.priority = 0;
    if (!Ok(xrCreateActionSet(instance, &setInfo, &g_actionSet), "create action set")) return false;

    // AIM and GRIP are both read because they answer different questions and
    // Task 05 needs both. Aim is the runtime's own "where is this device
    // pointing" ray, which is what a weapon and Task 04's menu ray want. Grip is
    // where the hand physically is, which is what a held object's position wants.
    // Prior art (BioShock, Halo) uses them for exactly that split.
    const bool created =
        CreateAction(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim pose", g_aimPoseAction) &&
        CreateAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip pose", g_gripPoseAction) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick click", g_thumbstickClickAction) &&
        CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", g_thumbstickAction) &&
        CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", g_triggerAction) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "primary_button", "Primary button (X/A)",
                     g_primaryButtonAction) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary_button", "Secondary button (Y/B)",
                     g_secondaryButtonAction) &&
        CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip squeeze", g_squeezeAction) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu button", g_menuAction) &&
        CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbrest", "Thumbrest touch",
                     g_thumbrestAction) &&
        // OUTPUT, not input. The game already rumbles -- xinput_pad intercepts
        // XInputSetState -- and that motor command has had nowhere to go.
        CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic feedback",
                     g_hapticAction);
    if (!created) return false;

    // Meta Quest 3 through VirtualDesktopXR reports the Touch profile, so that
    // is the one that matters; the simple controller is the standard fallback and
    // defines only the two poses, so it is suggested with only those.
    const Suggestion touch[] = {
        {g_aimPoseAction, "/user/hand/%s/input/aim/pose"},
        {g_gripPoseAction, "/user/hand/%s/input/grip/pose"},
        {g_thumbstickClickAction, "/user/hand/%s/input/thumbstick/click"},
        {g_thumbstickAction, "/user/hand/%s/input/thumbstick"},
        {g_triggerAction, "/user/hand/%s/input/trigger/value"},
        {g_primaryButtonAction, nullptr, "/user/hand/left/input/x/click",
         "/user/hand/right/input/a/click"},
        {g_secondaryButtonAction, nullptr, "/user/hand/left/input/y/click",
         "/user/hand/right/input/b/click"},
        {g_squeezeAction, "/user/hand/%s/input/squeeze/value"},
        // LEFT ONLY, and deliberately. On the Touch profile the right
        // controller's system button belongs to the Oculus dash and is never
        // delivered to an application; suggesting a binding for it makes the
        // whole profile suggestion fail, taking every other binding with it.
        {g_menuAction, nullptr, "/user/hand/left/input/menu/click", nullptr},
        {g_hapticAction, "/user/hand/%s/output/haptic"},
        // THE CAPACITIVE THUMBREST. Its absence from this list was the whole of
        // "holding the touch area does nothing" on 2026-08-29: the action was
        // created and read every frame, and no binding path was ever suggested
        // for it, so the runtime had nothing to make it active and it read
        // false forever. The count on the suggestion line is the falsifier --
        // 19 bindings is this list WITHOUT this row, 21 is with it.
        {g_thumbrestAction, "/user/hand/%s/input/thumbrest/touch"},
    };
    // THE SAME LIST WITHOUT THE THUMBREST, and it exists because suggestion is
    // ATOMIC. `/input/thumbrest/touch` is in the core Touch profile, so a
    // spec-compliant runtime takes it -- but if one does not, the rejection
    // takes every other binding down with it and the wearer loses ALL
    // controller input. A rejected call changes nothing and the last accepted
    // call for a profile wins, so trying the richer list first and falling back
    // costs nothing and cannot strand the session. The d-pad's other modifier
    // needs no binding at all, so the fallback still leaves it reachable.
    const Suggestion touchNoThumbrest[] = {
        {g_aimPoseAction, "/user/hand/%s/input/aim/pose"},
        {g_gripPoseAction, "/user/hand/%s/input/grip/pose"},
        {g_thumbstickClickAction, "/user/hand/%s/input/thumbstick/click"},
        {g_thumbstickAction, "/user/hand/%s/input/thumbstick"},
        {g_triggerAction, "/user/hand/%s/input/trigger/value"},
        {g_primaryButtonAction, nullptr, "/user/hand/left/input/x/click",
         "/user/hand/right/input/a/click"},
        {g_secondaryButtonAction, nullptr, "/user/hand/left/input/y/click",
         "/user/hand/right/input/b/click"},
        {g_squeezeAction, "/user/hand/%s/input/squeeze/value"},
        {g_menuAction, nullptr, "/user/hand/left/input/menu/click", nullptr},
        {g_hapticAction, "/user/hand/%s/output/haptic"},
    };
    const Suggestion simple[] = {
        {g_aimPoseAction, "/user/hand/%s/input/aim/pose"},
        {g_gripPoseAction, "/user/hand/%s/input/grip/pose"},
    };
    bool touchOk = SuggestProfile("/interaction_profiles/oculus/touch_controller",
                                  touch, sizeof(touch) / sizeof(touch[0]));
    if (!touchOk) {
        Tf2VrLog("[TF2VR] controllers: the Touch suggestion INCLUDING the thumbrest was rejected; "
                 "retrying without it. Losing the thumbrest costs only the d-pad's capacitive "
                 "modifier -- the held-beside-the-head modifier needs no binding and still "
                 "works.\n");
        touchOk = SuggestProfile("/interaction_profiles/oculus/touch_controller", touchNoThumbrest,
                                 sizeof(touchNoThumbrest) / sizeof(touchNoThumbrest[0]));
    }
    const bool simpleOk = SuggestProfile("/interaction_profiles/khr/simple_controller",
                                         simple, sizeof(simple) / sizeof(simple[0]));
    if (!touchOk && !simpleOk) {
        Tf2VrLog("[TF2VR] controllers: EVERY interaction profile suggestion was rejected. The action "
                 "set exists but nothing will ever be bound to it, so expect isActive=0 on every "
                 "pose. This is a binding-path problem, not a tracking one.\n");
    }
    g_actionsReady = true;
    Tf2VrLog("[TF2VR] controllers: action set 'gameplay' created -- aim pose, grip pose, thumbstick "
             "click, thumbstick axes and trigger, for both hands. Task 05 step 1 READS AND LOGS "
             "ONLY: nothing here writes to the engine.\n");
    return true;
}

bool XrInputAttachToSession(XrSession session) {
    if (!g_enabled || !g_actionsReady || session == XR_NULL_HANDLE) return false;
    g_session = session;
    // Action spaces are SESSION-scoped, so they are made here rather than with
    // the actions, and remade whenever the session is.
    for (int hand = 0; hand < kHandCount; ++hand) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.subactionPath = g_handSubaction[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        spaceInfo.action = g_aimPoseAction;
        if (!Ok(xrCreateActionSpace(session, &spaceInfo, &g_aimSpace[hand]), "create aim action space")) return false;
        spaceInfo.action = g_gripPoseAction;
        if (!Ok(xrCreateActionSpace(session, &spaceInfo, &g_gripSpace[hand]), "create grip action space")) return false;
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actionSet;
    if (!Ok(xrAttachSessionActionSets(session, &attach), "attach action sets")) return false;
    g_attached = true;
    Tf2VrLog("[TF2VR] controllers: action sets attached to the session.\n");
    return true;
}

void XrInputSync(XrSession session, XrSpace baseSpace, XrSpace viewSpace, XrTime displayTime) {
    if (!g_enabled || !g_attached || session == XR_NULL_HANDLE) return;
    g_session = session;
    ++g_frame;

    XrActiveActionSet active{g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult syncResult = xrSyncActions(session, &sync);
    // XR_SESSION_NOT_FOCUSED is a SUCCESS code, not an error, and it means every
    // action state comes back inactive. It is the single most likely explanation
    // for "the controllers do nothing", so it is reported by name on change
    // rather than being swallowed as success.
    if (!g_haveReportedSyncResult || syncResult != g_reportedSyncResult) {
        g_haveReportedSyncResult = true;
        g_reportedSyncResult = syncResult;
        char line[320]{};
        if (syncResult == XR_SESSION_NOT_FOCUSED) {
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: xrSyncActions -> XR_SESSION_NOT_FOCUSED. The runtime is not "
                "giving this session input focus, so every action reads inactive. Nothing is wrong "
                "with the bindings.\n");
        } else if (XR_SUCCEEDED(syncResult)) {
            std::snprintf(line, sizeof(line), "[TF2VR] controllers: xrSyncActions -> success (%d); "
                                              "action states are live.\n", syncResult);
        } else {
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: xrSyncActions FAILED with %d; no action state is readable.\n",
                syncResult);
        }
        Tf2VrLog(line);
    }
    if (!XR_SUCCEEDED(syncResult)) return;

    // Which physical device the runtime actually bound our actions to. Polled
    // rather than event-driven so it cannot be missed, but logged only on change.
    if (g_frame % 30 == 0) {
        for (int hand = 0; hand < kHandCount; ++hand) {
            XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
            if (!XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, g_handSubaction[hand], &profile))) continue;
            if (g_haveReportedProfile[hand] && profile.interactionProfile == g_reportedProfile[hand]) continue;
            g_haveReportedProfile[hand] = true;
            g_reportedProfile[hand] = profile.interactionProfile;
            char pathText[XR_MAX_PATH_LENGTH]{};
            char line[400]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: the %s hand is bound to interaction profile %s.%s\n",
                kHandName[hand], DescribePath(profile.interactionProfile, pathText, sizeof(pathText)),
                profile.interactionProfile == XR_NULL_PATH
                    ? " NONE means the runtime sees no device for that hand -- switch the controller"
                      " on and wake it, and this line will change."
                    : "");
            Tf2VrLog(line);
        }
    }

    PoseSample aim[kHandCount];
    PoseSample grip[kHandCount];
    float aimBasis[kHandCount][9]{};
    float aimYaw[kHandCount]{}, aimPitch[kHandCount]{};
    std::uint8_t buttonBound[kHandCount]{};
    for (int hand = 0; hand < kHandCount; ++hand) {
        aim[hand] = ReadPose(g_aimPoseAction, g_aimSpace[hand], hand, baseSpace, displayTime);
        grip[hand] = ReadPose(g_gripPoseAction, g_gripSpace[hand], hand, baseSpace, displayTime);

        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = g_handSubaction[hand];
        get.action = g_thumbstickClickAction;
        XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool haveClick = XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &click)) &&
                               click.isActive != XR_FALSE;
        get.action = g_thumbstickAction;
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        const bool haveStick = XR_SUCCEEDED(xrGetActionStateVector2f(session, &get, &stick)) &&
                               stick.isActive != XR_FALSE;
        get.action = g_triggerAction;
        XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
        const bool haveTrigger = XR_SUCCEEDED(xrGetActionStateFloat(session, &get, &trigger)) &&
                                 trigger.isActive != XR_FALSE;
        // Face buttons, by position. The right hand's secondary is reload.
        get.action = g_primaryButtonAction;
        XrActionStateBoolean primary{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool havePrimary = XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &primary)) &&
                                 primary.isActive != XR_FALSE;
        get.action = g_secondaryButtonAction;
        XrActionStateBoolean secondary{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool haveSecondary = XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &secondary)) &&
                                   secondary.isActive != XR_FALSE;
        // The grip squeeze, analogue. Ordnance on the left, tactical on the
        // right; the digital threshold lives with the consumer, not here.
        get.action = g_squeezeAction;
        XrActionStateFloat squeeze{XR_TYPE_ACTION_STATE_FLOAT};
        const bool haveSqueeze = XR_SUCCEEDED(xrGetActionStateFloat(session, &get, &squeeze)) &&
                                 squeeze.isActive != XR_FALSE;
        // Menu, left hand only -- see the binding note. Read inside the left
        // iteration so the right hand cannot clear a press the left just made.
        if (hand == 0) {
            get.action = g_menuAction;
            XrActionStateBoolean menu{XR_TYPE_ACTION_STATE_BOOLEAN};
            const bool haveMenu = XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &menu)) &&
                                  menu.isActive != XR_FALSE;
            g_controllerMenu = static_cast<std::uint8_t>(
                haveMenu && menu.currentState != XR_FALSE ? 1 : 0);
        }

        // The capacitive thumbrest. Read per hand, and an INACTIVE action
        // publishes 0 rather than being skipped: hardware without the pad must
        // read "not touched" forever, never a stale 1 from the last runtime
        // that did have one.
        get.action = g_thumbrestAction;
        XrActionStateBoolean thumbrest{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool haveThumbrest =
            XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &thumbrest)) &&
            thumbrest.isActive != XR_FALSE;
        g_controllerThumbrestTouch[hand] = static_cast<std::uint8_t>(
            haveThumbrest && thumbrest.currentState != XR_FALSE ? 1 : 0);
        // LIVENESS, NOT REGISTRATION. "21 bindings suggested" only says we
        // ASKED; isActive says the runtime actually bound the path to a device
        // it has. Those two were confused once already -- the action was
        // created, read every frame, and bound to nothing -- so each hand says
        // once which of the two it is, and a still-inactive action says so
        // after the controllers are demonstrably live rather than staying
        // silent and looking like an untouched pad.
        static bool activeLogged[kHandCount] = {false, false};
        static bool inactiveLogged[kHandCount] = {false, false};
        if (haveThumbrest && !activeLogged[hand]) {
            activeLogged[hand] = true;
            char line[240]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: the %s thumbrest is BOUND and active -- the d-pad's "
                "capacitive modifier is available on this hardware.\n", kHandName[hand]);
            Tf2VrLog(line);
        } else if (!haveThumbrest && !inactiveLogged[hand] && aim[hand].active) {
            inactiveLogged[hand] = true;
            char line[300]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: the %s thumbrest is INACTIVE while that hand's pose IS "
                "active, so this device has no thumbrest the runtime will report. The d-pad's "
                "held-beside-the-head modifier is the one that works here.\n", kHandName[hand]);
            Tf2VrLog(line);
        }

        const std::uint8_t flags =
            static_cast<std::uint8_t>((aim[hand].tracked ? kControllerAimTracked : 0u) |
                                      (grip[hand].tracked ? kControllerGripTracked : 0u));

        // One generation window per hand covering everything about that hand, so
        // a reader can never pair this frame's aim with last frame's grip.
        g_controllerGeneration[hand] = g_controllerGeneration[hand] + 1;   // odd: write in progress
        if (aim[hand].tracked) {
            PublishPose(aim[hand].pose, &g_controllerAimBasis[hand][0], &g_controllerAimPositionMetres[hand][0]);
        }
        if (grip[hand].tracked) {
            PublishPose(grip[hand].pose, &g_controllerGripBasis[hand][0], &g_controllerGripPositionMetres[hand][0]);
        }
        g_controllerPoseFlags[hand] = flags;
        g_controllerThumbstickClick[hand] = static_cast<std::uint8_t>(haveClick && click.currentState != XR_FALSE ? 1 : 0);
        g_controllerThumbstick[hand][0] = haveStick ? stick.currentState.x : 0.0f;
        g_controllerThumbstick[hand][1] = haveStick ? stick.currentState.y : 0.0f;
        g_controllerTrigger[hand] = haveTrigger ? trigger.currentState : 0.0f;
        g_controllerSqueeze[hand] = haveSqueeze ? squeeze.currentState : 0.0f;
        // A PRESS THAT STARTS AND ENDS BETWEEN TWO SYNCS MUST NOT VANISH.
        //
        // This read `currentState` only -- a LEVEL. If the button goes down and
        // back up between two xrSyncActions calls, currentState is false at both
        // samples and the press never happened as far as the whole stack is
        // concerned. OpenXR reports `changedSinceLastSync` for exactly this, and
        // it was being ignored.
        //
        // MEASURED, 2026-08-22: the wearer pressed A three times -- menu, then
        // one screen each -- and every press visibly did something, while the
        // ungated RAWBTN watch recorded only TWO press/release pairs. One press
        // reached the game and never reached us. That is this bug, and it is the
        // shape of the whole "I have to press more than once" complaint: a
        // dropped press is indistinguishable from a screen that ignored one.
        //
        // The gap widens exactly when it hurts most -- the loading screen runs
        // at half frame rate (40 fps against 80 measured), so syncs are twice as
        // far apart there as in play.
        //
        // A transition seen with the button now UP means a complete press was
        // missed; it is latched and delivered for one frame so a consumer
        // sampling on its own clock still sees the edge.
        auto latch = [](bool have, const XrActionStateBoolean& state, bool& pending) {
            if (!have) return false;
            const bool down = state.currentState != XR_FALSE;
            if (down) { pending = false; return true; }
            if (state.changedSinceLastSync != XR_FALSE) { pending = true; }
            if (pending) { pending = false; return true; }   // deliver the missed press once
            return false;
        };
        static bool pendingPrimary[kHandCount]{};
        static bool pendingSecondary[kHandCount]{};
        const bool primaryDown = latch(havePrimary, primary, pendingPrimary[hand]);
        const bool secondaryDown = latch(haveSecondary, secondary, pendingSecondary[hand]);
        g_controllerButtons[hand] = static_cast<std::uint8_t>(
            (primaryDown ? kControllerPrimaryButton : 0u) |
            (secondaryDown ? kControllerSecondaryButton : 0u));
        g_controllerGeneration[hand] = g_controllerGeneration[hand] + 1;   // even: complete

        XrQuaternionToSourceBasis(aim[hand].pose.orientation, aimBasis[hand]);
        SourceForwardToYawPitchDegrees(aimBasis[hand], aimYaw[hand], aimPitch[hand]);

        // Which of the three non-pose inputs the runtime actually bound. Logged
        // on change, once, and it is the line that settles whether a silent
        // button is unbound or merely unpressed.
        const std::uint8_t buttonActive =
            static_cast<std::uint8_t>((haveClick ? kButtonClickActive : 0u) |
                                      (haveStick ? kButtonStickActive : 0u) |
                                      (haveTrigger ? kButtonTriggerActive : 0u));
        buttonBound[hand] = buttonActive;
        if (buttonActive != g_reportedButtonActive[hand]) {
            g_reportedButtonActive[hand] = buttonActive;
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: %s hand non-pose inputs -> thumbstick click %s, thumbstick axes "
                "%s, trigger %s.%s\n", kHandName[hand],
                haveClick ? "BOUND" : "not bound", haveStick ? "BOUND" : "not bound",
                haveTrigger ? "BOUND" : "not bound",
                buttonActive == 0u
                    ? " All three unbound: the published values will stay at zero no matter what is"
                      " pressed, so do not read zeros as 'not pressed'."
                    : "");
            Tf2VrLog(line);
        }

        // Edge-triggered, the frame it happens. A press is transient and the
        // once-a-second sample cannot see one.
        const std::uint8_t clickNow = g_controllerThumbstickClick[hand];
        if (clickNow != g_reportedClick[hand]) {
            const bool first = g_reportedClick[hand] == 0xFF;
            g_reportedClick[hand] = clickNow;
            if (!first) {
                char line[240]{};
                std::snprintf(line, sizeof(line), "[TF2VR] controllers: %s thumbstick click %s.\n",
                              kHandName[hand], clickNow ? "DOWN" : "up");
                Tf2VrLog(line);
            }
        }
        const float triggerValue = g_controllerTrigger[hand];
        const int triggerStep = SteppedWithHysteresis(triggerValue, g_reportedTriggerStep[hand],
                                                      kTriggerEngage, kTriggerRelease);
        if (triggerStep != g_reportedTriggerStep[hand]) {
            const bool first = g_reportedTriggerStep[hand] < 0;
            g_reportedTriggerStep[hand] = triggerStep;
            if (!first || triggerStep == 1) {
                char line[240]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] controllers: %s trigger %s (%.2f).\n", kHandName[hand],
                    triggerStep ? "PULLED" : "released", static_cast<double>(triggerValue));
                Tf2VrLog(line);
            }
        }
        const float stickX = g_controllerThumbstick[hand][0];
        const float stickY = g_controllerThumbstick[hand][1];
        const float stickMagnitude = std::sqrt(stickX * stickX + stickY * stickY);
        const int stickStep = SteppedWithHysteresis(stickMagnitude, g_reportedStickStep[hand],
                                                   kStickEngage, kStickRelease);
        if (stickStep != g_reportedStickStep[hand]) {
            const bool first = g_reportedStickStep[hand] < 0;
            g_reportedStickStep[hand] = stickStep;
            if (!first || stickStep == 1) {
                char line[280]{};
                std::snprintf(line, sizeof(line),
                    "[TF2VR] controllers: %s thumbstick %s (%+.2f,%+.2f).\n", kHandName[hand],
                    stickStep ? "DEFLECTED" : "centred",
                    static_cast<double>(stickX), static_cast<double>(stickY));
                Tf2VrLog(line);
            }
        }

        // Every transition of active/tracked, immediately. A controller that is
        // put down and sleeps looks exactly like one that was never bound, and
        // the difference is which of these two lines was logged last.
        if (flags != g_reportedFlags[hand]) {
            g_reportedFlags[hand] = flags;
            char line[400]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: %s hand -> aim %s, grip %s.%s\n", kHandName[hand],
                aim[hand].tracked ? "TRACKED" : (aim[hand].active ? "bound but not tracked" : "not bound"),
                grip[hand].tracked ? "TRACKED" : (grip[hand].active ? "bound but not tracked" : "not bound"),
                (!aim[hand].active && !grip[hand].active)
                    ? " Not bound means the runtime has no device on that hand at all."
                    : "");
            Tf2VrLog(line);
        }
    }
    ++g_controllerSequence;

    // The chord Task 04 summons its menu with, confirmed reachable here so that
    // task does not have to discover it for itself. Edge-triggered, and OUTSIDE
    // the once-a-second sample below: a two-thumb press lasts a few hundred
    // milliseconds and the sample cannot catch it.
    const int chord = (g_controllerThumbstickClick[kHandLeft] &&
                       g_controllerThumbstickClick[kHandRight]) ? 1 : 0;
    if (chord != g_reportedChord) {
        const bool first = g_reportedChord < 0;
        g_reportedChord = chord;
        if (chord == 1) {
            Tf2VrLog("[TF2VR] controllers: BOTH thumbsticks clicked -- this is the L3+R3 chord Task 04's "
                     "in-VR menu is specified to open on, and it is readable.\n");
        } else if (!first) {
            Tf2VrLog("[TF2VR] controllers: L3+R3 chord released.\n");
        }
    }

    // The head, for the relative measurements below only. Located here rather
    // than taken from the head-tracking path on purpose: this step has to be
    // testable with head tracking disarmed.
    float headBasis[9]{};
    float headYaw = 0.0f, headPitch = 0.0f;
    bool haveHead = false;
    XrPosef headPose{{0, 0, 0, 1}, {0, 0, 0}};
    {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (viewSpace != XR_NULL_HANDLE &&
            XR_SUCCEEDED(xrLocateSpace(viewSpace, baseSpace, displayTime, &location)) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            headPose = location.pose;
            XrQuaternionToSourceBasis(headPose.orientation, headBasis);
            SourceForwardToYawPitchDegrees(headBasis, headYaw, headPitch);
            haveHead = true;
        }
    }

    // HANDEDNESS, MEASURED RATHER THAN ASSUMED.
    //
    // The deliverable for this step includes "handedness identified", and the
    // usual way to do that -- put a controller in one hand and see which line
    // moves -- needs a person in a headset and produces a claim nobody can check
    // afterwards. This is the same fact as a number: project the left-to-right
    // hand separation onto the HEAD'S OWN right axis. Positive means the
    // runtime's /user/hand/right is genuinely on the wearer's right, whatever
    // direction they happen to be facing.
    if (haveHead && aim[kHandLeft].tracked && aim[kHandRight].tracked) {
        float headRightXr[3]{};
        XrRotateVector(headPose.orientation, 1.0f, 0.0f, 0.0f, headRightXr);
        const float separation[3] = {
            aim[kHandRight].pose.position.x - aim[kHandLeft].pose.position.x,
            aim[kHandRight].pose.position.y - aim[kHandLeft].pose.position.y,
            aim[kHandRight].pose.position.z - aim[kHandLeft].pose.position.z};
        const float along = separation[0] * headRightXr[0] + separation[1] * headRightXr[1] +
                            separation[2] * headRightXr[2];
        const int sign = along > 0.02f ? 1 : (along < -0.02f ? -1 : 0);
        if (sign != 0 && sign != g_reportedHandednessSign) {
            g_reportedHandednessSign = sign;
            char line[420]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers HANDEDNESS: /user/hand/right sits %+.1f cm along the head's own "
                "right axis from /user/hand/left, so it %s the wearer's right hand. Measured, not "
                "assumed -- it holds whichever way the player is facing.\n",
                along * 100.0f, sign > 0 ? "IS" : "is on the WRONG SIDE and the two are SWAPPED");
            Tf2VrLog(line);
        }
    }

    const bool anyTracked = aim[kHandLeft].tracked || aim[kHandRight].tracked ||
                            grip[kHandLeft].tracked || grip[kHandRight].tracked;

    if (anyTracked && !g_reportedFirstPose) {
        g_reportedFirstPose = true;
        Tf2VrLog("[TF2VR] controllers: FIRST tracked pose received. Sample lines follow about once a "
                 "second; the numbers below are the deliverable for Task 05 step 1.\n");
    }

    if (!anyTracked) {
        // A null result that explains itself. Every cause is named, with the
        // state that distinguishes them already in the line.
        if (g_frame % kIdleEveryFrames == 0) {
            char line[520]{};
            std::snprintf(line, sizeof(line),
                "[TF2VR] controllers: nothing tracked. left aim active=%d grip active=%d, right aim "
                "active=%d grip active=%d, sync=%d. active=0 means no device is bound (controller "
                "off/asleep, or the profile line above says NONE); active=1 with nothing tracked "
                "means the device is bound but its pose is not being tracked (out of view of the "
                "headset cameras, or the runtime is not focused).\n",
                aim[kHandLeft].active, grip[kHandLeft].active, aim[kHandRight].active,
                grip[kHandRight].active, syncResult);
            Tf2VrLog(line);
        }
        return;
    }

    if (g_frame % kSampleEveryFrames != 0) return;
    for (int hand = 0; hand < kHandCount; ++hand) {
        if (!aim[hand].tracked && !grip[hand].tracked) continue;
        char relative[120]{};
        if (haveHead) {
            // The number Step 3 turns into either its own aim angles or a stick
            // deflection: how far off the head's own direction the hand points.
            std::snprintf(relative, sizeof(relative), " | relative to head: yaw=%+.1f pitch=%+.1f",
                          WrapDegrees(aimYaw[hand] - headYaw), aimPitch[hand] - headPitch);
        }
        char line[560]{};
        std::snprintf(line, sizeof(line),
            "[TF2VR] controller %-5s aim%s pos=(%+.3f,%+.3f,%+.3f) m yaw=%+.1f pitch=%+.1f deg%s | "
            "grip%s pos=(%+.3f,%+.3f,%+.3f) m | stick=(%+.2f,%+.2f) click=%u trigger=%.2f "
            "squeeze=%.2f (%s)\n",
            kHandName[hand],
            aim[hand].tracked ? "" : " (untracked)",
            static_cast<double>(aim[hand].pose.position.x), static_cast<double>(aim[hand].pose.position.y),
            static_cast<double>(aim[hand].pose.position.z), aimYaw[hand], aimPitch[hand], relative,
            grip[hand].tracked ? "" : " (untracked)",
            static_cast<double>(grip[hand].pose.position.x), static_cast<double>(grip[hand].pose.position.y),
            static_cast<double>(grip[hand].pose.position.z),
            static_cast<double>(g_controllerThumbstick[hand][0]),
            static_cast<double>(g_controllerThumbstick[hand][1]),
            static_cast<unsigned>(g_controllerThumbstickClick[hand]),
            static_cast<double>(g_controllerTrigger[hand]),
            // SQUEEZE, PER HAND. The wearer reports that the LEFT grip fires
            // BOTH ordnance and tactical, and nothing in the log could say
            // whether the two hands read different values -- the mapping is
            // per-hand and correct, the subaction paths are declared, so the
            // question is entirely about what the runtime reports. If both
            // hands read the same number here, the fault is upstream of us.
            static_cast<double>(g_controllerSqueeze[hand]),
            // So that a single sample line read on its own cannot be mistaken
            // for evidence about the buttons. These three are sampled once a
            // second and a press is transient; the edge lines above are the
            // record of what was actually pressed.
            buttonBound[hand] == (kButtonClickActive | kButtonStickActive | kButtonTriggerActive)
                ? "all bound; presses are logged on the edge, not here"
                : "SOME NOT BOUND -- see the non-pose inputs line");
        Tf2VrLog(line);
    }
}

void XrInputShutdown() {
    for (int hand = 0; hand < kHandCount; ++hand) {
        if (g_aimSpace[hand] != XR_NULL_HANDLE) { xrDestroySpace(g_aimSpace[hand]); g_aimSpace[hand] = XR_NULL_HANDLE; }
        if (g_gripSpace[hand] != XR_NULL_HANDLE) { xrDestroySpace(g_gripSpace[hand]); g_gripSpace[hand] = XR_NULL_HANDLE; }
        g_haveReportedProfile[hand] = false;
        g_reportedProfile[hand] = XR_NULL_PATH;
        g_reportedFlags[hand] = 0xFF;
        g_controllerPoseFlags[hand] = 0;
        g_reportedButtonActive[hand] = 0xFF;
        g_reportedClick[hand] = 0xFF;
        g_reportedTriggerStep[hand] = -1;
        g_reportedStickStep[hand] = -1;
    }
    g_reportedChord = -1;
    // Destroying the action set destroys its actions with it.
    if (g_actionSet != XR_NULL_HANDLE) { xrDestroyActionSet(g_actionSet); g_actionSet = XR_NULL_HANDLE; }
    g_aimPoseAction = g_gripPoseAction = XR_NULL_HANDLE;
    g_thumbstickClickAction = g_thumbstickAction = g_triggerAction = XR_NULL_HANDLE;
    g_squeezeAction = g_menuAction = g_thumbrestAction = XR_NULL_HANDLE;
    // Published state goes with the handles: a stale 1 here would leave the
    // d-pad modifier stuck on across a session restart.
    for (int hand = 0; hand < kHandCount; ++hand) g_controllerThumbrestTouch[hand] = 0;
    g_instance = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_actionsReady = false;
    g_attached = false;
    g_reportedFirstPose = false;
    g_haveReportedSyncResult = false;
    g_reportedHandednessSign = 0;
    g_frame = 0;
}

// ---------------------------------------------------------------------------
// HAPTICS. The game's own rumble, sent to the controllers.
//
// The game has been calling XInputSetState all along -- xinput_pad intercepts it
// for the synthetic pad -- and that motor command had nowhere to go. This is the
// other end of it.
//
// Amplitude is 0..1. XR_MIN_HAPTIC_DURATION asks the runtime for its shortest
// pulse, which is what a per-frame rumble level wants: the game re-sends its
// motor state continuously, so each call should be a short pulse that the next
// call replaces, not a long buzz that queues up behind itself.
// ---------------------------------------------------------------------------
void TriggerHaptic(int hand, float amplitude, float frequency, XrDuration duration) {
    if (g_session == XR_NULL_HANDLE || g_hapticAction == XR_NULL_HANDLE) return;
    if (hand < 0 || hand >= kHandCount) return;
    if (amplitude <= 0.0f) {
        // Stop rather than let a previous pulse run on. A game that drops its
        // rumble to zero means stop now.
        XrHapticActionInfo stop{XR_TYPE_HAPTIC_ACTION_INFO};
        stop.action = g_hapticAction;
        stop.subactionPath = g_handSubaction[hand];
        xrStopHapticFeedback(g_session, &stop);
        return;
    }
    if (amplitude > 1.0f) amplitude = 1.0f;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    // An explicit duration, not XR_MIN_HAPTIC_DURATION. The minimum is a single
    // tick of the actuator, which is inaudible for a sustained rumble -- the
    // caller re-applies on an interval and asks for slightly longer than that
    // interval so the pulses overlap.
    vibration.duration = duration > 0 ? duration : XR_MIN_HAPTIC_DURATION;
    // Carries the balance between the pad's two motors: low when the heavy one
    // dominates, high when the light one does. The first version threw that away
    // by leaving it unspecified, which is the whole difference between an
    // explosion and gunfire feeling different.
    vibration.frequency = frequency;

    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = g_hapticAction;
    info.subactionPath = g_handSubaction[hand];
    xrApplyHapticFeedback(g_session, &info,
                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}
