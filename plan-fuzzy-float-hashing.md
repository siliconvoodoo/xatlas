# Plan: reactivate fuzzy float hashing (UV colocality tolerance)

## Background

Commit `b645eea` ("use fuzzy spatial hashing") added two template specializations in
`source/xatlas/xatlas.cpp` (~line 2126):

- `Hash<float>` — hashes `round(k * 1000) / 1000.f` (1/1000 buckets)
- `Equal<float>` — `k0 - k1 < FLT_EPSILON`

Intent per the code comment: *"UV colocality tolerance in chart growing"*.

**They are dead code.** No `HashMap<float>` is ever instantiated; every map uses
`Vector2` / `Vector3` / `uint32_t` / `EdgeKey` keys, and `Hash<Vector2/3>` hashes the
struct's raw bytes without ever routing through the float specialization. So the
intended tolerance is not in effect anywhere. On top of that, both specializations are
broken as written:

- `Equal<float>` has no `fabs`: any `k0` smaller than `k1` compares "equal"
  (asymmetric, non-transitive, not an equivalence).
- Hash and Equal are inconsistent: values within `FLT_EPSILON` of each other can round
  into different 1/1000 buckets, so "equal" keys hash to different slots and lookups
  miss anyway. This is the fundamental flaw of naive rounding-bucket fuzzy hashing.

## Where fuzziness would actually help

1. **`ComputeUvMeshChartsTask::m_uvToEdgeMap`** (`HashMap<Vector2>`, AddUvMesh
   charting path) — the original target. Today, UV vertices that are colocal in
   spirit but not bitwise identical (import jitter, float roundtrips, `-0.0` vs
   `0.0`) are not recognized, so charts get split at fake seams.
2. **`Mesh::createColocalsHash`** (`HashMap<Vector3>`, position colocality) —
   currently finds only bitwise-identical positions; epsilon colocality exists only
   via the BVH path (`epsilon > FLT_EPSILON`). Optional secondary target; measure
   before touching, the BVH fallback already covers explicit-epsilon users.

## Design constraints

- The `HashMap` contract is multimap-style: `add` never dedups; callers walk all
  matches via `get` + `getNext`. A fuzzy variant must therefore **enumerate every key
  within tolerance**, not just report one.
- Rounding-bucket hashing alone can never be consistent with tolerance equality
  (boundary straddle). The correct scheme is **quantized-cell hash + neighbor-cell
  probing**:
  - hash key = cell index `floor(k / cellSize)` per component (cell size >= 2x tolerance);
  - lookup probes the 2^d candidate cells (4 for Vector2) picked by which half of the
    cell each component falls in;
  - equality = `fabs(a - b) <= tolerance` per component.
- Tolerance equality is **not transitive** (A~B, B~C, A!~C). Fine for flood-fill /
  colocality growth, but document it and keep results deterministic (stable insertion
  and iteration order).
- Default behavior must stay bit-exact: tolerance 0 = current exact matching.

## Steps

### 1. Instrument (no behavior change)
Add an offline/diagnostic pass (behind a define or a verbose print) that, for each UV
mesh, builds a uniform grid over the UVs and reports: count of vertex pairs within
candidate tolerances (1e-7 .. 1e-3, log-spaced) that are *not* bitwise equal, and the
resulting "missed colocality" edges. This quantifies the problem per asset.

### 2. Gather data
Run the instrumented build over representative SDK production assets plus the
`models/` suite (`--uv` mode). Produce a histogram of nearest-neighbor UV distances.
The "same point" cluster vs "genuinely distinct" gap picks the tolerance; the old
code's implied values (1e-3 hash buckets vs FLT_EPSILON equality) disagree by 4 orders
of magnitude, so this must come from data, not guesses.

### 3. Implement
- Delete the dead `Hash<float>` / `Equal<float>` specializations (they are a trap).
- Add a `FuzzyVector2Map` (or a `HashMap` policy variant) implementing the
  quantized-cell + neighbor-probe scheme above, with an iterator that chains matches
  across the probed cells so the `get`/`getNext` call sites port mechanically.
- Plumb a `uvColocalityEpsilon` option (UvMeshDecl or ChartOptions), default 0 =
  exact = today's behavior.

### 4. Wire in
Replace `m_uvToEdgeMap` usage in `ComputeUvMeshChartsTask::run` /
`canAddFaceToChart`. Leave every other HashMap exact.

### 5. Validate
- **Unit tests** (extend `source/test/test.cpp`, AddUvMesh path): micro OBJ with UVs
  differing by less than tolerance across a fake seam -> expect 1 chart with tolerance
  on, 2 with tolerance 0. Include a bucket-boundary-straddling pair (the case naive
  rounding gets wrong) and a `-0.0` / `0.0` pair.
- **Regression**: full existing suite unchanged with tolerance 0 (default).
- **Perf**: charting time on the largest UV assets; neighbor probing is ~4x hash
  lookups, verify it stays in noise.
- **Determinism**: identical chart counts across repeated runs and MT/ST.

### 6. Rollout
Enable per-asset from the data gathered in step 2; keep 0 as the library default.

## Risks

- Over-merge from non-transitive tolerance chains (mitigate: tolerance from data, and
  chart growth already re-validates geometrically).
- Silent behavior change if a nonzero default sneaks in — keep default exact.
- Perf regression on huge UV meshes from multi-probe — measured in step 5.

## References

- Dead specializations: `source/xatlas/xatlas.cpp` ~2126 (`**voddou` comments).
- Target map: `ComputeUvMeshChartsTask::m_uvToEdgeMap`, `segment::computeUvMeshCharts`.
- Secondary: `Mesh::createColocalsHash` / `createColocals` epsilon dispatch.
- Session appraisal that flagged this: 2026-07-06 (finding #4, deferred by V.Oddou).
