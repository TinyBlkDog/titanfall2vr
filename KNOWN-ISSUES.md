# Known issues

Everything below ships with `v0.1.0`. Each entry says what you will
see, what to do about it, and where it stands.

If you hit something that is not here, please report it — see
[README.md](README.md#reporting-bugs) for what to include.

---

## Scope of the alpha

These are not defects, but they will surprise you if nobody says them.

- **Single player only.** Play the campaign. Do not take this onto official
  servers.
- **Northstar is required.** titanfall2vr is a Northstar client plugin; it does
  not run without it.
- **Tested on two headsets**, Play For Dream MR and Meta Quest 3, both through
  Virtual Desktop (VDXR). Other OpenXR runtimes are untested rather than
  unsupported — if yours works, say so, and if it does not, that is a bug worth
  reporting.
- **The weapon grip is one global setting, tuned by hand.** It was dialled in on
  the weapons that came up in normal campaign play, so a weapon that was never
  carried may sit wrong in the hand. **Grip forward**, **Grip right** and
  **Grip up** in the settings panel move it live while you hold the gun and
  save themselves back to the ini — see
  [CONTROLS.md](CONTROLS.md#hands--weapon). The gun range has every weapon in
  the game if you want to work through them.
- **Alpha performance, and it is not tuned to your GPU.** The frame budget is
  met on the test hardware, which is an RTX 5090. Nothing detects your card, so
  the mod ships at **75%** of your headset's full panel height. Raise
  **Resolution** if you have the headroom, lower it if you do not: see
  [README](README.md#performance), then *Occasional frame spikes* below.
- **Stereo is alternate-frame.** Each game frame renders one eye and the eyes
  take turns, so each eye gets a fresh image at half the frame rate. Same-frame
  stereo — both eyes in one frame — is in development and is **not** in this
  release. Its machinery is compiled in and defaults to off; it deadlocks the
  engine after about 129 doubled frames, so do not switch it on. Nothing in this
  document describes a defect you can reach with it off.

---

## 1. Changing resolution during play broke the ability buttons (NO LONGER OBSERVED)

**Severity: was HIGH. Not reproducing as of 2026-09-11.** The wearer has not
seen it again in the sessions leading up to this release, including ones that
changed the resolution more than once. Nothing was written to fix it, so this
is a defect that stopped appearing rather than one that was repaired, and the
account below is kept in full in case it comes back.

### What the player saw

After changing the resolution from the VR config panel **while in a game**, the
buttons carrying an ability stop working — A and Y on a standard layout — and
the HUD shows **UNBOUND** where the button glyph belongs. Every other control
keeps working, and the other HUD glyphs still show correct controller art.

**The first resolution change of a session usually works.** The second and any
later one, in the same session, leaves the buttons dead. Occasionally the game
repairs itself a few seconds later; usually it does not.

### Workaround, while it was happening

Set the resolution once, then restart the game to change it again. A restart
always cleared it; nothing else did.

### What it actually is (measured, not assumed)

A HUD string carries a token that must be substituted before it can be drawn:

    healthy   "%[X_BUTTON|]% Select"     <- a BUTTON token
    broken    "%+ability 1%"             <- the RAW COMMAND, never substituted

The substitution `%+command%` → `%[BUTTON|]%` fails while the UI rebuilds after
a video mode change, **and the failure is cached** — nothing ever re-asks, so
the wrong answer sticks. This is why only `+ability` and `+offhand` commands are
affected: they are the ones that need substituting. Argument-less commands
(`+use`, `+toggle_duck`, `+offhand0` on its own binding) are never touched.

The engine's own position is that this operation is not supported: a video
settings change is expected to be followed by a restart, and
`r2/cfg/video_settings_changed_quit.cfg` contains exactly one word — `quit`.
Each live change also destroys and recreates the UI Squirrel VM, and the state
degrades after the first one.

### Status

**No longer observed, and NOT knowingly fixed.** Fourteen headset runs went
into this. Nine repairs were tried and every one failed, at nine different
layers, and none of them shipped. Live resolution changes stay unrestricted: a
restriction was tried, and the wearer's call was that a resolution change
must be a real one.

What changed around it is that the resolution now persists properly. The panel
writes the ini and the per-headset cache together, where before the cache was
left stale and won on the next launch. Whether that is why the fault stopped
appearing is unknown, and nobody has tried to reproduce it deliberately since.

If the ability buttons ever go UNBOUND after a resolution change, that is this,
and it is worth a report with your log.

---

## 2. The gun trails the hand slightly (OPEN)

**Severity: LOW.** Playable.

**What you see.** Moving the gun quickly in hip fire, it is not quite crisp — a
slight delay behind the hand. There is no sway and no overshoot; it simply
arrives a moment late.

**Workaround.** None. It does not affect where rounds land.

**Where it stands.** Most likely the controller is sampled a frame before the
gun is rendered. The candidate fix is to sample the hand at the display's
predicted time instead. Not started.

---

## 3. Hands mode still shows the forearm (OPEN)

**Severity: LOW.** Cosmetic.

**What you see.** With **Pilot body** set to *weapon + hands*, the arms and body
are gone, but the gloves carry on up the forearm — to about the elbow on one
weapon set and to the lower forearm on the other. The two cutoffs do not match
each other.

**Workaround.** Set **Pilot body** to *weapon only* if the forearm bothers you,
or to *full body* if you would rather have the whole pilot.

**Where it stands.** Not a tuning miss and not fixable from a setting. The
filter hides whole meshes, and the forearm is part of the gauntlet mesh it has
to keep — a gauntlet is hand *and* forearm, and the game reports it as one
indivisible draw. Collapsing the forearm at the bones instead drags the cuff
over the fingers, because hand and forearm are one connected mesh and every
partition of one draws its own boundary. The two cutoffs differ because the two
pilot arm models were split differently by their artists.

Hands-only would need a custom model.

---

## 5. Occasional frame spikes (OPEN)

**Severity: LOW to MEDIUM**, depending on how sensitive you are to a dropped
frame.

**What you see.**

- **In play**, every 20 to 40 seconds or so, one frame stalls for about **90 to
  100 ms**. You feel it as a hitch.
- **About twenty seconds into a session**, one frame of roughly **1.2 seconds**,
  while the mod sets the render resolution to match your headset. Once per
  session.
- **At level loads**, frames from 100 ms up to a couple of seconds.

**Workaround.** For the in-play stall, lowering **Resolution** gives the whole
pipeline more headroom and is worth trying first — see
[README](README.md#performance). The
startup and level-load ones are one-off and happen while the screen is already
changing.

**Where it stands.** The in-play stall is now measured and it is **not in the
mod**. Forty of them were timed in one four-minute session, and every one sat
inside the call that hands the finished frame to the OpenXR runtime. They
cluster very tightly — ten between 100.8 and 101.9 ms, six between 90.0 and
90.9 ms — and a spread under a millisecond across ten occurrences is a fixed
timeout, not variable work. That points at the runtime or the link carrying
your headset, which is why lowering **Resolution** or trying a wired
connection, a lower streaming bitrate or a different codec is worth more here
than anything in the mod.

The startup one is the mod setting your render resolution, which is a full
video-mode change and inherently slow. Once per session.

For context, measured on the test hardware: **the plugin's own share of a frame
is 0.6%**, and after the first thirty seconds its worst single frame in a whole
session was **2.01 ms**. These are stalls elsewhere in the pipeline, not a
baseline cost — and a stall is exactly what an average cannot see, which is why
they are listed separately rather than folded into a frame-time number.

---

## 6. Recentring turns the world a few degrees (BY DESIGN)

**Severity: INFO.**

**What you see.** After a long stick spin, using your headset's own recentre
gesture rotates the world by a few degrees. Nothing else moves, and the gun does
not jump.

**What it is.** That gesture is your OpenXR runtime's recentre, not the mod's: it
makes your head's current facing the new forward. The mod re-samples its own
reference to match, and composes your hand against the same reference as the
view, so the weapon stays where it should be. Whether the few degrees come from
your head having turned during the spin or from tracking drift is unmeasured.

Use your headset's own recentre. Nothing needs setting up for it.

---

## 7. Titan HUD briefly shrinking on head rotation (FIX SHIPPED, UNCONFIRMED)

**Severity: LOW.**

**What you saw**, before this release: in a Titan, turning your head left or
right sometimes made the entire HUD shrink toward the centre for a few frames
and then come back.

**Where it stands.** The cause was found and the fix is in this build. The HUD
correction was gated on the camera sitting within about 2.5 cm of the origin —
and rotating your head swings your eyes around your neck by ten centimetres or
more, which is several times that. Move, exceed the bound, lose the correction;
stop, fall back inside it, get it back. In two measurement windows the
correction was being lost on roughly 40% of frames.

The bound is now 32 units, about 80 cm — more than any seated head movement,
while keeping two to three orders of magnitude of margin on what it is actually
there to exclude. `hud.fov_origin_max` in the ini if you ever need to change it.

**Listed as unconfirmed on purpose.** No run has been spent deliberately
reproducing the symptom since the fix went in, and it has not been reported
again. That is not the same as a test. If you see it, please say so.

---

## 8. The first launch looks wrong for the first minute (OPEN)

**Severity: MEDIUM**, and it only happens once per headset.

**What you see.** On the very first launch with a given headset, the image is
badly magnified — you are looking at the middle of a picture that has been
stretched to fill a panel it does not fit. On a new campaign the opening
cutscene is essentially unwatchable. Roughly a minute in, once you are actually
in the level, it snaps to the right resolution and stays right.

**Every launch after that is correct from the start**, including after a level
change or a restart.

**Workaround.** Let it settle, or skip the opening cutscene and come back to it.
If you would rather never see it, set `render.width` and `render.height` in the
ini to your headset's per-eye panel resolution before the first launch; the mod
then has nothing to derive.

**Where it stands.** The mod does not know your headset's panel size until the
OpenXR runtime tells it, and it deliberately waits for the game to be rendering
a world before changing the resolution to match. The runtime reports the panel
in the first ten seconds; a new campaign has no world for about forty-five,
because of the cutscene. So the gap is the cutscene, and everything in between
renders at the game's own 16:9 setting into a panel that is not 16:9.

Once the size is known it is written to `titanfall2vr.headset.cache` next to the
ini, keyed by headset, which is why this is a first-launch problem and not a
launch problem.

Closing the gap means applying the resolution earlier than a world. That was
attempted on 2026-09-09 and made things worse rather than better, in ways that
reached the gun and the projection layer, so it was **taken back out** and the
release ships the behaviour above. It is being redesigned separately, against a
baseline that is known good.

---

## 9. The view can latch at the wrong zoom for minutes (OPEN)

**Severity: HIGH** when it happens, and it is not reliably reproducible.

**What you see.** The world is at the wrong magnification and stays there — one
observed episode held for about four and a half minutes before clearing.

**Workaround.** None known. It has cleared on its own in every observed case.

**Where it stands.** Observed twice, both times with the frustum reading a fixed
117.5 degrees while the view should have been changing. **Unexplained.** It has
not been reproduced deliberately, no cause has been isolated, and it is not
known whether it is reachable from the shipped defaults or only from the
configuration it was seen under. If you hit it, the single most useful thing you
can report is **what you were doing in the ten seconds before it started** —
particularly whether you were aiming down sights, in a Titan, or changing level.

---

## 10. The game can freeze and have to be killed (OPEN)

**Severity: HIGH, and this is the worst thing in the alpha.** The same fault has
been recorded **six times** over several days of testing.

**What you see.** Everything stops. The picture in the headset stops updating
and does not come back. Sometimes the game then closes to the desktop on its
own; sometimes it just sits there and **you have to end the process yourself**
from Task Manager. The freeze is not brief — measured at **over 38 seconds** of
no frames at all in the most recent one, and it never recovered.

**When.** There is no reliable trigger yet. It has happened during a level load
and it has happened in ordinary play well into a session — the most recent one
came during a cutscene, after four minutes in which the mod was hitting its
frame budget with 1.8% of frames late. It is not something you can see coming.

**Workaround.** None. If the picture stops and stays stopped for more than
twenty seconds or so, it is not coming back: end the process and restart. You
will lose progress since the last checkpoint.

**Where it stands.** All five have the same signature. The fault lands in
Virtual Desktop's own Direct3D 11 path, on an object that has already been
freed — the recorded register state is the debug fill pattern Windows writes
over released heap memory. No resolution change and no swapchain rebuild
happened in either of the runs where that was checked.

**The mod's own code has now been audited line by line for this**, and the
current answer is that it is not the cause. Every place the mod could block the
game's render thread was traced and ruled out by name, its share of a frame is
0.6%, and in the one run where the stall could be timed the mod had already
finished its work and was waiting to be called again.

Two honest caveats, because "not ours" without proof is how this kind of bug
survives a release:

- The crash record walks 24 stack frames and **hit that limit exactly**, still
  inside the runtime. So "no mod frame on the stack" is a limit of the record,
  not a fact about the crash.
- One narrow region — three OpenXR calls made while handling session events —
  is not covered by the marker that places the rest.

**The current best explanation is the streaming link**, not the renderer. The
in-play stalls in issue 5 are a fixed ~90 to 100 ms inside the runtime's submit
call, which is the shape of a link timeout rather than of slow rendering, and a
link that stops recovering would take the frame loop with it. That is a
hypothesis and it has not been tested.

**If it happens to you, try these before assuming it is the mod:** a wired
connection instead of Wi-Fi, a lower streaming bitrate, or a different codec.

Two more things are known. **The freeze comes first and the fault comes later** —
39 and 43 seconds in the two runs where the frame loop could be timed. The fault
is a consequence, not the cause.

And **it does not follow a slowdown.** In both timed runs the mod was hitting its
frame rate when the loop stopped: 82 frames per second in one, 1.8% of frames
late in the other, with the runtime reporting the session focused and displaying.
It stops mid-stride from a healthy state. If you get frame hitches in a heavy
fight that is a different problem, issue 5, and it is not a warning sign for
this one.

**This build records more about it than the last one did.** Whatever the runtime
says about the session at the moment the loop stops is now written to your log
instead of being filtered out of it, which is the single most useful thing a
report can carry.

**A note for anyone on a smaller graphics card.** Running out of video memory
was ruled out for this — but it was ruled out on a 32 GB card, which proves
nothing about an 8 GB one. If you hit this on a smaller card, say so and say which card: the mod
records video-memory pressure and allocation failures in your log, so your
report answers that question for everyone. Note the mod now ships at 75%
resolution rather than full panel height, which cuts the per-frame pixels by
44% and takes some pressure off a small card before you change anything.

**If it happens to you, please report it**, and say what you were doing and
roughly how long you had been playing. Your log is in `%LOCALAPPDATA%\titanfall2vr\`
as `titanfall2vr.log` and it records the fault. Three sightings is not enough
to find a pattern; more would help.

## 11. Fades and loading overlays sit on the right hand, not on the whole view (OPEN)

**What you see.** Entering a level, the loading screen's "Press A to continue"
overlay and the fade to black that follows are drawn as a rectangle in front of
your right hand instead of covering the whole view. The reticle sits in the
same rectangle. With the controller out of view the rectangle sits in front of
your face. Play is not affected; it is cosmetic.

**Workaround.** None worth having. There is a mode that puts the overlay and
the fade in front of your face instead, but it takes the reticle off the hand
and shots then go where the view points rather than where the gun does, which
is a worse trade than the one it fixes.

**Where it stands.** Measured over seven headset runs on 2026-09-10. The game
draws its flat HUD pass — reticle, overlays, fades — where the ATTACK angles
point, and this mod writes the controller's aim into those angles so that
shots follow the hand. Everything of ours that could move that pass was
switched off one subsystem at a time and the rectangle stayed on the hand;
switching off the aim feed alone moved it to the head. The offset is applied
by the engine below the HUD's own widget state (the widgets read as centred
on every draw while the rectangle sat on the hand), so a fix that keeps the
reticle on the gun needs the draw path that positions that pass, which has
not been located. Under investigation.

## 12. The main menu is cropped at the sides on a near-square headset (OPEN)

**What you see.** On a headset whose per-eye panel is close to square, the
game's 16:9 main menu is drawn fitted to the height, with its sides cut off,
on every launch. After you have been in a level and come back out, the same
menu can draw small and centred instead. Play is not affected.

**Workaround.** None that keeps the world right. The menu is fully usable.

**Where it stands.** Measured 2026-09-10 over five launches. The engine has a
letterbox of its own (threshold 1.59, goal 1.6) and applies it to any squarer
buffer, menu and world alike. This mod lowers that floor so the WORLD fills the
buffer instead of getting bars; the same setting is what crops the menu. The
engine reads the two values when the video mode is set and keeps them for the
session, so the menu cannot have one value and the world the other without a
second mode change per level, which is issue 1. The world is the one that
matters; the crop stays. A headset with a 1.6 or wider panel, such as the
Play For Dream MR, never sees it.

## 13. The gun sat high and to the left in the first campaign level (FIXED)

**What you saw.** Loading the first campaign level, the opening cutscene handed
you control with the rifle about two feet above and a foot left of your hand.
The training level and the gun range were fine.

**Fixed 2026-09-11, wearer-confirmed.** The rifle is on the hand from the frame
the level starts, with no snap.

**What it was.** Not the camera anchor, which was the first suspect and is
sound. Every gun fits itself by measuring its own `R_HAND` attachment -- where
that model puts the hand -- and applying the difference from the one weapon the
grip was dialled on. The V-47 Flatline was the first weapon in the game ever to
run that measurement in anger: the eight weapons measured by the 2026-09-03
census are seeded from their recorded values and never re-measure, and the
Flatline is not offered at the range where that census ran.

Two faults met on it. The measurement was being taken in the wrong reference
frame in a headset run -- `GetAttachment` builds its bones at the viewmodel's
current fields, so the attachment must be measured against those same fields,
and a flat census cannot tell the two frames apart because with no hand-driven
pin they are the same pose. And the latch accepted the first *steady* second
rather than the first *idle* one, so in a firefight it recorded a combat pose
as the weapon's grip point. Because a latch is first-wins and permanent for the
session, one bad second held for the rest of the run no matter how long you
then stood still.

**What changed.** The measurement is taken in the pinned pose, which a live
control now verifies every run against a known row -- the pistol reproduces its
census value to 0.01 units and the Flatline to 0.00. The latch additionally
requires the player at rest: not in ADS, and drifted under 2 units over the
sample window. A measured `R_HAND` further than 9/4/5 units from the reference
is refused outright, so a weapon can fall back to the dialled grip rather than
take a large wrong correction. And the Flatline now has a census row of its
own, so it is right on the frame it is drawn instead of waiting fifteen seconds
and then visibly snapping.

**Still open, and deliberately not papered over.** A latch is still first-wins,
so a wrong-but-plausible measurement would hold for a session; requiring two
idle windows to agree before committing would fix that and is post-launch work.
Any weapon without a census row still snaps once on first pickup -- a range
session would census the rest of the campaign's weapons and end that. And the
proper end state reads `R_HAND` from the model's rest pose in the studiohdr,
which needs no animation, no waiting and no table at all; the nine rows then
become its test set.

## 14. The settings panel may name the wrong headset (OPEN, not ours)

**What you see.** The VR settings panel names your headset as something else.
On a Play For Dream MR it reads "Meta Quest 3".

**Does it matter?** No. It is a label. The resolution, the field of view and
the lens geometry are all measured from your actual headset and are correct,
and the per-eye size shown underneath the name is the real one. The headset
memory keys on that measured geometry as well as the name, so two headsets
never share settings even when both report the same name.

**Where it stands.** The name comes from `XrSystemProperties::systemName`,
which is the field OpenXR specifies for it, read the way the specification
says. Virtual Desktop's VDXR runtime puts "Meta Quest 3" there for a Play For
Dream MR -- a headset Virtual Desktop has supported for over a year and names
correctly everywhere in its own interface, including the performance overlay on
L3 + R3. So this is not a missing device profile and it is not something this
mod can detect its way around: the runtime's OpenXR layer simply does not
report what the rest of Virtual Desktop knows.

The panel attributes the name rather than asserting it -- "as reported by
VirtualDesktopXR" -- so the question has a visible answer. The run log also
records `vendorId` and `systemId` now, in case those distinguish where the name
does not; if they do, a later version can name the headset properly. Worth
reporting to Virtual Desktop, since only they can fix the source.

---

## 15. Cutscenes are locked to your head (OPEN)

**Severity: MEDIUM.** It is the one thing in this alpha that can make you feel
ill.

**What you see.** In the rendered 3D cutscenes the camera is pinned to your
head rather than to the scene. The picture moves with you instead of staying
where the world is, which is the arrangement that provokes motion sickness
fastest -- your inner ear reports a turn and your eyes report none.

**Workaround.** Keep your head still through cutscenes. That is not a fix and
it is not comfortable advice, but it works, and the cutscenes are short.

**Where it stands.** Reported by the wearer during campaign play before the
alpha shipped. No instrumented run has been spent on it, so nothing here is
measured and no cause is claimed: the camera path a cutscene drives has not
been compared against the one ordinary play drives. It is listed because it is
the defect most likely to end someone's session, not because it is understood.

---

## 16. The loading screen shrinks to a small window just before it clears (OPEN)

**Severity: LOW.** Cosmetic, and it resolves the moment you continue.

**What you see.** At the end of a level load, the full loading screen collapses
to a smaller window in the middle of your view. Behind it you can see the VR
scene, frequently a part of the level you would never normally be looking at
from an angle the game never intended to show you. It is jarring the first
time, and it looks like something has gone wrong.

**Workaround.** Nothing is wrong. It can be a bit jarring, but just wait for
it to finish loading and press **A** to continue as instructed. The level
then starts normally.

**Where it stands.** Reported by the wearer; no instrumented run has been spent
on it, so no cause is claimed here.

It is probably the same defect as [issue 11](#11-fades-and-loading-overlays-sit-on-the-right-hand-not-on-the-whole-view-open),
which is the game's flat pass -- loading overlay, reticle, fades -- being drawn
into a small quad instead of across the whole view. That is a guess and it is
not measured: issue 11's finding is specifically that the pass follows the
ATTACK angles onto the hand, and a window centred in the view is not that. The
two are filed separately until something connects them.
