#include "VrItems.h"
#include "VrGame.h"
#include "VrRuntime.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/randomizer/draw.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <vector>

extern "C" {
#include "global.h"
#include "objects/object_gi_clothes/object_gi_clothes.h"
#include "soh/Enhancements/vr/Vr.h"
extern PlayState* gPlayState;
extern u8 gEquipAgeReqs[][4];
}

namespace SohVr {

namespace {

// The ring stands upright in front of the face, a little below eye level, facing the player (meters from the head).
constexpr float kMenuForward = 0.45f;
constexpr float kMenuDrop = 0.15f;
constexpr float kRingRadius = 0.17f;
constexpr float kTouchReach = 0.1f;  // a hand this close to an item touches it
constexpr float kPointAngle = 10.0f; // a controller aimed within this many degrees of an item points at it
constexpr float kBackReach = 0.07f;  // choosing the ring's center goes back to the categories
constexpr float kCloseDistance = 1.0f; // walking this far from the ring closes it
// Item-get models are drawn at 0.2 when Link holds one up. This makes them about 11 cm across, whatever his age.
constexpr float kModelScale = 0.06f;
constexpr float kModelScaleUnitsPerMeter = 40.0f;
constexpr float kHoverGrow = 1.5f;
constexpr s16 kMasterSwordModel = -1; // has no item-get model; the randomizer's drawing is used
constexpr s16 kKokiriTunicModel = -2; // has none either; the other tunics' model is drawn in green
// On the floating screen the ring sits in the middle of the picture, sized in meters on the screen: its radius,
// each item's size, and how close to an item a controller has to point (the middle, which goes back, is smaller).
constexpr float kScreenRingRadius = 0.35f;
constexpr float kScreenItemSize = 0.2f;
constexpr float kScreenPickReach = 0.15f;
constexpr float kScreenBackReach = 0.1f;
// How far in front of the game camera the ring is drawn, in game units: just past the near plane, so nothing in
// the scene comes between them.
constexpr float kScreenRingDepth = 20.0f;
constexpr float kItemModelSize = 60.0f; // about how wide an item-get model is at scale 1

enum class EntryKind { UseItem, Equip, Category };

struct Entry {
    EntryKind kind;
    s16 model;
    u8 item = ITEM_NONE;
    u8 equipType = 0;
    u8 equipValue = 0;
    int category = -1;
    s8 slot = -1; // a bottle's inventory slot, since bottles of the same thing are told apart only by it
};

// The rings, as a tree with a handful of choices on each: Gear (swords, shields, tunics, boots), Weapons (bow and
// arrows, slingshot, boomerang, hookshot, hammer), Explosives (bombs, bombchus, nuts, sticks), Magic (spells, lens)
// and Items (ocarina, bottles, trade items).
enum MenuCategory {
    CAT_ROOT,
    CAT_GEAR,
    CAT_SWORDS,
    CAT_SHIELDS,
    CAT_TUNICS,
    CAT_BOOTS,
    CAT_WEAPONS,
    CAT_BOWS,
    CAT_EXPLOSIVES,
    CAT_MAGIC,
    CAT_ITEMS,
    CAT_BOTTLES,
    CAT_TRADE,
};

s16 ItemModel(u8 item) {
    switch (item) {
        case ITEM_STICK: return GID_STICK;
        case ITEM_NUT: return GID_NUTS;
        case ITEM_BOMB: return GID_BOMB;
        case ITEM_BOW: return GID_BOW;
        case ITEM_BOW_ARROW_FIRE: return GID_ARROW_FIRE;
        case ITEM_BOW_ARROW_ICE: return GID_ARROW_ICE;
        case ITEM_BOW_ARROW_LIGHT: return GID_ARROW_LIGHT;
        case ITEM_SLINGSHOT: return GID_SLINGSHOT;
        case ITEM_OCARINA_FAIRY: return GID_OCARINA_FAIRY;
        case ITEM_OCARINA_TIME: return GID_OCARINA_TIME;
        case ITEM_BOMBCHU: return GID_BOMBCHU;
        case ITEM_HOOKSHOT: return GID_HOOKSHOT;
        case ITEM_LONGSHOT: return GID_LONGSHOT;
        case ITEM_DINS_FIRE: return GID_DINS_FIRE;
        case ITEM_FARORES_WIND: return GID_FARORES_WIND;
        case ITEM_NAYRUS_LOVE: return GID_NAYRUS_LOVE;
        case ITEM_BOOMERANG: return GID_BOOMERANG;
        case ITEM_LENS: return GID_LENS;
        case ITEM_BEAN: return GID_BEAN;
        case ITEM_HAMMER: return GID_HAMMER;
        case ITEM_BOTTLE: return GID_BOTTLE;
        case ITEM_POTION_RED: return GID_POTION_RED;
        case ITEM_POTION_GREEN: return GID_POTION_GREEN;
        case ITEM_POTION_BLUE: return GID_POTION_BLUE;
        case ITEM_FAIRY: return GID_FAIRY;
        case ITEM_FISH: return GID_FISH;
        case ITEM_MILK_BOTTLE:
        case ITEM_MILK_HALF: return GID_MILK;
        case ITEM_LETTER_RUTO: return GID_LETTER_RUTO;
        case ITEM_BLUE_FIRE: return GID_BLUE_FIRE;
        case ITEM_BUG: return GID_BUG;
        case ITEM_BIG_POE: return GID_BIG_POE;
        case ITEM_POE: return GID_POE;
        case ITEM_WEIRD_EGG:
        case ITEM_POCKET_EGG: return GID_EGG;
        case ITEM_CHICKEN:
        case ITEM_POCKET_CUCCO: return GID_CHICKEN;
        case ITEM_LETTER_ZELDA: return GID_LETTER_ZELDA;
        case ITEM_MASK_KEATON: return GID_MASK_KEATON;
        case ITEM_MASK_SKULL: return GID_MASK_SKULL;
        case ITEM_MASK_SPOOKY: return GID_MASK_SPOOKY;
        case ITEM_MASK_BUNNY: return GID_MASK_BUNNY;
        case ITEM_MASK_GORON: return GID_MASK_GORON;
        case ITEM_MASK_ZORA: return GID_MASK_ZORA;
        case ITEM_MASK_GERUDO: return GID_MASK_GERUDO;
        case ITEM_MASK_TRUTH: return GID_MASK_TRUTH;
        case ITEM_SOLD_OUT: return GID_SOLDOUT;
        case ITEM_COJIRO: return GID_COJIRO;
        case ITEM_ODD_MUSHROOM: return GID_ODD_MUSHROOM;
        case ITEM_ODD_POTION: return GID_ODD_POTION;
        case ITEM_SAW: return GID_SAW;
        case ITEM_SWORD_BROKEN: return GID_SWORD_BROKEN;
        case ITEM_PRESCRIPTION: return GID_PRESCRIPTION;
        case ITEM_FROG: return GID_FROG;
        case ITEM_EYEDROPS: return GID_EYEDROPS;
        case ITEM_CLAIM_CHECK: return GID_CLAIM_CHECK;
        default: return GID_BOTTLE;
    }
}

// Each piece of gear's model. The Kokiri Tunic and Boots have none: the tunic is the other tunics' model in green,
// and the Iron Boots stand in for the boots, always first in their ring.
s16 EquipModel(u8 type, u8 value) {
    static const s16 kModels[4][3] = {
        { GID_SWORD_KOKIRI, kMasterSwordModel, GID_SWORD_BGS },
        { GID_SHIELD_DEKU, GID_SHIELD_HYLIAN, GID_SHIELD_MIRROR },
        { kKokiriTunicModel, GID_TUNIC_GORON, GID_TUNIC_ZORA },
        { GID_BOOTS_IRON, GID_BOOTS_IRON, GID_BOOTS_HOVER },
    };
    return kModels[type][value - 1];
}

bool ItemUsableAtAge(u8 item) {
    u8 required = gItemAgeReqs[item];
    return required == 9 || required == gSaveContext.linkAge;
}

bool EquipUsableAtAge(u8 type, u8 value) {
    u8 required = gEquipAgeReqs[type][value];
    return GameInteractor_Should(VB_PLAYER_MEETS_AGE_REQ, required == 9 || required == gSaveContext.linkAge,
                                 required);
}

void AddItem(std::vector<Entry>& out, u8 item) {
    if (item < ITEM_NONE_FE && ItemUsableAtAge(item)) {
        out.push_back({ EntryKind::UseItem, ItemModel(item), item });
    }
}

// Items that run out of ammo stay off the ring while they're empty.
void AddAmmoItem(std::vector<Entry>& out, u8 item) {
    if (INV_CONTENT(item) == item && AMMO(item) > 0) {
        AddItem(out, item);
    }
}

void AddEquip(std::vector<Entry>& out, u8 type, u8 value, bool owned) {
    if (owned && EquipUsableAtAge(type, value)) {
        Entry entry = { EntryKind::Equip, EquipModel(type, value) };
        entry.equipType = type;
        entry.equipValue = value;
        out.push_back(entry);
    }
}

void AddEquipType(std::vector<Entry>& out, u8 type) {
    for (u8 value = 1; value <= 3; value++) {
        bool owned = CHECK_OWNED_EQUIP(type, value - 1);
        if (type == EQUIP_TYPE_SWORD && value == EQUIP_VALUE_SWORD_BIGGORON) {
            owned = owned || CHECK_OWNED_EQUIP_ALT(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE);
        }
        AddEquip(out, type, value, owned);
    }
}

void AddOwnedItem(std::vector<Entry>& out, u8 item) {
    if (INV_CONTENT(item) == item) {
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
            for (int child : { CAT_GEAR, CAT_WEAPONS, CAT_EXPLOSIVES, CAT_MAGIC, CAT_ITEMS }) {
                AddCategory(out, child);
            }
            break;
        case CAT_GEAR:
            for (int child : { CAT_SWORDS, CAT_SHIELDS, CAT_TUNICS, CAT_BOOTS }) {
                AddCategory(out, child);
            }
            break;
        case CAT_SWORDS:
            AddEquipType(out, EQUIP_TYPE_SWORD);
            break;
        case CAT_SHIELDS:
            AddEquipType(out, EQUIP_TYPE_SHIELD);
            break;
        case CAT_TUNICS:
            AddEquipType(out, EQUIP_TYPE_TUNIC);
            break;
        case CAT_BOOTS:
            AddEquipType(out, EQUIP_TYPE_BOOTS);
            break;
        case CAT_WEAPONS:
            AddCategory(out, CAT_BOWS);
            AddOwnedItem(out, ITEM_SLINGSHOT);
            AddOwnedItem(out, ITEM_BOOMERANG);
            AddItem(out, INV_CONTENT(ITEM_HOOKSHOT));
            AddOwnedItem(out, ITEM_HAMMER);
            break;
        case CAT_BOWS:
            if (INV_CONTENT(ITEM_BOW) == ITEM_BOW) {
                AddItem(out, ITEM_BOW);
                if (INV_CONTENT(ITEM_ARROW_FIRE) == ITEM_ARROW_FIRE) {
                    AddItem(out, ITEM_BOW_ARROW_FIRE);
                }
                if (INV_CONTENT(ITEM_ARROW_ICE) == ITEM_ARROW_ICE) {
                    AddItem(out, ITEM_BOW_ARROW_ICE);
                }
                if (INV_CONTENT(ITEM_ARROW_LIGHT) == ITEM_ARROW_LIGHT) {
                    AddItem(out, ITEM_BOW_ARROW_LIGHT);
                }
            }
            break;
        case CAT_EXPLOSIVES:
            AddAmmoItem(out, ITEM_BOMB);
            AddAmmoItem(out, ITEM_BOMBCHU);
            AddAmmoItem(out, ITEM_NUT);
            AddAmmoItem(out, ITEM_STICK);
            break;
        case CAT_MAGIC:
            for (u8 item : { ITEM_DINS_FIRE, ITEM_FARORES_WIND, ITEM_NAYRUS_LOVE, ITEM_LENS }) {
                AddOwnedItem(out, item);
            }
            break;
        case CAT_ITEMS:
            AddItem(out, INV_CONTENT(ITEM_OCARINA_FAIRY));
            AddCategory(out, CAT_BOTTLES);
            AddCategory(out, CAT_TRADE);
            break;
        case CAT_BOTTLES:
            for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_4; slot++) {
                size_t before = out.size();
                AddItem(out, gSaveContext.inventory.items[slot]);
                if (out.size() > before) {
                    out.back().slot = static_cast<s8>(slot);
                }
            }
            break;
        case CAT_TRADE:
            AddItem(out, gSaveContext.inventory.items[SLOT_TRADE_CHILD]);
            AddItem(out, gSaveContext.inventory.items[SLOT_TRADE_ADULT]);
            AddAmmoItem(out, ITEM_BEAN);
            break;
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

// The menu's ring: in the world, fixed in the play space where it opened, facing the head; on the floating screen,
// in the middle of the picture.
struct Menu {
    bool open = false;
    bool onScreen = false;
    Hand hand = HAND_LEFT; // the hand that opened it, for haptics
    float center[3] = {};
    Vec3 right = {};
    Vec3 up = {};
    std::vector<int> path;          // the rings opened to get here, starting with CAT_ROOT
    s16 centerModel = 0;            // inside a ring, the model it was opened from, sitting in the middle
    std::vector<Entry> entries;
    int hovered = -1;               // the item either hand is nearest to, drawn larger
    int handHovered[HAND_COUNT] = { -1, -1 };
};

Menu sMenu;
bool sGripLatched[HAND_COUNT] = {};
bool sTriggerLatched[HAND_COUNT] = {};
bool sMenuButtonLatched = false;

// A chosen piece of gear, applied on the next game tick.
bool sEquipPending = false;
u8 sPendingEquipType = 0;
u8 sPendingEquipValue = 0;

// Where an item sits around the ring, as a share of the ring's radius to the right and up. The first is at the top,
// and the rest go clockwise.
void EntryOffset(int index, float& right, float& up) {
    int count = static_cast<int>(sMenu.entries.size());
    float angle = static_cast<float>(M_PI) * 0.5f - index * 2.0f * static_cast<float>(M_PI) / count;
    right = cosf(angle);
    up = sinf(angle);
}

void EntryPosition(int index, float out[3]) {
    float right, up;
    EntryOffset(index, right, up);
    float c = right * kRingRadius;
    float s = up * kRingRadius;
    out[0] = sMenu.center[0] + sMenu.right.x * c + sMenu.up.x * s;
    out[1] = sMenu.center[1] + sMenu.right.y * c + sMenu.up.y * s;
    out[2] = sMenu.center[2] + sMenu.right.z * c + sMenu.up.z * s;
}

// Where a controller points on the floating screen, in meters from its middle, right and up.
bool PointOnScreen(const ControllerState& state, float& x, float& y) {
    Pose screen;
    float width, height;
    if (!state.aimPose.valid || !Runtime::Get().GetScreen(screen, width, height)) {
        return false;
    }
    const float* q = state.aimPose.orientation;
    Vec3 aim = Normalize({ -2.0f * (q[0] * q[2] + q[3] * q[1]), -2.0f * (q[1] * q[2] - q[3] * q[0]),
                           -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])) });
    // The screen only ever turns about the vertical, so its normal and right follow from that turn.
    float yaw = 2.0f * atan2f(screen.orientation[1], screen.orientation[3]);
    Vec3 normal = { sinf(yaw), 0.0f, cosf(yaw) };
    Vec3 right = { cosf(yaw), 0.0f, -sinf(yaw) };
    float facing = aim.x * normal.x + aim.y * normal.y + aim.z * normal.z;
    if (facing > -0.01f) {
        return false; // pointing away from the screen, or along it
    }
    Vec3 toScreen = Sub(screen.position, state.aimPose.position);
    float distance = (toScreen.x * normal.x + toScreen.y * normal.y + toScreen.z * normal.z) / facing;
    if (distance <= 0.0f) {
        return false;
    }
    float hit[3] = { state.aimPose.position[0] + aim.x * distance, state.aimPose.position[1] + aim.y * distance,
                     state.aimPose.position[2] + aim.z * distance };
    Vec3 fromMiddle = Sub(hit, screen.position);
    x = fromMiddle.x * right.x + fromMiddle.z * right.z;
    y = fromMiddle.y;
    return fabsf(x) < width * 0.5f && fabsf(y) < height * 0.5f;
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
    if (sMenu.path.size() >= 1) {
        // The parent's own model is the one it has in its parent's ring.
        for (const Entry& entry : CategoryEntries(sMenu.path.back())) {
            if (entry.kind == EntryKind::Category && entry.category == parent) {
                centerModel = entry.model;
            }
        }
    }
    ShowRing(parent, centerModel);
}

