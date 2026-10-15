========================================================================
 EarShot - miniaudio edition (earshot_reminid)   v0.1.0-m1
========================================================================

A rebuild of EarShot (by HzanRsxa2959) on the miniaudio audio engine -
the same mod you know, but it no longer needs OpenAL. There is no
OpenAL32.dll, no alsoft.ini and no runtime install: one .asi file and
your existing sound content.

Dynamic gunshots, after-shot echo, distant shots, reverb, 3D spatial-
ization, distance attenuation, air absorption - all present, working
from the same EarShot folder layout and the same EarShot.ini keys as
EarShotOpenAL, so your old content and packs work as-is.

  Please read this file before asking for support. Most problems are
  file placement, filename spelling, or stereo/mono format.


------------------------------------------------------------------------
 IMPORTANT NOTES
------------------------------------------------------------------------

- Audio files must be MONO for correct 3D spatialization. Stereo files
  play in 2D with no distance attenuation. Stereo is only right for
  sounds with no position in the world (bullet whizz, ambience).
- Supported formats: .wav, .mp3, .flac, .ogg
- For alternative/randomized sounds, add a number to the end of the
  filename: shoot0.wav, shoot1.wav, ... up to 10 alternatives
  (shoot0..shoot9), plus the unnumbered shoot.wav as a plain fallback.
- If sounds are randomly picked when you do not want that, make sure
  the filename does not end in a number (rename sound1.wav to sound.wav).
- The log file is EarshotReminid.log, next to earshot_reminid.asi.
  It is NOT EarShotOpenAL.log - your old log is never touched.
- Set Logging = true in EarShot.ini for full load/playback tracing.


------------------------------------------------------------------------
 INSTALLATION
------------------------------------------------------------------------

1. REMOVE the old EarShot.asi (EarShotOpenAL / EarShot). Do not run both
   mods at the same time - they hook the same game functions.

   OpenAL32.dll and alsoft.ini are no longer needed by EarShot. You may
   delete them, but keep them if another mod uses OpenAL.

2. Copy these files anywhere the game loads ASI plugins from (game
   root next to gta_sa.exe, scripts\, or a ModLoader subfolder):

       earshot_reminid.asi
       EarShot.ini
       readme.txt

   Example layouts:

       GameFolder\earshot_reminid.asi, EarShot.ini, EarShot\
       GameFolder\scripts\earshot_reminid.asi, EarShot.ini, EarShot\
       GameFolder\modloader\EarShotMod\earshot_reminid.asi, EarShot.ini, EarShot\

3. Your sound content folder (the EarShot\ folder with all your packs):

       - If GameFolder\data\EarShot\ exists, it is used automatically.
       - Otherwise the EarShot\ folder NEXT TO THE .ASI is used. It is
         created empty on first run - move your content into it.

4. Start the game. Done.


------------------------------------------------------------------------
 COMPATIBILITY WITH OLD EARSHOT CONTENT
------------------------------------------------------------------------

- Same folder layout as EarShot / EarShotOpenAL: weapon folders with
  .earshot marker files, generic\ explosions, ricochet, gunshells,
  bullet_whizz and Tank Cannon folders are all read exactly as before.
- Same EarShot.ini sections and keys: MAIN, EXPLOSIONS,
  EXPLOSION_TYPE_0..11, RICOCHET, RICOCHET_SURFACE_TYPE_<n>, GUNSHELLS,
  TANKCANNON. An old ini can be dropped in unchanged.
- Existing weapon packs (VC weapon pack, Manhunt, Driv3r footsteps,
  CS1.6, ...) work unmodified - just keep the folder structure.
- No OpenAL files are required. HRTF/alsoft.ini settings do not apply;
  headphones/speakers are handled by the game's own output.

NOT YET IN THIS BUILD (planned for later stages):

  footsteps, landing/collapse sounds, missiles, vehicle cannon (fired
  as a weapon), fire sounds, car jacking, grenade bouncing, vehicle
  SFX, ambience / map ambience / LS gunfire ambience, looped weapons
  (minigun, chainsaw, flamethrower, fire extinguisher, spray can).

  Their INI sections are ignored for now; everything else is active.


------------------------------------------------------------------------
 FILE STRUCTURE OVERVIEW
------------------------------------------------------------------------

All custom sounds live under the EarShot\ folder:

  EarShot\
    <WeaponFolderName>\
      <weaponname>.earshot          marker file: weapon name + type,
                                     optional attenuation/pitch settings
      <weaponname>\                 sound folder (same name as marker)
        shoot.wav                   the shot
        shoot0.wav, shoot1.wav ...  randomized alternatives
        after.wav                   after-shot (echo tail)
        distant.wav                 distant shot (far away)
        reload.wav / reload_one.wav / reload_two.wav
        dryfire.wav                 click on empty
        low_ammo.wav                low-ammo warning shot
        hit.wav / hitmetal.wav / hitwood.wav / stomp.wav
        swing.wav                   melee swing
        martial_punch.wav / martial_kick.wav
        projfire.wav                projectile fire (vehicle/attached)
    generic\
      explosions\                   explosion, debris, distant,
                                    underwater (+0..N alternatives)
      explosionTypes\<id>\          per-explosion-type overrides
      ricochet\<surface>\           default, flesh, glass, metal, water,
                                    wood, sand, stone, dirt
      gunshells\<surface>\          gunshell / shotgunshell drops
      bullet_whizz\                 left_front, left_rear, right_front,
                                    right_rear (2D - stereo is fine here)
    Tank Cannon\
      cannon_fire0.wav ...          rhino tank main gun

Sound files are picked as <name>0..9.<ext> first (random), then the
unnumbered <name>.<ext>. Extension order: .wav, .mp3, .flac, .ogg.


------------------------------------------------------------------------
 EARSHOT.INI - MAIN SETTINGS
------------------------------------------------------------------------

  Logging = true|false          load/playback logging to the log file
  Max bytes in log = <n>        log size cap (default 9000000)
  Distant gunshot distance = <m>  beyond this, distant.wav is used
  Distant explosion distance = <m> beyond this, distant explosion plays
                                  with a speed-of-sound delay

Attenuation keys (all sections): maxDist, refDist, rolloffFactor,
airAbsorption, pitch. See the comments inside EarShot.ini.


------------------------------------------------------------------------
 TROUBLESHOOTING
------------------------------------------------------------------------

- No custom sounds at all?
    Check EarshotReminid.log. "mod folder" lines show which EarShot\
    folder was picked; weapon lines show what was registered.
- Sounds have no distance / come from your face?
    The file is stereo - convert it to mono.
- A weapon keeps the vanilla sound?
    Its folder has no matching .earshot marker, or the stem file name
    is misspelled (must match the sound type names listed above).
- Crash on startup?
    Make sure the old EarShot.asi is removed and that you are on
    GTA San Andreas 1.0 US (the only supported exe).
- Full sound-type documentation (markers, prefixes, properties):
    see the EarShotOpenAL readme - the format is the same.


------------------------------------------------------------------------
 CREDITS
------------------------------------------------------------------------

- HzanRsxa2959        - original EarShot mod
- wuzimu7171_ (TheArtemMaps) - EarShotOpenAL revamp (reference)
- miniaudio           - Ben Rayner (audio engine)
- openal-soft         - reference for EFX/filter/doppler parity
- GTA SA reverse engineering community (gta-reversed, plugin-sdk)

========================================================================
