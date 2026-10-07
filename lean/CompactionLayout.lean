-- SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
-- SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

import Std

namespace Jam.CompactionLayout

/-- Alignment in eight-byte cells, after resolving joined rank-block groups. -/
inductive Alignment where
  | cell | pair | quad | oct
  deriving DecidableEq, Repr

def Alignment.cells : Alignment → Nat
  | .cell => 1 | .pair => 2 | .quad => 4 | .oct => 8

/-- Mathematical form of `(x + (1 << k) - 1) & ~((1 << k) - 1)`.
    Machine arithmetic must not overflow; that obligation is outside this model. -/
def alignUp (a : Alignment) (x : Nat) : Nat :=
  (x + a.cells - 1) / a.cells * a.cells

theorem alignUp_ge (a : Alignment) (x : Nat) : x ≤ alignUp a x := by
  cases a <;> simp [alignUp, Alignment.cells] <;> omega

theorem alignUp_aligned (a : Alignment) (x : Nat) :
    alignUp a x % a.cells = 0 := by
  simp [alignUp]

theorem alignUp_least (a : Alignment) (x y : Nat)
    (hxy : x ≤ y) (hy : y % a.cells = 0) : alignUp a x ≤ y := by
  cases a <;> simp_all [alignUp, Alignment.cells] <;> omega

theorem alignUp_padding (a : Alignment) (x : Nat) :
    alignUp a x - x < a.cells := by
  cases a <;> simp [alignUp, Alignment.cells] <;> omega

theorem alignUp_mono (a : Alignment) (x y : Nat) (h : x ≤ y) :
    alignUp a x ≤ alignUp a y := by
  cases a <;> simp [alignUp, Alignment.cells] <;> omega

/-- Padding depends only on the input residue modulo eight. -/
theorem alignUp_residue (a : Alignment) (x : Nat) :
    alignUp a x = x + (alignUp a (x % 8) - x % 8) := by
  cases a <;> simp [alignUp, Alignment.cells] <;> omega

/-- One already joined group; `retained` includes cells retained by dilation.
    It need not be positive or satisfy a divisibility hypothesis for these laws. -/
structure Group where
  alignment : Alignment
  retained : Nat
  deriving Repr

def Group.finish (g : Group) (x : Nat) : Nat :=
  alignUp g.alignment x + g.retained

/-- The end cursor after serial placement, as in `generation::prepare`. -/
def serial : List Group → Nat → Nat
  | [], x => x
  | g :: gs, x => serial gs (g.finish x)

theorem serial_ge (gs : List Group) (x : Nat) : x ≤ serial gs x := by
  induction gs generalizing x with
  | nil => simp [serial]
  | cons g gs ih =>
    have := alignUp_ge g.alignment x
    have := ih (g.finish x)
    simp only [serial, Group.finish] at *
    omega

theorem serial_mono (gs : List Group) (x y : Nat) (h : x ≤ y) :
    serial gs x ≤ serial gs y := by
  induction gs generalizing x y with
  | nil => exact h
  | cons g gs ih =>
    apply ih
    exact Nat.add_le_add_right (alignUp_mono g.alignment x y h) g.retained

/-- Reducing a group's retained count cannot increase final space, even after
    every subsequent group realigns its start. It may merely move the padding. -/
theorem smaller_group_no_more_space (a : Alignment) (small large x : Nat)
    (rest : List Group) (h : small ≤ large) :
    serial (⟨a, small⟩ :: rest) x ≤ serial (⟨a, large⟩ :: rest) x := by
  simp only [serial, Group.finish]
  apply serial_mono
  exact Nat.add_le_add_left h _

theorem serial_append (xs ys : List Group) (x : Nat) :
    serial (xs ++ ys) x = serial ys (serial xs x) := by
  induction xs generalizing x with
  | nil => rfl
  | cons g gs ih => simp [serial, ih]

/-- Total space includes retained cells and at most q-1 padding cells per group. -/
theorem serial_space_bound (gs : List Group) (x : Nat) :
    serial gs x ≤ x + (gs.map fun g => g.retained + (g.alignment.cells - 1)).sum := by
  induction gs generalizing x with
  | nil => simp [serial]
  | cons g gs ih =>
    have hpad := alignUp_padding g.alignment x
    have hge := alignUp_ge g.alignment x
    have htail := ih (g.finish x)
    simp only [serial, List.map_cons, List.sum_cons, Group.finish] at *
    omega

