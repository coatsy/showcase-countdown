<!-- markdownlint-disable-file -->
# Research: Injecting `.env` secrets into PlatformIO build flags (ESP32/Arduino)

## Research Topics and Questions

1. `extra_scripts` pre-script pattern — complete working `scripts/load_env.py` that parses `.env`, appends `-D KEY=VALUE`, handles string literal escaping (`\"` issue, `env.StringifyMacros`, `'\\"%s\\"'`, `CPPDEFINES` tuple vs raw `BUILD_FLAGS`), and whether `python-dotenv` is needed / how to declare extra Python deps.
2. `platformio.ini` wiring — `extra_scripts = pre:scripts/load_env.py`; does `pre:` vs `post:` matter?
3. Rebuild correctness — does changing `.env` trigger a rebuild? Workarounds. Compare (a) build flags vs (b) generated `secrets.h` header. Recommendation.
4. Alternatives — `${sysenv.VAR}` interpolation; git-ignored `include/secrets.h` + committed template. Tradeoffs.
5. `.gitignore` correctness — canonical PlatformIO `.gitignore`, plus `.env` ignored / `.env.template` committed.

## Status

Complete

## Key Verified Findings

### Stringification helper (authoritative)

- Official helper is `env.StringifyMacro(value)` — SINGULAR, not `StringifyMacros`.
- Added in **PlatformIO Core 6.1.0 (2022-07-06)** per HISTORY.rst: "Added `env.StringifyMacro(value)` helper function for the Advanced Scripting" and "Documented Stringification ... (issue #4310)".
- Exact implementation, `platformio/builder/tools/piobuild.py`:

  ```python
  def StringifyMacro(env, value):
      return '\\"%s\\"' % value.replace('"', '\\\\\\"')
  ```

  Registered via `env.AddMethod(StringifyMacro)` in the same module's `generate()`.
- Core unit test `tests/commands/test_run.py::test_stringification` proves the round-trip for
  `MACRO_2 = projenv.StringifyMacro('Text is "Quoted"')` and
  `MACRO_4 = projenv.StringifyMacro("Special chars: ',(,),[,],:")`.
  Note `$` is NOT in the tested special-char set and is NOT escaped by `StringifyMacro`.

### `CPPDEFINES` tuple vs `BUILD_FLAGS` string (the behavioural difference)

- `build_flags` (ini) and `env.Append(BUILD_FLAGS=...)` funnel through `ProcessFlags` -> `ParseFlagsExtended` (piobuild.py ~L190-L253):
  - `env.ParseFlags(str(raw))` (SCons) tokenizes with shell/shlex semantics -> whitespace splits, one quoting layer consumed.
  - Post-processing: for tuple defines, `if '"' in value: value = value.replace('"', '\\"')`; digit-only values coerced to Python `int`; float-like coerced to `float`.
- `env.Append(CPPDEFINES=[("KEY", value)])` bypasses that parser entirely. No shlex split, no numeric coercion, no auto-escaping. Caller owns the escaping.
- Therefore the ini form needs BOTH outer single quotes and `\\"`:
  `'-D MYSTRING="Text is \\"Quoted\\""'` (documented on the build_flags page).
  The script form needs a `\"`-style value: `("KEY", '\\"My Network\\"')` == `env.StringifyMacro("My Network")`.

### `$` in values is an unhandled hazard

- CPPDEFINES values pass through SCons `env.subst()` (command-line expansion; `piointegration.dump_defines` also does `env.subst(item)`), so a literal `$` in a password is interpreted as an SCons variable reference.
- `StringifyMacro` does not escape `$`. Escape manually: `value.replace("$", "$$")`.
- Corroborated by the interpolation docs warning for `${sysenv.*}`:
  `export WIFI_PASS='\"my\~p\&a\\\$\$\$\$word\"'`.

### Launch types (`pre:` vs `post:`) and construction environments

- `pre:` runs before the development platform's main script. Docs: useful to "pre-generate extra source files ... pass flags to the global building environment". `projenv` is NOT available at PRE.
- `post:` runs after; `projenv` available. **If no prefix is given, `post:` is assumed** (confirmed in `piomisc.GetExtraScripts`: `if scope == "post" and ":" not in item`).
- Construction Environments docs:
  - `Import("env")` at PRE == `DefaultEnvironment()` -> flags apply to project source + frameworks + dependent libraries.
  - "Flags passed to the global construction environment using PRE-type scripts will affect the `projenv` too."
  - `Import("projenv")` (POST only) -> project `src_dir` only. Equivalent to `build_src_flags`.
- `env.IsIntegrationDump()` + `Return()` hook documented on Launch Types for skipping non-build runs.

### Rebuild correctness (answer: YES, defines invalidate cached objects)

