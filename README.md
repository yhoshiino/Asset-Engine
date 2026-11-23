# Asset Engine — binary archives (GTech 3)

Standalone C++20 archiving library (no serialization library), a demo, tests, a benchmark, and a physics adapter.

## Build & run
```
cmake -S . -B build && cmake --build build && ctest --test-dir build
# or without CMake:
g++ -std=c++20 -O2 -Iinclude src/archive.cpp tests/tests.cpp -o asset_tests && ./asset_tests
# sanitizers (what the "ASan/UBSan" claim below refers to):
g++ -std=c++20 -g -fsanitize=address,undefined -Iinclude src/archive.cpp tests/tests.cpp -o asset_tests_san && ./asset_tests_san
```
CMake builds in Release by default. The scene adapter targets (`asset_scene_tests`, `asset_scene_demo`) are enabled automatically when `third_party/moteur3d/header` exists, or when you pass `-DMOTEUR3D_HEADERS=/path/to/Moteur3DCPU/header`. Without engine headers, the library, core tests, demo and benchmark build on their own.

Reproduce the data: `asset_demo write data` · `asset_demo list data/general_v1.asen` · `asset_demo load data/general_v1.asen` (v1 file migrated: `level=1`) · `asset_demo load data/scene.asen` · `asset_bench`.

## Layers
| Layer | Files | Role |
|---|---|---|
| Byte access | `byte_io.hpp`, `crc32.hpp` | `ByteWriter`/`ByteReader`: explicit little-endian, bounds-checked, UTF-8 validated |
| Archive format | `archive.hpp/.cpp` | header, directory, checksums, validation. Never names a domain type |
| Adapters | `adapters/general.hpp`, `adapters/physics.hpp`, `adapters/scene3d.hpp` | one `Codec<T>` specialisation per type |

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

Asset types: 1 `Person` · 2 `Playlist` · 100 `World` · 101 `Snapshot` · 200 `Mesh` · 201 `Texture` · 202 `Scene` (0 is reserved).

Physics payload layout (`adapters/physics.hpp`):
```
World   : gravity 3×f32 | fixedTimestep f32 | bodyCount u32 | Body × n | constraintCount u32 | Constraint × m
Body (94 B)       : id u64 | isStatic u8 | position 3f | orientation 4f (x,y,z,w) | mass f | inertia 3f
                    | linearVelocity 3f | angularVelocity 3f | colliderKind u8 | radius f | halfExtents 3f
Constraint (20 B) : bodyA u64 | bodyB u64 | restLength f
Snapshot          : stepIndex u64 | simTime f64 | World
```
`sizeof(Body)` in memory is larger than the 94 serialized bytes (padding, derived fields `invMass`/`invInertia`): the file format is defined by the codec, not by the struct layout.

## Checksum
CRC-32/IEEE (poly 0xEDB88320, init & xorout 0xFFFFFFFF), own implementation (check value of "123456789" = 0xCBF43926, tested).
Footer CRC covers the header and the directory; each payload has its own CRC in its directory entry. Because the reader enforces that the payloads tile the area between header and directory exactly (no gap, no overlap), **every byte of the file is covered by a CRC, or is a CRC** (the footer itself); a test flips every byte one by one and another rejects gapped/overlapping entries. CRC detects accidental corruption only: no authenticity (anyone can recompute it).

## Compatibility / versioning
- `formatVersion` (container): only 1 accepted, others rejected (`UnsupportedVersion`).
- `schemaVersion` (per asset): `Person` v1 `{name, age}` → v2 adds `level` (default 1 when reading v1). A schema newer than the reader knows, or 0, is rejected. `PersonV1` codec exists only to produce v1 archives.

## Validation (before any allocation)
magic · versions · flags/reserved · entryCount ≤ (fileSize−24)/36 · directory ends exactly at EOF · offset/size inside the payload area (overflow-safe `size ≤ dirOff − offset`) · payloads contiguous, in directory order, ending exactly at the directory (no gap/overlap) · duplicate ids · type 0 · collection counts `n ≤ remaining/minElemSize` before `reserve` · trailing bytes in an asset · UTF-8 · bool bytes · finite floats. Assets are decoded into a local object and returned: a failed load never modifies the caller's data (`world = r.load<World>(id)`).

