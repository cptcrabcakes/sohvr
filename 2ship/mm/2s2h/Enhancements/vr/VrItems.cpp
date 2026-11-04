#include "VrItems.h"
#include "VrLauncher.h"
#include "VrGame.h"
#include "VrRuntime.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "global.h"
#include "2s2h/Enhancements/vr/Vr.h"
extern PlayState* gPlayState;
}

namespace SohVr {

namespace {

// The ring stands upright in front of the face, a little below eye level, facing the player (meters from the head).
constexpr float kMenuForward = 0.45f;
constexpr float kMenuDrop = 0.15f;
constexpr float kRingRadius = 0.17f;
constexpr float kTouchReach = 0.1f;    // a hand this close to an item touches it
constexpr float kPointAngle = 10.0f;   // a controller aimed within this many degrees of an item points at it
constexpr float kBackReach = 0.07f;    // choosing the ring's center goes back to the ring before
constexpr float kCloseDistance = 1.0f; // walking this far from the ring closes it
// Item models are about kItemModelSize units across at scale 1; on the ring they're kItemSize meters across.
constexpr float kItemModelSize = 60.0f;
constexpr float kItemSize = 0.09f;
constexpr float kHoverGrow = 1.5f;
// A hand this close to the head (meters) is at the face, where masks go on and come off. A mask in hand is about
// kMaskSize meters across.
constexpr float kFaceReach = 0.18f;
constexpr float kMaskSize = 0.2f;

enum class EntryKind { UseItem, Mask, Category, Pause, SwitchGame };

struct Entry {
    EntryKind kind;
    s16 model;
    u8 item = ITEM_NONE;
    int category = -1;
};

// The rings, as a tree. The first ring's sections: Ranged (the bow and its magic arrows, the hookshot), Explosives
// (bombs, bombchus, the powder keg, Deku sticks and nuts, beans), Tools (the lens, the pictograph box, the Great
// Fairy's Sword), Bottles, Masks (a ring of the four that transform Link, and two rings of the rest), Quest (the
// ocarina and the trade items) and System (the pause menu).
enum MenuCategory {
    CAT_ROOT,
    CAT_RANGED,
    CAT_EXPLOSIVES,
    CAT_TOOLS,
    CAT_BOTTLES,
    CAT_MASKS,
    CAT_FORM_MASKS,
    CAT_MASKS_1,
    CAT_MASKS_2,
    CAT_QUEST,
    CAT_SYSTEM,
};

// The masks other than the four transformation masks, in the pause menu's order, split into two rings.
constexpr u8 kMasks1[] = { ITEM_MASK_POSTMAN, ITEM_MASK_ALL_NIGHT, ITEM_MASK_BLAST,  ITEM_MASK_STONE,
                           ITEM_MASK_GREAT_FAIRY, ITEM_MASK_KEATON, ITEM_MASK_BREMEN, ITEM_MASK_BUNNY,
                           ITEM_MASK_DON_GERO, ITEM_MASK_SCENTS };
constexpr u8 kMasks2[] = { ITEM_MASK_ROMANI, ITEM_MASK_CIRCUS_LEADER, ITEM_MASK_KAFEIS_MASK, ITEM_MASK_COUPLE,
                           ITEM_MASK_TRUTH, ITEM_MASK_KAMARO, ITEM_MASK_GIBDO, ITEM_MASK_GARO,
                           ITEM_MASK_CAPTAIN, ITEM_MASK_GIANT };

bool IsMask(u8 item) {
    return item >= ITEM_MASK_DEKU && item <= ITEM_MASK_GIANT;
}

// The item-get model for an item. The bow with a magic arrow shows that arrow.
s16 ItemModel(u8 item) {
    switch (item) {
        case ITEM_BOW_FIRE:
            item = ITEM_ARROW_FIRE;
            break;
        case ITEM_BOW_ICE:
            item = ITEM_ARROW_ICE;
            break;
        case ITEM_BOW_LIGHT:
            item = ITEM_ARROW_LIGHT;
            break;
        default:
            break;
    }
    return Player_GetItemModelVr(item);
}

// Whether Link can use an item in the form he's in, as the game greys out its button otherwise.
bool UsableNow(u8 item) {
    return item < ITEM_FD && gPlayerFormItemRestrictions[GET_PLAYER_FORM][item] != 0;
}

void AddItem(std::vector<Entry>& out, u8 item) {
    if (item < ITEM_FD && UsableNow(item) && ItemModel(item) != GID_NONE) {
        out.push_back({ IsMask(item) ? EntryKind::Mask : EntryKind::UseItem, ItemModel(item), item });
    }
}

void AddOwnedItem(std::vector<Entry>& out, u8 item) {
    if (INV_CONTENT(item) == item) {
        AddItem(out, item);
    }
}

// Items that run out of ammo stay off the ring while they're empty.
void AddAmmoItem(std::vector<Entry>& out, u8 item) {
    if (INV_CONTENT(item) == item && AMMO(item) > 0) {
        AddItem(out, item);
    }
}

std::vector<Entry> CategoryEntries(int category);

// A ring inside this one, shown by the model of the first thing in it. One with nothing usable in it is left out,
// and one with a single choice is replaced by that choice, so no ring is a pointless extra step.
void AddCategory(std::vector<Entry>& out, int category) {
    std::vector<Entry> entries = CategoryEntries(category);
    if (entries.size() == 1) {
        out.push_back(entries[0]);
    } else if (!entries.empty()) {
        Entry entry = { EntryKind::Category, entries[0].model };
        entry.category = category;
        out.push_back(entry);
    }
}

std::vector<Entry> CategoryEntries(int category) {
    std::vector<Entry> out;
    switch (category) {
        case CAT_ROOT:
            for (int child : { CAT_RANGED, CAT_EXPLOSIVES, CAT_TOOLS, CAT_BOTTLES, CAT_MASKS, CAT_QUEST, CAT_SYSTEM }) {
                AddCategory(out, child);
            }
            break;
        case CAT_RANGED:
            if (INV_CONTENT(ITEM_BOW) == ITEM_BOW && AMMO(ITEM_BOW) > 0) {
                AddItem(out, ITEM_BOW);
                if (INV_CONTENT(ITEM_ARROW_FIRE) == ITEM_ARROW_FIRE) {
                    AddItem(out, ITEM_BOW_FIRE);
                }
                if (INV_CONTENT(ITEM_ARROW_ICE) == ITEM_ARROW_ICE) {
                    AddItem(out, ITEM_BOW_ICE);
                }
                if (INV_CONTENT(ITEM_ARROW_LIGHT) == ITEM_ARROW_LIGHT) {
                    AddItem(out, ITEM_BOW_LIGHT);
                }
            }
            AddOwnedItem(out, ITEM_HOOKSHOT);
            break;
        case CAT_EXPLOSIVES:
            AddAmmoItem(out, ITEM_BOMB);
            AddAmmoItem(out, ITEM_BOMBCHU);
            AddAmmoItem(out, ITEM_POWDER_KEG);
            AddAmmoItem(out, ITEM_DEKU_STICK);
            AddAmmoItem(out, ITEM_DEKU_NUT);
            AddAmmoItem(out, ITEM_MAGIC_BEANS);
            break;
        case CAT_TOOLS:
            AddOwnedItem(out, ITEM_LENS_OF_TRUTH);
            AddOwnedItem(out, ITEM_PICTOGRAPH_BOX);
            AddOwnedItem(out, ITEM_SWORD_GREAT_FAIRY);
            break;
        case CAT_BOTTLES:
            for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_6; slot++) {
                AddItem(out, gSaveContext.save.saveInfo.inventory.items[slot]);
            }
            break;
        case CAT_MASKS:
            AddCategory(out, CAT_FORM_MASKS);
            AddCategory(out, CAT_MASKS_1);
            AddCategory(out, CAT_MASKS_2);
            break;
        case CAT_FORM_MASKS:
            for (u8 mask : { ITEM_MASK_DEKU, ITEM_MASK_GORON, ITEM_MASK_ZORA, ITEM_MASK_FIERCE_DEITY }) {
                AddOwnedItem(out, mask);
            }
            break;
        case CAT_MASKS_1:
            for (u8 mask : kMasks1) {
                AddOwnedItem(out, mask);
            }
            break;
        case CAT_MASKS_2:
            for (u8 mask : kMasks2) {
                AddOwnedItem(out, mask);
            }
            break;
        case CAT_QUEST:
            AddOwnedItem(out, ITEM_OCARINA_OF_TIME);
            for (int slot : { SLOT_TRADE_DEED, SLOT_TRADE_KEY_MAMA, SLOT_TRADE_COUPLE }) {
                AddItem(out, gSaveContext.save.saveInfo.inventory.items[slot]);
            }
            break;
        case CAT_SYSTEM: {
            // The pause menu, shown by a map.
            Entry pause = { EntryKind::Pause, Player_GetItemModelVr(ITEM_TINGLE_MAP) };
            if (pause.model != GID_NONE) {
                out.push_back(pause);
            }
            // The other game, shown by its ocarina.
            Entry other = { EntryKind::SwitchGame, Player_GetItemModelVr(ITEM_OCARINA_OF_TIME) };
            if (other.model != GID_NONE) {
                out.push_back(other);
            }
            break;
        }
        default:
            break;
    }
    return out;
}

struct Vec3 {
    float x, y, z;
};

Vec3 Sub(const float a[3], const float b[3]) {
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

float Length(Vec3 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 Normalize(Vec3 v) {
    float length = Length(v);
    return length > 0.0001f ? Vec3{ v.x / length, v.y / length, v.z / length } : Vec3{ 0.0f, 0.0f, 1.0f };
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

// The ring, fixed in the play space where it opened, facing the head.
struct Menu {
    bool open = false;
    Hand hand = HAND_LEFT; // the hand choosing, for haptics
    float center[3] = {};
    Vec3 right = {};
    Vec3 up = {};
    std::vector<int> path; // the rings opened to get here, starting with CAT_ROOT
    s16 centerModel = 0;   // inside a ring, the model it was opened from, sitting in the middle
    std::vector<Entry> entries;
    int hovered = -1; // the item either hand is nearest to, drawn larger
    int handHovered[HAND_COUNT] = { -1, -1 };
};

Menu sMenu;
bool sGripLatched[HAND_COUNT] = {};
bool sTriggerLatched[HAND_COUNT] = {};
bool sMenuButtonLatched = false;

// A chosen item, put on its C button on the next game tick, and the buttons to press then.
u8 sPendingItem = ITEM_NONE;
s32 sPendingSlot = EQUIP_SLOT_NONE;
u16 sPendingPress = 0;

// A mask in one hand, on its way to the face (see UpdateMasks). One taken off at the face is held while that grip
// is, and put away when it's let go.
u8 sHeldMask = ITEM_NONE;
Hand sMaskHand = HAND_LEFT;
bool sMaskLeftFace = false;
bool sMaskFromFace = false;
bool sMaskGripLatched[HAND_COUNT] = {};

void EntryOffset(int index, float& right, float& up) {
    int count = static_cast<int>(sMenu.entries.size());
    float angle = static_cast<float>(M_PI) * 0.5f - index * 2.0f * static_cast<float>(M_PI) / count;
    right = cosf(angle);
    up = sinf(angle);
}

// Where an item sits around the ring. The first is at the top, and the rest go clockwise.
void EntryPosition(int index, float out[3]) {
    float right, up;
    EntryOffset(index, right, up);
    float c = right * kRingRadius;
    float s = up * kRingRadius;
    out[0] = sMenu.center[0] + sMenu.right.x * c + sMenu.up.x * s;
    out[1] = sMenu.center[1] + sMenu.right.y * c + sMenu.up.y * s;
    out[2] = sMenu.center[2] + sMenu.right.z * c + sMenu.up.z * s;
}

bool InsideRing() {
    return sMenu.path.size() > 1;
}

void ShowRing(int category, s16 centerModel) {
    sMenu.path.push_back(category);
    sMenu.centerModel = centerModel;
    sMenu.entries = CategoryEntries(category);
    sMenu.hovered = -1;
    sMenu.handHovered[HAND_LEFT] = sMenu.handHovered[HAND_RIGHT] = -1;
}

// Back to the ring this one was opened from.
void ShowParentRing() {
    sMenu.path.pop_back();
    int parent = sMenu.path.back();
    sMenu.path.pop_back();
    s16 centerModel = 0;
    if (!sMenu.path.empty()) {
        // The parent's own model is the one it has in its parent's ring.
        for (const Entry& entry : CategoryEntries(sMenu.path.back())) {
            if (entry.kind == EntryKind::Category && entry.category == parent) {
                centerModel = entry.model;
            }
        }
    }
    ShowRing(parent, centerModel);
}

void Open(Hand hand) {
    const Pose& head = Runtime::Get().GetHead();
    if (!head.valid) {
        return;
    }
    // Ahead along where the head faces, ignoring how far it's tipped up or down.
    const float* q = head.orientation;
    Vec3 forward = Normalize({ -2.0f * (q[0] * q[2] + q[3] * q[1]), 0.0f,
                               -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])) });

    sMenu = {};
    sMenu.open = true;
    sMenu.hand = hand;
    sMenu.center[0] = head.position[0] + forward.x * kMenuForward;
    sMenu.center[1] = head.position[1] - kMenuDrop;
    sMenu.center[2] = head.position[2] + forward.z * kMenuForward;
    // Upright, facing straight back at the player.
    Vec3 normal = { -forward.x, 0.0f, -forward.z };
    sMenu.right = Normalize(Cross({ 0.0f, 1.0f, 0.0f }, normal));
    sMenu.up = Cross(normal, sMenu.right);
    ShowRing(CAT_ROOT, 0);
    if (sMenu.entries.empty()) {
        sMenu.open = false;
        return;
    }
    Runtime::Get().Vibrate(hand, 0.3f, 0.04f);
}

void Close() {
    sMenu.open = false;
    sMenu.entries.clear();
}

// The C button an item goes on: a mask on C-down, anything else on the chooser's side. One already on a C button
// stays where it is.
s32 SlotFor(u8 item, Hand hand) {
    for (s32 slot = EQUIP_SLOT_C_LEFT; slot <= EQUIP_SLOT_C_RIGHT; slot++) {
        if (BUTTON_ITEM_EQUIP(0, slot) == item) {
            return slot;
        }
    }
    if (IsMask(item)) {
        return EQUIP_SLOT_C_DOWN;
    }
    return hand == HAND_LEFT ? EQUIP_SLOT_C_LEFT : EQUIP_SLOT_C_RIGHT;
}

u16 SlotButton(s32 slot) {
    switch (slot) {
        case EQUIP_SLOT_C_LEFT:
            return BTN_CLEFT;
        case EQUIP_SLOT_C_DOWN:
            return BTN_CDOWN;
        default:
            return BTN_CRIGHT;
    }
}

// The mask Link has on now, a transformation's included, or ITEM_NONE.
u8 WornMask() {
    Player* player = GET_PLAYER(gPlayState);
    switch (player->transformation) {
        case PLAYER_FORM_DEKU:
            return ITEM_MASK_DEKU;
        case PLAYER_FORM_GORON:
            return ITEM_MASK_GORON;
        case PLAYER_FORM_ZORA:
            return ITEM_MASK_ZORA;
        case PLAYER_FORM_FIERCE_DEITY:
            return ITEM_MASK_FIERCE_DEITY;
        default:
            return static_cast<u8>(Player_GetCurMaskItemId(gPlayState));
    }
}

bool HandAtFace(const Pose& hand) {
    const Pose& head = Runtime::Get().GetHead();
    if (!hand.valid || !head.valid) {
        return false;
    }
    float dx = hand.position[0] - head.position[0];
    float dy = hand.position[1] - head.position[1];
    float dz = hand.position[2] - head.position[2];
    return dx * dx + dy * dy + dz * dz < kFaceReach * kFaceReach;
}

// Presses the C button a mask is on, putting it there (on C-down) if it's on none, which puts it on or takes it off.
void PressMask(u8 mask) {
    s32 slot = SlotFor(mask, HAND_LEFT);
    if (BUTTON_ITEM_EQUIP(0, slot) != mask) {
        sPendingItem = mask;
        sPendingSlot = slot;
    }
    sPendingPress |= SlotButton(slot);
}

// A mask in a hand goes on once the hand, having left the face, reaches it. With one on, a grip squeezed at the face
// takes it off into that hand, until the grip lets go. Majora's Mask puts masks on and takes them off by their
// button, so these press it.
void UpdateMasks() {
    Runtime& runtime = Runtime::Get();
    Player* player = GET_PLAYER(gPlayState);
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        bool pressed = state.gripHeld && !sMaskGripLatched[hand];
        sMaskGripLatched[hand] = state.gripHeld;
        bool atFace = HandAtFace(state.gripPose);
        if (sHeldMask != ITEM_NONE) {
            if (hand != sMaskHand) {
                continue;
            }
            if (sMaskFromFace && !state.gripHeld) {
                // Let go of a mask taken off: it goes back with the others.
                sHeldMask = ITEM_NONE;
            } else if (!atFace) {
                sMaskLeftFace = true;
            } else if (sMaskLeftFace) {
                PressMask(sHeldMask);
                runtime.Vibrate(static_cast<Hand>(hand), 0.6f, 0.06f);
                SPDLOG_INFO("[VR] Put on mask {}", sHeldMask);
                sHeldMask = ITEM_NONE;
            }
        } else if (pressed && atFace && !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)) {
            u8 worn = WornMask();
            if (worn == ITEM_NONE) {
                continue;
            }
            PressMask(worn);
            sHeldMask = worn;
            sMaskHand = static_cast<Hand>(hand);
            sMaskLeftFace = false;
            sMaskFromFace = true;
            runtime.Vibrate(static_cast<Hand>(hand), 0.6f, 0.06f);
            SPDLOG_INFO("[VR] Took off mask {}", worn);
        }
    }
}

