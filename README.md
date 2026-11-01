> **This is an unofficial VR fork of Ship of Harkinian.** It adds an experimental six-degrees-of-freedom VR mode for PC headsets. See **[docs/VR.md](docs/VR.md)** for how to build it, turn VR on and play. It isn't made or supported by HarbourMasters. Like Ship of Harkinian, it contains no game assets, so you need your own copy of the game. Everything from the **Ship of Harkinian** heading down is that project's own README.
>
> **The aim:** to make every item and action feel like it belongs in a game made for VR, not to lay a VR view over the original controls. You draw the sword from over your shoulder and swing it, block with the shield where you hold it, pull back the bowstring and aim down the arrow, tip a bottle to your mouth to drink, hold a mask to your face to put it on, and pick up pots, bombs and rocks to throw them. Buttons are kept only where your hands have nothing better to do.

# Playing in VR

HAS ONLY BEEN TESTED WITH A QUEST 3 ON STEAM AIR LINK

How to build it, turn VR on and change its settings is in **[docs/VR.md](docs/VR.md)**.

## Tries to turn OOT into a AAA VR Game. Revamped UI, and items are now fully 6DoF interactable.

The item wheel: press Y

Your items, gear, the pause menu and the VR settings are all on the item wheel. There are no item buttons.

1. **Press Y.** A ring of items opens in front of you.
2. **Reach out and touch one, or point at it, then squeeze the grip or pull the trigger.** It goes into the hand that took it.
3. **Groups open their own ring.** Take one the same way. The middle of a ring goes back.
4. **Press Y again** to close the wheel.

The first ring's groups, clockwise from the top. Each shows the first thing in it, and only what you've found is there.

| Group | What's in it |
| - | - |
| Ranged | Slingshot, bow, boomerang, hookshot or longshot |
| Explosives/Power Weapons | Bombs, bombchus, Megaton Hammer, Deku sticks, magic beans |
| Magic | Lens of Truth, fire, ice and light arrows, Nayru's Love, Farore's Wind, Din's Fire |
| Consumables | Deku nuts and bottles |
| Quest | Ocarina, and the child and adult trade items |
| Gear | Shields, swords, boots and tunics, each in its own ring |
| System (the ship) | The pause menu (the map) and the VR settings (the compass) |

## With your hands

| To | Do this |
| - | - |
| Draw or put away the sword | Reach over your shoulder with your sword hand and squeeze the grip |
| Attack | Swing the sword |
| Block | Hold the shield where the attack is coming from |
| Pick up a drop | Walk into it, or with Pick Up Drops by Hand on, close a hand on it |
| Lift a pot, rock, bush, bomb or bomb flower | Close a hand on it. Close your other hand on it too to carry it in both. |
| Throw | Swing and let go of the grip; a slow release sets it down. Carried in both hands, let go of both together to throw with both. |
| Push or pull a block or Armos statue | Put both hands out on it and squeeze both grips, then push the stick forward while looking at it to push, or while looking away to pull |
| Open a door or chest | Close a hand on the door and push or pull, or on the chest's lid and lift |
| Fire the bow or slingshot | With your other hand at the bow, pull its trigger, draw back and let go. Pull lower to aim higher. |
| Fire the hookshot | Point, pull the trigger to aim, let go to fire |
| Use two-handed weapons | Close your other hand on the handle of the Deku stick, hammer or Biggoron's Sword |
| Use a bottle | Tip it over; at your mouth for potions and milk, anywhere for fairies and the rest. Swing an empty bottle through a bug, fish, fairy or blue fire to catch it. |
| Put on or take off a mask | Hold the mask to your face, or close a hand at your face to take it off |
| Play the ocarina | A and the right stick are its notes; X and Y play C-left and C-right |

## Buttons

| Control | What it does |
| - | - |
| Y | Open or close the item wheel |
| Left stick | Walk, toward where you're looking (or where your left hand points; see Movement Direction) |
| Walk around your room | Link walks too; walls stop you |
| Right stick left/right | Turn |
| Right stick down | L; talks to Navi when she has something to say |
| Right stick click | Recenter the view, or bring the floating screen back |
| Left stick click or Menu button | Pause |
| A | Talk, open, climb, roll; while carrying something, put it down or throw it |
| B | Advance text, and put the ocarina away |
| Left trigger | Z-target |