- Authoritative proof from the official `build_flags` docs:

  ```ini
  [env:ignore_incremental_builds]
  ; We dynamically change the value of "LAST_BUILD_TIME" macro,
  ; PlatformIO will not cache objects
  build_flags = -DLAST_BUILD_TIME=$UNIX_TIME
  ```

  i.e. a changed `-D` value changes the SCons build-action signature -> objects rebuild. No staleness.
- Consequence / downside: a PRE-stage global `env.Append(CPPDEFINES=...)` changes the command line for the framework core and every library, so editing one secret triggers a FULL rebuild (ESP32 Arduino core included).
- Generated-header alternative gets precise granularity via SCons implicit `#include` dependency scanning + content signature: only TUs that include the header recompile, and a write-if-changed guard means a no-op `.env` edit rebuilds nothing.
- Docs explicitly list "Generate dynamic headers (`*.h`)" as a supported use case (build_flags -> Dynamic build flags section).

### Extra Python packages / `python-dotenv`

- `python-dotenv` is NOT a PlatformIO Core dependency. Verified `platformio/dependencies.py::get_pip_dependencies()`: pip, bottle, click, colorama, marshmallow, pyelftools, pyserial, requests, semantic_version, tabulate (+ PIO Home extras). No dotenv.
- Official way to add deps (scripting/examples/extra_python_packages.html):

  ```python
  Import("env")
  env.Execute("$PYTHONEXE -m pip install pkg1 pkg2")
  # or lazily
  try:
      import some_package
  except ImportError:
      env.Execute("$PYTHONEXE -m pip install some_package")
  ```

- Conclusion: hand-parsing is preferable (zero deps, no pip mutation of the PIO venv, deterministic in CI).

### `${sysenv.VAR}` interpolation

- Supported: `${sysenv.<name>}` embeds an OS env var into any option value.
- Official example in the interpolation docs recommends it for secrets:

  ```
  ; Unix
  ; export WIFI_SSID='\"my\ ssid\ name\"'
  ; Windows
  ; set WIFI_SSID='"my ssid name"'
  ```

- Documented warning: on Unix, `$`, `&`, `~` must be explicitly escaped in the exported value.
- Tradeoff: shell-dependent escaping, not persisted per project, poor DX for teams; but zero project files to leak. Good for CI.

### Canonical PlatformIO `.gitignore`

- Plain `pio project init` -> `platformio/project/commands/init.py::init_cvs_ignore()` writes exactly:

  ```
  .pio
  ```

- `pio project init --ide vscode` -> `platformio/project/integration/tpls/vscode/.gitignore.tpl`:

  ```
  .pio
  .vscode/.browse.c_cpp.db*
  .vscode/c_cpp_properties.json
  .vscode/launch.json
  .vscode/ipch
  ```

- `ProjectGenerator._merge_contents` skips writing if a `.gitignore` already exists, so PIO will not clobber a customised file.
- Other IDE templates for reference: clion `.pio`; qtcreator `.pio` + `.qtc_clangd`; emacs `.pio` + `.clang_complete` + `.ccls`; vim `.pio` + `.clang_complete` + `.gcc-flags.json` + `.ccls`.

### Secret-leakage surfaces (build-flags approach)

- Defines are dumped into IDE integration metadata: `piointegration.DumpIntegrationData` -> `dump_defines(projenv)` -> `.vscode/c_cpp_properties.json` (ignored by the PIO vscode template) and `.pio/build/<env>/idedata.json` (under ignored `.pio`).
- `pio run -t compiledb` writes `compile_commands.json` to the **project root** by default (`COMPILATIONDB_PATH=os.path.join("$PROJECT_DIR", "compile_commands.json")`) and it is NOT in the stock PIO `.gitignore`. Contains full `-D` flags => must be gitignored.
- Verbose builds / CI logs print the full compiler command line including secrets.

### Community evidence (the classic failure mode)

- Topic 20166 "Advanced scripting env.append() macro with string does not compile":
  `env.Append(CPPDEFINES=[("STATION_NAME_1", "FKS7")])` -> `<command-line>:0:16: error: 'FKS7' was not declared in this scope`.
  Escalation: `"\"FKS7\""` still failed; `"\\\"FKS7\\\""` (Python source) worked. Accepted answer by ivankravets.
- Topic 21526 "Inject Board Name into Code" (accepted answer by maxgerhardt):

  ```python
  Import("env")
  board = env["BOARD"]
  macro_value = "\\\"" + board + "\\\""
  env.Append(CPPDEFINES=[("PLATFORMIO_BOARD", macro_value)])
  ```

  Follow-up post shows the alternative: pass the raw identifier and stringify in C++ with
  `#define stringify(s) _stringifyDo(s)` / `#define _stringifyDo(s) #s`.

### Real-world repos using this exact pattern (GitHub code search, 43 hits)

