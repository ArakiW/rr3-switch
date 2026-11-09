import pathlib

root = pathlib.Path(r"E:\AI\RealRacing3-switch")

# ---------------------------------------------------------------- the brief
brief = """You are implementing ONE fix for a Nintendo Switch port of an Android game
(Real Racing 3 guest, libnx + Mesa): a VISIBLE TEXTURE FAULT that has been
there for weeks.

Files live under E:\\AI\\RealRacing3-switch\\native-switch\\.

=============================================================================
THE SYMPTOM
=============================================================================

In the race, one car renders MAGENTA -- the renderer's fallback colour --
while the other car is textured correctly, and a HUD widget renders as a flat
GREY-WHITE blob.  The magenta car is the featured weekly event's car (the
guest asks for "lexus_is_f350_catalunya" sprites).

=============================================================================
WHAT WAS MEASURED -- AND WHAT WAS MEASURED WRONG
=============================================================================

A FIRST READING OF THIS TASK WAS WRONG, and it is recorded here so that you
do not repeat it.  An earlier count claimed "45 asset-list entries the port
could not resolve, so the manifest kept the server's digest for them".  That
count was a MISREADING of the log: the counter grepped only the PATH field
of the "assetlist-entry" lines and ignored the rest of the line.  Those 45
lines are REWRITES THAT SUCCEEDED.

WHAT THE LOG ACTUALLY SAYS (the same round, verbatim):

    phase=assetlist-entry path=/assets_2048x1536/common/sprites.android.atlas
      md5=2ab3d810182c1b4baf275e5d11ab4981->72bcfd205a917770eebb8a1a9e3dcbd0
      size=39460 source=rr3gui0:/files/.depot/gui_assets/2048x1536/common/
      sprites.android.atlas
    phase=asset-invariant path=/assets_2048x1536/common/sprites.android.atlas
      published=72bcfd205a917770eebb8a1a9e3dcbd0
      served=72bcfd205a917770eebb8a1a9e3dcbd0 agree=1 found=1
      src=rr3gui0:/files/.depot/gui_assets/2048x1536/common/sprites.android.atlas
    phase=cc-asset-req path=/assets_2048x1536/common/sprites.android.atlas
      want_md5=72bcfd205a917770eebb8a1a9e3dcbd0 digest_field=wire candidates=13
      tried=.depot/assets_2048x1536/common/sprites.android.atlas|files/.depot/...

and for the magenta car:

    phase=repair-candidate path=assets_2048x1536/events/map/featured/
      weekly_events/lexus_is_f350_catalunya/sprites.android.atlas
      resolved=1 digest=debbef8a79a15e0b2aeb475039e2beab
      card=sdmc:/switch/rr3-native-rehost/.depot/gui_assets/2048x1536/events/
      map/featured/weekly_events/lexus_is_f350_catalunya/sprites.android.atlas
      present=0
      source=sdmc:/switch/rr3-native-rehost/.depot/assets_2048x1536/events/...

  => THE FILE IS FOUND, THE DIGEST IS PUBLISHED, THE SERVED DIGEST MATCHES
     THE PUBLISHED ONE (agree=1, found=1), AND THE GUEST ASKS FOR EXACTLY
     THAT DIGEST (want_md5=72bcfd2...).

  => ALSO: no texture failed to LOAD.  Every "unable to load file" line in
     the round was counted by extension: 16 .decomp (audio), 6 .gui, 1 .evt,
     and NOT ONE texture.

**⇒ SO THE TEXTURE IS CORRECTLY SERVED AND THE FAULT IS DOWNSTREAM OF
THAT.**  The bytes arrive and the digest is right; the surface still renders
in the fallback colour.  The possibilities are:

  (a) THE TEXTURE UPLOAD IS REJECTED BY THE DRIVER.  The guest's atlases are
      "sprites_0.rgb.pvr.z" -- a COMPRESSED PVR texture.  The port serves
      the bytes; if the host driver cannot decode that format, the upload
      fails and the material samples an incomplete texture.  **The port
      never checks.**  It traces glTexImage2D / glCompressedTexImage2D /
      glCopyTexSubImage2D and COUNTS the calls; it never reads glGetError()
      afterwards.
  (b) THE SHADER NEVER BECOMES USABLE.  The port traces glCompileShader and
      glLinkProgram and counts the calls; it never reads
      glGetShaderiv(GL_COMPILE_STATUS) or glGetProgramiv(GL_LINK_STATUS),
      and never reads the driver's info log.  A failed link and a good link
      are the SAME OBSERVATION in the log.
  (c) THE GUEST'S ATLAS AND THE SERVED ATLAS DISAGREE about the region the
      surface samples, which the port cannot see either way.

**⇒ (a) AND (b) ARE THE TWO THE PORT CANNOT CURRENTLY TELL APART.  THAT
IS WHY THIS TASK IS AN INSTRUMENT FIRST AND A FIX SECOND.**

=============================================================================
YOUR JOB
=============================================================================

  1. MAKE THE FAILURE VISIBLE.  In the GL tracer
     (native-switch/source/guest_view_trace.cpp, which already hooks 23 GL
     entry points):

       a. after every guest glCompileShader / glLinkProgram, read
          glGetShaderiv(GL_COMPILE_STATUS) and glGetProgramiv(GL_LINK_STATUS)
          and the driver's info log, and report a FAILURE -- one line per
          failure, naming the program id and the driver's words;
       b. after every guest texture upload (glTexImage2D,
          glCompressedTexImage2D, glTexSubImage2D, glCopyTexSubImage2D),
          read glGetError() and report the ones that are not GL_NO_ERROR,
          with the level, the internal format, the format/type and the
          width/height, and with the asset path if the tracer can know it.

     THE PORT'S OWN LOG VOLUME IS A MEASURED COST, so: NO line for a
     successful call (there are tens of thousands), a TOTAL budget on the
     failure lines per process, and one line saying the budget was spent.
     Do not regress the frame rate.

  2. THEN FIX WHAT THE INSTRUMENT FINDS, if it is findable from the source.
     If the fault is in the ASSET PATH -- a texture the port serves in a
     format the driver rejects, or an atlas the port serves from the wrong
     tree -- say so and fix it.  If the fault is not findable from the
     source, say so plainly and make the instrument good enough that the
     next round on the console names it.  **An honest "the instrument now
     tells us which of (a) and (b) it is" is a complete answer.**

  3. ADD A TEST for whatever you fixed, in the project's style
     (native-switch/tests/, driven by native-switch/tests/test_*.py, run by
     the gate).  If the fix is the instrument alone, the test is that the
     instrument reports a failure for a deliberately broken shader and stays
     silent for a good one.

=============================================================================
RULES
=============================================================================

  - C++17, no GNU extensions, M_PI is not available.
  - The existing comment style: every non-obvious block explains WHY and
    cites a measurement where there is one.  That is the house style and
    it is the documentation.
  - Do NOT touch the pursuit, the race-state probe, the cond-wait code or
    the input code.  They are under active measurement or under separate
    work.
  - Do not run the emulator and do not build.  Output the code.

=============================================================================
DELIVERABLE
=============================================================================

  1. One paragraph: what now happens that did not before.
  2. The complete contents of every new file, and for every EXISTING file
     you touch, the exact hunks to apply (with enough surrounding context
     that they can be applied unambiguously).
  3. The test you added, and how it fails without the fix.
  4. Your own review: for each change, the way it can go wrong, and the ONE
     measurement that would show it.
  5. The single most likely way this is still wrong, and the one
     measurement that would show it.
"""
(root / ".build" / "agy-asset-resolver-fix.txt").write_text(brief, encoding="utf-8", newline="\n")
print("brief rewritten")