void Open(Hand hand, bool onScreen) {
    const Pose& head = Runtime::Get().GetHead();
    if (!head.valid) {
        return;
    }
    // Ahead along where the head faces, ignoring how far it's tipped up or down.
    const float* q = head.orientation;
    Vec3 forward = { -2.0f * (q[0] * q[2] + q[3] * q[1]), 0.0f, -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])) };
    forward = Normalize(forward);

    sMenu = {};
    sMenu.open = true;
    sMenu.onScreen = onScreen;
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

void Choose(const Entry& entry) {
    switch (entry.kind) {
        case EntryKind::UseItem:
            Game_TakeItem(entry.item, sMenu.hand, entry.slot);
            SPDLOG_INFO("[VR] Took item {} from the menu with the {} hand", entry.item,
                        sMenu.hand == HAND_LEFT ? "left" : "right");
            Close();
            break;
        case EntryKind::Equip:
            sEquipPending = true;
            sPendingEquipType = entry.equipType;
            sPendingEquipValue = entry.equipValue;
            SPDLOG_INFO("[VR] Equipped gear type {} value {}", entry.equipType, entry.equipValue);
            Close();
            break;
        case EntryKind::Category:
            ShowRing(entry.category, entry.model);
            break;
    }
    Runtime::Get().Vibrate(sMenu.hand, 0.5f, 0.05f);
}