## Integration with the engine (Moteur3DCPU)
The engine has no physics solver yet: its "physics" components are only `Velocity` and `Spin`. The real integration is therefore on the **ECS scene** (`adapters/scene3d.hpp`), which is what the brief allows when the engine is incomplete. The fields the brief lists for a physics scene that do not exist in the engine (mass, inertia, colliders, gravity, fixed timestep) are **not invented in the scene format**; the full physics shape is demonstrated separately on stand-in structs (`adapters/physics.hpp`, not connected to the engine).

Engine headers: `third_party/moteur3d/header/` holds a copy of the engine headers needed by the scene adapter (`vec.h`, `obj.h`, `texture.h`, `ecs/`, `components/`).

What is persisted: `Mesh` (type 200) and `Texture` (type 201) as independent assets, and the scene (type 202): entities with a presence mask and their components (Transform, MeshRenderer, Velocity, Spin, Camera). Per-component layout is documented at the top of `scene3d.hpp`.
- **Stable ids instead of pointers:** `MeshRenderer` holds raw `const Mesh*` / `const Texture*`; the archive stores asset ids (0 = null). A mesh shared by two entities is stored once. On load the assets are created in a `LoadedAssets` object that owns them (keep it alive as long as the entities) and the pointers are rebuilt.
- **Runtime ids are never persisted:** entity ids are recycled by `entityManager`, and component-type ids depend on `addComponentType` order; the tests load into an ECS registered in a different order.
- **Conventions:** rotation_y in radians around +Y, uniform scale > 0, camera yaw/pitch in radians (|pitch| <= pi/2), mesh indices 0-based, uv index stored +1 (0 = none), pixels = engine's packed ARGB8888 `uint32`.
- **Rebuilt on load (not stored):** entity ids, the id -> storage mapping, the `MeshRenderer` pointers. (Vertex normals are computed by `main.cpp` from the mesh, so they are not stored.)
- **Atomic load:** `loadScene` decodes everything into plain structs; `instantiate` validates references, material range and capacity *before* touching the ECS (`LoadMode::IntoEmpty` or `ReplaceAll`).
- **Limits:** `MAX_ENTITIES = 100` (99 usable entities); entities without any component are not saved; entity names are not saved; `ReplaceAll` only destroys entities that own a component and computes capacity conservatively; materials are an index into the app's own table, so the caller passes `materialCount`.

Commands: `asset_scene_demo save data/scene3d_demo.asen` then `asset_scene_demo load data/scene3d_demo.asen` (optional args to save: `spot.obj spot_texture.png`).

### Conventions of the stand-in physics adapter (`adapters/physics.hpp`)
SI units (m, kg, s), right-handed, Y up; quaternion (x, y, z, w), unit length (renormalised on load, tolerance 1e-3); inertia = diagonal of the body-space inertia tensor; velocities in world space; static bodies store mass = 0 and inertia = 0; references between objects (constraints) use body ids (u64). Persisted: gravity, fixed timestep, bodies (id, static flag, position, orientation, mass, inertia, linear/angular velocity, collider), constraints (two body ids + rest length); a snapshot adds step index and simulation time and is taken at the end of a step. Not persisted, rebuilt on load: inverse mass, inverse inertia, id → index map. Rejected on load: unknown/duplicate/self-referencing ids, non-finite values, non-positive mass/inertia/timestep, non-unit quaternion, unknown collider kind.

Restoring a snapshot: `world = reader.load<World>(id)` (or `.world` of a loaded `Snapshot`) replaces an existing world, or fills an empty one. The new world is fully built and validated first, then move-assigned, so a failed load leaves the target untouched.

## Tests (`tests/tests.cpp`: 41 checks, `tests/scene_tests.cpp`: 41 checks; all pass, also under ASan/UBSan)
Primitives & endianness, CRC vector, general round-trips (string + collection), empty archive/collections, access by id, wrong type/missing id, v1→v2 migration, unknown schema/format version, bad magic, **truncation at every length**, **altered byte at every position**, excessive counts/sizes (valid CRC), gigantic directory size, trailing bytes, duplicate ids (writer + file), **overlapping and gapped payload entries**, stand-in physics round-trip + snapshot + world replacement, invalid reference / duplicate body / NaN / zero mass / non-unit quaternion / dt=0 with caller state untouched, write failure (bad stream, bad path).

