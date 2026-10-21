# AI Decorator

A Windows Geode mod for Geometry Dash 2.2081 and Geode 5.11.1 (or a compatible newer 5.x release). Select a section of your level, describe the decoration you want, review a preview, regenerate variations until you like one, then apply it. You bring your own API key — or use a free local model (Ollama).

This download is source code, not a compiled `.geode` file. The included GitHub Actions workflow builds the installable file without setting up a compiler on your PC. The platform-independent core (`src/Plan.hpp`) is compiled and unit-tested in this repository's history (`tests/plan.cpp`); the full mod is compiled by CI on every push.

## What was wrong with v0.1 (why it felt fake)

v0.1 did call a real AI API, but it was set up so badly that the result barely mattered:

- The model was only allowed to place **one** object (ID 211, a plain square), scaled and rotated. Every "decoration" was rectangles.
- It was told nothing about what your selected objects *are* — just numeric IDs and center points, with every object assumed to be 30×30. It could not tell a spike from a slab, so it generated abstract patterns inside the selection's bounding box instead of decorating your structures.
- No sampling temperature was sent and the request was identical every run, so providers returned essentially **the same plan every time** (sometimes rotated). There was no way to ask for a different variation.
- Strict all-or-nothing parsing rejected normal model output (markdown fences, extra fields) and every failure showed the same generic "invalid plan" message, hiding the real problem.

## How v0.2 works

- **A real vocabulary.** The model picks from ~30 named decoration pieces — panels, outlines, three sizes of glow, glow lines, rotating shine, pulsing circles/stars/arrows, gears, chains, rods, grass, clouds, pipes, split rings, quarter-discs, brick fills and more. It never supplies raw object IDs; the mod maps names to IDs.
- **Verified decoration-only.** At runtime the mod asks the game itself (`GameObject::createWithKey` + object type) whether each vocabulary piece is a true *Decoration* object. Anything that isn't — no hitboxes, no hazards, no triggers, ever — is removed from the list before the model ever sees it, and re-checked again when you press Apply. A wrong entry in the table can't place a solid block.
- **The model can see your structures.** Each selected object is sent with its real type (solid / slope / hazard / pad / orb / portal / deco / trigger / collectible, read from the game) and its real measured footprint — plus an ASCII grid map of the whole selection so the model can reason about the layout spatially.
- **Anchored placement.** Pieces can be attached to a specific selected object ("grass on top of block #12", "outline framing this slab", "glow behind that structure"), so decoration belongs to your geometry instead of floating randomly in a box.
- **Actual variation.** A Creativity control sets the sampling temperature, every run carries a fresh variation nonce, and **Regenerate** tells the model what the previous plan mainly used and demands a different composition. If a regenerated plan still fingerprints as ≈ the same arrangement, the mod automatically re-rolls once instead of showing you the same design again.
- **Self-repairing requests.** Markdown-fenced JSON, extra fields and multi-part API responses are parsed tolerantly. If the plan still doesn't validate, the concrete error is fed back to the model once for a corrected answer. Real failures show the HTTP status and the provider's message instead of "invalid plan".

## Build using GitHub

1. Push this repository to GitHub (or fork it). The mod lives in `ai-decorator/`; the workflow in `.github/workflows/main.yml` builds it from there.
2. Open the repository's **Actions** tab. Enable workflows if GitHub asks.
3. A push starts a build automatically, or choose **Build Windows mod → Run workflow**.
4. Open the completed successful run. Under **Artifacts**, download `ai-decorator-windows`.
5. Extract that ZIP to get `minveraz.ai-decorator.geode`.

The workflow targets Windows 64-bit only. A failed build produces no installable mod; its Actions log contains the compiler or setup error.

## Install

1. Install Geode from <https://geode-sdk.org/> and launch Geometry Dash once.
2. Check that the game is **2.2081** and Geode is **5.11.1 or a compatible newer 5.x**. Older GD / Geode 4.x are not targeted.
3. Open the Geode menu's folder button and place `minveraz.ai-decorator.geode` in the `mods` folder (default Steam install: `steamapps/common/Geometry Dash/geode/mods`).
4. Restart Geometry Dash and confirm **AI Decorator** appears in the installed mod list.

## Decorate

1. Open a **copy** of your level in the editor. Set color channels **1**, **2** and **3** to your primary, accent and highlight colors — generated pieces use these channels, so recoloring later is one channel edit.
2. Select a section (up to **400 objects**, up to 6000×6000 units). Smaller coherent sections give much better results than half a level at once.
3. Click **AI** near the upper-right of the editor.
4. First time: open **API Setup**. Pick a provider preset (OpenAI, OpenRouter, Groq, or local Ollama), paste your API key, and set a model ID your account can use. For Ollama no key is needed (type anything) — the model is whatever you've pulled, e.g. `qwen2.5-coder`.
5. Describe the look — or tap a style preset (Industrial / Neon / Nature / Crystal / Minimal / Spooky) and edit it.
6. Pick **Creativity** (Precise → Wild), **Amount** (Sparse → Dense) and **Layer** (Behind / Mixed / In front), then **Generate**. Requests can take up to ~2 minutes on slow providers; closing the window cancels locally.
7. Review the preview: gray shapes are your selection (drawn by real type/size), blue/purple/cream are channels 1/2/3. It's a faithful schematic of positions, sizes and shapes — final colors and glow blending are the game's.
8. **Apply** to paste it in, **Regenerate** for a different variation of the same prompt, **Back** to tweak the prompt, or **Discard**. After applying, the editor's normal **Undo** removes the whole batch.