/-- Any later group starts at or beyond this group's end, even with intervening
    groups. Hence their half-open retained-cell intervals cannot overlap. -/
theorem later_group_disjoint (g h : Group) (between : List Group) (x : Nat) :
    g.finish x ≤ alignUp h.alignment (serial between (g.finish x)) := by
  exact Nat.le_trans (serial_ge between (g.finish x)) (alignUp_ge _ _)

/-- Eight unbounded displacements; the table index is always in bounds. -/
abbrev Summary := Fin 8 → Nat

def residue (x : Nat) : Fin 8 := ⟨x % 8, Nat.mod_lt x (by decide)⟩

def evaluate (s : Summary) (x : Nat) : Nat := x + s (residue x)

def identity : Summary := fun _ => 0

/-- Apply `a`, then `b`: c[r] = a[r] + b[(r + a[r]) mod 8]. -/
def compose (a b : Summary) : Summary :=
  fun r => a r + b (residue (r.val + a r))

theorem residue_add (x n : Nat) :
    residue ((residue x).val + n) = residue (x + n) := by
  apply Fin.ext
  simp [residue, Nat.add_mod]

theorem evaluate_compose (a b : Summary) (x : Nat) :
    evaluate (compose a b) x = evaluate b (evaluate a x) := by
  simp [evaluate, compose, residue_add, Nat.add_assoc]

theorem evaluate_identity (x : Nat) : evaluate identity x = x := by
  simp [evaluate, identity]

/-- Evaluating all cursors distinguishes summaries; no table entries are hidden. -/
theorem summary_ext (a b : Summary)
    (h : ∀ x, evaluate a x = evaluate b x) : a = b := by
  funext r
  have hr : residue r.val = r := by
    apply Fin.ext
    exact Nat.mod_eq_of_lt r.isLt
  have hx := h r.val
  simp only [evaluate, hr] at hx
  omega

theorem compose_assoc (a b c : Summary) :
    compose (compose a b) c = compose a (compose b c) := by
  apply summary_ext
  intro x
  simp only [evaluate_compose]

theorem compose_identity_left (a : Summary) : compose identity a = a := by
  apply summary_ext
  intro x
  simp only [evaluate_compose, evaluate_identity]

theorem compose_identity_right (a : Summary) : compose a identity = a := by
  apply summary_ext
  intro x
  simp only [evaluate_compose, evaluate_identity]

def groupSummary (g : Group) : Summary :=
  fun r => alignUp g.alignment r.val - r.val + g.retained

theorem evaluate_group (g : Group) (x : Nat) :
    evaluate (groupSummary g) x = g.finish x := by
  simp only [evaluate, groupSummary, residue, Group.finish]
  rw [alignUp_residue g.alignment x]
  omega

def summarize : List Group → Summary
  | [] => identity
  | g :: gs => compose (groupSummary g) (summarize gs)

/-- Exact for every list of groups, every starting cursor, and arbitrary counts. -/
theorem summarize_correct (gs : List Group) (x : Nat) :
    evaluate (summarize gs) x = serial gs x := by
  induction gs generalizing x with
  | nil => exact evaluate_identity x
  | cons g gs ih =>
    simp only [summarize, evaluate_compose, evaluate_group, ih, serial]

/-- Any chunk boundary is valid once joined groups and their alignment are fixed.
    Associativity then permits any order-preserving parenthesization of chunks. -/
theorem summarize_append (xs ys : List Group) :
    summarize (xs ++ ys) = compose (summarize xs) (summarize ys) := by
  apply summary_ext
  intro x
  simp only [evaluate_compose, summarize_correct, serial_append]

/-- A prefix summary gives the same next-group base as the serial collector,
    and that base is aligned and does not overlap the prefix. -/
theorem prefix_placement (before : List Group) (g : Group) (x : Nat) :
    alignUp g.alignment (evaluate (summarize before) x) =
      alignUp g.alignment (serial before x) ∧
    alignUp g.alignment (evaluate (summarize before) x) % g.alignment.cells = 0 ∧
    serial before x ≤ alignUp g.alignment (evaluate (summarize before) x) := by
  rw [summarize_correct]
  exact ⟨rfl, alignUp_aligned _ _, alignUp_ge _ _⟩

/-- Same maximum alignment and total retained count, different layout.
    Both groups have positive counts divisible by their own alignment. -/
theorem coarse_summary_loses_padding :
    let a : Group := ⟨.cell, 1⟩
    let b : Group := ⟨.oct, 8⟩
    serial [a, b] 0 = 16 ∧ serial [b, a] 0 = 9 := by
  decide