Scene adapter tests (`scene_tests.cpp`): mesh/texture round-trip (empty mesh, inconsistent faces, out-of-range index), full scene round-trip with shared assets stored once and a different component registration order, null pointers, only referenced assets saved, `ReplaceAll`, `IntoEmpty` on a non-empty ECS, failures leaving the ECS untouched (bad material, missing asset, over capacity; exactly 99 entities accepted), crafted payloads with a *valid CRC* (bad indices, huge counts, 16384x16384 texture claimed in 8 bytes, duplicate/zero entity id, NaN, scale 0, pitch out of range), truncation at every length and an altered byte at every position.

## Measurements (`asset_bench`)

| bodies | archive size | save | load | save peak temp | load peak temp |
|---|---|---|---|---|---|
| 1 000 | 104 064 B | 0.58 ms | 0.39 ms | 110 KiB | 303 KiB |
| 100 000 | 10 400 064 B | 43.3 ms | 40.6 ms | 9.9 MiB | 30.0 MiB |

Method: wall clock (`steady_clock`) around save-to-file / open+load; "temporary memory" = peak heap above baseline measured by a counting `operator new` (the in-memory world is built before the baseline; the loaded result is included). Limits: OS page cache (warm file), heap only (no stack/mmap), one machine, bodies with a sphere collider only, Debug builds are not representative, the counting allocator slightly distorts timings.

Reading the numbers: 104 B/body = 94 B body + 10 B of constraints. **Save peak ≈ 1× the payload**: the payload buffer is allocated once (`ByteWriter::reserve` with the exact size). Before that fix the peak was 2.4× (24.6 MiB): `vector::push_back` doubles its capacity and the old and new buffers (8 MiB + 16 MiB) coexist during the copy; reserving removes it (a first attempt with a 4-byte-too-small reserve still triggered one doubling and gave 3×). **Load peak ≈ 3× the payload**: the payload buffer read from disk + the decoded world (bodies, constraints, id → index map) + the temporary duplicate-id set used for validation; a streaming reader would avoid the payload buffer.

## Design choices
- **Directory at the end of the file.** The header holds `directoryOffset`, which depends on the total payload size. Placing the directory after the payloads lets the writer emit header, payloads, directory and footer in one sequential pass, with no seek-back. The reader finds the directory through the header.
- **Two levels of checksum.** The footer CRC covers header and directory; each payload has its own CRC in its entry. Opening an archive reads and verifies only header and directory (fast, bounded by the file size); a payload is verified when it is loaded. Trade-off: corruption of a payload nobody loads is only detected on access.
- **Directory in memory:** a flat `vector<Entry>` (listing in file order) plus an `unordered_map` id → index (O(1) average lookup). A sorted vector with binary search would use less memory (O(log n) lookup); the hash map was kept for simple, fast access by id.
- **Errors: exceptions, and objects built on the side.** Validation runs at every nested level of an adapter; with `bool` or `optional` every field read would need a manual check. `load<T>` builds a local object and returns it, so a failed load never leaves the caller with a partially loaded state.
- **Copies when reading:** one copy from the stream into a `vector<uint8_t>`, then `ByteReader` reads through a `std::span` over that buffer (no further copy), and finally the values are decoded into the domain object. Writing: each payload is built once and moved into the writer; adapters call `reserve` with the exact size.
- **Bounded allocations:** a collection count `n` is accepted only if `n × minimum element size` fits in the remaining bytes, before any `reserve`.
- **Format vs memory layout:** the file format is defined field by field by the codecs (fixed-width, little-endian, no padding); `sizeof` of a C++ struct plays no role in it.

## Known limitations
- The writer keeps all payloads in RAM until `write`; the reader loads a whole payload into one buffer.
- Payload CRCs are only checked when an asset is loaded, not when the archive is opened.
- `ArchiveReader` is not thread-safe (`seekg` inside `const` methods).
- The `remaining / minElemSize` bound does not limit memory amplification per element (a `Track` or an `EntityDesc` takes more memory than its bytes on disk).
- The physics `World` is validated when read, not when written: an invalid world can be saved, but the resulting archive is rejected on load. Scene and mesh/texture adapters also validate on write.
- The quaternion is renormalised on load (tolerance 1e-3), so the loaded value can differ from the stored one by rounding.
- The format has no signed integer primitives (signed values are stored with an explicit offset, e.g. uv index + 1).
- Float format assumes IEEE-754.
- No compression / lazy-loading extension.

## Sources & AI use
Sources: CRC-32 — zlib manual / IEEE 802.3 (own implementation, no checksum library used); format ideas — FlatBuffers internals (offsets, alignment), Boost.Serialization (versioning principles); C++ standard library reference — cppreference (`std::span`, `std::bit_cast`).