void Equip(u8 type, u8 value) {
    Inventory_ChangeEquipment(type, value);
    if (type == EQUIP_TYPE_SWORD) {
        // As the pause menu's equipment screen does, which also keeps B pointing at the sword.
        gSaveContext.infTable[29] = 0;
        u8 item = ITEM_SWORD_KOKIRI + value - 1;
        if (value == EQUIP_VALUE_SWORD_BIGGORON) {
            if (gSaveContext.bgsFlag != 0) {
                item = ITEM_SWORD_BGS;
                gSaveContext.swordHealth = 8;
            } else if (CHECK_OWNED_EQUIP_ALT(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BROKENGIANTKNIFE)) {
                item = ITEM_SWORD_KNIFE;
            }
        }
        gSaveContext.equips.buttonItems[0] = item;
        Interface_LoadItemIcon1(gPlayState, 0);
    }
    Player_SetEquipmentData(gPlayState, GET_PLAYER(gPlayState));
    Sfx_PlaySfxCentered(NA_SE_SY_DECIDE);
}

} // namespace

void Items_Close() {
    if (sMenu.open) {
        Close();
    }
}

void Items_OnFrame(bool onScreen) {
    Runtime& runtime = Runtime::Get();

    bool gripPressed[HAND_COUNT];
    bool choosePressed[HAND_COUNT]; // the grip or the trigger, which both choose
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        gripPressed[hand] = state.gripHeld && !sGripLatched[hand];
        choosePressed[hand] = gripPressed[hand] || (state.triggerHeld && !sTriggerLatched[hand]);
        sGripLatched[hand] = state.gripHeld;
        sTriggerLatched[hand] = state.triggerHeld;
    }

    // Y opens the menu and closes it again. While the ocarina is out, Y is one of its notes instead.
    bool menuButton = runtime.GetController(HAND_LEFT).secondary;
    bool menuPressed = menuButton && !sMenuButtonLatched && !Game_PlayingOcarina();
    sMenuButtonLatched = menuButton;

    // Switching between the world and the screen closes it, since it was laid out for the other.
    if (sMenu.open && sMenu.onScreen != onScreen) {
        Close();
    }
    if (!sMenu.open) {
        Player* player = GET_PLAYER(gPlayState);
        bool carrying = (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) != 0;
        if (menuPressed && !carrying) {
            Open(HAND_LEFT, onScreen);
        }
        return;
    }

    const Pose& head = runtime.GetHead();
    if (menuPressed || !head.valid ||
        (!onScreen && Length(Sub(head.position, sMenu.center)) > kCloseDistance)) {
        Close();
        return;
    }

    // Each hand picks out an item by touching it or, failing that, by aiming at it; on the screen only by aiming
    // at it. Index -2 is the ring's center. A touching hand beats a pointing one for which item is drawn larger.
    int overall = -1;
    bool overallTouching = false;
    float pointCos = cosf(DEG_TO_RAD(kPointAngle));
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        int touched = -1;
        float nearest = kTouchReach;
        int pointed = -1;
        float bestCos = pointCos;
        if (onScreen) {
            float x, y;
            float nearestOnScreen = kScreenPickReach;
            if (PointOnScreen(state, x, y)) {
                int count = static_cast<int>(sMenu.entries.size());
                for (int i = (InsideRing() ? -2 : 0); i < count; i++) {
                    float itemX = 0.0f;
                    float itemY = 0.0f;
                    if (i >= 0) {
                        EntryOffset(i, itemX, itemY);
                        itemX *= kScreenRingRadius;
                        itemY *= kScreenRingRadius;
                    } else if (i != -2) {
                        continue;
                    }
                    float distance = sqrtf((x - itemX) * (x - itemX) + (y - itemY) * (y - itemY));
                    float reach = (i == -2) ? kScreenBackReach : kScreenPickReach;
                    if (distance < reach && distance < nearestOnScreen) {
                        nearestOnScreen = distance;
                        pointed = i;
                    }
                }
            }
            if (pointed != sMenu.handHovered[hand] && pointed != -1) {
                runtime.Vibrate(static_cast<Hand>(hand), 0.15f, 0.02f);
            }
            sMenu.handHovered[hand] = pointed;
            if (pointed >= 0 && overall < 0) {
                overall = pointed;
            }
            continue;
        }
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

