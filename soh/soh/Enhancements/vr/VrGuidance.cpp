// The game's tutorials teach its buttons. In VR most of what they teach is done by hand, so while the world is in
// stereo, the ones that matter are replaced by what to do in the headset instead: the item lessons, the Kokiri
// Forest signs and lessons, Navi's first words, the shields, the ocarina, the horse and the fishing pond. On the
// floating screen, where the controller is a normal gamepad, the game's own text stays.

#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Enhancements/custom-message/CustomMessageTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

#include <libultraship/bridge/consolevariablebridge.h>

#include <unordered_map>

extern "C" {
#include "global.h"
#include "soh/Enhancements/vr/Vr.h"
}

#define CVAR_VR_ENABLED CVAR_SETTING("VR.Enabled")
// Whether VR replaces the game's button lessons with its own (see above).
#define CVAR_VR_GUIDANCE CVAR_SETTING("VR.Guidance")

namespace {

struct Guide {
    ItemID icon; // the item shown with an item lesson, or ITEM_NONE
    const char* text;
};

// "&" breaks a line and "^" starts a new box; lines are wrapped to fit. %y marks what to do, %r names an item.
const std::unordered_map<uint16_t, Guide> kGuides = {
    // Items.
    { 0x0030, { ITEM_SLINGSHOT, "You found the %rFairy Slingshot%w!^Take it from the %yitem wheel (Y)%w. Bring your "
                                "other hand to it, %ypull its trigger, draw back and let go%w. Pull lower to shoot "
                                "higher. A ring shows where the seed will land." } },
    { 0x0031, { ITEM_BOW, "You found the %rFairy Bow%w!^Take it from the %yitem wheel (Y)%w. Bring your other hand to "
                          "the string, %ypull its trigger, draw back and let go%w. The further you draw, the further "
                          "the arrow flies." } },
    { 0x0032, { ITEM_BOMB, "You got %rBombs%w!^Take one from the %yitem wheel (Y)%w and %yswing your hand and let "
                           "go%w to throw it, or let go slowly to set it down. If you see something suspicious, bomb "
                           "it!" } },
    { 0x0033, { ITEM_BOMBCHU, "You got %rBombchus%w!^Take one from the %yitem wheel (Y)%w and %yset it down facing "
                              "where it should go%w. It runs along the ground, and even up walls!" } },
    { 0x0034, { ITEM_NUT, "You got a %rDeku Nut%w!^Take it from the %yitem wheel (Y)%w and %ythrow it%w. It will "
                          "flash and stun the enemy!" } },
    { 0x0035, { ITEM_BOOMERANG, "You found the %rBoomerang%w!^Take it from the %yitem wheel (Y)%w, %yswing and let "
                                "go of the grip%w to throw it, and reach out to catch it on its way back." } },
    { 0x0036, { ITEM_HOOKSHOT, "You found the %rHookshot%w!^It's a spring-loaded chain that drags distant things to "
                               "you, or pulls you to them.^Take it from the %yitem wheel (Y)%w, point it, %yhold the "
                               "trigger to aim and let go to fire%w. Its laser turns green where it will catch." } },
    { 0x0037, { ITEM_STICK, "You got a %rDeku Stick%w!^Take it from the %yitem wheel (Y)%w and %yswing it%w. Close "
                            "your other hand on it to hold it in both. Swing it into a wall and it breaks, so don't "
                            "waste them." } },
    { 0x0038, { ITEM_HAMMER, "You found the %rMegaton Hammer%w!^%ySwing it%w to smash junk, with your other hand on "
                             "the handle too. Bring it down on the ground to shake it!" } },
    { 0x0039, { ITEM_LENS, "You found the %rLens of Truth%w!^Take it from the %yitem wheel (Y)%w and %yhold it up to "
                           "your eye%w to see what's hidden. It spends magic only while it's there." } },
    { 0x0042, { ITEM_BOTTLE, "You got an %rEmpty Bottle%w!^Take it from the %yitem wheel (Y)%w and %yswing it "
                             "through%w a bug, a fish, a fairy or blue fire to catch it." } },
    { 0x0043, { ITEM_POTION_RED, "You got a %rRed Potion%w!^Take it from the %yitem wheel (Y)%w and %ytip it at your "
                                 "mouth%w to drink it and recover your life energy." } },
    { 0x0044, { ITEM_POTION_GREEN, "You got a %rGreen Potion%w!^Take it from the %yitem wheel (Y)%w and %ytip it at "
                                   "your mouth%w to drink it and recover your magic power." } },
    { 0x0045, { ITEM_POTION_BLUE, "You got a %rBlue Potion%w!^%yTip it at your mouth%w to recover your life energy "
                                  "and magic power." } },
    { 0x0046, { ITEM_FAIRY, "You caught a %rFairy%w in a bottle!^She will revive you the moment you run out of life "
                            "energy. Or %ytip the bottle%w to ask for her help any time." } },
    { 0x0047, { ITEM_FISH, "You got a %rFish%w!^Take its bottle from the %yitem wheel (Y)%w and %ytip it%w somewhere "
                           "and something might happen... It looks so fresh and delicious!" } },
    { 0x0048, { ITEM_BEAN, "You got a %rMagic Bean%w!^Find a soft patch of soil, take it from the %yitem wheel "
                           "(Y)%w and plant it there. Then wait for something fun to happen!" } },
    { 0x004A, { ITEM_OCARINA_FAIRY, "You received the %rFairy Ocarina%w!^This is a memento from Saria. Take it from "
                                    "the %yitem wheel (Y)%w.^%yWhistle, hum or sing%w its notes, or play them with %yA "
                                    "and the right stick%w. %yB%w puts it away." } },
    { 0x004B, { ITEM_SWORD_KNIFE, "You got the %rGiant's Knife%w!^%yHold it with both hands%w and swing! It's so "
                                  "long, you can't use it with a shield." } },
    { 0x004C, { ITEM_SHIELD_DEKU, "You got a %rDeku Shield%w!^Change shields from the %yGear%w group of the %yitem "
                                  "wheel (Y)%w. %yHold it where the attack is coming from%w to block it." } },
    { 0x004D, { ITEM_SHIELD_HYLIAN, "You got a %rHylian Shield%w!^Change shields from the %yGear%w group of the "
                                    "%yitem wheel (Y)%w. %yHold it up where the attack is coming from%w. As a child, "
                                    "crouch behind it." } },
    { 0x004E, { ITEM_SHIELD_MIRROR, "You found the %rMirror Shield%w!^Its polished surface reflects light and energy. "
                                    "%yHold it up and turn it%w to send the light where you want it." } },
    { 0x0058, { ITEM_BOMB_BAG_20, "You found a %rBomb Bag%w!^You found 20 Bombs inside! Take them from the %yitem "
                                  "wheel (Y)%w and blow stuff up! What a lucky guy!" } },
    { 0x005C, { ITEM_GAUNTLETS_GOLD, "You found the %rGolden Gauntlets%w!^You can feel even more power coursing "
                                     "through your arms! %yGrab things with both hands%w and lift them up!" } },
    { 0x005D, { ITEM_BLUE_FIRE, "You put a %rBlue Fire%w into the bottle!^It's a cool flame. %yTip the bottle%w to "
                                "pour it out." } },
    { 0x0066, { ITEM_DUNGEON_MAP, "You found the %rDungeon Map%w!^Pause with %ythe left stick click%w, or the "
                                  "%yship%w on the %yitem wheel (Y)%w, and look at the Map.^Blue chambers are places "
                                  "you have already visited. Your current location is the flashing room." } },
    { 0x0069, { ITEM_LETTER_ZELDA, "You received %rZelda's Letter%w!^Wow! This letter has Princess Zelda's autograph! "
                                   "Take it from the %yitem wheel (Y)%w and %ypull the trigger%w to show it to "
                                   "people." } },
    { 0x0070, { ITEM_ARROW_FIRE, "You got the %rFire Arrow%w!^Take it from the %yMagic%w group of the %yitem wheel "
                                 "(Y)%w and your arrows will catch fire!" } },
    { 0x0071, { ITEM_ARROW_ICE, "You got the %rIce Arrow%w!^Take it from the %yMagic%w group of the %yitem wheel "
                                "(Y)%w and your arrows will freeze their target!" } },
    { 0x0072, { ITEM_ARROW_LIGHT, "You got the %rLight Arrow%w!^Take it from the %yMagic%w group of the %yitem wheel "
                                  "(Y)%w. The light of justice will smite evil!" } },
    { 0x0079, { ITEM_BRACELET, "You got the %rGoron's Bracelet%w!^Now you can pull up Bomb Flowers. %yClose a hand "
                               "on one%w to pull it up!" } },
    { 0x007A, { ITEM_BUG, "You put a %rBug%w in the bottle!^%yTip the bottle%w to let it go. This kind of bug prefers "
                          "to live in small holes in the ground." } },
    { 0x0098, { ITEM_MILK_BOTTLE, "You got %rLon Lon Milk%w!^This milk is very nutritious! %yTip it at your mouth%w "
                                  "to drink it: five hearts a drink, two drinks a bottle." } },
    { 0x009A, { ITEM_WEIRD_EGG, "You got a %rWeird Egg%w!^Feels like there's something moving inside! Take it from "
                                "the %yitem wheel (Y)%w and see what happens!" } },
    { 0x00A4, { ITEM_SWORD_KOKIRI, "You got the %rKokiri Sword%w!^%yReach over your shoulder and squeeze the grip%w "
                                   "to draw it, and swing! Do the same to put it away.^This is a hidden treasure of "
                                   "the Kokiri, but you can borrow it for a while. Be sure to practice with it!" } },
    { 0x00AD, { ITEM_DINS_FIRE, "You got %rDin's Fire%w!^Its fireball engulfs everything! Take it from the %yitem "
                                "wheel (Y)%w and %yhold both triggers%w to cast it." } },
    { 0x00AE, { ITEM_FARORES_WIND, "You got %rFarore's Wind%w!^Warp magic: take it from the %yitem wheel (Y)%w and "
                                   "%yhold both triggers%w to cast it. The first time, it makes a Warp Point; "
                                   "after that, you can warp to it or dispel it." } },
    { 0x00AF, { ITEM_NAYRUS_LOVE, "You got %rNayru's Love%w!^A protective barrier: take it from the %yitem wheel "
                                  "(Y)%w and %yhold both triggers%w to cast it." } },
    { 0x00CD, { ITEM_SCALE_SILVER, "You got the %rSilver Scale%w!^Jump into the water and press %yA%w. You can dive "
                                   "deeper than you could before." } },
    { 0x00CE, { ITEM_SCALE_GOLDEN, "You got the %rGolden Scale%w!^Jump into the water and press %yA%w. Now you can "
                                   "dive much deeper than you could before!" } },
    { 0x00DD, { ITEM_NONE, "You mastered the secret sword technique of the %rSpin Attack%w!^%yHold your sword "
                           "hand's trigger%w to charge, and %ylet go%w to spin!^Or %yturn around for real%w with your "
                           "sword held out for a quick spin!" } },
    { 0x70A3, { ITEM_NONE, "Take a mask from the %yitem wheel (Y)%w and %yhold it to your face%w to wear it. Close a "
                           "hand at your face to take it off.^If you talk to someone with a mask on, you might get a "
                           "reaction.^Somewhere in the world, somebody is waiting for these masks.^Just have "
                           "faith..." } },

    // Kokiri Forest, the Great Deku Tree and Navi.
    { 0x0103, { ITEM_NONE, "%yClose a hand on a door%w and push or pull to open it. %yA%w works too, when the Action "
                           "Icon shows you can." } },
    { 0x0107, { ITEM_NONE, "From here on, we'll be going through some narrow passages! If you take it slow, maybe "
                           "you can sneak up on some enemies.^Lean and look around corners before you turn them, and "
                           "keep your shield up!" } },
    { 0x0108, { ITEM_NONE, "%yPut both hands on this block and squeeze both grips%w. Then push the stick while "
                           "looking at it to push it, or while looking away to pull it.^To climb on top, %ygrab its "
                           "edge with both hands and pull%w." } },
    { 0x010C, { ITEM_NONE, "After you get into the water, press %yA%w to dive! I bet there are some interesting "
                           "things underwater!" } },
    { 0x0119, { ITEM_NONE, "You never know what will be around the corner in these narrow paths....^Look before you "
                           "step, and keep your shield handy!" } },
    { 0x0218, { ITEM_NONE, "Hi, @! Look this way!^Look at me and press %yA%w to talk to me." } },
    { 0x0336, { ITEM_NONE, "Thrust Attack Signs!^%yThrust your sword%w straight at your target to stab it!" } },
    { 0x0337, { ITEM_NONE, "Hole of \"Z\"&Let's go through this small hole!^%yGet down%w in front of the hole to crawl "
                           "into it." } },
    { 0x0338, { ITEM_NONE, "Cut Grass With Your Sword&%ySwing sideways%w to cut sideways, and %yoverhead%w to cut "
                           "down. Swing hard for a heavy blow!" } },
    { 0x0340, { ITEM_NONE, "Look Around&Your eyes are Link's: %yturn your head%w to look around, and push the right "
                           "stick sideways to turn.^%yClick the right stick%w to recenter yourself." } },
    { 0x0343, { ITEM_NONE, "Keep Your Eyes Open&Look at what you want to deal with: there's no need to lock on in "
                           "VR.^When Navi has something to say about what you're looking at, %ypush the right stick "
                           "down%w to listen." } },
    { 0x0401, { ITEM_NONE, "They say you can swim faster by pulling both arms back, %ybreaststroke style%w." } },
    { 0x1004, { ITEM_NONE, "%yClose a hand on a stone%w to pick it up!^Mean old Mido...he made me pick up the rocks "
                           "in front of his house." } },
    { 0x1007, { ITEM_NONE, "Hey, let's work on some moves!^Press %yA%w while you move to roll. While you roll, you "
                           "can avoid damage.^With your sword out, %yswing it overhead as hard as you can%w for a "
                           "heavy blow that does double damage! Try it on the stone next to me!" } },
    { 0x100D, { ITEM_NONE, "When a fairy flies near a person or thing, take a look at what she's found.^Look at "
                           "someone and press %yA%w to talk to them." } },
    { 0x1017, { ITEM_NONE, "Then enter, brave @, and thou too, Navi...^Navi the fairy... Thou must aid @...^And @... "
                           "When Navi speaks, push the %yright stick down%w to listen well to her words of "
                           "wisdom..." } },
    { 0x1018, { ITEM_NONE, "Perhaps you do not yet have confidence in your abilities... Return to the Kokiri Forest "
                           "to learn the skills of combat...^When thou art ready, return here and look upon me to "
                           "speak with me..." } },
    { 0x101A, { ITEM_NONE, "Ow-ow-ow! Forgive me, master! If I give you a clue, will you let me go?^When you jump off "
                           "a high cliff, hold the stick forward and you will roll when you land and won't get hurt "
                           "from the fall.^I can't guarantee it will work, though, if the cliff is really, really "
                           "high, heh heh!" } },
    { 0x1025, { ITEM_NONE, "You have a cool Slingshot, huh?^You know what's cool about it? The lower you pull the "
                           "pouch, the higher the seed flies! How cool!" } },
    { 0x1036, { ITEM_NONE, "Your things are on the %yitem wheel%w: press %yY%w and it opens in front of you.^Reach out "
                           "and touch something, or point at it, and squeeze the grip or pull the trigger. It goes "
                           "into that hand!" } },
    { 0x1037, { ITEM_NONE, "The icon on your right wrist is the Action Icon.^It shows what %yA%w will do right now. A "
                           "lot of what it shows, you can also do with your hands!" } },
    { 0x1039, { ITEM_NONE, "Your hearts, magic and rupees are on the back of your %yleft wrist%w.^If you want to see "
                           "the map, pause with %ythe left stick click%w, or the %yship%w on the %yitem wheel (Y)%w, "
                           "and look at the Map." } },
    { 0x103A, { ITEM_NONE, "Everything you have is on the %yitem wheel (Y)%w: weapons, things to use, and in its "
                           "%yGear%w group, your swords, shields, tunics and boots.^Take something and it goes into "
                           "the hand that took it.^To check your quest items or the map, pause with %ythe left stick "
                           "click%w. Did you understand everything?" } },
    { 0x103B, { ITEM_NONE, "Well, just remember this. If you want to save, pause with %ythe left stick click%w and "
                           "press %yB%w." } },
    { 0x103D, { ITEM_NONE, "This shop...It sells things you can get in the forest for free! Tee hee!^Do you know how "
                           "to use the Deku Shield? Tee hee!^Change shields from the %yGear%w group of the %yitem "
                           "wheel (Y)%w, then %yhold it up where the attack is coming from%w. Tee hee!" } },
    { 0x103F, { ITEM_NONE, "When Navi has something to tell you, push the %yright stick down%w to listen to her!^And "
                           "to look around, just look around!" } },
    { 0x10C5, { ITEM_NONE, "Where a melody is needed, take out the Ocarina and a musical staff will appear.^Whistle "
                           "or sing a song you know. I also suggest that you play even when a score is not "
                           "displayed. Just like this: Hoo hoo hoo hoot hoot hoot!" } },
    { 0x10D8, { ITEM_NONE, "Look! Isn't this view pretty? Have a good look around the forest!" } },
    { 0x5056, { ITEM_NONE, "To get a good view, just look around!" } },
    { 0x7004, { ITEM_NONE, "Man, I am SO bored... Things would sure be more interesting if there were more... "
                           "troubles in the world...^Hey, that's just between you and me, OK?^Anyway, maybe we can "
                           "let off some steam by breaking these jars.^%yClose a hand on a jar%w to lift it, and "
                           "%yswing and let go%w to toss it!" } },
    { 0x302B, { ITEM_NONE, "%yClose a hand on a Bomb Flower%w to pick it, and %ythrow it%w, or let go slowly to set "
                           "it down." } },
    { 0x3022, { ITEM_NONE, "I know a trick to conserve sticks! If you light a stick on fire, it will burn to ashes. "
                           "Put it away before it completely burns!^By the way, I hid a stick somewhere... "
                           "Hehehee..." } },
    { 0x406A, { ITEM_NONE, "Take a bean from the %yitem wheel (Y)%w and plant it in the dirt right here." } },
    { 0x2044, { ITEM_NONE, "Keep the egg with you and see what happens. Hee hee!^My dad is really troublesome, for "
                           "an adult, isn't he?! Hee hee!" } },
    { 0x2035, { ITEM_NONE, "Approach the horse quietly and stand by its side. Press %yA%w to mount up.^Use the stick "
                           "to make it run. Once it's running, press %yA%w to tap it with the whip and speed up!^To "
                           "jump a fence, speed up and hit it head on!^When it has come to a complete stop, press "
                           "%yA%w to dismount. Now, have fun!" } },
};

void RegisterVrGuidance() {
    bool enabled = CVarGetInteger(CVAR_VR_ENABLED, 0) && CVarGetInteger(CVAR_VR_GUIDANCE, 1);
    COND_HOOK(OnOpenText, enabled, [](uint16_t* textId, bool* loadFromMessageTable) {
        if (!Vr_IsWorldStereo()) {
            return;
        }
        auto guide = kGuides.find(*textId);
        if (guide == kGuides.end()) {
            return;
        }
        // The game's own box: its kind and where it goes.
        CustomMessage vanilla = CustomMessage::LoadVanillaMessageTableEntry(*textId);
        CustomMessage message(guide->second.text, vanilla.GetTextBoxType(), vanilla.GetTextBoxPosition());
        if (guide->second.icon != ITEM_NONE) {
            message.AutoFormat(guide->second.icon);
        } else {
            message.AutoFormat();
        }
        message.LoadIntoFont();
        *loadFromMessageTable = false;
    });
}

static RegisterShipInitFunc initFunc(RegisterVrGuidance, { CVAR_VR_ENABLED, CVAR_VR_GUIDANCE });

} // namespace