void Choose(const Entry& entry) {
    switch (entry.kind) {
        case EntryKind::UseItem:
            sPendingItem = entry.item;
            sPendingSlot = SlotFor(entry.item, sMenu.hand);
            SPDLOG_INFO("[VR] Item {} onto C button {} from the {} hand", entry.item, sPendingSlot,
                        sMenu.hand == HAND_LEFT ? "left" : "right");
            Close();
            break;
        case EntryKind::Mask:
            // Into the hand that chose it, on its way to the face. One already on stays on.
            if (entry.item != WornMask()) {
                sPendingItem = entry.item;
                sPendingSlot = SlotFor(entry.item, sMenu.hand);
                sHeldMask = entry.item;
                sMaskHand = sMenu.hand;
                sMaskLeftFace = !HandAtFace(Runtime::Get().GetController(sMenu.hand).gripPose);
                sMaskFromFace = false;
            }
            Close();
            break;
        case EntryKind::Category:
            ShowRing(entry.category, entry.model);
            break;
        case EntryKind::Pause:
            Close();
            sPendingPress = BTN_START;
            break;
        case EntryKind::SwitchGame:
            Close();
            SwitchToGame("oot");
            break;
    }
    Runtime::Get().Vibrate(sMenu.hand, 0.5f, 0.05f);
}

