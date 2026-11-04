#include "VrItems.h"
#include "VrGame.h"
#include "VrRuntime.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/randomizer/draw.h"
#include "soh/ResourceManagerHelpers.h"

#include <fast/resource/type/DisplayList.h>

#include <spdlog/spdlog.h>

#include <cmath>
#include <vector>

extern "C" {
#include "global.h"
#include "objects/object_gi_bomb_1/object_gi_bomb_1.h"
#include "objects/object_gi_boots_2/object_gi_boots_2.h"
#include "objects/object_gi_clothes/object_gi_clothes.h"
#include "objects/object_haka_objects/object_haka_objects.h"
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
constexpr s16 kKokiriBootsModel = -3; // has none either; the Iron Boots' model is drawn as leather
constexpr s16 kShipModel = -4;        // the System ring's: the Shadow Temple's ship, shrunk to an item's size
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

enum class EntryKind { UseItem, Equip, Category, Action };

// What an Action entry does.
enum MenuAction { ACTION_NONE, ACTION_PAUSE, ACTION_SETTINGS };

struct Entry {
    EntryKind kind;
    s16 model;
    u8 item = ITEM_NONE;
    u8 equipType = 0;
    u8 equipValue = 0;
    int category = -1;
    s8 slot = -1; // a bottle's inventory slot, since bottles of the same thing are told apart only by it
    int action = ACTION_NONE;
};

// The rings, as a tree. The first ring's sections: Ranged & Tools (slingshot, bow, boomerang, hookshot), Explosives &
// Heavy (bombs, bombchus, hammer, Deku sticks), Magic & Spells (the lens, the magic arrows, the spells), Bottles &
// Consumables (Deku nuts, beans and the bottles, shown by a bottle), Quest & Lore (the ocarina and the trade items), Equipment & Gear (a ring of
// rings: shields, swords, boots, tunics) and System (the pause menu and the VR settings).
enum MenuCategory {
    CAT_ROOT,
    CAT_RANGED,
    CAT_EXPLOSIVES,
    CAT_MAGIC,
    CAT_CONSUMABLES,
    CAT_QUEST,
    CAT_GEAR,
    CAT_SWORDS,
    CAT_SHIELDS,
    CAT_TUNICS,
    CAT_BOOTS,
    CAT_SYSTEM,
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
// and the boots are the Iron Boots' model in leather.
s16 EquipModel(u8 type, u8 value) {
    static const s16 kModels[4][3] = {
        { GID_SWORD_KOKIRI, kMasterSwordModel, GID_SWORD_BGS },
        { GID_SHIELD_DEKU, GID_SHIELD_HYLIAN, GID_SHIELD_MIRROR },
        { kKokiriTunicModel, GID_TUNIC_GORON, GID_TUNIC_ZORA },
        { kKokiriBootsModel, GID_BOOTS_IRON, GID_BOOTS_HOVER },
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

// Whether the game lets an item be used here and now, as it decides which C buttons to grey out (see
// Interface_UpdateButtonsPart2 in z_parameter.c): nothing on a ladder or while crawling, only the hookshot under
// water, and whatever the scene's own restrictions allow.
bool ItemAllowedHere(u8 item) {
    if (gPlayState == nullptr) {
        return true;
    }
    Player* player = GET_PLAYER(gPlayState);
    if ((player->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER) || (player->stateFlags2 & PLAYER_STATE2_CRAWLING)) {
        return false;
    }
    bool hookshot = item == ITEM_HOOKSHOT || item == ITEM_LONGSHOT;
    s32 hazard = Player_GetEnvironmentalHazard(gPlayState);
    if (hazard >= 2 && hazard < 5) {
        return hazard == 2 && hookshot;
    }
    const auto& restrictions = gPlayState->interfaceCtx.restrictions;
    bool ocarina = item == ITEM_OCARINA_FAIRY || item == ITEM_OCARINA_TIME;
    if (item >= ITEM_BOTTLE && item <= ITEM_POE) {
        return restrictions.bottles == 0;
    }
    if (item >= ITEM_WEIRD_EGG && item <= ITEM_CLAIM_CHECK) {
        return restrictions.tradeItems == 0 || !GameInteractor_Should(VB_DISABLE_TRADE_ITEM_BUTTON, true, item);
    }
    if (ocarina) {
        return restrictions.ocarina == 0;
    }
    if ((hookshot && restrictions.hookshot != 0) || (item == ITEM_FARORES_WIND && restrictions.farores != 0) ||
        ((item == ITEM_DINS_FIRE || item == ITEM_NAYRUS_LOVE) && restrictions.dinsNayrus != 0)) {
        return false;
    }
    if (restrictions.all != 0) {
        // The treasure chest shop lets the Lens of Truth through.
        return gPlayState->sceneNum == SCENE_TREASURE_BOX_SHOP && item == ITEM_LENS;
    }
    return true;
}

void AddItem(std::vector<Entry>& out, u8 item) {
    if (item < ITEM_NONE_FE && ItemUsableAtAge(item) && ItemAllowedHere(item)) {
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
        s16 model = entries[0].model;
        if (category == CAT_SYSTEM) {
            model = kShipModel;
        } else if (category == CAT_CONSUMABLES) {
            model = GID_BOTTLE;
        }
        Entry entry = { EntryKind::Category, model };
        entry.category = category;
        out.push_back(entry);
    }
}

std::vector<Entry> CategoryEntries(int category) {
    std::vector<Entry> out;
    switch (category) {
        case CAT_ROOT:
            for (int child : { CAT_RANGED, CAT_EXPLOSIVES, CAT_MAGIC, CAT_CONSUMABLES, CAT_QUEST, CAT_GEAR, CAT_SYSTEM }) {
                AddCategory(out, child);
            }
            break;
        case CAT_RANGED:
            // The slingshot beside the bow, and the boomerang beside the hookshot (or longshot).
            AddOwnedItem(out, ITEM_SLINGSHOT);
            AddOwnedItem(out, ITEM_BOW);
            AddOwnedItem(out, ITEM_BOOMERANG);
            AddItem(out, INV_CONTENT(ITEM_HOOKSHOT));
            break;
        case CAT_EXPLOSIVES:
            AddAmmoItem(out, ITEM_BOMB);
            AddAmmoItem(out, ITEM_BOMBCHU);
            AddOwnedItem(out, ITEM_HAMMER);
            AddAmmoItem(out, ITEM_STICK);
            break;
        case CAT_MAGIC:
            // The lens at the top, the magic arrows down the right and the spells up the left.
            AddOwnedItem(out, ITEM_LENS);
            if (INV_CONTENT(ITEM_BOW) == ITEM_BOW) {
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
            AddOwnedItem(out, ITEM_NAYRUS_LOVE);
            AddOwnedItem(out, ITEM_FARORES_WIND);
            AddOwnedItem(out, ITEM_DINS_FIRE);
            break;
        case CAT_CONSUMABLES:
            AddAmmoItem(out, ITEM_NUT);
            AddAmmoItem(out, ITEM_BEAN);
            for (int slot = SLOT_BOTTLE_1; slot <= SLOT_BOTTLE_4; slot++) {
                size_t before = out.size();
                AddItem(out, gSaveContext.inventory.items[slot]);
                if (out.size() > before) {
                    out.back().slot = static_cast<s8>(slot);
                }
            }
            break;
        case CAT_QUEST:
            AddItem(out, INV_CONTENT(ITEM_OCARINA_FAIRY));
            AddItem(out, gSaveContext.inventory.items[SLOT_TRADE_CHILD]);
            AddItem(out, gSaveContext.inventory.items[SLOT_TRADE_ADULT]);
            break;
        case CAT_GEAR:
            // Clockwise from the top: shields, swords, boots, tunics.
            for (int child : { CAT_SHIELDS, CAT_SWORDS, CAT_BOOTS, CAT_TUNICS }) {
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
        case CAT_SYSTEM: {
            // The pause menu (its map and quest pages), shown by a map, and the VR settings, by a compass.
            Entry pause = { EntryKind::Action, GID_DUNGEON_MAP };
            pause.action = ACTION_PAUSE;
            out.push_back(pause);
            Entry settings = { EntryKind::Action, GID_COMPASS };
            settings.action = ACTION_SETTINGS;
            out.push_back(settings);
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

void OpenPanel(bool onScreen);

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
        case EntryKind::Action: {
            bool onScreen = sMenu.onScreen;
            Close();
            if (entry.action == ACTION_PAUSE) {
                Game_PressStart();
            } else if (entry.action == ACTION_SETTINGS) {
                OpenPanel(onScreen);
            }
            break;
        }
    }
    Runtime::Get().Vibrate(sMenu.hand, 0.5f, 0.05f);
}

// The VR settings panel, opened from the System ring. It's drawn into the game's overlay with its debug text, on the
// 320x240 screen (the panel rect): in the world the HUD texture is shown on a panel in front of the player, and on
// the floating screen it's simply part of the picture. Either hand points at a row; pulling the trigger or
// squeezing the grip on it flips a switch, or lowers a number on the row's left half and raises it on its right
// half, again and again while held. Settings are saved when it closes.
constexpr int kPanelX = 60;
constexpr int kPanelY = 36;
constexpr int kPanelW = 200;
constexpr int kPanelH = 200;
constexpr int kPanelFirstRow = 62;     // y of the first row's text
constexpr int kPanelRowStep = 14;
constexpr float kPanelWidth = 0.55f;   // meters, in the world
constexpr float kPanelForward = 0.55f; // meters ahead of the head when it opens
constexpr float kPanelDrop = 0.15f;    // and below eye level
constexpr float kRepeatDelay = 0.4f;   // seconds a number row is held before it repeats
constexpr float kRepeatInterval = 0.08f;

enum class RowKind { Switch, Number, Choice, Recenter, Page, Close };

struct PanelRow {
    const char* label;
    RowKind kind;
    const char* cvar;
    int defaultValue;
    int min;
    int max;
    int step;
    const char* off;  // a switch's two settings
    const char* on;
    const char* unit; // after a number
    const char* zero; // shown instead of a number that's 0, if set
    const char* const* choices = nullptr; // a choice's names, from min to max
};

constexpr PanelRow SwitchRow(const char* label, const char* cvar, const char* off = "OFF", const char* on = "ON") {
    return { label, RowKind::Switch, cvar, 0, 0, 1, 1, off, on, "", nullptr };
}

constexpr PanelRow NumberRow(const char* label, const char* cvar, int defaultValue, int min, int max, int step,
                             const char* unit, const char* zero = nullptr) {
    return { label, RowKind::Number, cvar, defaultValue, min, max, step, "", "", unit, zero };
}

// A switch that's on unless turned off, like the menu's checkboxes for it.
constexpr PanelRow OnRow(const char* label, const char* cvar) {
    return { label, RowKind::Switch, cvar, 1, 0, 1, 1, "OFF", "ON", "", nullptr };
}

// One of a few named settings, stepped through by pointing at the row's left or right half.
constexpr PanelRow ChoiceRow(const char* label, const char* cvar, int count, const char* const* choices) {
    return { label, RowKind::Choice, cvar, 0, 0, count - 1, 1, "", "", "", nullptr, choices };
}

constexpr PanelRow ActionRow(const char* label, RowKind kind) {
    return { label, kind, nullptr, 0, 0, 0, 0, "", "", "", nullptr };
}

constexpr const char* kTitleViews[] = { "CAMERA", "LINK", "SCREEN" };

// Two pages: comfort and the view on the first, how things are played on the second, each with a row to the other.
const PanelRow kComfortRows[] = {
    SwitchRow("SEATED MODE", CVAR_SETTING("VR.Seated")),
    NumberRow("YOUR HEIGHT", CVAR_SETTING("VR.PlayerHeight"), 170, 100, 220, 1, " CM"),
    NumberRow("ADULT SCALE", CVAR_SETTING("VR.AdultWorldScale"), 100, 70, 150, 5, "%"),
    NumberRow("CHILD SCALE", CVAR_SETTING("VR.ChildWorldScale"), 100, 70, 150, 5, "%"),
    NumberRow("SNAP TURN", CVAR_SETTING("VR.SnapTurn"), 30, 0, 90, 15, " DEG", "SMOOTH"),
    SwitchRow("MOVEMENT", CVAR_SETTING("VR.HandMovement"), "HEAD", "HAND"),
    SwitchRow("SWORD HAND", CVAR_SETTING("VR.SwordInRightHand"), "LEFT", "RIGHT"),
    NumberRow("DRAW DISTANCE", CVAR_SETTING("VR.DrawDistance"), 3, 1, 5, 1, "X"),
    ChoiceRow("TITLE SCREEN", CVAR_SETTING("VR.TitleView"), 3, kTitleViews),
    ActionRow("RECENTER VIEW", RowKind::Recenter),
    ActionRow("MORE SETTINGS >", RowKind::Page),
    ActionRow("CLOSE", RowKind::Close),
};
const PanelRow kPlayRows[] = {
    OnRow("AIM MARKER", CVAR_SETTING("VR.ShotMarker")),
    SwitchRow("AIM PATH", CVAR_SETTING("VR.ShotPath")),
    SwitchRow("GRAB DROPS", CVAR_SETTING("VR.GrabDrops")),
    OnRow("WHISTLE NOTES", CVAR_SETTING("VR.MicOcarina")),
    OnRow("CLIMB BY HAND", CVAR_SETTING("VR.ClimbByHand")),
    SwitchRow("GRAB LADDERS", CVAR_SETTING("VR.GrabToClimb")),
    OnRow("ARMS TO RUN", CVAR_SETTING("VR.ArmRun")),
    OnRow("ARMS TO SWIM", CVAR_SETTING("VR.SwimStrokes")),
    OnRow("NOD TO ANSWER", CVAR_SETTING("VR.HeadAnswers")),
    OnRow("VR TUTORIALS", CVAR_SETTING("VR.Guidance")),
    ActionRow("MORE SETTINGS >", RowKind::Page),
    ActionRow("CLOSE", RowKind::Close),
};
const PanelRow kMoreRows[] = {
    OnRow("QUIVER", CVAR_SETTING("VR.Quiver")),
    OnRow("CLOSE TO WALLS", CVAR_SETTING("VR.CloseToWalls")),
    OnRow("HAPTICS", CVAR_SETTING("VR.Haptics")),
    OnRow("HEADPHONE MIX", CVAR_SETTING("VR.HeadphoneMix")),
    SwitchRow("LINK'S BODY", CVAR_SETTING("VR.ShowBody"), "HIDDEN", "SHOWN"),
    SwitchRow("THIRD PERSON", CVAR_SETTING("VR.ThirdPerson")),
    SwitchRow("STREAMER VIEW", CVAR_SETTING("VR.StreamerView")),
    NumberRow("TEXT SIZE", CVAR_SETTING("VR.TextSize"), 100, 50, 200, 10, "%"),
    NumberRow("TEXT OPACITY", CVAR_SETTING("VR.TextOpacity"), 100, 20, 100, 10, "%"),
    ActionRow("< FIRST PAGE", RowKind::Page),
    ActionRow("CLOSE", RowKind::Close),
};
constexpr int kPanelPageCount = 3;
const PanelRow* const kPanelPages[kPanelPageCount] = { kComfortRows, kPlayRows, kMoreRows };
constexpr int kPanelPageRows[kPanelPageCount] = { sizeof(kComfortRows) / sizeof(kComfortRows[0]),
                                                  sizeof(kPlayRows) / sizeof(kPlayRows[0]),
                                                  sizeof(kMoreRows) / sizeof(kMoreRows[0]) };

struct Panel {
    bool open = false;
    bool onScreen = false;
    bool changed = false;
    int page = 0;
    Pose pose;
    int hovered = -1;
    float cursor[2] = {};
    bool cursorShown = false;
    int handRow[HAND_COUNT] = { -1, -1 };
    float handX[HAND_COUNT] = {};
    int repeatHand = -1;
    float repeatTimer = 0.0f;
};

Panel sPanel;

void OpenPanel(bool onScreen) {
    sPanel = {};
    sPanel.open = true;
    sPanel.onScreen = onScreen;
    if (!onScreen) {
        sPanel.pose = Game_PanelInFront(kPanelForward, kPanelDrop);
        if (!sPanel.pose.valid) {
            sPanel.open = false;
        }
    }
}

void ClosePanel() {
    if (sPanel.changed) {
        CVarSave();
    }
    sPanel.open = false;
}

// Where a controller points on the panel, on the 320x240 screen, if it does.
bool PanelPoint(const ControllerState& state, float& x, float& y) {
    if (sPanel.onScreen) {
        Pose screen;
        float width, height;
        float meterX, meterY;
        if (!Runtime::Get().GetScreen(screen, width, height) || !PointOnScreen(state, meterX, meterY)) {
            return false;
        }
        // The game's 320x240 screen spans the picture's height, centered across its width.
        x = SCREEN_WIDTH * 0.5f + meterX / height * SCREEN_HEIGHT;
        y = SCREEN_HEIGHT * 0.5f - meterY / height * SCREEN_HEIGHT;
    } else {
        if (!state.aimPose.valid) {
            return false;
        }
        const float* q = state.aimPose.orientation;
        Vec3 aim = Normalize({ -2.0f * (q[0] * q[2] + q[3] * q[1]), -2.0f * (q[1] * q[2] - q[3] * q[0]),
                               -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1])) });
        // The panel only turns about the vertical, and faces the player along its +Z.
        float yaw = 2.0f * atan2f(sPanel.pose.orientation[1], sPanel.pose.orientation[3]);
        Vec3 normal = { sinf(yaw), 0.0f, cosf(yaw) };
        Vec3 right = { cosf(yaw), 0.0f, -sinf(yaw) };
        float facing = aim.x * normal.x + aim.y * normal.y + aim.z * normal.z;
        if (facing > -0.01f) {
            return false;
        }
        Vec3 toPanel = Sub(sPanel.pose.position, state.aimPose.position);
        float distance = (toPanel.x * normal.x + toPanel.y * normal.y + toPanel.z * normal.z) / facing;
        if (distance <= 0.0f) {
            return false;
        }
        float hit[3] = { state.aimPose.position[0] + aim.x * distance, state.aimPose.position[1] + aim.y * distance,
                         state.aimPose.position[2] + aim.z * distance };
        Vec3 fromMiddle = Sub(hit, sPanel.pose.position);
        float height = kPanelWidth * kPanelH / kPanelW;
        x = kPanelX + ((fromMiddle.x * right.x + fromMiddle.z * right.z) / kPanelWidth + 0.5f) * kPanelW;
        y = kPanelY + (0.5f - fromMiddle.y / height) * kPanelH;
    }
    return x >= kPanelX && x < kPanelX + kPanelW && y >= kPanelY && y < kPanelY + kPanelH;
}

// The rows of the page the panel is on.
const PanelRow* PageRows(int& count) {
    count = kPanelPageRows[sPanel.page];
    return kPanelPages[sPanel.page];
}

int PanelRowAt(float y) {
    int count;
    PageRows(count);
    for (int i = 0; i < count; i++) {
        float top = kPanelFirstRow + i * kPanelRowStep - 3.0f;
        if (y >= top && y < top + kPanelRowStep) {
            return i;
        }
    }
    return -1;
}

void ActivateRow(int row, float x, Hand hand) {
    int count;
    const PanelRow& r = PageRows(count)[row];
    switch (r.kind) {
        case RowKind::Switch:
            CVarSetInteger(r.cvar, CVarGetInteger(r.cvar, r.defaultValue) ? 0 : 1);
            sPanel.changed = true;
            break;
        case RowKind::Number: {
            int value = CVarGetInteger(r.cvar, r.defaultValue);
            value += (x < kPanelX + kPanelW * 0.5f) ? -r.step : r.step;
            CVarSetInteger(r.cvar, std::clamp(value, r.min, r.max));
            sPanel.changed = true;
            break;
        }
        case RowKind::Choice: {
            int value = CVarGetInteger(r.cvar, r.defaultValue);
            value += (x < kPanelX + kPanelW * 0.5f) ? -1 : 1;
            CVarSetInteger(r.cvar, (value + r.max + 1) % (r.max + 1));
            sPanel.changed = true;
            break;
        }
        case RowKind::Recenter:
            Game_Recenter();
            break;
        case RowKind::Page:
            sPanel.page = (sPanel.page + 1) % kPanelPageCount;
            sPanel.hovered = -1;
            sPanel.handRow[HAND_LEFT] = sPanel.handRow[HAND_RIGHT] = -1;
            sPanel.repeatHand = -1;
            break;
        case RowKind::Close:
            ClosePanel();
            break;
    }
    Runtime::Get().Vibrate(hand, 0.3f, 0.03f);
}

void UpdatePanel(bool onScreen, bool menuPressed, const bool choosePressed[HAND_COUNT]) {
    Runtime& runtime = Runtime::Get();
    const Pose& head = runtime.GetHead();
    if (sPanel.onScreen != onScreen || menuPressed || !head.valid ||
        (!onScreen && Length(Sub(head.position, sPanel.pose.position)) > kCloseDistance)) {
        ClosePanel();
        return;
    }
    sPanel.hovered = -1;
    sPanel.cursorShown = false;
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        float x = 0.0f;
        float y = 0.0f;
        int row = PanelPoint(state, x, y) ? PanelRowAt(y) : -1;
        if (row != sPanel.handRow[hand] && row >= 0) {
            runtime.Vibrate(static_cast<Hand>(hand), 0.15f, 0.02f);
        }
        sPanel.handRow[hand] = row;
        sPanel.handX[hand] = x;
        if (row >= 0 && sPanel.hovered < 0) {
            sPanel.hovered = row;
            sPanel.cursor[0] = x;
            sPanel.cursor[1] = y;
            sPanel.cursorShown = true;
        }
    }
    float frame = runtime.GetRefreshRate() > 0 ? 1.0f / runtime.GetRefreshRate() : 0.0f;
    for (int hand = 0; hand < HAND_COUNT && sPanel.open; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        int row = sPanel.handRow[hand];
        bool held = state.triggerHeld || state.gripHeld;
        if (choosePressed[hand] && row >= 0) {
            sPanel.repeatHand = hand;
            sPanel.repeatTimer = kRepeatDelay;
            ActivateRow(row, sPanel.handX[hand], static_cast<Hand>(hand));
        } else if (hand == sPanel.repeatHand && held && row >= 0 &&
                   kPanelPages[sPanel.page][row].kind == RowKind::Number) {
            sPanel.repeatTimer -= frame;
            if (sPanel.repeatTimer <= 0.0f) {
                sPanel.repeatTimer = kRepeatInterval;
                ActivateRow(row, sPanel.handX[hand], static_cast<Hand>(hand));
            }
        } else if (hand == sPanel.repeatHand && !held) {
            sPanel.repeatHand = -1;
        }
    }
}

// The bomb flower's bomb in VR: the shop's bomb model in the flower bomb's colors, a blue body, a green cap and a red
// sprout for a fuse (its display list sets its colors itself, so a copy has them changed, made from the game's data
// the first time it's drawn), with a ring of petals around the cap: spikes of its own, pale at the cap and yellow
// at their tips, long and short in turn. Model units, as the shop's bomb's: its cap is a ring of radius 12 from 17
// to 22 up, and its fuse runs from 22 to 30.
constexpr u32 kShopBombColorSwaps[][2] = {
    { 0x003C50FF, 0x4664E6FF }, // body, main: dark teal to blue
    { 0x001428FF, 0x0F1450FF }, // body, shadow
    { 0x32323CFF, 0x28501EFF }, // cap, shadow: grey to dark green (its main color, white, is shared with no other part)
    { 0xFFC85AFF, 0xF04628FF }, // fuse, main: gold to red
    { 0x643C1EFF, 0x781A0AFF }, // fuse, shadow
};
constexpr u32 kShopBombCapPrim = 0xFFFFFFFF;
constexpr u32 kBombFlowerCapPrim = 0x96C850FF;
constexpr int kPetalCount = 16;
constexpr float kPetalBaseRadius = 11.5f;
constexpr float kPetalBaseHeight = 17.5f;
constexpr float kPetalHalfWidth = 2.6f;
std::vector<Gfx> sBombFlowerDL;
Vtx sPetals[kPetalCount * 3];

void BuildBombFlower() {
    auto bomb = std::static_pointer_cast<Fast::DisplayList>(ResourceMgr_GetResourceByNameHandlingMQ(gGiBombDL));
    if (bomb == nullptr) {
        return;
    }
    sBombFlowerDL = bomb->Instructions;
    bool capColored = false;
    for (Gfx& command : sBombFlowerDL) {
        u32 opcode = static_cast<u32>(command.words.w0 >> 24) & 0xFF;
        if (opcode != G_SETPRIMCOLOR && opcode != G_SETENVCOLOR) {
            continue;
        }
        u32 color = static_cast<u32>(command.words.w1);
        // The cap's main color is the first white the list sets; the body's comes before it.
        if (opcode == G_SETPRIMCOLOR && color == kShopBombCapPrim && !capColored) {
            command.words.w1 = kBombFlowerCapPrim;
            capColored = true;
            continue;
        }
        for (const auto& swap : kShopBombColorSwaps) {
            if (color == swap[0]) {
                command.words.w1 = swap[1];
                break;
            }
        }
    }
    for (int i = 0; i < kPetalCount; i++) {
        float angle = i * 2.0f * static_cast<float>(M_PI) / kPetalCount;
        float c = cosf(angle);
        float s = sinf(angle);
        bool longPetal = (i % 2) == 0;
        float tipRadius = longPetal ? 25.0f : 20.0f;
        float tipHeight = longPetal ? 21.0f : 19.5f;
        Vtx* v = &sPetals[i * 3];
        auto set = [](Vtx& vtx, float x, float y, float z, u8 r, u8 g, u8 b) {
            vtx.v.ob[0] = static_cast<s16>(lroundf(x));
            vtx.v.ob[1] = static_cast<s16>(lroundf(y));
            vtx.v.ob[2] = static_cast<s16>(lroundf(z));
            vtx.v.flag = 0;
            vtx.v.tc[0] = vtx.v.tc[1] = 0;
            vtx.v.cn[0] = r;
            vtx.v.cn[1] = g;
            vtx.v.cn[2] = b;
            vtx.v.cn[3] = 255;
        };
        set(v[0], kPetalBaseRadius * c - kPetalHalfWidth * s, kPetalBaseHeight,
            kPetalBaseRadius * s + kPetalHalfWidth * c, 245, 240, 215);
        set(v[1], kPetalBaseRadius * c + kPetalHalfWidth * s, kPetalBaseHeight,
            kPetalBaseRadius * s - kPetalHalfWidth * c, 245, 240, 215);
        set(v[2], tipRadius * c, tipHeight, tipRadius * s, 235, 175, 45);
    }
}

} // namespace

// Draws the bomb flower's bomb at the current matrix, in the shop's bomb's model units.
void DrawBombFlowerBomb(PlayState* play) {
    if (sBombFlowerDL.empty()) {
        BuildBombFlower();
        if (sBombFlowerDL.empty()) {
            return;
        }
    }
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Gfx_SetupDL_25Opa(gfxCtx);
    Gfx*& opa = gfxCtx->polyOpa.p;
    gSPMatrix(opa++, MATRIX_NEWMTX(gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    gSPDisplayList(opa++, sBombFlowerDL.data());

    // The petals: plain colors, seen from both sides.
    gDPPipeSync(opa++);
    gSPTexture(opa++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(opa++, G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN);
    gDPSetCombineMode(opa++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(opa++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gSPMatrix(opa++, MATRIX_NEWMTX(gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    constexpr int kPetalsPerLoad = 8;
    for (int first = 0; first < kPetalCount; first += kPetalsPerLoad) {
        gSPVertex(opa++, reinterpret_cast<uintptr_t>(&sPetals[first * 3]), kPetalsPerLoad * 3, 0);
        for (int k = 0; k < kPetalsPerLoad; k++) {
            gSP1Triangle(opa++, k * 3, k * 3 + 1, k * 3 + 2, 0);
        }
    }
}

namespace {

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
    if (sPanel.open) {
        ClosePanel();
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
    bool menuButton = Controls_ActionHeld(VR_ACTION_WHEEL);
    bool menuPressed = menuButton && !sMenuButtonLatched && !Game_PlayingOcarina();
    sMenuButtonLatched = menuButton;

    if (sPanel.open) {
        UpdatePanel(onScreen, menuPressed, choosePressed);
        return;
    }

    // Switching between the world and the screen closes it, since it was laid out for the other.
    if (sMenu.open && sMenu.onScreen != onScreen) {
        Close();
    }
    if (!sMenu.open) {
        Player* player = GET_PLAYER(gPlayState);
        bool carrying = (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) != 0;
        if (menuPressed && !carrying && !Game_DrawingShot()) {
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

// The Iron Boots' item-get model as plain leather boots: its display list sets the color of its iron (blue-grey)
// and of its leather (browns) itself, so a copy has the iron's color changed to its dark leather's, and its rivets,
// a display list of their own, are left out. The copy is made from the game's data the first time it's drawn.
constexpr u32 kIronBootsIronPrim = 0x8CAAAAFF;
constexpr u32 kIronBootsIronEnv = 0x141E1EFF;
constexpr u32 kIronBootsDarkLeatherPrim = 0x502800FF;
constexpr u32 kIronBootsDarkLeatherEnv = 0x281400FF;
std::vector<Gfx> sKokiriBootsDL;

void DrawKokiriBoots(PlayState* play) {
    if (sKokiriBootsDL.empty()) {
        auto boots = std::static_pointer_cast<Fast::DisplayList>(ResourceMgr_GetResourceByNameHandlingMQ(gGiIronBootsDL));
        if (boots == nullptr) {
            return;
        }
        sKokiriBootsDL = boots->Instructions;
        for (Gfx& command : sKokiriBootsDL) {
            u32 opcode = static_cast<u32>(command.words.w0 >> 24) & 0xFF;
            u32 color = static_cast<u32>(command.words.w1);
            if (opcode == G_SETPRIMCOLOR && color == kIronBootsIronPrim) {
                command.words.w1 = kIronBootsDarkLeatherPrim;
            } else if (opcode == G_SETENVCOLOR && color == kIronBootsIronEnv) {
                command.words.w1 = kIronBootsDarkLeatherEnv;
            }
        }
    }
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Gfx_SetupDL_25Opa(gfxCtx);
    Gfx*& opa = gfxCtx->polyOpa.p;
    gSPMatrix(opa++, MATRIX_NEWMTX(gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    gSPDisplayList(opa++, sKokiriBootsDL.data());
}

// The System ring's model: the Shadow Temple's ship, its hull as its actor draws it, about 10000 units long, shrunk to
// an item's size and centered on its middle.
constexpr float kShipScale = kItemModelSize / 10064.0f;
constexpr Vec3f kShipMiddle = { -1432.0f, 2796.0f, 0.0f };

void DrawShip(PlayState* play) {
    Matrix_Scale(kShipScale, kShipScale, kShipScale, MTXMODE_APPLY);
    Matrix_Translate(-kShipMiddle.x, -kShipMiddle.y, -kShipMiddle.z, MTXMODE_APPLY);
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Gfx_SetupDL_25Opa(gfxCtx);
    Gfx*& opa = gfxCtx->polyOpa.p;
    gSPMatrix(opa++, MATRIX_NEWMTX(gfxCtx), G_MTX_MODELVIEW | G_MTX_LOAD);
    gSPDisplayList(opa++, (Gfx*)object_haka_objects_DL_00D330);
}

void DrawModel(PlayState* play, s16 model) {
    if (model == kMasterSwordModel) {
        Randomizer_DrawMasterSword(play, nullptr);
    } else if (model == kKokiriTunicModel) {
        DrawKokiriTunic(play);
    } else if (model == kKokiriBootsModel) {
        DrawKokiriBoots(play);
    } else if (model == kShipModel) {
        DrawShip(play);
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

// A box on the 320x240 screen, see-through as its alpha says.
void DrawBox(Gfx*& overlay, int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b, u8 a) {
    gDPPipeSync(overlay++);
    gDPSetCycleType(overlay++, G_CYC_1CYCLE);
    gDPSetRenderMode(overlay++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(overlay++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(overlay++, 0, 0, r, g, b, a);
    gDPFillRectangle(overlay++, x0, y0, x1, y1);
}

void DrawPanel(PlayState* play) {
    Gfx*& overlay = play->state.gfxCtx->overlay.p;
    DrawBox(overlay, kPanelX, kPanelY, kPanelX + kPanelW, kPanelY + kPanelH, 10, 15, 30, 215);
    if (sPanel.hovered >= 0) {
        int top = kPanelFirstRow + sPanel.hovered * kPanelRowStep - 3;
        DrawBox(overlay, kPanelX + 4, top, kPanelX + kPanelW - 4, top + kPanelRowStep - 1, 60, 100, 170, 230);
    }

    GfxPrint printer;
    GfxPrint_Init(&printer);
    GfxPrint_Open(&printer, overlay);
    GfxPrint_SetColor(&printer, 255, 230, 120, 255);
    const char* const titles[kPanelPageCount] = { "VR SETTINGS", "VR SETTINGS 2", "VR SETTINGS 3" };
    const char* title = titles[sPanel.page];
    GfxPrint_SetPosPx(&printer, kPanelX + (kPanelW - static_cast<int>(strlen(title)) * 8) / 2, kPanelY + 8);
    GfxPrint_Printf(&printer, "%s", title);
    int rowCount;
    const PanelRow* rows = PageRows(rowCount);
    for (int i = 0; i < rowCount; i++) {
        const PanelRow& row = rows[i];
        int y = kPanelFirstRow + i * kPanelRowStep;
        GfxPrint_SetColor(&printer, 255, 255, 255, 255);
        GfxPrint_SetPosPx(&printer, kPanelX + 10, y);
        GfxPrint_Printf(&printer, "%s", row.label);
        char value[16] = "";
        if (row.kind == RowKind::Switch) {
            snprintf(value, sizeof(value), "%s", CVarGetInteger(row.cvar, row.defaultValue) ? row.on : row.off);
        } else if (row.kind == RowKind::Choice) {
            int choice = std::clamp(CVarGetInteger(row.cvar, row.defaultValue), row.min, row.max);
            snprintf(value, sizeof(value), "%s", row.choices[choice]);
        } else if (row.kind == RowKind::Number) {
            int number = CVarGetInteger(row.cvar, row.defaultValue);
            if (number == 0 && row.zero != nullptr) {
                snprintf(value, sizeof(value), "%s", row.zero);
            } else {
                snprintf(value, sizeof(value), "%d%s", number, row.unit);
            }
            // Arrows at the row's two ends, where pointing lowers or raises it.
            GfxPrint_SetColor(&printer, 150, 230, 255, 255);
            GfxPrint_SetPosPx(&printer, kPanelX + 112, y);
            GfxPrint_Printf(&printer, "<");
            GfxPrint_SetPosPx(&printer, kPanelX + kPanelW - 16, y);
            GfxPrint_Printf(&printer, ">");
        }
        if (value[0] != '\0') {
            int right = kPanelX + kPanelW - ((row.kind == RowKind::Number) ? 20 : 10);
            GfxPrint_SetColor(&printer, 150, 230, 255, 255);
            GfxPrint_SetPosPx(&printer, right - 8 * static_cast<int>(strlen(value)), y);
            GfxPrint_Printf(&printer, "%s", value);
        }
    }
    overlay = GfxPrint_Close(&printer);
    GfxPrint_Destroy(&printer);

    if (sPanel.cursorShown) {
        int x = static_cast<int>(sPanel.cursor[0]);
        int y = static_cast<int>(sPanel.cursor[1]);
        DrawBox(overlay, x - 2, y - 2, x + 2, y + 2, 255, 220, 60, 255);
    }
}

void Items_Draw(PlayState* play) {
    if (sPanel.open) {
        DrawPanel(play);
    }
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
    return sMenu.open || sPanel.open;
}

bool Items_PanelShown(Pose& pose, float& width, int rect[4]) {
    if (!sPanel.open || sPanel.onScreen) {
        return false;
    }
    pose = sPanel.pose;
    width = kPanelWidth;
    rect[0] = kPanelX;
    rect[1] = kPanelY;
    rect[2] = kPanelW;
    rect[3] = kPanelH;
    return true;
}

s16 Items_ItemModel(u8 item) {
    return ItemModel(item);
}

} // namespace SohVr

extern "C" void Vr_DrawBombFlowerBomb(PlayState* play) {
    SohVr::DrawBombFlowerBomb(play);
}
