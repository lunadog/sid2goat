# sid2goat — SID → GoatTracker 2 SNG converter

## What this is

`sid2goat.cpp` reverse-engineers compiled GoatTracker 2 / GTUltra C64 SID binaries
back into editable `.sng` files. It works by disassembling the embedded 6502
player code (not just reading raw bytes) to locate instrument columns,
wavetable/pulsetable/filtertable/speedtable boundaries, and pattern/song
pointer tables, then applies inverse byte-encoding transforms to recover the
GoatTracker editor's original representation.

## Build

```
cd /home/claude
g++ -std=c++17 -O2 -o sid2goat sid2goat.cpp gsong.cpp
# debug build with column-search tracing:
g++ -std=c++17 -O2 -DDEBUG_COLS -o sid2goat_dbg sid2goat.cpp gsong.cpp
# WTBL transform tracing:
WTBL_DEBUG=1 ./sid2goat_dbg file.sid /tmp/x.sng 2>&1 | grep "^k="
```

`pip install ... --break-system-packages` if any Python tooling is needed for
inspection scripts (this session used inline `python3 -c` for byte-level
disassembly cross-checks rather than a persistent script).

## Key files

- `sid2goat.cpp` — the converter (~700 lines)
- `gsong.cpp` / `gsong.hpp` — `.sng` file I/O (`gt::Song` struct, save/load)
- `greloc.c` / `greloc.h` — GT2.73 relocator/packer source, used as **read-only
  ground truth** for table layout and forward-encoding logic (not compiled —
  just grepped/read for reference)
- `2_0greloc.c` — GT2.0-era relocator source; confirms the v2.0/v2.06 era has
  **no WTBL/FTBL encoding transform at all** (raw `fwrite`, no `insertbyte`
  per-byte transform)
- `gtable.c` / `gtable.h` — table helpers (`makespeedtable` etc.)
- `/mnt/user-data/uploads/gt2reloc.c` — actual GT2.73 command-line
  relocator/packer source. Could in principle be compiled for real round-trip
  verification (`.sng` → `.sid`, byte-compare against original), but depends
  on `bme.h` (GUI library) not present in this environment. **Worth revisiting
  if round-trip verification is needed** — may be possible to stub out the
  GUI-only parts and isolate just the relocate-to-PRG function.
- Reference player sources in `/mnt/user-data/uploads/`: `player1-4.s` (v2.0),
  `2_06_player1/1b/2/2b/3/3b.s` (v2.06), `2_35_player.s` (v2.34/2.35),
  `2_55_player.s` (v2.51), `2_76_player.s` / `2_76_altplayer.s` (v2.73),
  `altplayer.s`. `goattrk2.exe` / `gtultra.exe` are Windows binaries —
  **no WINE installed**, can't run them.
- `/tmp/dump_sng` — small helper binary (built from `/tmp/dump_sng.cpp`,
  linked against `gsong.cpp`) that prints instrument table + WTBL contents
  from a `.sng` file for manual sanity-checking. Rebuild with:
  `g++ -std=c++17 -O2 -I. -o /tmp/dump_sng /tmp/dump_sng.cpp gsong.cpp`
- `/tmp/cmdargcheck` — helper that checks pattern command (toneporta/portaup/
  portadown/etc.) arg values against STBL range for a given `.sng`.

## Regression / test files (`sid2goat/`)

All files live in `sid2goat/`. Run with `./sid2goat file.sid /tmp/out.sng`.