/-- A canonical, three-scalar summary. Normalizing the bias is essential:
    otherwise equivalent layouts need not be equal as summary values. -/
structure Compact where
  alignment : Alignment
  bias : Nat
  tail : Nat
  bias_lt : bias < alignment.cells

def compactEvaluate (s : Compact) (x : Nat) : Nat :=
  alignUp s.alignment (x + s.bias) + s.tail

def normalize (a : Alignment) (bias tail : Nat) : Compact :=
  ⟨a, bias % a.cells, bias / a.cells * a.cells + tail,
    Nat.mod_lt _ (by cases a <;> decide)⟩

theorem normalize_correct (a : Alignment) (bias tail x : Nat) :
    compactEvaluate (normalize a bias tail) x = alignUp a (x + bias) + tail := by
  cases a <;> simp [compactEvaluate, normalize, alignUp, Alignment.cells] <;> omega

/-- For nested power-of-two alignments, either the first alignment subsumes the
    second, or the first rounding can move into the bias of the second. -/
def compactCompose (a b : Compact) : Compact :=
  if b.alignment.cells ≤ a.alignment.cells then
    normalize a.alignment a.bias (alignUp b.alignment (a.tail + b.bias) + b.tail)
  else
    normalize b.alignment (a.bias + alignUp a.alignment (a.tail + b.bias)) b.tail

theorem compactCompose_correct (a b : Compact) (x : Nat) :
    compactEvaluate (compactCompose a b) x =
      compactEvaluate b (compactEvaluate a x) := by
  unfold compactCompose
  split <;> rw [normalize_correct]
  all_goals
    cases ha : a.alignment <;> cases hb : b.alignment <;>
      simp_all [compactEvaluate, alignUp, Alignment.cells] <;> omega

/-- Canonical summaries have unique representations. Observe one rounding jump:
    its height determines the alignment and its position determines the bias. -/
theorem compact_ext (a b : Compact)
    (h : ∀ x, compactEvaluate a x = compactEvaluate b x) : a = b := by
  rcases a with ⟨ak, ab, aTail, ha⟩
  rcases b with ⟨bk, bb, bt, hb⟩
  have hboundary := h (ak.cells - ab)
  have hnext := h (ak.cells - ab + 1)
  clear h
  cases ak <;> cases bk <;>
    dsimp only [Alignment.cells] at ha hb <;>
    simp_all [compactEvaluate, alignUp, Alignment.cells] <;> omega

def compactIdentity : Compact := ⟨.cell, 0, 0, by decide⟩

theorem compactIdentity_correct (x : Nat) : compactEvaluate compactIdentity x = x := by
  simp [compactEvaluate, compactIdentity, alignUp, Alignment.cells]

theorem compactCompose_assoc (a b c : Compact) :
    compactCompose (compactCompose a b) c = compactCompose a (compactCompose b c) := by
  apply compact_ext
  intro x
  simp only [compactCompose_correct]

theorem compactCompose_identity_left (a : Compact) : compactCompose compactIdentity a = a := by
  apply compact_ext
  intro x
  simp only [compactCompose_correct, compactIdentity_correct]

theorem compactCompose_identity_right (a : Compact) : compactCompose a compactIdentity = a := by
  apply compact_ext
  intro x
  simp only [compactCompose_correct, compactIdentity_correct]

def compactGroup (g : Group) : Compact := normalize g.alignment 0 g.retained

def compactSummarize : List Group → Compact
  | [] => compactIdentity
  | g :: gs => compactCompose (compactGroup g) (compactSummarize gs)

theorem compactSummarize_correct (gs : List Group) (x : Nat) :
    compactEvaluate (compactSummarize gs) x = serial gs x := by
  induction gs generalizing x with
  | nil => exact compactIdentity_correct x
  | cons g gs ih =>
    simp only [compactSummarize, compactCompose_correct, ih, compactGroup,
      normalize_correct, Nat.add_zero, serial, Group.finish]

theorem compactSummarize_append (xs ys : List Group) :
    compactSummarize (xs ++ ys) = compactCompose (compactSummarize xs) (compactSummarize ys) := by
  apply compact_ext
  intro x
  simp only [compactCompose_correct, compactSummarize_correct, serial_append]

/-- Occupancies of the aligned q-cell chunks of a rank block. Each entry is
    the number of exact live bits in that chunk, regardless of their positions. -/