// Puts an item on a C button, as the pause menu does. A slot the item has in the inventory is kept with it, since
// bottles of the same thing are told apart only by it.
void PutOnButton(u8 item, s32 slot) {
    u8 inventorySlot = SLOT(item);
    if (item >= ITEM_BOTTLE && item <= ITEM_OBABA_DRINK) {
        for (int bottle = SLOT_BOTTLE_1; bottle <= SLOT_BOTTLE_6; bottle++) {
            if (gSaveContext.save.saveInfo.inventory.items[bottle] == item) {
                inventorySlot = bottle;
                break;
            }
        }
    } else if (item >= ITEM_BOW_FIRE && item <= ITEM_BOW_LIGHT) {
        inventorySlot = SLOT_BOW;
    }
    if (BUTTON_ITEM_EQUIP(0, slot) == item) {
        return;
    }
    BUTTON_ITEM_EQUIP(0, slot) = item;
    C_SLOT_EQUIP(0, slot) = inventorySlot;
    Interface_LoadItemIcon(gPlayState, slot);
    Audio_PlaySfx(NA_SE_SY_DECIDE);
}

} // namespace

void Items_Close() {
    if (sMenu.open) {
        Close();
    }
}

void Items_OnFrame() {
    Runtime& runtime = Runtime::Get();

    bool choosePressed[HAND_COUNT]; // the grip or the trigger, which both choose
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        choosePressed[hand] = (state.gripHeld && !sGripLatched[hand]) || (state.triggerHeld && !sTriggerLatched[hand]);
        sGripLatched[hand] = state.gripHeld;
        sTriggerLatched[hand] = state.triggerHeld;
    }

    // Y opens the ring and closes it again. While the ocarina is out, Y is one of its notes instead.
    bool menuButton = runtime.GetController(HAND_LEFT).secondary;
    bool menuPressed = menuButton && !sMenuButtonLatched && !Game_PlayingOcarina();
    sMenuButtonLatched = menuButton;

    if (!sMenu.open) {
        Player* player = GET_PLAYER(gPlayState);
        if (menuPressed && !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)) {
            // A mask in hand goes back with the others.
            sHeldMask = ITEM_NONE;
            Open(HAND_LEFT);
            return;
        }
        UpdateMasks();
        return;
    }

    const Pose& head = runtime.GetHead();
    if (menuPressed || !head.valid || Length(Sub(head.position, sMenu.center)) > kCloseDistance) {
        Close();
        return;
    }

    // Each hand picks out an item by touching it or, failing that, by aiming at it. Index -2 is the ring's center.
    // A touching hand beats a pointing one for which item is drawn larger.
    int overall = -1;
    bool overallTouching = false;
    float pointCos = cosf(DEG_TO_RAD(kPointAngle));
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        int touched = -1;
        float nearest = kTouchReach;
        int pointed = -1;
        float bestCos = pointCos;
        Vec3 aim = {};
        if (state.aimPose.valid) {
            const float* q = state.aimPose.orientation;
            aim = Normalize({ -2.0f * (q[0] * q[2] + q[3] * q[1]), -2.0f * (q[1] * q[2] - q[3] * q[0]),
                              -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])) });
        }
        int count = static_cast<int>(sMenu.entries.size());
        for (int i = (InsideRing() ? -2 : 0); i < count; i++) {
            float position[3];
            if (i == -2) {
                memcpy(position, sMenu.center, sizeof(position));
            } else if (i < 0) {
                continue;
            } else {
                EntryPosition(i, position);
            }
            float reach = (i == -2) ? kBackReach : kTouchReach;
            if (state.gripPose.valid) {
                float distance = Length(Sub(state.gripPose.position, position));
                if (distance < reach && distance < nearest) {
                    nearest = distance;
                    touched = i;
                }
            }
            if (state.aimPose.valid) {
                Vec3 toItem = Normalize(Sub(position, state.aimPose.position));
                float c = toItem.x * aim.x + toItem.y * aim.y + toItem.z * aim.z;
                if (c > bestCos) {
                    bestCos = c;
                    pointed = i;
                }
            }
        }
        int hovered = touched != -1 ? touched : pointed;
        if (hovered != sMenu.handHovered[hand] && hovered != -1) {
            runtime.Vibrate(static_cast<Hand>(hand), 0.15f, 0.02f);
        }
        sMenu.handHovered[hand] = hovered;
        if (hovered >= 0 && (overall < 0 || (touched >= 0 && !overallTouching))) {
            overall = hovered;
            overallTouching = touched >= 0;
        }
    }
    sMenu.hovered = overall;

    // Squeeze or pull the trigger on an item to choose it, on a ring to open it, or on the ring's center to go back
    // one ring. Doing it on nothing does nothing, so a miss doesn't lose the way back down the rings.
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        if (!choosePressed[hand]) {
            continue;
        }
        sMenu.hand = static_cast<Hand>(hand);
        int target = sMenu.handHovered[hand];
        if (target >= 0) {
            Choose(sMenu.entries[target]);
        } else if (target == -2) {
            ShowParentRing();
            runtime.Vibrate(static_cast<Hand>(hand), 0.3f, 0.03f);
        }
        return;
    }
}