| File | Era | Expected output |
|---|---|---|
| `For_Attitude_8.sid` | v2.06 | `legacy_format=true`, bmi/verbatim, WTBL=56 PTBL=16 FTBL=21 STBL=7 |
| `gt2.0.sid` | v2.0 | `legacy_format=true`, bmi/verbatim, WTBL=31 PTBL=15 FTBL=6 STBL=3 |
| `1982_Xyce.sid` | v2.2+ | WTBL=115 PTBL=16 FTBL=15 STBL=6 |
| `MoreFunToComputeTSJ_4x.sid` | v2.2+ | WTBL=172 PTBL=31 FTBL=149 STBL=4 |
| `$3LastNight_Jammer.sid` | v2.2+ | WTBL=144 PTBL=36 FTBL=45 STBL=8 |
| `rs232-thewitchinghour.sid` | v2.76 (not legacy) | nopulse=0 nofilter=1 noinsvib=1 fixedparams=1, WTBL=4 PTBL=3 FTBL=0 STBL=1 |
| `Nineteen.sid` | v2.2+ | WTBL=49 PTBL=31 FTBL=65 STBL=8 — **verified correct** |
| `N_O_U_A_G_O_O_T_L_A_part_4.sid` | v2.2+ | noinsvib=1 fixedparams=0, WTBL=55 PTBL=5 FTBL=20 STBL=1 — **verified correct** |
| `sids/FamiCommodore.sid` | v2.76 | noinsvib=1 fixedparams=0, WTBL=171 PTBL=64 FTBL=2 STBL=8 — **verified correct** |

These two newest files are confirmed v2.2+ "speedtable era" — but specifically
*which* sub-version (2.2, 2.34/2.35, 2.51, 2.73) can't be pinned down further:
the three reference sources checked (2.35/2.51/2.73) are byte-identical on
every marker tried (`.IF (NUMSONGS>1)` init guard, `CMP #$10` wavetable
threshold, `BPL mt_wavenoteabs` R-byte polarity).

## Architecture: how the converter works

1. Parse PSID/RSID header, resolve load address.
2. Disassemble the 6502 code to find:
   - `icols[]` — instrument-cluster column base addresses, found via
     clustering analysis of `LDA abs,Y` operand addresses (not a single
     fixed anchor — a statistical cluster of close-together, evenly-spaced
     references). **Important**: per-instrument column data is read via
     `sid[s2f(base + 1 + i)]` for instrument `i` (0-indexed) — the `+1` is
     easy to forget when manually cross-checking addresses by hand (cost real
     time this session — see Gotchas below).
   - song/pattern pointer tables (`mt_songtbllo/hi`, `mt_patttbllo/hi`)
   - WTBL/PTBL/FTBL boundaries via a **jump-consistent self-referencing
     table search** (`find_jump_consistent_len`): these three tables encode
     loop/jump points as `(0xFF, R)` entries where R is a 1-indexed jump
     target, so a valid length N is one where every internal `0xFF` entry's
     R is in `[0,N]`.
   - STBL boundary: **STBL has no loop/jump structure at all** (just flat
     `(L,R)` speed pairs), so it can't be found the same way — its length is
     computed as whatever remains between FTBL's end and the orderlist start,
     after accounting for 2 "extra zero" padding bytes that greloc.c inserts
     whenever the song uses any vibrato/portamento/toneporta/funktempo
     *command* (regardless of whether any used arg value is actually nonzero).
3. Try multiple `Combo` candidates — different `ncols_try` totals and
   `(a,b,c,d)` flags for which optional instrument columns
   (pulse/filter/vibrato-pair/gatetimer+firstwave-pair) exist — then pick the
   "best" one via a sort comparator. **This sort can have genuine ties** when
   `2c + 2d` has more than one boolean solution for a given `ncols_try`
   (e.g. both `c=1,d=0` and `c=0,d=1` satisfy "+2 more columns needed"). Both
   tied candidates can pass every structural self-consistency check — only
   real evidence from disassembly can settle it (see Bugs below).
4. Apply inverse transforms (WTBL_L, WTBL_R, FTBL_L need inversion; PTBL is
   passthrough — see below) to convert compiled-binary encoding back to
   editor-format bytes.
5. Unpack patterns, build the `gt::Song` struct, save as `.sng`.

## Player-version differences (the actual substance of this debugging effort)

### v2.0 / v2.06 ("legacy_format" in code)
- WTBL delay threshold: `CMP #$08` (values 0-7 = delay)
- **No `+0x10`/`-0x10` shift mechanism at all** — compiled byte == `chnwave`
  value directly, used verbatim once `>= 8`. Confirmed via `2_0greloc.c`:
  literal `fwrite(&ltable[WTBL], wavetblsize/2, 1, songhandle)`, no per-byte
  `insertbyte`-based transform.
- WTBL R-byte polarity: detected per-file by disassembly — `gt2.0.sid` is `BMI`
  (verbatim), `For_Attitude_8.sid` is `BPL` (XOR needed). Do NOT assume v2.0/v2.06
  always implies BMI; use the `wave_entry` scan result.
