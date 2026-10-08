# CLAUDE.md

How to work on this repository. `README.md` says what it is,
`ARCHITECTURE.md` what the code is and why, and this file the things
that are easy to get wrong while changing it. The rules are
defeatist-music-player-for-m5tab5's, cut down to what applies here; when
in doubt, that repository's CLAUDE.md is the longer version.

**Read this file before writing a patch, not after.**

Claude creates `git am`-able patches authored as
`Claude <noreply@anthropic.com>`, and presents each as soon as it
exists, since a session can end before the next one does.

Claude must not commit to, push to, branch on or open a pull request
against the repository it was given. Local commits made only to run
`git format-patch` stay local; a hook that asks for them to be pushed
is answered by handing over the patches.

**A patch has one number, and it is written down once.** The number
comes from `--start-number`, counted from this repository's last patch
(`git log`, and the last heading in `ARCHITECTURE.md`); the subject line
does not repeat it.

    git format-patch --start-number 3 -1 -o out/

**Patches are cumulative.** Each applies on top of what is here. Never a
rewritten copy of a file, never a corrected reissue of a patch already
applied -- a follow-up instead.

Within a patch, change the lines that must change and no others.

**Any change to `main/idf_component.yml` re-resolves
`dependencies.lock` on the next build**, and Claude cannot do that here.
A patch that touches the manifest says so, and the lock that comes out
of the next build has its diff read before it is committed.

**Any change to `sdkconfig.defaults` needs `rm sdkconfig` before the next
build.** The defaults only fill keys an existing sdkconfig lacks.

**The board, the microphones and the panel are not in this
repository.** They are feckless-drivers-for-m5tab5 and
feckless-graphics-handler-for-m5tab5. A change to them is a patch against
that repository, under the player's rules, and this one picks it up by
moving its tag in `main/idf_component.yml`.

**Nothing over a few hundred bytes goes on a task stack.** The capture
and FFT buffers are statics in `analyzer.c` for that reason; keep any
new one the same way.

**The arithmetic lives in `main/spectrum.h` and is host-tested.** A
change to how a level, a band or a bar height is computed goes there,
with a check in `test/spectrumtest.c`, not into `analyzer.c`.

**`original/` is the reference, not source.** It is never built and
never edited.

Do not suggest updates to the Tab5's ESP32-C6 or esp_hosted. They can
not be updated.