- `elliotnash/ESP32-PIO-Template` -> `scripts/load_env.py`: `python-dotenv` + `env.StringifyMacro(os.getenv(...))`. Cleanest modern example.
- `supcik/lora-valves-control` -> `define_secrets.py`: hand-rolled `.env` parser, strips surrounding double quotes, appends raw tuples (no stringification; works only for token-style values).
- `Baanaaana/esp32-irk-finder` -> `scripts/load_env.py`: hand parser + explicit string-key allowlist using `env.Append(CPPDEFINES=[(key, f'\\"{value}\\"')])`, plus `true/false` -> `1/0` coercion and a `RELEASE_BUILD` bypass.
- Others: `elliotnash/OSU-ECE-341-Project`, `seventor/esp32-wallpanel-e-paper`, `matej-lohnicky/BTC-ticker`, `thatbeautifuldream/homeboard` (`load_wifi_env.py`), `NitsujY/esp32-drive-orbit`, `gbaranski/houseflow` (`apply-dotenv.py`).

## Gotchas Recorded

1. `env.StringifyMacro` (singular). Requires PIO Core >= 6.1.0; keep a manual fallback.
2. `$` in a value is expanded by SCons subst. Escape as `$$`. Not handled by `StringifyMacro`.
3. Digit-only values via `build_flags` are coerced to `int` by `ParseFlagsExtended`; via `CPPDEFINES` tuples they are not. An all-numeric password auto-typed as a number yields `#define PW 12345678` (not a string) -> silent breakage. Drive string-ness off explicit quoting in `.env`, not off value shape.
4. Duplicate keys between `.env` and `build_flags` produce `-D` twice -> "macro redefined" warning.
5. PRE + `env` forces a full rebuild of framework + libs on any secret change; POST + `projenv` limits it to `src/`.
6. `compile_commands.json` at project root leaks secrets and is not in the stock PIO `.gitignore`.
7. `.gitignore` line `.env` does NOT match `.env.template` (different path component), so no negation is needed unless you also use a glob like `.env.*`.
8. Already-tracked files are unaffected by `.gitignore`; requires `git rm --cached .env`.
9. Secrets are plaintext in the firmware image either way; `-D`/header injection is DX, not security.
10. Generated header must use write-if-changed, otherwise every build rewrites it and (with a timestamp) causes perpetual rebuilds.
11. `extra_scripts` with no `pre:`/`post:` prefix defaults to `post:`.
12. Extra scripts cannot be run standalone with `python`; `Import()` only exists inside SCons.

## Source URLs Verified

- <https://docs.platformio.org/en/latest/projectconf/sections/env/options/build/build_flags.html>
- <https://docs.platformio.org/en/latest/projectconf/sections/env/options/build/build_src_flags.html>
- <https://docs.platformio.org/en/latest/projectconf/sections/env/options/advanced/extra_scripts.html>
- <https://docs.platformio.org/en/latest/projectconf/interpolation.html>
- <https://docs.platformio.org/en/latest/scripting/index.html>
- <https://docs.platformio.org/en/latest/scripting/launch_types.html>
- <https://docs.platformio.org/en/latest/scripting/construction_environments.html>
- <https://docs.platformio.org/en/latest/scripting/examples/extra_python_packages.html>
- <https://github.com/platformio/platformio-core/blob/develop/platformio/builder/tools/piobuild.py>
- <https://github.com/platformio/platformio-core/blob/develop/platformio/builder/tools/piomisc.py>
- <https://github.com/platformio/platformio-core/blob/develop/platformio/builder/tools/piointegration.py>
- <https://github.com/platformio/platformio-core/blob/develop/platformio/project/commands/init.py>
- <https://github.com/platformio/platformio-core/blob/develop/platformio/project/integration/tpls/vscode/.gitignore.tpl>
- <https://github.com/platformio/platformio-core/blob/develop/platformio/dependencies.py>
- <https://github.com/platformio/platformio-core/blob/develop/tests/commands/test_run.py>
- <https://github.com/platformio/platformio-core/blob/develop/HISTORY.rst>
- <https://community.platformio.org/t/advanced-scripting-env-append-macro-with-string-does-not-compile/20166>
- <https://community.platformio.org/t/inject-board-name-into-code/21526>
- <https://github.com/elliotnash/ESP32-PIO-Template/blob/main/scripts/load_env.py>
- <https://github.com/supcik/lora-valves-control/blob/main/define_secrets.py>
- <https://github.com/Baanaaana/esp32-irk-finder/blob/main/scripts/load_env.py>

## Recommended Next Research (not done)

- [ ] Empirically time full-rebuild-vs-incremental delta on an ESP32 Arduino project to quantify gotcha 5.
- [ ] Trace exact SCons arg-escaping path on Windows `cmd.exe` vs POSIX for `\"` in `_CPPDEFFLAGS` (behaviour is empirically correct on both; mechanism not traced end-to-end).
- [ ] Check interaction with `build_cache_dir` when defines change.

## Clarifying Questions

- None blocking. (Optional: whether any *library* in the project needs the secrets — that decides PRE/`env` vs POST/`projenv`.)