- No STBL section at all
- Detected via: song/pattern pointer tables sitting *inside* the would-be
  table region (sets `legacy_format=true`), which **bypasses
  `wtbl_l_inv`/`wtbl_r_inv` entirely** and uses raw bytes directly — this is
  correct, not a shortcut.
- Init routine does an unconditional ×3 multiply for song count (no
  `.IF` guard).

### v2.2+ "speedtable era" (confirmed v2.18 through at least v2.73)
- WTBL delay threshold: `CMP #$10` (values 0-15 = delay)
- `NOWAVEDELAY` is a **genuine per-song compile flag** (not per-version!),
  set by greloc.c (`insertdefine("NOWAVEDELAY", nowavedelay)`) based on
  whether *this specific song's* WTBL editor data contains any value in the
  delay range `[0,15]` at all:
  - `NOWAVEDELAY==0` (song uses delay rows, our bool `nowavedelay=false`):
    compiled code is `CMP #$10; BCS mt_nowavedelay; ...; SBC #$10; STA
    chnwave,x`. Compiled value = `editor_value + 0x10` for normal rows
    (reversed by subtracting 0x10). **The `SBC #$10` instruction's
    presence/absence in the disassembly is the only reliable signal** — do
    not infer this from specific byte *values* present in the table (that
    heuristic produced a real bug, see below).
  - `NOWAVEDELAY!=0` (song never uses delay rows, our bool
    `nowavedelay=true`): simpler compiled code, just `BEQ
    mt_nowavechange; ...; STA chnwave,x` — **no `CMP`/`SBC` at all**.
  - Verified the inverse transform (`wtbl_l_inv`'s two branches) is
    mathematically exact against greloc.c's forward logic (lines ~1270-1271:
    `wave &= 0xf` for the "silent" range `[WAVESILENT,WAVELASTSILENT]`, then
    `wave += 0x10` iff `!nowavedelay` and value is in the non-delay/silent
    range) by hand-tracing concrete byte values in both directions.
- WTBL R-byte polarity: `BPL` (bit7 set = relative) — **opposite** of
  v2.06's `BMI`, needs XOR 0x80 to translate. (`wtbl_r_needs_xor` in code,
  detected via disassembly, not by version inference.)
- Real speedtable/effectjumptbl mechanism: `LDA $xxxx,Y` with
  `Y=mt_chnparam` as a genuine STBL index, vs v2.06's raw continuous-tick
  args used directly with no table indirection.
- Init routine wraps the ×3 multiply in `.IF (NUMSONGS > 1)` — compiles to a
  trivial 4-byte form when `NUMSONGS==1`.
- **FTBL and PTBL encoding confirmed identical to v2.06** (same `BPL`/`BMI` +
  `ASL` passband-bit extraction for FTBL; same general dispatch structure for
  PTBL) — checked this session by direct source comparison
  (`2_06_player1.s` vs `2_35_player.s`), no version dependency found here.
  PTBL encoding is **version-independent for SIMPLEPULSE=0** (identity
  passthrough). For **SIMPLEPULSE=1** songs, greloc clips `editor_L > 0x80`
  to `0x80` and packs as `binary_R = (editor_L & 0x0F) | (editor_R & 0xF0)`.
  Inverse: `editor_L = 0x80 | (binary_R & 0x0F)`, `editor_R = binary_R & 0xF0`.
  SIMPLEPULSE=1 is detected from the compiled player by finding `STA $D402,X`
  immediately followed by `STA $D403,X` (pattern `9D 02 D4 9D 03 D4`) — this
  adjacency appears in both the pulse player loop and in SIMPLEPULSE=1 voice-init
  sequences, never in SIMPLEPULSE=0 players. Speed entries (binary L < 0x80) pass
  through without transform in both modes — the binary R speed value is added
  directly by both SIMPLEPULSE=0 and SIMPLEPULSE=1 players, so no re-encoding
  is needed. Jump entries (L=0xFF) always pass through.

## Bugs found and fixed