abbrev Occupancies (q : Nat) := List (Fin (q + 1))

def claimed {q : Nat} (cs : Occupancies q) : Nat := (cs.map Fin.val).sum

/-- Dilation fills precisely the chunks with nonzero occupancy. -/
def retained {q : Nat} (cs : Occupancies q) : Nat :=
  (cs.map fun c => if c.val = 0 then 0 else q).sum

/-- Arbitrary chunk counts, including empty chunks: dilation cannot drop a live
    cell, costs at most q times the claims, and cannot exceed physical capacity. -/
theorem dilation_bounds {q : Nat} (cs : Occupancies q) :
    claimed cs ≤ retained cs ∧
    retained cs ≤ q * claimed cs ∧
    retained cs ≤ q * cs.length ∧
    retained cs ≤ claimed cs + (q - 1) * cs.length := by
  induction cs with
  | nil => simp [claimed, retained]
  | cons c cs ih =>
    have hc : c.val ≤ q := by have := c.isLt; omega
    have hm (hz : c.val ≠ 0) : q ≤ q * c.val := by
      have h := Nat.mul_le_mul_left q (by omega : 1 ≤ c.val)
      simpa using h
    simp only [claimed, retained, List.map_cons, List.sum_cons, List.length_cons] at *
    by_cases hz : c.val = 0
    · simp [hz, Nat.mul_add] at *
      omega
    · have hm := hm hz
      simp only [hz, if_false, Nat.mul_add, Nat.mul_one]
      omega

/-- Each occupied q-cell chunk wastes at most q-1 cells per exact claim. -/
theorem dilation_loss {q : Nat} (hq : 0 < q) (cs : Occupancies q) :
    retained cs - claimed cs ≤ (q - 1) * claimed cs := by
  have h := dilation_bounds cs
  have he : q * claimed cs = claimed cs + (q - 1) * claimed cs := by
    have hq' : q = 1 + (q - 1) := by omega
    calc
      q * claimed cs = (1 + (q - 1)) * claimed cs := congrArg (· * claimed cs) hq'
      _ = claimed cs + (q - 1) * claimed cs := by rw [Nat.add_mul, Nat.one_mul]
  omega

/-- For the actual 32-cell rank blocks: at most 32 retained cells and at most
    (q-1)*(32/q) wasted cells (0, 16, 24, or 28 for k = 0, 1, 2, or 3). -/
theorem block_dilation_bounds (a : Alignment) (cs : Occupancies a.cells)
    (hlen : cs.length = 32 / a.cells) :
    retained cs ≤ 32 ∧
    retained cs - claimed cs ≤ (a.cells - 1) * (32 / a.cells) ∧
    retained cs ≤ a.cells * claimed cs := by
  have h := dilation_bounds cs
  have hcapacity : a.cells * cs.length = 32 := by
    rw [hlen]
    cases a <;> decide
  rw [hlen] at h hcapacity
  omega

/-- The eightfold bound and 28-cell loss are attained, not merely estimates. -/
theorem dilation_worst_case :
    let cs : Occupancies 8 := List.replicate 4 ⟨1, by decide⟩
    claimed cs = 4 ∧ retained cs = 32 ∧ retained cs - claimed cs = 28 := by
  decide

/-- Public space accounting is in bytes. Internal ranks mirror Jam's 8-byte unit. -/
def Alignment.bytes (a : Alignment) : Nat := 8 * a.cells

def claimedBytes {q : Nat} (cs : Occupancies q) : Nat := 8 * claimed cs
def retainedBytes {q : Nat} (cs : Occupancies q) : Nat := 8 * retained cs

/-- Per 256-byte rank block, independently of record layout. The additive bounds
    for alignments 8,16,32,64 are respectively 0,128,192,224 bytes. -/
theorem block_space_bytes (a : Alignment) (cs : Occupancies a.cells)
    (hlen : cs.length = 32 / a.cells) :
    retainedBytes cs ≤ 256 ∧
    retainedBytes cs - claimedBytes cs ≤ (a.bytes - 8) * (256 / a.bytes) ∧
    retainedBytes cs ≤ a.cells * claimedBytes cs := by
  have h := block_dilation_bounds a cs hlen
  cases a <;> simp_all [retainedBytes, claimedBytes, Alignment.bytes, Alignment.cells] <;> omega