// The tunic item-get model, as GetItem_DrawOpa1023 draws the Goron and Zora ones, with its collar and body colors
// (main and shadow, as their color lists set them) in Kokiri green.
void DrawKokiriTunic(PlayState* play) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Gfx_SetupDL_25Opa(gfxCtx);
    Gfx*& opa = gfxCtx->polyOpa.p;
    gSPMatrix(opa++, MATRIX_NEWMTX(gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    gDPSetPrimColor(opa++, 0, 0, 30, 110, 25, 255);
    gDPSetEnvColor(opa++, 20, 70, 25, 255);
    gSPDisplayList(opa++, (Gfx*)gGiTunicCollarDL);
    gDPSetPrimColor(opa++, 0, 0, 60, 170, 50, 255);
    gDPSetEnvColor(opa++, 30, 100, 30, 255);
    gSPDisplayList(opa++, (Gfx*)gGiTunicDL);
}

void DrawModel(PlayState* play, s16 model) {
    if (model == kMasterSwordModel) {
        Randomizer_DrawMasterSword(play, nullptr);
    } else if (model == kKokiriTunicModel) {
        DrawKokiriTunic(play);
    } else {
        GetItem_Draw(play, model);
    }
}

// On the floating screen, the ring is drawn into the game's own view, just in front of its camera and facing it,
// sized so it covers the same part of the picture whatever the camera's field of view.
void DrawOnScreen(PlayState* play) {
    Pose screen;
    float width, height;
    if (!Runtime::Get().GetScreen(screen, width, height)) {
        return;
    }
    View& view = play->view;
    Vec3 eye = { view.eye.x, view.eye.y, view.eye.z };
    Vec3 forward =
        Normalize({ view.lookAt.x - view.eye.x, view.lookAt.y - view.eye.y, view.lookAt.z - view.eye.z });
    Vec3 right = Normalize(Cross(forward, { view.up.x, view.up.y, view.up.z }));
    Vec3 up = Cross(right, forward);
    // Game units at the ring's depth per meter on the screen. The picture's height spans the field of view.
    float unitsPerMeter = kScreenRingDepth * tanf(DEG_TO_RAD(view.fovy) * 0.5f) / (height * 0.5f);
    float scale = kScreenItemSize * unitsPerMeter / kItemModelSize;

    auto drawAt = [&](float x, float y, s16 model, float size, bool spin) {
        MtxF m;
        float along[3] = { x * unitsPerMeter, y * unitsPerMeter, kScreenRingDepth };
        Vec3 axes[3] = { right, up, { -forward.x, -forward.y, -forward.z } };
        for (int i = 0; i < 3; i++) {
            m.mf[i][0] = axes[i].x;
            m.mf[i][1] = axes[i].y;
            m.mf[i][2] = axes[i].z;
            m.mf[i][3] = 0.0f;
        }
        m.mf[3][0] = eye.x + right.x * along[0] + up.x * along[1] + forward.x * along[2];
        m.mf[3][1] = eye.y + right.y * along[0] + up.y * along[1] + forward.y * along[2];
        m.mf[3][2] = eye.z + right.z * along[0] + up.z * along[1] + forward.z * along[2];
        m.mf[3][3] = 1.0f;
        Vr_PutWorldMatrix(&m);
        if (spin) {
            Matrix_RotateY(BINANG_TO_RAD(static_cast<s16>(play->gameplayFrames * 1500)), MTXMODE_APPLY);
        }
        Matrix_Scale(size, size, size, MTXMODE_APPLY);
        DrawModel(play, model);
    };

    for (int i = 0; i < static_cast<int>(sMenu.entries.size()); i++) {
        float x, y;
        EntryOffset(i, x, y);
        bool hovered = i == sMenu.hovered;
        drawAt(x * kScreenRingRadius, y * kScreenRingRadius, sMenu.entries[i].model,
               hovered ? scale * kHoverGrow : scale, hovered);
    }
    if (InsideRing()) {
        drawAt(0.0f, 0.0f, sMenu.centerModel, scale * 0.6f, false);
    }
}

void Items_Draw(PlayState* play) {
    if (!sMenu.open || sMenu.entries.empty()) {
        return;
    }
    if (sMenu.onScreen) {
        DrawOnScreen(play);
        return;
    }
    const Pose& head = Runtime::Get().GetHead();
    Vec3f headWorld = Game_PlayToWorld(head.position);
    float scale = kModelScale * Game_UnitsPerMeter() / kModelScaleUnitsPerMeter;

    auto drawModel = [&](const float position[3], s16 model, float size, bool spin) {
        Vec3f world = Game_PlayToWorld(position);
        s16 yaw = RADF_TO_BINANG(atan2f(headWorld.x - world.x, headWorld.z - world.z));
        if (spin) {
            yaw += static_cast<s16>(play->gameplayFrames * 1500);
        }
        Matrix_Translate(world.x, world.y, world.z, MTXMODE_NEW);
        Matrix_RotateZYX(0, yaw, 0, MTXMODE_APPLY);
        Matrix_Scale(size, size, size, MTXMODE_APPLY);
        DrawModel(play, model);
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

void Items_ApplyPending() {
    if (sEquipPending && gPlayState != nullptr) {
        sEquipPending = false;
        Equip(sPendingEquipType, sPendingEquipValue);
    }
}

bool Items_MenuOpen() {
    return sMenu.open;
}

s16 Items_ItemModel(u8 item) {
    return ItemModel(item);
}

} // namespace SohVr