1. **Combo-selection tiebreak was evidence-blind.** When the search finds
   two structurally-valid `(c,d)` interpretations tied at the same
   `ncols_try`, the old comparator just blindly preferred "real vibrato
   columns" (`noinsvib=false`) over "real gatetimer/firstwave columns"
   (`fixedparams=false`), with no actual evidence either way. For
   `N_O_U_A_G_O_O_T_L_A_part_4.sid` this picked the *wrong* combo
   (`fixedparams=true, STBL=0`) over the correct one (`noinsvib=true,
   STBL=1`). **Fix**: added `firstwave_evidence` — locate the universal gate
   write `LDA chnwave,X; AND chngate,X; STA $D404,X` (present in every player
   version), trace backward to find `chnwave`'s address, then check whether
   what feeds `chnwave` via `STA chnwave,X` is a table read (`LDA tbl,Y`
   immediately before → real per-instrument column) or an immediate (`LDA
   #imm` immediately before → genuine fixedparams). Use this to break the
   tie instead of guessing. Also tightened the separate `fp_gate`/`fp_first`
   byte-scanner, which was matching *any* `LDA #imm; STA addr,X` with
   `imm>=0x08` anywhere in the whole binary (a near-guaranteed false-positive
   source) — `fp_first` is now anchored to the same `chnwave_addr`.
   `fp_gate` (gatetimer) still has no comparably universal anchor and keeps
   the weaker heuristic — **documented as a known gap**, not yet exercised
   as a real bug on any test file since it only runs when `fixedparams=true`
   is genuinely the winning combo.

2. **`nowavedelay` detection was guessing from byte values, not disassembly.**
   An earlier-session fix replaced an even-cruder heuristic with one that
   checked for specific byte *values* (`0x41`/`0x81` vs `0x51`/`0x91`)
   present in the raw WTBL_L data — still fundamentally unreliable, since
   it's inferring a structural fact (does this song's *binary* contain the
   `SBC #$10` step) from incidental data values rather than checking the
   actual code. This wrongly classified `Nineteen.sid` as
   `nowavedelay=true` when direct disassembly showed it clearly has the
   `CMP #$10; BCS ...; SBC #$10` pattern (should be `false`). **Fix**: removed
   the byte-value heuristic entirely and reused the *existing* `wave_entry`
   disassembly scan (originally written only for the R-byte polarity check,
   which already locates `LDA mt_wavetbl-1,Y` and distinguishes `CMP
   #$08`/`#$10` immediately following it from a bare `BEQ`) to set
   `nowavedelay` directly from that same branch shape. Verified across the
   full regression suite that no previously-validated file's *actual output
   bytes* changed (some files' raw `nowavedelay` print value changed, e.g.
   `gt2_0.sid` 1→0, but those are all `legacy_format=true` files where the
   value is never actually used — confirmed via `WTBL_DEBUG` showing
   `rawL==outL` unconditionally for those files).

3. **"Legacy = BMI polarity" assumption was wrong.**
   An intermediate version of the code added `if(best.legacy_format)
   wtbl_r_needs_xor=false` to override the disassembly scan for all
   `legacy_format=true` files, on the theory that v2.0/v2.06 always uses
   BMI polarity. This turned out to be false: `For_Attitude_8.sid` is
   detected as `legacy_format=true` (pointer tables sit inside the table
   region) but its player genuinely uses BPL polarity — R-bytes need XOR
   0x80 applied (raw 0x80,0x85,0x87 → editor 0x00,0x05,0x07). The
   disassembly scan correctly detects this when the override is absent.
   **Fix**: removed the legacy override entirely. The `wave_entry` scan is
   reliable enough on its own — it finds BPL for For_Attitude_8 and BMI for
   gt2.0.sid without any version-based assumption.

