# Ship of Harkinian VR

An experimental six-degrees-of-freedom VR mode for Ship of Harkinian. You stand in the world at Link's eye height. Your hands are Link's hands, and the sword, shield, bow, hookshot and other items are used with your body instead of buttons.

This is an unofficial fork. It isn't made or supported by HarbourMasters, so please don't ask for help with it in the Ship of Harkinian support channels.

## You bring the game

Like Ship of Harkinian, this repository contains no copyrighted game assets. To play, you need your own legally dumped copy of the game in a version Ship of Harkinian supports. You can check a dump with the compatibility checker at https://ship.equipment/ or against the hashes in [supportedHashes.json](supportedHashes.json). Never upload a ROM, an `oot.o2r` or `oot-mq.o2r` file, or anything extracted from them.

## What you need

* Windows 10 or 11. VR uses the DirectX 11 renderer and isn't available on Linux or macOS.
* A PC VR headset with an OpenXR runtime: SteamVR, Quest Link or Steam Link, Virtual Desktop, Pico Connect, Windows Mixed Reality, or a headset maker's own. It has been played on a Quest 3 through SteamVR and Quest Link; the others haven't been tried yet.
* Motion controllers. The game has its controls laid out for Quest Touch (and the Quest Pro's and Quest 3's, which work as Touch), Valve Index, HTC Vive wands, Windows Mixed Reality, HP Reverb G2, Samsung Odyssey, Pico 4 and Vive Focus 3 controllers, and the most basic layout any runtime offers. On the Index, A and B are on both hands; it has no menu button, so pause from the item wheel's System section. On Vive wands, the trackpad is the stick, clicking it is A or X, and the menu button is B or Y; pause and recenter from the item wheel's System section. The log names the layout the runtime picked (`[VR] Left hand: ...`).
* Everything listed under Windows in [BUILDING.md](BUILDING.md).

## Building

Follow the Windows steps in [BUILDING.md](BUILDING.md); nothing extra is needed. The OpenXR loader is installed through vcpkg along with SoH's other dependencies, the first time CMake configures the project. Use the Release configuration (`--config Release`) for playing, since the debug build is too slow for VR.

On first launch, give the game your ROM when it asks, the same as in regular Ship of Harkinian. It makes `oot.o2r` from it.

## The launcher

`HarkinianVR.exe` starts either game, so there's one thing to open and one thing to add to Steam. It sits next to `soh.exe`.

- It finds your ROMs by what's in them, whatever they're called, and looks next to itself, in a `roms` folder, a few folders up, and in any folder you add. It reads inside `.zip` files and handles `.z64`, `.n64` and `.v64`.
- A game shown as needing setup takes one press: the launcher hands the ROM to that game, which extracts it as usual.
- A ROM neither game can read is listed and says so. ROM hacks that change the game's code can't run on these engines; asset packs (`.o2r`) go in each game's mods folder, which the launcher opens for you.

Inside the headset, you can swap games without taking it off: **PLAY MAJORA'S MASK** is on the last page of Ocarina of Time's VR settings panel, and the wheel's System ring has an ocarina that goes back from Majora's Mask. Each game has the same button in its menu, under Settings > VR.

## Turning VR on

1. Start your headset's runtime (SteamVR, or the Quest Link or Air Link connection).
2. Launch `soh.exe`, and open the menu with F1 or Esc.
3. Under **Settings > Graphics**, set **Renderer API** to **DirectX 11**, then restart the game.
4. Under **Settings > VR**, check **Enable VR**. The game appears in the headset within a few seconds; if no headset is found yet, it keeps checking.

