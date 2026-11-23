# Asset Engine — binary archives (GTech 3)

Standalone C++20 archiving library (no serialization library), a demo, tests, a benchmark, and a physics adapter.

## Build & run
```
cmake -S . -B build && cmake --build build && ctest --test-dir build
# or without CMake:
g++ -std=c++20 -O2 -Iinclude src/archive.cpp tests/tests.cpp -o asset_tests && ./asset_tests
```
Reproduce the data: `asset_demo write data` · `asset_demo list data/general_v1.asen` · `asset_demo load data/general_v1.asen` (v1 file migrated: `level=1`) · `asset_demo load data/scene.asen` · `asset_bench`.

## Layers
| Layer | Files | Role |
|---|---|---|
| Byte access | `byte_io.hpp`, `crc32.hpp` | `ByteWriter`/`ByteReader`: explicit little-endian, bounds-checked, UTF-8 validated |
| Archive format | `archive.hpp/.cpp` | header, directory, checksums, validation. Never names a domain type |
| Adapters | `adapters/general.hpp`, `adapters/physics.hpp` | one `Codec<T>` specialisation per type |

**Add a type without touching the core:** specialise `Codec<T>` (`kType`, `kVersion`, `write`, `read(reader, schemaVersion)`).

## API
```cpp
ArchiveWriter w; w.add(id, value); w.save("file.asen");        // or w.write(std::ostream&)
std::ifstream f("file.asen", std::ios::binary);
ArchiveReader r(f);                 // validates header + directory only
for (auto& e : r.entries()) ...;    // list
auto p = r.load<Person>(100);       // loads ONE asset by id (reads only its bytes)
```
Errors: `ArchiveError` with a code (`Err::BadChecksum`, `Truncated`, …) and a diagnostic message.

## Binary format (little-endian everywhere)
```
Header  (20 B): "ASEN" | formatVersion u16 (=1) | flags u16 (=0) | entryCount u32 | directoryOffset u64
Payloads      : asset blobs back to back, in directory order, starting at offset 20 (no gap, no overlap: enforced by the reader)
Directory (36 B/entry): assetId u64 | type u32 | schemaVersion u16 | reserved u16 (=0) | offset u64 | size u64 | payloadCrc32 u32
Footer   (4 B): CRC-32 over [header 20 B ‖ directory bytes]
```
Primitives: `uN` fixed-width LE · `bool` 1 byte (0/1 only) · `f32/f64` IEEE-754 bits as u32/u64 LE · `string` = u32 byte length + UTF-8 (validated) · sequence = u32 count + elements · struct = fields in declared order. No padding, no pointers, no raw memory dump. Cross-object references = stable u64 ids.

## Checksum
CRC-32/IEEE (poly 0xEDB88320, init & xorout 0xFFFFFFFF), own implementation (check value of "123456789" = 0xCBF43926, tested).
Footer CRC covers the header and the directory; each payload has its own CRC in its directory entry → because the reader enforces that the payloads tile the area between header and directory exactly (no gap, no overlap), **every byte of the file is covered by exactly one checksum**; a test flips every byte one by one and another rejects gapped/overlapping entries. CRC detects accidental corruption only: no authenticity (anyone can recompute it).

## Compatibility / versioning
- `formatVersion` (container): only 1 accepted, others rejected (`UnsupportedVersion`).
- `schemaVersion` (per asset): `Person` v1 `{name, age}` → v2 adds `level` (default 1 when reading v1). A schema newer than the reader knows, or 0, is rejected. `PersonV1` codec exists only to produce v1 archives.

## Validation (before any allocation)
magic · versions · flags/reserved · entryCount ≤ (fileSize−24)/36 · directory ends exactly at EOF · offset/size inside the payload area (overflow-safe `size ≤ dirOff − offset`) · payloads contiguous, in directory order, ending exactly at the directory (no gap/overlap) · duplicate ids · type 0 · collection counts `n ≤ remaining/minElemSize` before `reserve` · trailing bytes in an asset · UTF-8 · bool bytes · finite floats. Assets are decoded into a local object and returned: a failed load never modifies the caller's data (`world = r.load<World>(id)`).

## Integration with the engine (Moteur3DCPU)
The engine has no physics solver yet: its "physics" components are only `Velocity` and `Spin`. The real integration is therefore on the **ECS scene** (`adapters/scene3d.hpp`), which is what the brief allows when the engine is incomplete. The fields the brief lists for a physics scene that do not exist in the engine (mass, inertia, colliders, gravity, fixed timestep) are **not invented in the scene format**; the full physics shape is demonstrated separately on stand-in structs (`adapters/physics.hpp`, not connected to the engine).