Generated decoration goes on the editor layer after the highest selected editor layer (capped at 999), at Z layer B1 (behind) or T1 (in front) per piece. If it seems invisible after applying, switch the editor to **All** layers.

### Prompt tips

- Name materials and structure, not colors: colors come from your channels 1/2/3. "Rusted pipes behind the platforms, gears in the gaps, grass on top" beats "make it green".
- Mention what *your selection contains* if it's ambiguous: "frame the pillars", "backlight the ship corridor".
- If results look scattered, lower Creativity to Precise/Balanced and Amount to Sparse; if they look same-y, raise Creativity and use Regenerate.

## What it can and can't generate

It generates static decoration only: no gameplay objects, triggers, groups, moving parts, text, or arbitrary object IDs, and it never executes model-supplied level strings or code. Pieces are verified decoration-type in your game at runtime. Each plan is capped at 450 objects, scales 0.05–10, channels 1–3 (plus an optional detail channel), and every rotated corner must fit within your selection plus a 120-unit margin. Invalid pieces are dropped per-item (and counted in the preview) instead of failing the whole plan.

It is not a one-click finished level: expect a strong decorated draft that you'll nudge, recolor and detail by hand — like a very fast first pass.

## API key handling & privacy

The key stays in process memory for the game session. It is not written to Geode settings, level data or logs (use **Clear key** in API Setup to forget it; this is not an OS credential store). Only the endpoint, model ID, your last prompt and control settings persist between sessions.

Requests go **directly** from your PC to the endpoint you entered — there is no relay server. HTTPS is required, except plain HTTP to `localhost`/`127.0.0.1` for local model servers like Ollama/LM Studio. The provider receives: your prompt, and the selected objects' types, positions, sizes and rotations (plus a text grid map of the selection). No screenshots, level name, account info or full-level data is sent. API usage is billed by your provider; a ChatGPT subscription does not include API credit. Never commit a real key to a repository.

## Troubleshooting

- **No AI button:** check the mod is enabled and GD/Geode versions match above.
- **HTTP 401/403:** wrong key or the model isn't available to your account.
- **HTTP 404:** the endpoint must end in `/chat/completions` and the model ID must exist. For Ollama the endpoint is `http://127.0.0.1:11434/v1/chat/completions`.
- **HTTP 429:** out of credit or rate-limited.
- **"Invalid plan" with a specific reason:** the alert now says what was wrong (unknown piece names, out-of-range values…). Simpler prompts, lower Creativity, or a stronger instruction-following model help. Small local models (<8B) often struggle with the JSON layout task.
- **Same design repeatedly:** raise Creativity, and use Regenerate from the preview (it explicitly asks for a different composition and auto-re-rolls near-duplicates). Note some providers/models are near-deterministic regardless.
- **Everything one color:** set different colors on channels 1/2/3; by default they may be identical.
- **Preview differs in game:** the schematic uses placeholder channel colors and simplified shapes; positions/scales/rotations are exact, rendering is the game's.

## Local Windows build

Install Visual Studio 2022 (Desktop C++), LLVM/Clang tools, CMake 3.25+, Ninja, Git, and the [Geode CLI](https://github.com/geode-sdk/cli). Install Geode SDK **v5.11.1**, set `GEODE_SDK` to its directory, then from `ai-decorator/`:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEODE_TARGET_PLATFORM=Win64 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

## Core tests

The platform-independent core (prompt building, grid map, parsing/validation, anchoring, dedupe, plan similarity, serialization, endpoint rules) is covered by unit tests that need only a C++ compiler:

```sh
g++ -std=c++23 -Wall -Wextra -Werror -Ivendor -Isrc tests/plan.cpp -o tests/plan-test
./tests/plan-test
```

## Sources and licensing

- Geode SDK, bindings and `GameObjectType`/`ZLayer` enums: <https://github.com/geode-sdk/geode>, <https://github.com/geode-sdk/bindings>
- Object ID → name catalog cross-checked against <https://github.com/Alphalaneous/GDObjectNames> (IDs are additionally verified as decoration-type at runtime)
- GD object serialization keys: <https://github.com/gd-programming/gd.docs/blob/main/docs/resources/client/level-components/level-object.md>
- Official build action: <https://github.com/geode-sdk/build-geode-mod>
- Vendored nlohmann/json v3.12.0 is MIT-licensed; see `vendor/LICENSE.nlohmann-json`.

The mod source is MIT-licensed; see `LICENSE`.
