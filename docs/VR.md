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

The other options on the VR page are below. Most of them can also be changed in the headset: press Y, open the System section (the ship) and pick VR Settings (the compass).

| Option | What it does |
| - | - |
| OpenXR Runtime | Auto uses SteamVR while it's running, and otherwise Windows' active OpenXR runtime. Turn VR off and on to apply a change. |
| Seated Mode | For playing seated: lifts your view, and your hands with it, to a standing Link's eye height. It's measured when you turn it on and each time you recenter, so sit as you'll play and recenter. |
| Your Height | Your real height. Link's height is mapped onto yours, so the world looks bigger when he's a child. |
| Adult World Scale, Child World Scale | Scales the world further if people and doors look too small or too big. |
| Pick Up Drops by Hand | Off (the default): you collect ammo and magic drops by walking into them, as in the game. On: you close a hand on them instead. |
| Snap Turn | How far one flick of the right stick turns you; 0 turns smoothly. |
| Movement Direction | Whether pushing the left stick forward moves you where you look or where your left hand points. |
| Sword Hand | Link is left-handed. Choose Right to put the sword in your right hand instead. |
| Hand Fine Tuning | Small rotation adjustments for each hand model. Leave these at 0 unless the hands look misaligned. |

## Playing

The title screen's cutscene plays around you in 3D, seen from its own camera as it moves; press A or Start to skip it. File select, the pause menu, shops, and rooms drawn on a fixed background (such as Link's house and the market) appear on a floating screen in front of you. There you play with the controller as a normal gamepad. Everywhere else you're in the world, cutscenes included: they play out around you, seen through Link's eyes, and your hands don't act on the game until they're over.

### In the world

| Control | What it does |
| - | - |
| Left stick | Walk, relative to where you're looking (or where your left hand points; see Movement Direction) |
| Walk around your room | Link walks too; walls stop you |
| Right stick left/right | Turn |
| Right stick down | L; talks to Navi when she has something to say |
| Right stick click | Recenter the view, or bring the floating screen back |
| Left stick click or Menu button | Pause |
| A | Talk, open, climb, roll; while carrying something, put it down or throw it |
| B | Advance text, and put the ocarina away |
| Y | Open or close the item menu, which also holds the pause menu and the VR settings |
| Left trigger | Z-target |

| To | Do this |
| - | - |
| Draw or put away the sword | Reach over your shoulder with your sword hand and squeeze the grip |
| Attack | Swing the sword. A very hard swing hits as hard as a jump attack. A blade swung into a wall or the floor sparks and stops, which wears out the Deku stick and Biggoron's Sword as the game's own hits do, and the Megaton Hammer swung into the ground shakes it, which some switches answer to. |
| Spin attack | Squeeze your sword hand's grip (away from your shoulder) to charge, and let go to spin: the sword sweeps around you. Or turn around for real, most of the way, with the sword held out. |
| Block | Hold the shield where the attack is coming from |
| Take an item, tunic, boots or shield | Press Y. Touch or point at it in the ring that opens, and squeeze the grip or pull the trigger. Picking a group opens its own ring, and the middle of a ring goes back. The item goes into the hand that picked it. The first ring's groups: Ranged & Tools, Explosives & Heavy, Magic & Spells (with the magic arrows), Bottles & Consumables, Quest & Lore, Equipment & Gear, and System (the ship). |
| Pick up a drop | Walk into it, or with Pick Up Drops by Hand on, close a hand on it |
| Lift a pot, rock, bush, bomb or bomb flower | Close a hand on it. Close your other hand on it too to carry it in both; let go with one and the other keeps it. |
| Climb | On a ladder, vine or climbable wall, close a hand to hold on and pull down to climb, hand over hand: reach up with one hand while the other pulls. Push down to climb down, and pull sideways to move along a wall. Hanging from a ledge, pull up with a hand that holds on to climb onto it, or let go of it to drop. Where A says Climb, grab the ledge with both hands and pull. The stick climbs too; Climb by Hand turns this off. |
| Push or pull a block or Armos statue | Put both hands out on it and squeeze both grips; Link takes hold once he's up against it, so you can squeeze first and walk in. Then push the stick forward while looking at it to push it, or while looking away from it to pull it. Let go to let go of it. A works too. |
| Throw | Swing the hand holding it and let go of the grip; a slow release sets it down. Carried in both hands, swing both and let go of both together to throw it with both. |
| Open a door or chest | Close a hand on the door and push or pull, or on the chest's lid and lift; A works too |
| Fire the bow or slingshot | Pull the trigger of your other hand with that hand at the bow, draw back and let go. The shot flies from your drawing hand through the bow or the slingshot's arms, so pull lower to aim higher, and the further you draw, the faster and further it flies. While it's drawn, a ring shows where it will land (Bow Landing Marker), and dots can show its path (Bow Flight Path). |
| Fire the hookshot | Point, pull the trigger to aim, let go to fire. A laser shows where it would catch: green where it will hook, red where it would bounce off. |
| Use two-handed weapons | Close your other hand on the handle of the Deku stick, hammer or Biggoron's Sword |
| Use a bottle | Tip it over; at your mouth for potions and milk, anywhere for fairies and the rest. Swing an empty bottle through a bug, fish, fairy or blue fire to catch it. |
| Put on or take off a mask | Hold the mask to your face, or close a hand at your face to take it off |
| Play the ocarina | Whistle, hum or sing its notes (D, F, A, B and the high D) into your headset's microphone, in any octave; how far off the note you are bends it, as the stick does. A note started again after a breath plays again. A and the right stick (up, down, left, right) play the notes too, and X and Y also play C-left and C-right. The right stick doesn't turn you while the ocarina is up. The microphone is only open while the ocarina is out; Whistle Ocarina Notes turns it off. |
| Pause | Y, then the System section (the ship), then the map; or click the left stick |
| Change VR settings | Y, then the System section (the ship), then the compass. Point at a row and pull the trigger: switches flip, and numbers go down on the row's left half and up on its right half, repeating while you hold. Y or Close puts it away. |

The hearts, magic, rupees and keys are on the back of your left wrist, and the A and B buttons on your right. While the bow or slingshot is out, the arrows or seeds left show on the back of the hand holding it. Text boxes appear on a panel in front of you. The desktop window shows what your left eye sees.

## Known limitations

This is a work in progress, and several parts of the game still use its original controls:

* Magic spells play their usual casting scenes.
* The pause menu is the original one on the floating screen, navigated with the stick.
* The Lens of Truth's view is drawn into each eye as the game draws it, which may look odd.
* The ocarina plays wherever you hold it; it doesn't need to be at your mouth yet.
* Some interactions haven't been tried everywhere in the game, so expect rough edges.

The VR code is in [soh/soh/Enhancements/vr](../soh/soh/Enhancements/vr). Where the game's own code was changed, it calls into the VR code through functions named `Vr_...`.