/-- Additional padding before a joined group is less than its byte alignment. -/
theorem group_padding_bytes (a : Alignment) (x : Nat) :
    8 * alignUp a x - 8 * x ≤ a.bytes - 8 := by
  have h := alignUp_padding a x
  cases a <;> simp_all [Alignment.bytes, Alignment.cells] <;> omega

def roundBytes (a : Alignment) (sizeBytes : Nat) : Nat :=
  (sizeBytes + a.bytes - 1) / a.bytes * a.bytes

/-- Rounding one record at its own alignment wastes less than that alignment;
    with size >= alignment it uses strictly less than twice its size. This does
    not substitute the record's alignment for a larger joined-group alignment. -/
theorem own_alignment_size_bound (a : Alignment) (sizeBytes : Nat)
    (hsize : a.bytes ≤ sizeBytes) :
    roundBytes a sizeBytes < sizeBytes + a.bytes ∧
    roundBytes a sizeBytes < 2 * sizeBytes := by
  cases a <;> simp_all [roundBytes, Alignment.bytes, Alignment.cells] <;> omega

/-- Ordinary C++ sizeof(T) is already a multiple of alignof(T). -/
theorem own_alignment_multiple_exact (a : Alignment) (sizeBytes : Nat)
    (hsize : sizeBytes % a.bytes = 0) : roundBytes a sizeBytes = sizeBytes := by
  cases a <;> simp_all [roundBytes, Alignment.bytes, Alignment.cells] <;> omega

abbrev Mask := Nat → Bool

/-- Exclusive endpoint of the highest live position, zero for an empty mask. -/
def extent (live : Mask) : Nat → Nat
  | 0 => 0
  | n + 1 => if live n then n + 1 else extent live n

theorem extent_le (live : Mask) (n : Nat) : extent live n ≤ n := by
  induction n with
  | zero => simp [extent]
  | succ n ih => simp only [extent]; split <;> omega

theorem live_below_extent (live : Mask) (n i : Nat) (hi : i < n)
    (hlive : live i = true) : i < extent live n := by
  induction n with
  | zero => omega
  | succ n ih =>
    simp only [extent]
    split
    · omega
    · rename_i hn
      have hin : i < n := by
        by_cases h : i < n
        · exact h
        · have : i = n := by omega
          subst i
          exact False.elim (hn hlive)
      exact ih hin

theorem empty_extent (live : Mask) (n : Nat) (h : ∀ i < n, live i = false) :
    extent live n = 0 := by
  induction n with
  | zero => rfl
  | succ n ih =>
    rw [extent, h n (by omega)]
    exact ih (fun i hi => h i (by omega))

/-- Remove only the suffix at or above the exclusive live endpoint. -/
def trimAt (cutoff : Nat) (kept : Mask) : Mask :=
  fun i => decide (i < cutoff) && kept i

theorem empty_tail_trim (live kept : Mask) (n : Nat)
    (h : ∀ i < n, live i = false) :
    trimAt (extent live n) kept = fun _ => false := by
  rw [empty_extent live n h]
  funext i
  simp [trimAt]

def rank (kept : Mask) (i : Nat) : Nat := (List.range i).countP kept

theorem trimAt_rank (kept : Mask) (cutoff i : Nat) (hi : i ≤ cutoff) :
    rank (trimAt cutoff kept) i = rank kept i := by
  apply List.countP_congr
  intro j hj
  have hj' := List.mem_range.mp hj
  simp [trimAt, show j < cutoff by omega]

/-- The forwarding rank of every live target is unchanged. -/
theorem tail_trim_preserves_forwarding (live kept : Mask) (n i : Nat)
    (hi : i < n) (hlive : live i = true) :
    rank (trimAt (extent live n) kept) i = rank kept i := by
  apply trimAt_rank
  have := live_below_extent live n i hi hlive
  omega

theorem tail_trim_preserves_live (live kept : Mask) (n i : Nat)
    (hi : i < n) (hlive : live i = true) (hkept : kept i = true) :
    trimAt (extent live n) kept i = true := by
  simp [trimAt, live_below_extent live n i hi hlive, hkept]

theorem trimAt_idempotent (kept : Mask) (cutoff : Nat) :
    trimAt cutoff (trimAt cutoff kept) = trimAt cutoff kept := by
  funext i
  simp [trimAt]

theorem tail_trim_space_bytes (live kept : Mask) (n : Nat) :
    8 * rank (trimAt (extent live n) kept) n ≤ 8 * rank kept n := by
  apply Nat.mul_le_mul_left
  apply List.countP_mono_left
  intro i _ hi
  simp only [trimAt, Bool.and_eq_true] at hi
  exact hi.2

