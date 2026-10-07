# Compaction layout

A Lean model of the layout pass and its alignment costs. The model follows
`compact.ccm` and `generation::prepare` in `heap.ccm`, starting from commit
`dd42e70520be8cfd69d414e8ce135e177c853067`.

```sh
cd lean
lean -DwarningAsError=true CompactionLayout.lean
```

The toolchain is pinned to Lean 4.24.0. The proof uses bundled `Std`; there are
no additional packages. No `sorry`, custom axioms or native evaluation are used.

## The layout monoid

Once joined groups and their maximum alignments are known, each group maps its
incoming cursor to an outgoing cursor. For byte addresses divisible by eight,
we can represent compositions of those maps with three scalars:

```
F(x) = round_up(x + B, A) + T
A in {8, 16, 32, 64}, 0 <= B < A
B and T are multiples of eight
```

An ordinary group has zero bias and its retained byte count as the tail. The
identity is `(8, 0, 0)` on these addresses.

To compose `(A, B, T)` followed by `(C, D, U)`:

```
if A >= C: (A, B, round_up(T + D, C) + U)
otherwise: (C, B + round_up(T + D, A), U)
```

Normalize the bias by moving `(B / A) * A` into the tail and keeping `B % A`.
The resulting representation is unique. `compactCompose_assoc` proves structural
associativity; `compactSummarize_correct` and `compactSummarize_append` connect
it to sequential placement and arbitrary chunk boundaries. An eight-residue
table gives a separate, simpler reference representation.

We can reassociate, but cannot reorder. Maximum alignment plus total retained
bytes is not enough to describe a composition. Group discovery is still a
separate operation: the join flags must be resolved before using this monoid.
The proof does not add a parallel scan.

## Space in bytes

The implementation counts eight-byte units internally. The model keeps that
correspondence and states the space results in bytes. For a 256-byte rank block,
before trimming:

| Effective alignment | Maximum dilation overhead | Maximum group-start padding |
| --- | ---: | ---: |
| 8 bytes | 0 | 0 |
| 16 bytes | 128 | 8 |
| 32 bytes | 192 | 24 |
| 64 bytes | 224 | 56 |

Dilation retains at most `(alignment / 8) * live_bytes`, capped at 256 bytes per
block. These bounds exclude the null slot, arena reserve, metadata and OS page
rounding. Rank blocks are not OS pages.

Rounding a record at its own alignment uses less than twice its size when
`size >= alignment`. A size already divisible by its alignment needs no rounding.
That does not give a global twofold bound: the maximum alignment of a joined
group can exceed an individual record's alignment.

The examples check nonoverlapping records with `size >= own alignment`:

- A 64-byte aligned, 64-byte record followed by an 8-byte record has 72 live
  bytes but retains 128 under full dilation. Trimming the suffix retains 72.
- Two joined blocks can hold 120 live bytes and retain 512. A live 16-byte record
  crosses the boundary and a live 64-byte-aligned record raises the group's
  alignment. Both blocks end in a live position, so neither suffix can be trimmed.

Alignment metadata is cleared and rebuilt during live marking. Dead records do
not contribute requirements.

## Trimming the suffix

Remove retained bits strictly above the highest exact live bit.
`tail_trim_preserves_forwarding` proves that this leaves every live target's
rank unchanged for a fixed destination. Consequently SIMD forwarding can keep
its untrimmed dilation calculation; sizing, movement and pointer-mask packing
must agree on the trimmed retained counts.

The remaining laws cover live-bit preservation, idempotence, empty masks, and
the `k = 0` fast path. A crossing record marks the final bit of the block live,
so trimming changes nothing at an internal joined boundary. Reducing a group's
retained count cannot increase the final footprint, including subsequent
alignment padding.

The next group's alignment can absorb the saving. Following the 72-byte example
with an 8-byte group changes the total from 136 to 80 bytes. Following it with a
64-byte-aligned, 64-byte group leaves the total at 192 either way.

## What remains outside the proof

`dilated` models filling each aligned chunk that contains a live position. It
does not prove refinement of the C++ bit-twiddling loop or its SIMD variants.
The arithmetic uses natural numbers, so finite-width overflow is a separate
obligation. Tracing, concurrency, pointer repair and the complete moving
collector are not verified here. The crossing-boundary theorem takes the live
final-bit condition explicitly; complete-record marking supplies that condition
in the intended implementation.