# ---------------------------------------------------------------- the doc
doc = root / "docs" / "OPEN-ISSUES.md"
text = doc.read_text(encoding="utf-8")

marker = "### 9.16"
if marker in text:
    print("doc: already present")
else:
    tail = """
### 9.16 A CORRECTION: the magenta car is NOT a path-resolution bug

**THIS CORRECTS SECTION 9.4's LAST PARAGRAPH, WHICH WAS WRONG.**

The earlier reading claimed that 45 asset-list entries could not be resolved
locally, so the manifest kept the server's digest for them, and that the
port therefore served the guest a digest for a file it did not have.  **That
count was a misreading of the log**: the counter grepped only the PATH field
of the "assetlist-entry" lines and ignored the rest of the line.  Those 45
lines are REWRITES THAT SUCCEEDED.

WHAT THE SAME LOG ACTUALLY SAYS:

    phase=assetlist-entry path=/assets_2048x1536/common/sprites.android.atlas
      md5=2ab3d810...->72bcfd205a917770eebb8a1a9e3dcbd0 size=39460
      source=rr3gui0:/files/.depot/gui_assets/2048x1536/common/sprites.android.atlas
    phase=asset-invariant path=... published=72bcfd20... served=72bcfd20...
      agree=1 found=1 src=rr3gui0:/files/.depot/gui_assets/2048x1536/common/...
    phase=cc-asset-req path=... want_md5=72bcfd20... digest_field=wire candidates=13
      tried=.depot/assets_2048x1536/common/sprites.android.atlas|files/.depot/...

and for the magenta car itself:

    phase=repair-candidate path=assets_2048x1536/events/map/featured/
      weekly_events/lexus_is_f350_catalunya/sprites.android.atlas
      resolved=1 digest=debbef8a79a15e0b2aeb475039e2beab
      card=.../.depot/gui_assets/2048x1536/.../sprites.android.atlas present=0
      source=.../.depot/assets_2048x1536/.../sprites.android.atlas

**⇒ THE FILE IS FOUND, THE DIGEST IS PUBLISHED, THE SERVED DIGEST MATCHES
THE PUBLISHED ONE (agree=1, found=1), AND THE GUEST ASKS FOR EXACTLY THAT
DIGEST.**  And no texture failed to LOAD: every "unable to load file" line
in the round was counted by extension and there is not one texture among
them.

**⇒ SO THE TEXTURE IS CORRECTLY SERVED AND THE FAULT IS DOWNSTREAM OF
THAT.**  What is left is the part the port cannot currently see:

  (a) THE TEXTURE UPLOAD IS REJECTED BY THE DRIVER.  The guest's atlases
      are "sprites_0.rgb.pvr.z" -- a COMPRESSED PVR texture.  The port
      serves the bytes; if the host driver cannot decode that format the
      upload fails and the material samples an incomplete texture.  The
      port traces the upload calls and COUNTS them; it never reads
      glGetError() afterwards.
  (b) THE SHADER NEVER BECOMES USABLE.  The port traces glCompileShader
      and glLinkProgram and counts the calls; it never reads
      GL_COMPILE_STATUS or GL_LINK_STATUS, and never reads the driver's
      info log.

**⇒ (a) AND (b) ARE THE TWO THINGS THE PORT CANNOT CURRENTLY TELL APART,
and that is the whole problem: the instrument does not exist.**  So the
work is an instrument first -- read the shader status and the upload error
after every guest call that can fail, and report only failures, with a
budget -- and a fix second, if the fault is findable from the source.

**⇒ AND THE LESSON, WHICH IS THE THIRD TIME THIS PROJECT HAS PAID IT IN
ONE WEEK: a count taken from a log is only as good as the fields that were
actually read.**  Three readings of the same rounds were wrong in a row --
the 2x2 (the cells were not comparable), the flip hypothesis (killed by its
own experiment), and this one (the counter read a field and ignored the rest
of the line).  **⇒ PRINT THE WHOLE LINE, NOT A FIELD OF IT, AND GREP FOR
WHAT THE LINE SAYS, NOT FOR A SUBSTRING OF IT.**
"""
    doc.write_text(text.rstrip("\n") + "\n" + tail, encoding="utf-8", newline="\n")
    print("doc: 9.16 appended")