void Items_Draw(PlayState* play) {
    const Pose& head = Runtime::Get().GetHead();
    Vec3f headWorld = Game_PlayToWorld(head.position);

    // A mask in hand, held up by its middle with its face toward the player.
    const Pose& maskGrip = Runtime::Get().GetController(sMaskHand).gripPose;
    if (sHeldMask != ITEM_NONE && maskGrip.valid) {
        Vec3f world = Game_PlayToWorld(maskGrip.position);
        s16 yaw = RAD_TO_BINANG(atan2f(headWorld.x - world.x, headWorld.z - world.z));
        float size = kMaskSize * Game_UnitsPerMeter() / kItemModelSize;
        Matrix_Translate(world.x, world.y, world.z, MTXMODE_NEW);
        Matrix_RotateZYX(0, yaw, 0, MTXMODE_APPLY);
        Matrix_Scale(size, size, size, MTXMODE_APPLY);
        GetItem_Draw(play, ItemModel(sHeldMask));
    }

    if (!sMenu.open || sMenu.entries.empty()) {
        return;
    }
    float scale = kItemSize * Game_UnitsPerMeter() / kItemModelSize;

    auto drawModel = [&](const float position[3], s16 model, float size, bool spin) {
        Vec3f world = Game_PlayToWorld(position);
        s16 yaw = RAD_TO_BINANG(atan2f(headWorld.x - world.x, headWorld.z - world.z));
        if (spin) {
            yaw += static_cast<s16>(play->gameplayFrames * 1500);
        }
        Matrix_Translate(world.x, world.y, world.z, MTXMODE_NEW);
        Matrix_RotateZYX(0, yaw, 0, MTXMODE_APPLY);
        Matrix_Scale(size, size, size, MTXMODE_APPLY);
        GetItem_Draw(play, model);
    };

    for (int i = 0; i < static_cast<int>(sMenu.entries.size()); i++) {
        float position[3];
        EntryPosition(i, position);
        bool hovered = i == sMenu.hovered;
        drawModel(position, sMenu.entries[i].model, hovered ? scale * kHoverGrow : scale, hovered);
    }
    // Inside a ring the model it was opened from sits in the middle, where the hand goes to get back out.
    if (InsideRing()) {
        drawModel(sMenu.center, sMenu.centerModel, scale * 0.6f, false);
    }
}

uint16_t Items_TakeButtons() {
    if (gPlayState == nullptr) {
        return 0;
    }
    if (sPendingItem != ITEM_NONE) {
        PutOnButton(sPendingItem, sPendingSlot);
        sPendingItem = ITEM_NONE;
    }
    u16 press = sPendingPress;
    sPendingPress = 0;
    return press;
}

bool Items_MenuOpen() {
    return sMenu.open;
}

bool Items_HandAtFace(Hand hand) {
    return HandAtFace(Runtime::Get().GetController(hand).gripPose);
}

} // namespace SohVr