The hearts, magic, rupees and keys are on the back of your left wrist, and the A and B buttons on your right. While the bow or slingshot is out, the arrows or seeds left show on the back of the hand holding it. Text boxes appear on a panel in front of you. The desktop window shows what your left eye sees.

# Ship of Harkinian

* [Website](https://www.shipofharkinian.com)
* [Discord](https://discord.com/invite/harbourmasters)

# Quick Start

The Ship does not include any copyrighted assets.  You are required to provide a supported copy of the game.

### 1. Verify your ROM dump
You can verify you have dumped a supported copy of the game by using the compatibility checker at https://ship.equipment/. If you'd prefer to manually validate your ROM dump, you can cross-reference its `sha1` hash with the hashes [here](docs/supportedHashes.json).

### 2. Download The Ship of Harkinian from [Releases](https://github.com/HarbourMasters/Shipwright/releases)

### 3. Launch the Game!
#### Windows
* Extract the zip
* Launch `soh.exe`

#### Linux
* Execute `soh.appimage`.  You may have to `chmod +x` the appimage via terminal.
* When prompted, select your supported copy of the game.
* Saves, settings and `oot.o2r` are stored in `~/.local/share/soh/`. If the folder you launch from already has `shipofharkinian.json`, `oot.o2r` or `oot-mq.o2r`, that folder is used instead. Set `SHIP_HOME` to choose another folder.

#### macOS
* Run `soh.app`. When prompted, select your supported copy of the game.
* You should see a notification saying `Processing OTR`, then, once the process is complete, you should get a notification saying `OTR Successfully Generated`, then the game should start.

### 4. Play!

Congratulations, you are now sailing with the Ship of Harkinian! Have fun!

# Configuration

### Default keyboard configuration
| N64 | A | B | Z | Start | Analog stick | C buttons | D-Pad |
| - | - | - | - | - | - | - | - |
| Keyboard | X | C | Z | Space | WASD | Arrow keys | TFGH |

### Other shortcuts
| Keys | Action |
| - | - |
| ESC | Toggle menu |
| F2 | Toggle capture mouse input |
| F5 | Save state |
| F6 | Change state |
| F7 | Load state |
| F9 | Toggle Text-to-Speech |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |
| Ctrl+R | Reset |

# Project Overview
Ship of Harkinian (SOH) is built atop a custom library dubbed libultraship (LUS). Back in the N64 days, there was an SDK distributed to developers named libultra; LUS is designed to mimic the functionality of libultra on modern hardware. In addition, we are dependent on the source code provided by the OOT decompilation project.

In order for the game to function, you will require a **legally acquired** ROM for Ocarina of Time. Click [here](https://ship.equipment/) to check the compatibility of your specific rom. Any copyrighted assets are extracted from the ROM and reformatted as a .o2r archive file which the code uses.

### Graphics Backends
Currently, there are three rendering APIs supported: DirectX11 (Windows), OpenGL (all platforms), and Metal (MacOS). You can change which API to use in the `Settings` menu of the menubar, which requires a restart.  If you're having an issue with crashing, you can change the API in the `shipofharkinian.json` file by finding the line `gfxbackend:""` and changing the value to `sdl` for OpenGL. DirectX 11 is the default on Windows.

# Custom Assets

Custom assets are packed in `.otr` archive files. To use custom assets, place them in the `mods` folder.

If you're interested in creating and/or packing your own custom asset `.otr` files, check out the following tools:
* [**retro - OTR generator**](https://github.com/HarbourMasters64/retro)
* [**fast64 - Blender plugin**](https://github.com/HarbourMasters/fast64)

# Development
### Building

If you want to manually compile SoH, please consult the [building instructions](docs/BUILDING.md).

### Playtesting
If you want to playtest a continuous integration build, you can find them at the links below. Keep in mind that these are for playtesting only, and you will likely encounter bugs and possibly crashes. 

* [Windows](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-windows.zip)
* [macOS](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-mac.zip)
* [Linux X86_64](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-linux-x86_64.zip)
* [Linux ARM](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-linux-aarch64.zip)

### Further Reading
More detailed documentation can be found in the 'docs' directory, including the aforementioned [building instructions](docs/BUILDING.md).

* [Credits](docs/CREDITS.md)
* [Custom Music](docs/CUSTOM_MUSIC.md)
* [Formatting](docs/FORMATTING.md)
* [Controller Mapping](docs/GAME_CONTROLLER_DB.md)
* [Modding](docs/MODDING.md)
* [Versioning](docs/VERSIONING.md)

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
