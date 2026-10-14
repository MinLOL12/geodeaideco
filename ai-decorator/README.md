# AI Decorator

A first-version Windows Geode mod for Geometry Dash 2.2081 and Geode 5.11.1 or a compatible newer 5.x release. Select a section, enter your own API key and a decoration prompt, review a schematic, then apply the result.

This download is source code, not a compiled `.geode` file. The included GitHub Actions workflow builds the installable file without setting up a compiler on your PC. The plan validator and serializer were compiled and tested locally. The complete mod has not been compiled or run in Geometry Dash in this environment.

## Build using GitHub

1. Extract this ZIP.
2. Create an empty GitHub repository. It can be private.
3. Upload the **contents** of `ai-decorator` into the repository root. `CMakeLists.txt`, `mod.json`, `src`, `vendor`, and `.github` must be at the root. Include the hidden `.github/workflows/build.yml` file. GitHub's browser uploader can miss hidden folders; Git or GitHub Desktop can upload the entire folder reliably.
4. Open the repository's **Actions** tab. Enable workflows if GitHub asks.
5. Choose **Build Windows mod**, then **Run workflow**. A push also starts a build automatically. GitHub Actions usage limits apply to your account.
6. Open the completed successful run. Under **Artifacts**, download `ai-decorator-windows`.
7. Extract that artifact ZIP to get `minveraz.ai-decorator.geode`.

The workflow targets Windows 64-bit only and downloads the SDK version specified in `mod.json`. Its build action is pinned to a specific commit. A failed build does not produce an installable mod; its Actions log contains the compiler or setup error.

## Install

1. Install Geode from <https://geode-sdk.org/> and launch Geometry Dash once.
2. Check that the installed game is **2.2081** and Geode is **5.11.1 or a compatible newer 5.x release**. This source does not target older GD versions or Geode 4.x.
3. Open the Geode menu and use its folder button to open the Geode directory. Place the compiled `.geode` file in its `mods` folder. On a default Steam installation this is under `steamapps/common/Geometry Dash/geode/mods`.
4. Restart Geometry Dash. Check that **AI Decorator** appears in the installed mod list.

## Decorate

1. Open a copy of a level in the editor. Set color channels **1**, **2**, and **3** to your primary, accent, and highlight colors using the editor's color controls.
2. Select a small section of blocks or other objects.
3. Click **AI** near the upper-right corner of the editor.
4. Paste your API key into the masked key field.
5. Enter your provider's full **HTTPS Chat Completions endpoint**, for example `https://api.openai.com/v1/chat/completions`.
6. Enter a model ID your API account can use. The model must support system/user text messages and produce JSON through an OpenAI-compatible Chat Completions API. This version does not implement Anthropic's native API or the Responses API.
7. Enter a prompt such as `industrial frames around the blocks, thin accents, sparse background panels; channel 1 primary, channel 2 accent, channel 3 highlights`.
8. Click **Generate**. The request may take up to 90 seconds. Closing the prompt window cancels the local request; the provider may still complete and bill a request already received.
9. Review the schematic. Gray marks represent selected object centers. Blue, purple and white stand for channels 1, 2 and 3, regardless of your actual palette. It is a layout preview, not an exact in-game render.
10. Click **Apply** to paste the decoration into the level, or **Discard** to leave the level untouched. Use the editor's normal **Undo** after applying to remove the pasted batch. To try another design, discard and open AI again.

The generated decoration goes onto the editor layer after the highest selected editor layer, capped at 999. Switch to **All** editor layers if it appears hidden. Its Z layer is B1 with Z order -5; adjust this in Edit Group for levels using other background layers.

## What this version generates

It uses scaled and rotated colored inner squares, object ID **211**, to make geometric panels, borders and accents. It never accepts model-supplied object IDs, arbitrary level strings, triggers, executable code or group IDs. It does not generate glow assets, arbitrary object styles, screenshots, or fully polished decoration from the entire GD object library. A prompt's color names are suggestions; your editor color channels determine the actual colors.

Each request accepts at most 800 selected objects in a region no larger than 6000 by 6000 units. Each generated plan contains 1 to 300 objects. Scales are restricted to 0.05 through 8, channels to 1 through 3, and rotated decoration must fit within a 120-unit margin around the selected object centers. Invalid responses are rejected before editing the level. Selection bounds approximate each source object's footprint as 30 by 30 units; large or irregular source objects are not measured exactly.

## API key handling

The key remains in process memory for this game session. It is not written to Geode settings, level data, source files or mod logs. Use **Clear key** to clear the remembered session key. This is not an OS credential-store integration and does not promise secure erasure of all memory copies.

Only the endpoint and model ID are saved between sessions. Requests go directly to the endpoint you enter, using `Authorization: Bearer <your key>`. HTTPS is required and redirects are disabled. Enter a provider you trust: it receives the key, prompt and selected objects' IDs, relative positions, rotations and scales. No screenshot, level name or full-level upload is sent.

API use is billed by your provider; a ChatGPT subscription is separate from API billing. Never put a real key into the repository or workflow.

## Troubleshooting

- **No AI button:** verify that the compiled mod is enabled and that GD and Geode match the versions above.
- **HTTP 401/403:** check the key and model permissions.
- **HTTP 404:** use a full `/chat/completions` URL and a valid model ID.
- **HTTP 429:** check provider credit and rate limits.
- **Invalid decoration plan:** retry with a simpler geometric prompt or a model that follows JSON instructions. Markdown-fenced JSON and extra object fields are deliberately rejected.
- **All objects have the same color:** set different colors for channels 1, 2 and 3; the default channels may be identical.
- **The schematic looks different in game:** it approximates the source geometry and uses placeholder palette colors. Actual GD object rendering and Z layers control the final appearance.

## Local Windows build

Install Visual Studio 2022 with Desktop development with C++, the LLVM/Clang compiler tools, CMake 3.25+, Ninja, Git, and the [Geode CLI](https://github.com/geode-sdk/cli). Install the Geode SDK **v5.11.1**, then set `GEODE_SDK` to its absolute directory. Follow the SDK's Windows toolchain setup at <https://docs.geode-sdk.org/>.

From an x64 Native Tools command prompt configured for Clang, in this project folder:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGEODE_TARGET_PLATFORM=Win64 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

The packaged `.geode` file is produced inside `build`. The CLI may also install it into a configured game profile automatically. GitHub Actions is the simpler route when your local compiler is not already configured.

## Core tests

The platform-independent tests validate request endpoints, plan limits, type checks, rotated bounds and GD serialization. They do not test the Geode UI, editor undo integration or a live API response.

```sh
g++ -std=c++23 -Wall -Wextra -Werror -Ivendor -Isrc tests/plan.cpp -o tests/plan-test
./tests/plan-test
```

## Sources and licensing

- Geode SDK and editor bindings: <https://github.com/geode-sdk/geode> and <https://github.com/geode-sdk/bindings>
- Official build action: <https://github.com/geode-sdk/build-geode-mod>
- GD object serialization reference: <https://github.com/gd-programming/gd.docs/blob/main/docs/resources/client/level-components/level-object.md>
- Object names and scale properties cross-checked against <https://github.com/flowvix/gd-info-explorer>
- Vendored nlohmann/json v3.12.0 is MIT-licensed; its license is in `vendor/LICENSE.nlohmann-json`.

The mod source is MIT-licensed; see `LICENSE`.
