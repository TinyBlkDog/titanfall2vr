titanfall2vr @TF2VR_VERSION_TAG@
================================================================

Titanfall 2's campaign in VR. Single player only.

titanfall2vr.dll sha256:
  @TF2VR_DLL_SHA256@


READ THIS BEFORE YOU JUDGE THE PICTURE
----------------------------------------------------------------

IT SHIPS AT 75% RESOLUTION, NOT 100%.

Nothing in this mod measures your graphics card, so the default
is set for one that may not be fast. If the image looks soft,
that is why, and it is one setting away.

Open the settings panel (click BOTH THUMBSTICKS) and raise
Resolution. 100% is your headset's own full panel; it goes to
200%. Raise it until the frame rate stops holding, then come
back a step.

The change applies when you close the panel.


INSTALL
----------------------------------------------------------------

1. Install Northstar and check the game launches through it.
   https://northstar.tf

2. Close the game.

3. Copy BOTH of these into your Northstar plugins directory:

       titanfall2vr.dll
       titanfall2vr.ini

   That directory is inside your Titanfall 2 install, at:

       Titanfall2\R2Northstar\plugins

   Create it if it is not there.

4. Set your OpenXR runtime. This was tested through Virtual
   Desktop (VDXR) on Play For Dream MR and Meta Quest 3. Other
   runtimes are untested rather than unsupported.

5. Launch through Northstar and start the campaign.

Windows may warn about an unsigned DLL being injected into a
game. That is what this is. The hash above is the file you should
have; check it if you did not download this from the project's
own release page.

Steam users: turn the Steam overlay off for Titanfall 2. It
injects into the same process and has not been tested alongside
this.


FIRST RUN
----------------------------------------------------------------

Start the campaign and play. Nothing needs configuring -- the VR
stack comes up with the game and no keyboard key is bound.

To look at the settings, click BOTH THUMBSTICKS at once (L3 +
R3). Either stick moves between rows, A changes a setting, B goes
back, the grips change category, and the same chord closes it.
Changes are saved as you make them.


WHAT TO EXPECT FROM AN ALPHA
----------------------------------------------------------------

Read KNOWN-ISSUES on the project page before reporting anything.

THE ONE TO KNOW FIRST: the game can lock up and have to be closed
from Task Manager. Seen a handful of times over several days of
testing, with no known trigger. If the picture stops and stays
stopped for more than about twenty seconds it is not coming back,
and you lose progress since the last checkpoint. Please report it
with your log from %LOCALAPPDATA%\titanfall2vr\, and say what you
were
doing and roughly how long you had been playing.


FULL DOCUMENTATION
----------------------------------------------------------------

README, INSTALL, CONTROLS, KNOWN-ISSUES and BUILDING are on the
project page. CONTROLS covers the whole controller map, all 69
settings, and every action you can bind to a key. This build binds
no keyboard key by default.


LICENCES
----------------------------------------------------------------

titanfall2vr is MIT licensed. See licenses/ for its text, for
every dependency's, and for THIRD_PARTY_NOTICES.md.

This mod contains no Titanfall 2 assets and no game code. It is
not affiliated with Respawn Entertainment or Electronic Arts. You
need a legitimately owned copy of the game.