**AI Assistance:** Claude (Anthropic) was used as a development assistant to accelerate boilerplate implementation (adapters, unit tests, benchmark scaffolding, and initial documentation drafts) based on our architectural specifications and the Moteur3DCPU source code. It also assisted in debugging edge cases during development (such as a zero-length read issue with gcount() and a 4-byte reservation offset).

**Verification & Manual Refinements:** we built it, ran the tests (also under ASan/UBSan) and the benchmark, and reviewed the files. Review found and fixed: payload contiguity was not enforced (the "every byte covered" claim was stronger than the code), the save-time memory peak came from `vector` growth rather than from the copies the first draft blamed (fixed with an exact `reserve`, measured before/after), and the README measurements had to be re-checked on our own build. Review suggestions we did not implement are listed under Known limitations.

## Division of work

Two developers, **Tarik** and **Nicolas**. The split follows the layers of the library. The contract between the two sides is the `Codec<T>` interface and the binary format section of this README, so Nicolas could write adapters while Tarik was still hardening the core.

### Tarik — the core (format, validation, general-purpose types)
- `include/asset/byte_io.hpp`, `include/asset/crc32.hpp`: byte access, little-endian, UTF-8, bounds checks, CRC-32
- `include/asset/archive.hpp`, `src/archive.cpp`: header, directory, checksums, validation, `Codec<T>` interface
- `include/asset/adapters/general.hpp`: `Person` (v1→v2 migration) and `Playlist`
- `demo/main.cpp`: `write`, `list` and `load` for the general-purpose types
- `tests/tests.cpp` (core part): primitives, round-trips, truncation, altered byte, hostile sizes, duplicates, payload layout, write failure
- `data/general_v1.asen`, `data/general_v2.asen`
- `CMakeLists.txt`: library, core tests, demo
- README: format, API, checksum, versioning, validation

### Nicolas — everything built on the core (adapters, engine, measurements)
- `include/asset/adapters/physics.hpp`: `World`, `Snapshot`, conventions, reference validation
- `include/asset/adapters/scene3d.hpp`: Mesh, Texture and ECS scene of Moteur3DCPU
- `demo/scene_demo.cpp`, `tests/scene_tests.cpp`, `third_party/moteur3d/header/`
- `tests/tests.cpp` (physics part): scene round-trip, snapshot, world replacement, rejection without partial state
- `demo/main.cpp` (`World` branch that produces `data/scene.asen`)
- `data/scene.asen`, `data/scene3d_demo.asen`
- `bench/bench.cpp` and measurements
- `CMakeLists.txt`: scene and bench targets
- README: engine integration, physics conventions, measurements

### Shared
Cross-review: each of us reviewed the other's files before merging (details below). `.gitignore` and `.gitattributes` were set up together.

**Integration.** Tarik first fixed the format and the `Codec<T>` interface, then Nicolas built the adapters against it. Each area was merged through a short-lived branch, and `ctest` had to pass before every merge.

**Reviews.**
- Core (Tarik's code, cross-checked by Nicolas): payloads were not checked for gaps or overlaps, so the claim that every byte is covered by a checksum was stronger than the code. This was found during an AI-assisted review; `ArchiveReader` now enforces contiguity, and a test crafts overlapping and gapped directories with a recomputed footer CRC.
- Physics adapter and benchmark (Nicolas's code, cross-checked by Tarik): the benchmark showed a save peak of 2.4× the payload (24.6 MiB for 100 000 bodies). The cause, identified during an AI-assisted review, was `vector` doubling its capacity, with the old and new buffers coexisting. `writeWorld` now reserves the exact size, and the peak is ≈ 1× the payload (9.9 MiB).
- Build: `/utf-8` was added for MSVC because the tests contain UTF-8 literals.

**Blockers and scope changes.**
- Moteur3DCPU has no physics solver (its only "physics" components are `Velocity` and `Spin`). Nicolas therefore integrated the archive with the real ECS scene, as the brief allows for an incomplete engine, and demonstrated the full physics data shape (mass, inertia, colliders, gravity, timestep, constraints) on stand-in structs in `physics.hpp`.
- The scene adapter needs the engine headers. They are vendored under `third_party/moteur3d/header`, so the library, core tests and demo build without the engine, and the scene targets are enabled automatically when the headers are present.