Press **F8** on the keyboard to turn VR on or off without opening the menu (F9 is already the game's text-to-speech key). When the headset comes off, the window keeps playing flat.

The other options on the VR page are below. Most of them can also be changed in the headset: press Y, open the System section (the ship) and pick VR Settings (the compass). Its second page (More Settings) has the aim marker and path, picking up drops, whistling, climbing, running and swimming with your arms, nodding to answer, and the VR tutorials.

| Option | What it does |
| - | - |
| OpenXR Runtime | Auto uses SteamVR while it's running, and otherwise Windows' active OpenXR runtime. Turn VR off and on to apply a change. |
| Seated Mode | For playing seated: lifts your view, and your hands with it, to a standing Link's eye height. It's measured when you turn it on and each time you recenter, so sit as you'll play and recenter. |
| Your Height | Your real height. Link's height is mapped onto yours, so the world looks bigger when he's a child. |
| Adult World Scale, Child World Scale | Scales the world further if people and doors look too small or too big. |
| Pick Up Drops by Hand | Off (the default): you collect ammo and magic drops by walking into them, as in the game. On: you close a hand on them instead. |
| Snap Turn | How far one flick of the right stick turns you; 0 turns smoothly. |
| Movement Direction | Whether pushing the left stick forward moves you where you look or where your left hand points. |
| Sword Hand | Link is left-handed. Choose Right to put the sword in your right hand instead. It also decides which shoulder the sword is drawn from; the shield is drawn from the other one. |
| Title Screen | How the title screen's cutscene is watched: from its own camera as it moves (kept above the ground), through the eyes of the Link riding in it, or on the floating screen, which is the calmest. |
| Draw Distance | How much further than usual people, enemies and things stay drawn (1x to 5x, 3x by default), so they don't pop in as you look around. Far-away models always use their detailed versions too. |
| Third Person | Play from the game's own camera behind Link, in 3D, looking around with your head; the controllers work as a gamepad, as on the floating screen. |
| Streamer View | The desktop window shows the game from its own camera, with Link and the HUD, while you play first person. It draws the game once more each frame. |
| Text Size, Text Opacity | How big and how see-through the text panel is (opacity where your headset's runtime can fade a panel). |
| Haptics | A heartbeat at one heart or less, blows felt hardest on the side they came from, every step in the iron boots, and the ocarina humming as you play. |
| Headphone Mix | While in the headset, the game mixes its sound for headphones, as its own Headset sound setting does. |
| Show Link's Body | Link's body below your view, with his arms reaching to your hands. It doesn't crouch or lean with you. |
| Arrows from a Quiver, Get Close to Walls | See Playing. Both on by default. |
| VR Controls | Which controller button does A, B, the item wheel, pause, recenter and Navi. Pick one from the list, or choose Press to Bind and press the button you want within ten seconds. |
| Wrist HUD | For each piece (hearts, rupees, buttons, map, timer, ammo): its own place, the other wrist, floating ahead, or hidden, and its size. |
| Hand Fine Tuning | Small rotation adjustments for each hand model. Leave these at 0 unless the hands look misaligned. |

## Playing

The title screen's cutscene plays around you in 3D, seen from its own camera as it moves (or through Link's eyes, or on the floating screen: see Title Screen above); press A or Start to skip it. File select, the pause menu, shops, and rooms drawn on a fixed background (such as Link's house and the market) appear on a floating screen in front of you. There you play with the controller as a normal gamepad. Everywhere else you're in the world, cutscenes included: they play out around you, seen through Link's eyes, and your hands don't act on the game until they're over.

### In the world

| Control | What it does |
| - | - |
| Left stick | Walk, relative to where you're looking (or where your left hand points; see Movement Direction) |
| Walk around your room | Link walks too; walls stop you |
| Right stick left/right | Turn |
| Right stick down | L; talks to Navi when she has something to say |
| Right stick click | Recenter the view, or bring the floating screen back |
| Menu button | Pause |
| A | Talk, open, climb, roll; while carrying something, put it down or throw it |
| B | Advance text, and put the ocarina away |
| Y | Open or close the item menu, which also holds the pause menu and the VR settings |

| To | Do this |
| - | - |
| Draw or put away the sword | Reach over your sword hand's shoulder and squeeze the grip |
| Take out or put away the shield | Reach over your other shoulder and squeeze the grip. The sword and shield come out separately, so you can hold just one. |
| Put away the bow, slingshot, hookshot or boomerang | Reach over the shield's shoulder and squeeze, or over the sword's to swap straight to the sword. |
| Attack | Swing the sword. Sideways, overhead and thrusting swings are the game's horizontal, vertical and stab attacks, which enemies that guard or dodge react to, and a downward swing in the air or a very hard swing hits as hard as a jump attack. Your blade rings and sparks off shields and other blades, and pushes enemies aside when it's moved slowly through them. A blade swung into a wall or the floor sparks and stops, which wears out the Deku stick and Biggoron's Sword as the game's own hits do, and the Megaton Hammer swung into the ground shakes it, which some switches answer to. |
| Spin attack | Hold your sword hand's trigger to charge, and let go to spin: the sword sweeps around you. Or turn around for real, most of the way, with the sword held out. |
| Block | Hold the shield where the attack is coming from |
| Take an item, tunic, boots or shield | Press Y. Touch or point at it in the ring that opens, and squeeze the grip or pull the trigger. Picking a group opens its own ring, and the middle of a ring goes back. The item goes into the hand that picked it. The first ring's groups: Ranged & Tools, Explosives & Heavy, Magic & Spells (with the magic arrows), Bottles & Consumables, Quest & Lore, Equipment & Gear, and System (the ship). |
| Pick up a drop | Walk into it, or with Pick Up Drops by Hand on, close a hand on it |
| Lift a pot, rock, bush, bomb or bomb flower | Close a hand on it. Close your other hand on it too to carry it in both; let go with one and the other keeps it. |
| Climb | Walk into a ladder, vine or climbable wall to climb onto it, and use the stick as usual. Or close a hand to hold on and pull down to climb, hand over hand; pull sideways to move along a wall. Hanging from a ledge, pull up to climb onto it, or let go to drop. Grab Ladders to Climb Them makes a hand on the rungs the only way on. |
| Run faster | Swing your arms, one up as the other goes down, while you walk with the stick: up to a quarter faster (Swing Your Arms to Run) |
| Swim | Pull both hands back, breaststroke style, to swim the way you look; the stick swims too |
| Crawl into a crawlspace | Get down for real in front of it |
| Break free of a ReDead or Like Like | Shake the controllers |
| Answer yes or no | Nod for the first answer, shake your head for the second (Nod or Shake Your Head to Answer). A and the stick still choose. |
| Push or pull a block or Armos statue | Put both hands out on it and squeeze both grips; Link takes hold once he's up against it, so you can squeeze first and walk in. Then push the stick forward while looking at it to push it, or while looking away from it to pull it. Let go to let go of it. A works too. |
| Throw | Swing the hand holding it and let go of the grip; a slow release sets it down. Carried in both hands, swing both and let go of both together to throw it with both. |
| Open a door or chest | Close a hand on the door and push or pull, or on the chest's lid and lift; A works too |
| Fire the bow or slingshot | Pull the trigger of your other hand with that hand at the bow, draw back and let go. The shot flies from your drawing hand through the bow or the slingshot's arms, so pull lower to aim higher, and the further you draw, the faster and further it flies. While it's drawn, a ring shows where it will land (Bow Landing Marker), on top of everything and on the enemy it will hit, and dots can show its path (Bow Flight Path). |
| Fire the hookshot | Point, pull the trigger to aim, let go to fire. A laser shows where it would catch: green where it will hook, red where it would bounce off. The hook leaves the hookshot and flies on its chain, and goes back on when it returns. |
| Throw the boomerang | Swing and let go of the grip; reach out to catch it in that hand on its way back |
| Cast a spell | Take Din's Fire, Farore's Wind or Nayru's Love from the wheel; it glows between your hands. Hold both triggers to cast it, or let go first to cast nothing. |
| See through the Lens of Truth | Take it from the wheel, hold it up and look through the glass: hidden things only show through it, and fake walls vanish behind it, as a real window would. It spends magic only while you look through it. Reach over your shoulder and squeeze to put it away. |
| Use two-handed weapons | Close your other hand on the handle of the Deku stick, hammer or Biggoron's Sword |
| Use a bottle | Tip it over; at your mouth for potions and milk, anywhere for fairies and the rest. Swing an empty bottle through a bug, fish, fairy or blue fire to catch it. |
| Put on or take off a mask | Hold the mask to your face, or close a hand at your face to take it off |
| Play the ocarina | Whistle, hum or sing its notes (D, F, A, B and the high D) into your headset's microphone, in any octave; how far off the note you are bends it, as the stick does. A note started again after a breath plays again. A and the right stick (up, down, left, right) play the notes too, and X and Y also play C-left and C-right. The right stick doesn't turn you while the ocarina is up. The microphone is only open while the ocarina is out; Whistle Ocarina Notes turns it off. |
| Pause | Y, then the System section (the ship), then the map; or click the left stick |
| Read the map | Turn your right palm up: the minimap is on it. A running timer shows on your left wrist. |
| Read a sign | Its words appear over it, where it stands (gossip stones too) |
| Talk to Navi | Close a hand on her, or push the right stick down. She fades when she flies close to your face. |
| Give someone a trade item | Hold it out toward them for a moment |
| Ride Epona by the reins | Close both grips in front of you. Hold them out to go (further out is faster), pull them in to stop, pull one rein back to turn that way, and flick both down to spur her on. |
| Dig with Dampé | Dig: a hand low down, scooping down fast, near him |
| Reel in a fish | Twist your free wrist, or pull its trigger |
| Lift a silver rock or the golden gauntlets' pillar | Close both hands on it |
| Climb out of the water | Reach up onto the ledge ahead and squeeze the grip |
| Take an arrow | With the bow out, reach over your drawing hand's shoulder and squeeze the grip, then nock it at the bow as usual (Arrows from a Quiver). |
| Change VR settings | Y, then the System section (the ship), then the compass. Point at a row and pull the trigger: switches flip, and numbers and choices go down on the row's left half and up on its right half, repeating while you hold. More Settings turns to the second page and Back returns. Y or Close puts it away. |

The game's lessons on its buttons (what each item you find does, the Kokiri Forest signs and lessons, Navi's first words, the shields, the horse) say what to do in VR instead; VR Tutorials turns this off. On the floating screen they're the game's own.

The hearts, magic, rupees and keys are on the back of your left wrist, and the A and B buttons on your right. While the bow or slingshot is out, the arrows or seeds left show on the back of the hand holding it. Text boxes appear on a panel in front of you. The desktop window shows what your left eye sees.

## Majora's Mask (early)

`2ship/` holds Majora's Mask: 2Ship2Harkinian with the same VR core (`vr/core/`). So far it shows the world in stereo from Link's eyes, sized to whichever form he's in (towering as a Deku Scrub, small as the Fierce Deity), with its HUD on your wrists.

Link's hands are on your controllers in every form. Swing the sword (or the Fierce Deity's, or a Deku stick) to hit with it, and swing harder to hit as hard as a jump attack. As a Goron or a Zora, punch. The shield blocks wherever you hold it. Link is left-handed, so his sword is on your left controller; **Sword in Right Hand** under Settings > VR swaps them.