/-- Complete live records crossing a boundary make the final position live.
    Consequently trimming cannot shorten an internal joined-group block. -/
theorem crossing_boundary_unchanged (live kept : Mask) (n i : Nat)
    (hi : i < n + 1) (hlast : live n = true) :
    trimAt (extent live (n + 1)) kept i = kept i := by
  simp [extent, hlast, trimAt, hi]

/-- Semantic dilation: fill the aligned chunk containing any live position.
    The source bit-twiddling implementation is a separate refinement obligation. -/
def dilated (a : Alignment) (live : Mask) : Mask := fun i =>
  (List.range a.cells).any fun j => live (i / a.cells * a.cells + j)

theorem k0_dilation (live : Mask) : dilated .cell live = live := by
  funext i
  simp [dilated, Alignment.cells, List.range_succ]

theorem k0_tail_trim (live : Mask) (n i : Nat) (hi : i < n) :
    trimAt (extent live n) (dilated .cell live) i = live i := by
  rw [k0_dilation]
  cases h : live i with
  | false => simp [trimAt, h]
  | true => exact tail_trim_preserves_live live live n i hi h h

structure Record where
  startBytes : Nat
  sizeBytes : Nat
  alignment : Alignment

/-- The user's size assumption, plus source alignment and 8-byte storage extent. -/
def validRecord (r : Record) : Bool :=
  r.startBytes % r.alignment.bytes == 0 &&
  decide (r.alignment.bytes ≤ r.sizeBytes) && r.sizeBytes % 8 == 0

def recordMask (baseBytes : Nat) (rs : List Record) : Mask := fun i =>
  rs.any fun r => decide (r.startBytes ≤ baseBytes + 8 * i ∧
    baseBytes + 8 * i < r.startBytes + r.sizeBytes)

/-- A dense 64-byte record plus an adjacent 8-byte record: the size assumption
    holds, yet current dilation adds 56 bytes; tail trimming removes all of them. -/
theorem dense_tail_bytes :
    let rs : List Record := [⟨256, 64, .oct⟩, ⟨320, 8, .cell⟩]
    let live := recordMask 256 rs
    let kept := dilated .oct live
    rs.all validRecord = true ∧
    8 * rank live 32 = 72 ∧ 8 * rank kept 32 = 128 ∧
    8 * rank (trimAt (extent live 32) kept) 32 = 72 := by
  decide

/-- The next independent group determines whether reclaimed tail bytes become
    a net saving: 56 bytes saved with 8-byte alignment, none with 64-byte alignment. -/
theorem tail_savings_depend_on_next_alignment :
    8 * serial [⟨.oct, 16⟩, ⟨.cell, 1⟩] 0 = 136 ∧
    8 * serial [⟨.oct, 9⟩, ⟨.cell, 1⟩] 0 = 80 ∧
    8 * serial [⟨.oct, 16⟩, ⟨.oct, 8⟩] 0 = 192 ∧
    8 * serial [⟨.oct, 9⟩, ⟨.oct, 8⟩] 0 = 192 := by
  decide

/-- Two joined blocks. The live 16-byte record straddles byte 512, propagating
    the 64-byte requirement from the first block to the second. Every record
    obeys size >= its OWN alignment. This is not an arbitrary bitmap witness. -/
def mixedRecords : List Record :=
  [⟨256, 8, .cell⟩, ⟨320, 64, .oct⟩, ⟨384, 8, .cell⟩,
   ⟨504, 16, .cell⟩, ⟨576, 8, .cell⟩, ⟨640, 8, .cell⟩, ⟨760, 8, .cell⟩]

theorem mixed_records_valid :
    mixedRecords.all validRecord = true ∧
    mixedRecords.Pairwise (fun a b => a.startBytes + a.sizeBytes ≤ b.startBytes) ∧
    mixedRecords.any (fun r => decide (r.startBytes < 512 ∧ 512 < r.startBytes + r.sizeBytes)) = true := by
  decide

/-- Dilation can also waste space internally; stripping a tail does not fix
    every loss from raising smaller records to a joined group's alignment. -/
theorem mixed_records_bytes :
    let live := recordMask 256 mixedRecords
    let kept := dilated .oct live
    8 * rank live 64 = 120 ∧ 8 * rank kept 64 = 512 ∧
    live 31 = true ∧ live 63 = true := by
  decide

end Jam.CompactionLayout
