# Ship of Harkinian VR

An experimental six-degrees-of-freedom VR mode for Ship of Harkinian. You stand in the world at Link's eye height. Your hands are Link's hands, and the sword, shield, bow, hookshot and other items are used with your body instead of buttons.

This is an unofficial fork. It isn't made or supported by HarbourMasters, so please don't ask for help with it in the Ship of Harkinian support channels.

## You bring the game

Like Ship of Harkinian, this repository contains no copyrighted game assets. To play, you need your own legally dumped copy of the game in a version Ship of Harkinian supports. You can check a dump with the compatibility checker at https://ship.equipment/ or against the hashes in [supportedHashes.json](supportedHashes.json). Never upload a ROM, an `oot.o2r` or `oot-mq.o2r` file, or anything extracted from them.

## What you need

* Windows 10 or 11. VR uses the DirectX 11 renderer and isn't available on Linux or macOS.
* A PC VR headset with an OpenXR runtime. It has been played on a Quest 3 through SteamVR and Quest Link; other OpenXR runtimes (Virtual Desktop, Windows Mixed Reality) should work but haven't been tried.
* Motion controllers with a thumbstick, a trigger, a grip and two face buttons on each hand, as on Quest Touch or Index controllers.
* Everything listed under Windows in [BUILDING.md](BUILDING.md).

## Building

Follow the Windows steps in [BUILDING.md](BUILDING.md); nothing extra is needed. The OpenXR loader is installed through vcpkg along with SoH's other dependencies, the first time CMake configures the project. Use the Release configuration (`--config Release`) for playing, since the debug build is too slow for VR.

On first launch, give the game your ROM when it asks, the same as in regular Ship of Harkinian. It makes `oot.o2r` from it.

## Turning VR on

1. Start your headset's runtime (SteamVR, or the Quest Link or Air Link connection).
2. Launch `soh.exe`, and open the menu with F1 or Esc.
3. Under **Settings > Graphics**, set **Renderer API** to **DirectX 11**, then restart the game.
4. Under **Settings > VR**, check **Enable VR**. The game appears in the headset within a few seconds; if no headset is found yet, it keeps checking.

The other options on the VR page:

| Option | What it does |
| - | - |
| OpenXR Runtime | Auto uses SteamVR while it's running, and otherwise Windows' active OpenXR runtime. Turn VR off and on to apply a change. |
| Your Height | Your real height. Link's height is mapped onto yours, so the world looks bigger when he's a child. |
| Adult World Scale, Child World Scale | Scales the world further if people and doors look too small or too big. |
| Pick Up Drops by Hand | On: you collect ammo and magic drops by closing a hand on them. Off: you walk into them. |
| Snap Turn | How far one flick of the right stick turns you; 0 turns smoothly. |
| Sword Hand | Link is left-handed. Choose Right to put the sword in your right hand instead. |
| Hand Fine Tuning | Small rotation adjustments for each hand model. Leave these at 0 unless the hands look misaligned. |

## Playing

Title screens, file select, the pause menu, cutscenes, shops, and rooms drawn on a fixed background (such as Link's house and the market) appear on a floating screen in front of you. There you play with the controller as a normal gamepad. Everywhere else you're in the world.

### In the world

| Control | What it does |
| - | - |
| Left stick | Walk, relative to where you're looking |
| Walk around your room | Link walks too; walls stop you |
| Right stick left/right | Turn |
| Right stick down | L |
| Right stick click | Recenter the view, or bring the floating screen back |
| Left stick click or Menu button | Pause |
| A | Talk, open, climb, roll; while carrying something, put it down or throw it |
| B | Advance text, and put the ocarina away |
| Y | Open or close the item menu |
| Left trigger | Z-target |

| To | Do this |
| - | - |
| Draw or put away the sword | Reach over your shoulder with your sword hand and squeeze the grip |
| Attack | Swing the sword |
| Block | Hold the shield where the attack is coming from |
| Take an item, tunic, boots or shield | Press Y. Touch or point at it in the ring that opens, and squeeze the grip or pull the trigger. Picking a group opens its own ring, and the middle of a ring goes back. The item goes into the hand that picked it. |
| Pick up a drop | Close a hand on it |
| Lift a pot, rock, bush, bomb or bomb flower | Close a hand on it |
| Throw | Swing the hand holding it and let go of the grip; a slow release sets it down |
| Open a door or chest | Close a hand on the door and push or pull, or on the chest's lid and lift; A works too |
| Fire the bow or slingshot | Pull the trigger of your other hand with that hand at the bow, draw back and let go |
| Fire the hookshot | Point, pull the trigger to aim, let go to fire |
| Use two-handed weapons | Close your other hand on the handle of the Deku stick, hammer or Biggoron's Sword |
| Use a bottle | Tip it over; at your mouth for potions and milk, anywhere for fairies and the rest. Swing an empty bottle through a bug, fish, fairy or blue fire to catch it. |
| Put on or take off a mask | Hold the mask to your face, or close a hand at your face to take it off |
| Play the ocarina | A, X, Y and the right stick up and down are its notes |

The hearts, magic, rupees and keys are on the back of your left wrist, and the A and B buttons on your right. Text boxes appear on a panel in front of you. The desktop window shows what your left eye sees.

## Known limitations

This is a work in progress, and several parts of the game still use its original controls:

* Magic spells play their usual casting scenes.
* The pause menu is the original one on the floating screen, navigated with the stick.
* The Lens of Truth's view is drawn into each eye as the game draws it, which may look odd.
* The ocarina is played with buttons, not held to your mouth.
* Ladders, ledges and blocks are climbed and pushed with the stick and A, not by hand.
* Some interactions haven't been tried everywhere in the game, so expect rough edges.

The VR code is in [soh/soh/Enhancements/vr](../soh/soh/Enhancements/vr). Where the game's own code was changed, it calls into the VR code through functions named `Vr_...`.