**Y** opens the item ring: an upright ring of your items in front of you, with rings inside rings (Ranged, Explosives, Tools, Bottles, Masks, Quest, and System for the pause menu). Touch or point at an item and squeeze the grip or pull the trigger to take it. An item goes to the hand that took it, and that hand's trigger uses it. A mask stays in your hand: bring it to your face to put it on, transformation masks included. Squeeze a grip at your face to take it off (or turn back into Link); let go to put it away. **X** also takes off or puts back on your mask.

The HUD is on your wrists: the three-day clock is a watch on your left wrist, with your hearts and magic above it and your rupees beside it, and the buttons on your right wrist show what each trigger holds. Text, the ocarina's staff and minigame timers show on a panel ahead of you.

| Control | What it does |
| - | - |
| Left stick | Walk, toward where you're looking |
| Right stick left/right | Turn |
| Right stick up | Talk to Tatl (C-up) |
| Right stick click | Recenter |
| A / B | A / B |
| Y | Item ring |
| Left / right trigger | Use the item in that hand (C-left / C-right) |
| X | Take off or put on your mask (C-down) |
| Left grip | Shield, or another form's guard (R) |
| Menu button | Pause |

Each form has its own moves too:

| Form | Do this |
| - | - |
| Deku Scrub | Blow into your headset's microphone to blow a bubble, and stop blowing to shoot it where you're looking. Spin around for real to do the Deku spin. |
| Deku Scrub, Goron, Zora | Cross your arms over your chest to guard. As a Zora with magic, that raises the electric barrier. |
| Goron, Zora | Punch with your fists. |