What is persisted: `Mesh` (type 200) and `Texture` (type 201) as independent assets, and the scene (type 202): entities with a presence mask and their components (Transform, MeshRenderer, Velocity, Spin, Camera). Per-component layout is documented at the top of `scene3d.hpp`.
- **Stable ids instead of pointers:** `MeshRenderer` holds raw `const Mesh*` / `const Texture*`; the archive stores asset ids (0 = null). A mesh shared by two entities is stored once. On load the assets are created in a `LoadedAssets` object that owns them (keep it alive as long as the entities) and the pointers are rebuilt.
- **Runtime ids are never persisted:** entity ids are recycled by `entityManager`, and component-type ids depend on `addComponentType` order; the tests load into an ECS registered in a different order.
- **Conventions:** rotation_y in radians around +Y, uniform scale > 0, camera yaw/pitch in radians (|pitch| <= pi/2), mesh indices 0-based, uv index stored +1 (0 = none), pixels = engine's packed ARGB8888 `uint32`.
- **Rebuilt on load (not stored):** entity ids, the id -> storage mapping, the `MeshRenderer` pointers. (Vertex normals are computed by `main.cpp` from the mesh, so they are not stored.)
- **Atomic load:** `loadScene` decodes everything into plain structs; `instantiate` validates references, material range and capacity *before* touching the ECS (`LoadMode::IntoEmpty` or `ReplaceAll`).
- **Limits:** `MAX_ENTITIES = 100` (99 usable entities); entities without any component are not saved; `ReplaceAll` only destroys entities that own a component and computes capacity conservatively; materials are an index into the app's own table, so the caller passes `materialCount`.
Commands: `asset_scene_demo save data/scene3d_demo.asen` then `asset_scene_demo load data/scene3d_demo.asen` (optional args to save: `spot.obj spot_texture.png`). Build with `-DMOTEUR3D_HEADERS=<repo>/header`.

### Conventions of the stand-in physics adapter (`adapters/physics.hpp`)
SI units (m, kg, s), right-handed, Y up; quaternion (x, y, z, w), unit length (renormalised on load, tolerance 1e-3); inertia = diagonal of the body-space inertia tensor; velocities in world space; static bodies store mass = 0 and inertia = 0; references between objects (constraints) use body ids (u64). Persisted: gravity, fixed timestep, bodies (id, static flag, position, orientation, mass, inertia, linear/angular velocity, collider), constraints (two body ids + rest length); a snapshot adds step index and simulation time and is taken at the end of a step. Not persisted, rebuilt on load: inverse mass, inverse inertia, id → index map. Rejected on load: unknown/duplicate/self-referencing ids, non-finite values, non-positive mass/inertia/timestep, non-unit quaternion, unknown collider kind.

## Tests (`tests/tests.cpp`: 41 checks, `tests/scene_tests.cpp`: 41 checks; all pass, also under ASan/UBSan)
Primitives & endianness, CRC vector, general round-trips (string + collection), empty archive/collections, access by id, wrong type/missing id, v1→v2 migration, unknown schema/format version, bad magic, **truncation at every length**, **altered byte at every position**, excessive counts/sizes (valid CRC), gigantic directory size, trailing bytes, duplicate ids (writer + file), stand-in physics round-trip + snapshot + world replacement, invalid reference / duplicate body / NaN / zero mass / non-unit quaternion / dt=0 with caller state untouched, write failure (bad stream, bad path).

## Measurements (`asset_bench`)
**TODO before submission: re-run `asset_bench` yourself in Release and replace the table and this line** (OS, compiler + version, CPU, build type). The figures below come from the development run: g++ 13 -O2, Linux, median of 5 runs, deterministic data (fixed LCG seed), after the buffer-reservation fix described below.
| bodies | archive size | save | load | save peak temp | load peak temp |
|---|---|---|---|---|---|
| 1 000 | 104 064 B | 0.58 ms | 0.39 ms | 110 KiB | 303 KiB |
| 100 000 | 10 400 064 B | 43.3 ms | 40.6 ms | 9.9 MiB | 30.0 MiB |
Method: wall clock (`steady_clock`) around save-to-file / open+load; "temporary memory" = peak heap above baseline measured by a counting `operator new` (the in-memory world is built before the baseline; the loaded result is included). Limits: OS page cache (warm file), heap only (no stack/mmap), one machine, bodies with a sphere collider only, Debug builds are not representative.
Reading the numbers: 104 B/body = 94 B body + 10 B of constraints. **Save peak ≈ 1× the payload**: the payload buffer is allocated once (`ByteWriter::reserve` with the exact size). Before that fix the peak was 2.4× (24.6 MiB): `vector::push_back` doubles its capacity and the old and new buffers (8 MiB + 16 MiB) coexist during the copy; reserving removes it (a first attempt with a 4-byte-too-small reserve still triggered one doubling and gave 3×). **Load peak ≈ 3× the payload**: the payload buffer read from disk + the decoded world (bodies, constraints, id → index map) + the temporary duplicate-id set used for validation; a streaming reader would avoid the payload buffer.

## Design choices (memory)
`ByteWriter`/payload moved, not copied, into the writer; one `vector<uint8_t>` per asset; directory = flat vector + `unordered_map` index (O(1) lookup by id); reader seeks and reads only the requested payload.

## Known limitations
Writer keeps all payloads in RAM until `write`; float format assumes IEEE-754; no compression/lazy-loading extension.

## Sources & AI use
Sources: CRC-32 — zlib manual / IEEE 802.3 (own implementation, no checksum library used); format ideas — FlatBuffers internals (offsets, alignment), Boost.Serialization (versioning principles).
**Disclosure of significant AI use:** the initial implementation (core library, general and physics stand-in adapters, scene adapter for Moteur3DCPU, tests, benchmark, README draft) was generated with Claude (Anthropic) from the project brief and the engine source we provided. AI-found/fixed during development: a CRC-coverage test helper, a zero-length-read bug (`gcount()`), an off-by-4 `reserve` (see Measurements). 
TODO (us): list what we reviewed, modified and tested ourselves, which suggestions we rejected or corrected, and the issues we found in the AI's output.

## Division of work
TODO (us): who did what (core / adapters / tests / measurements / README), with the partner's name.