4. **Abs-indexed `firstwave_evidence` scan required direct SID write, missing RAM-shadow players.**
   The converter's `firstwave_evidence` scan (abs-indexed form) required the final `STA` to be
   at exactly `$D404,X` (direct SID waveform register write). `FamiCommodore.sid` uses a player
   that writes SID registers indirectly: a 25-byte RAM shadow at `$1473...$1487` is filled during
   each tick, then bulk-copied to `$D400,X` via a loop (`LDX #$18; LDA $1473,X; STA $D400,X; DEX; BPL`).
   The actual firstwave write is `LDA $1438,X; AND $144F,X; STA $1477,X` (shadow offset 4 = `$D404`).
   Because `$1477,X ≠ $D404,X`, the scan returned -1, and the combo-selection fallback incorrectly
   preferred `noinsvib=0,fixedparams=1` over the correct `noinsvib=1,fixedparams=0`. This made all 13
   instruments use `vibp=1→STBL[1]=(04,03)` as vibrato, corrupting the sound completely.
   **Fix**: removed the `9D 04 D4` constraint — the scan now accepts ANY `STA abs,X` after
   `LDA abs,X; AND abs,X`. The pattern `BD lo hi; 3D lo2 hi2; 9D lo3 hi3` is specific enough to the
   firstwave-AND-gate write (no AND-abs,X occurs in pulse/vibrato/freq processing), and the inner
   search (find where `lo|hi<<8` is written; check if `LDA abs,Y` or `LDA #imm` precedes it)
   still correctly sets evidence=1 (per-column) or evidence=0 (fixed immediate). Verified across all
   regression files: WTBL/PTBL/FTBL/STBL sizes and `noinsvib`/`fixedparams` selections are unchanged.

5. **PTBL passthrough was wrong for SIMPLEPULSE=1 songs.**
   The converter used identity passthrough for PTBL in all cases, on the theory
   that greloc.c's PTBL encoding was identity. This is true for SIMPLEPULSE=0,
   but for SIMPLEPULSE=1, greloc clips editor L to 0x80 and packs both nybbles
   into binary R. `Blaster.sid` and `N_O_U_A_G_O_O_T_L_A_part_4.sid` are both
   SIMPLEPULSE=1 songs; their SET entries were being passed through raw (e.g.
   binary `(0x80, 0x04)` output as editor `(0x80, 0x04)`) when the correct editor
   value is `(0x84, 0x00)`. **Fix**: detect SIMPLEPULSE=1 from the player binary
   by scanning for `9D 02 D4 9D 03 D4` (`STA $D402,X; STA $D403,X` adjacent),
   then apply the inverse `editor_L = 0x80 | (R & 0x0F)`, `editor_R = R & 0xF0`
   for all entries with binary L=0x80. Speed entries (L < 0x80) and jump entries
   (L=0xFF) pass through unchanged.

6. **Pattern notes with instr=0 didn't retrigger instrument tables (filter cutoff bug).**
   In the original binary, every non-REST pattern entry causes a full instrument
   retrigger via the gate-timer mechanism: when the gate timer fires, it reads the
   next note from the compiled pattern stream and queues it. At the next tick that
   note triggers, WTBL/PTBL/FTBL all restart for that instrument. In `unpack_patt`,
   pattern bytes with no preceding instrument byte were emitting `instr=0` in the
   SNG, which tells GoatTracker "no retrigger". For `Skyscape.sid`: channel 1
   (cutoff 0xF0) only retriggered its FTBL at row 0 of each pattern; channel 0
   (cutoff 0x80) also didn't retrigger on subsequent notes. When channel 0's bass
   notes started (row 8 of patt0), the filter dropped to 0x80 instead of staying
   at 0xF0 (channel 1 should have overridden it on the same tick since it processed
   second). **Fix**: in `unpack_patt`, track `cur_instr` across the while loop.
   When a real note (FIRSTNOTE..LASTNOTE) appears with no explicit instrument byte,
   emit `cur_instr` instead of 0. KEYOFF/KEYON/REST rows remain at 0. Also widened
   `firstwave_evidence` look-back to handle a BEQ instruction between `LDA abs,Y`
   and `STA chnwave,X` (B9 at m-5, F0 at m-2 instead of B9 at m-3), which was
   causing Skyscape's scan to return -1 (combo still selected correctly by
   structural elimination, but evidence-quality improved).

## Outstanding — none known

`Nineteen.sid` and `N_O_U_A_G_O_O_T_L_A_part_4.sid` verified correct by ear as of
2026-06-18. `rs232-thewitchinghour.sid` and `MoreFunToComputeTSJ_4x.sid` produce
byte-identical output to the original release binary as of 2026-06-18.
`Blaster.sid` PTBL fix applied 2026-06-21. `N_O_U_A_G_O_O_T_L_A_part_4.sid`
SIMPLEPULSE=1 fix applied 2026-06-21 (both files). `sids/FamiCommodore.sid`
abs-indexed firstwave_evidence scan extended 2026-06-21. `sids/Skyscape.sid`
instrument carry-forward fix applied 2026-06-21. No further known bugs.