A Deku bubble goes where you look. With the ocarina out, whistle, hum or sing its notes, or play them with A, X, Y and the right stick (up, down, left, right). **Whistle Ocarina Notes** and **Blow for Deku Bubbles** under Settings > VR turn the microphone uses off; it only listens while the ocarina is out or you're a Deku Scrub. In the pause menu, title screen and file select the controllers work like a gamepad: X and Y are C-left and C-right, the right stick up and down are C-up and C-down, the left trigger is Z and the right trigger is R.

To build it, check out the submodules (`git submodule update --init 2ship/libultraship 2ship/ZAPDTR 2ship/OTRExporter`), then follow `2ship/docs/BUILDING.md` from the `2ship` folder. Put your Majora's Mask ROM (the US N64 one) in `2ship/OTRExporter/` and build the `ExtractAssets` target to make `mm.o2r`, then copy `2ship.o2r` from `2ship/build/x64/mm/` next to `2ship.exe`. Turn VR on under Settings > VR.

## Known limitations

This is a work in progress, and several parts of the game still use its original controls:

* Spells still play their usual casting scenes once cast.
* The pause menu is the original one on the floating screen, navigated with the stick.
* The ocarina plays wherever you hold it; it doesn't need to be at your mouth yet.
* Some interactions haven't been tried everywhere in the game, so expect rough edges.

The VR code is in [soh/soh/Enhancements/vr](../soh/soh/Enhancements/vr). Where the game's own code was changed, it calls into the VR code through functions named `Vr_...`.
