# Dark Forces for iPhone and iPad

Star Wars: Dark Forces on [The Force Engine](https://theforceengine.github.io),
for iPhone and iPad (iOS 18 or newer). Unofficial, and a work in progress: this
first version plays the game with its menus by touch, but has no touch controls
for playing yet (a game controller or a Bluetooth keyboard should work, but is
untested).

**You bring the game.** No game files come with the app. You need your own copy
of Dark Forces: the **Dark Forces Remaster** (Steam or GOG) or the **original**
(Steam, GOG or CD).

## 1. Install the app

The app comes as an unsigned `.ipa` file. Download the newest
`DarkForces-iOS-unsigned.ipa` from the
[iOS builds](https://github.com/sithewok13-dev/TheForceEngine/actions/workflows/ios.yml)
(open the newest green run, then the file under *Artifacts*; it downloads as a
`.zip` with the `.ipa` inside).

An iPhone only runs apps signed with an Apple certificate, so sign and install
it with a sideloading tool, as for any `.ipa`:

- **[SideStore](https://sidestore.io)**: after a one-time setup with a computer,
  signs and refreshes apps on the phone itself. In SideStore, *My Apps*, **+**,
  pick the `.ipa`.
- **[AltStore](https://altstore.io)** (AltStore Classic): AltServer on a Mac or
  Windows PC puts AltStore on the phone; then *My Apps*, **+**, pick the `.ipa`.
- **[Sideloadly](https://sideloadly.io)**: a Mac or Windows program. Connect the
  phone, drop the `.ipa` in, sign in with your Apple ID.

With a free Apple ID the app stays signed for 7 days; refresh it (SideStore and
AltStore can do this for you) or install it again. Your game files and saves are
kept.

## 2. Copy in your game files

Open the app once: it shows what it needs, and its folder appears in the
**Files** app under **On My iPhone > Dark Forces** (On My iPad on an iPad).
Copy your game folder into that folder. From a computer, Finder (Mac) or
iTunes (Windows) can copy it in too, under the app's files.

- **Dark Forces Remaster:** copy its whole folder, for example
  `STAR WARS Dark Forces Remaster` (on Steam: right-click the game, *Manage >
  Browse local files*). The large `movies` folder inside can be left out (the
  remastered video cutscenes don't play in this version yet).
- **Original Dark Forces:** copy the folder that has `DARK.GOB` in it, with its
  `LFD` folder (on Steam that is `dark forces/Game`; on GOG, the game's folder).

The folder can keep its own name. The app looks for `DARK.GOB`, also one folder
further down, and starts the game by itself as soon as it finds it (or tap
**Check Again**). Upper or lower case in file names doesn't matter.

The app's own files (settings, saves, screenshots and a log) are in the
`TheForceEngine` folder next to your game folder.

## What works and what doesn't yet

- Plays in TFE's software renderer, widescreen at the screen's own resolution,
  with sound and music, also with the phone on silent.
- TFE's menus work by touch: tap to choose, drag to scroll.
- Not yet: touch controls for playing (next step), the remastered HD video
  cutscenes, TFE's GPU renderer.