## Gotchas / lessons learned (worth re-reading before further work)

- `read_col()`'s actual indexing is `sid[s2f(icols[0] + ci*N_INSTR + 1 + i)]`
  — note the `+1`. Forgetting it when manually cross-checking addresses by
  hand against disassembly produces plausible-looking but wrong numbers
  (cost real time this session — initial manual column dump didn't match
  `max_p`/`max_f` from debug output until the `+1` was found).
- The `fixedparams: gate=XX firstwave=YY` printed values are **not**
  independent corroborating evidence for anything — they're generated
  *after* a combo is already chosen, by a scan loose enough to match almost
  any binary. Don't use their mere existence to justify a selection;
  if anything, suspicious/implausible values are a hint the wrong combo won.
- `ptbl_evidence` checks for `LDA (ptbl_start-1),Y` with nearby X-indexed ops to
  confirm a real pulse table. Filter table accesses are also per-channel (X-indexed),
  so this heuristic is not perfectly discriminating — but it is better than structural
  evidence alone and has produced no false positives on any test file to date.
- Do NOT assume `legacy_format=true` implies BMI R-byte polarity. `For_Attitude_8`
  is legacy-format but uses BPL polarity and needs XOR 0x80. Always trust the
  `wave_entry` disassembly scan result over any version-based assumption.
- When two structurally self-consistent table-length candidates tie, resist
  the urge to pick based on "which seems more common/likely" — find an
  actual, version-independent anchor in the disassembly (the SID gate
  register write `$D404,X` and the universal `chnwave`/`chngate` pattern
  were useful anchors this session) and trace evidence from there.
- Player-version differences are **multi-dimensional**, not one flag. WTBL
  alone differs across eras in three independent ways (delay threshold value,
  presence/absence of the `SBC` shift, R-byte XOR polarity) that must each be
  confirmed by disassembly separately — don't assume one marker implies the
  others without checking.
- Available reference player sources for v2.34/2.35, v2.51, and v2.73 are
  byte-identical on every marker tried so far (init guard, WTBL threshold,
  R-byte polarity) — current tooling can identify "v2.2+ speedtable era" but
  not the exact sub-version. If finer version pinning becomes necessary,
  will need either more reference sources with known differences, or to
  diff the full player binaries byte-for-byte across versions to find where
  they actually first diverge.

## Regression file table (updated)

| File | SIMPLEPULSE | Expected output |
|---|---|---|
| `For_Attitude_8.sid` | 0 | `legacy_format=true`, WTBL=56 PTBL=16 FTBL=21 STBL=7 |
| `gt2.0.sid` | 0 | `legacy_format=true`, WTBL=31 PTBL=15 FTBL=6 STBL=3 |
| `1982_Xyce.sid` | 0 | WTBL=115 PTBL=16 FTBL=15 STBL=6 |
| `MoreFunToComputeTSJ_4x.sid` | 0 | WTBL=172 PTBL=31 FTBL=149 STBL=4 |
| `$3LastNight_Jammer.sid` | 0 | WTBL=144 PTBL=36 FTBL=45 STBL=8 |
| `rs232-thewitchinghour.sid` | 0 | nopulse=0 nofilter=1 noinsvib=1 fixedparams=1, WTBL=4 PTBL=3 FTBL=0 STBL=1 |
| `Nineteen.sid` | 0 | WTBL=49 PTBL=31 FTBL=65 STBL=8 |
| `N_O_U_A_G_O_O_T_L_A_part_4.sid` | **1** | noinsvib=1 fixedparams=0, WTBL=55 PTBL=5 FTBL=20 STBL=1 |
| `sids/Blaster.sid` | **1** | WTBL=23 PTBL=19 FTBL=3 STBL=1 |
| `sids/Skyscape.sid` | **1** | noinsvib=1 fixedparams=0, WTBL=13 PTBL=11 FTBL=6 STBL=0 |
