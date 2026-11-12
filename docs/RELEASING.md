# Building and publishing a release

Release packages contain one production S/PDIF analyzer and an explicit set of public documents. They exclude the test signal plugin, SDK runtimes, captures, credentials, source checkout metadata, and local planning files. `PACKAGE.json` identifies the centralized version, source commit, pinned AnalyzerSDK revision, binary-derived platform/architecture, minimum Logic version, and runtime/loading limitations. `SHA256SUMS.txt` covers every other archive member; the adjacent `.zip.sha256` covers the whole ZIP. Hashes detect corruption, not publisher identity or code signing.

Maintainers update these highlights and the linked feature/validation documents before tagging. The workflow includes this section in generated release notes.

## Release highlights

- Inspect bounded AC-3, E-AC-3 and DTS frame headers and distinguish transport integrity, codec structure, and supported CRC results. Compressed audio is preserved; it is not decompressed.
- Check professional channel-status CRCC separately from parity and format reliability.
- Compare delay across time windows with explicit gaps, uncertainty and informational drift; generate standalone interactive capture reports from existing exports.
- Exercise malformed inputs with bounded sanitizer fuzz targets and measure the shared pipeline with reproducible workload/cancellation reports.
- Add an explicit sparse-table option that keeps waveform annotations and full CSV samples while reducing dense structured-result ingestion.
- Deliver verified Windows, Linux and macOS production packages through a staged release workflow.

Physical AC-3/DTS payloads were checked against independent sources. Conflicting PCM status remains rejected. Physical unplug/reconnect and reacquisition remain unverified; closing and reopening playback is not carrier-loss evidence. Audio decompression and DTS full-frame CRC verification remain outside scope.

Packages require Logic 2.3.43 or later and identify their exact source/SDK revisions and binary architecture. They are unsigned and not notarized. Windows Logic loading is tested; Linux/macOS GUI loading is not established by CI. Installation, validation and physical-source limitations are documented with the packages.


## Local packaging and verification

The original Windows command remains valid:

```sh
python scripts/package.py --dll build-win/Analyzers/Release/SpdifAnalyzer.dll \
  --output artifacts/SpdifAnalyzer-0.3.0-windows-x64.zip
python scripts/package.py --verify artifacts/SpdifAnalyzer-0.3.0-windows-x64.zip
```

With no arguments, the legacy Windows input and `artifacts/SpdifAnalyzer-windows-x64.zip` output are retained. Existing archives or checksum files are never replaced: choose a new output path for another build. `--plugin PATH` accepts a production Windows DLL, Linux ELF shared object, or macOS Mach-O bundle/dylib and defaults to a versioned, platform/architecture-labelled filename. macOS `.so` and `.dylib` names are both supported by Saleae. Architecture comes from the binary header, including supported universal2 files, rather than the runner label or suffix. The pinned SDK builds one macOS architecture per invocation; the workflow does not claim universal coverage.

For a clean checkout at an existing matching tag:

```sh
python scripts/package.py --plugin build/Analyzers/libSpdifAnalyzer.so --release --tag v0.3.0
python scripts/package.py --verify artifacts/SpdifAnalyzer-0.3.0-linux-x64.zip \
  --release --tag v0.3.0 --source-sha COMMIT_SHA
```

The version is read from `src/core/Version.h`; it is not duplicated in the packager/workflow. Release mode requires a matching `vMAJOR.MINOR.PATCH` tag pointing to HEAD, a clean tracked checkout, and any explicitly requested source SHA. Local dirty builds are labelled as such and cannot pass release verification. Provenance attests the CI checkout/build process; it does not inspect an embedded binary version or prove a separately supplied binary was built from that source. ZIP ordering, metadata timestamps, and permissions are fixed; identical inputs produce identical archive bytes within the same Python/zlib toolchain. Compiler binaries and compression implementations can differ, so this is not a claim of reproducible compiler output across toolchains.

Archive/checksum files are staged and verified before atomic hard-link promotion. Their names must not already exist. A filesystem must support hard links within the output directory; unsupported filesystems fail rather than use an overwrite-prone fallback. If a promotion fails, the packager removes only files created by that attempt. The pair cannot be promoted as a single filesystem transaction; publication consumes only complete, reverified pairs.

## Safe workflow rehearsal

Run the **Release packages** workflow with `workflow_dispatch` against the intended source ref. This builds/tests/packages all three platforms and runs sanitizer and bounded fuzz gates, then verifies the complete platform set and uploads `validated-release-packages` as an Actions artifact. **A dispatch never creates, updates, or publishes a GitHub release, even when dispatched against an existing tag.** It uses repository read permissions; normal Actions artifact uploads still occur. Do not retag an existing public version to repeat a rehearsal.

A successful rehearsal requires:

1. Windows, Linux and macOS Release builds, with all CTest checks passing on each.
2. Linux address/undefined-behavior sanitizer tests and the bounded edge/input/codec fuzz jobs passing.
3. Exactly three platform ZIPs plus their checksums, with matching version/source/SDK provenance, actual binary architecture, allowed contents, and valid inner/outer hashes.

Download the validated artifact and run `scripts/package.py --verify` on its archives locally. Review the generated `RELEASE_NOTES.md`/`RELEASE_MANIFEST.json`, platform architecture, public documentation, and GUI-loading evidence. Dispatch artifacts are explicitly not release-qualified. A successful CI build does not substitute for opening the plugin in Logic. Record actual Windows/Linux/macOS GUI checks separately and describe untested combinations honestly.

## Tagged publication

After source, documentation, release notes and validation are complete, a maintainer creates and pushes a new version tag matching `Version.h`. Tag creation/push and public publication are explicit maintainer actions; packaging itself never writes to GitHub. A pushed `v*` tag triggers the same full validation with release-qualified packages. A mismatch such as `v0.3.1` with version `0.3.0` fails.

Only the publish job has `contents: write`. It runs after all platform, sanitizer, fuzz, and complete-set validation gates succeed. It refuses any existing draft or public release for the tag. It creates a new draft, uploads only the six validated ZIP/checksum assets, downloads each uploaded asset through the authenticated API, and verifies its actual SHA-256 before changing the draft to public. HTTPS redirects within the same origin retain authorization; cross-origin CDN redirects strip it, and HTTP downgrades or URLs with embedded credentials are rejected. Remote JSON responses are limited to 16 MiB; asset downloads are limited to their expected byte length plus one detection byte. No asset uses an overwrite option. Failures before draft creation create no release; failures afterwards leave an unpublished draft for inspection. A failed draft is not silently resumed or overwritten: inspect it and explicitly decide its disposition before rerunning. A rerun refuses an existing version.

Generated release notes link the exact source revision's installation, feature, validation and release documents, and enumerate the packaged architectures. Review these documents before tagging. GitHub also provides source archives independently; those are not plugin installation packages. Workflow concurrency prevents overlapping runs for the same ref; server-side duplicate-release rejection remains a final safeguard.

## Installation, upgrades and runtime scope

Logic **2.3.43 or newer** is required for the structured result API; primary interactive validation has used Logic 2.4.44. Follow [INSTALL.md](INSTALL.md). Select the package matching the OS **and Logic process architecture**. The workflow currently builds Windows x64, Linux x64, and whichever architecture the macOS 15 runner actually emits; an Intel-only or arm64-only package does not establish support for the other architecture.

Close Logic before changing its custom analyzer binary. Preserve a copy of the working analyzer/version and keep unrelated custom analyzers in place. Extract the new package into a separate versioned directory, then point Logic's custom analyzer setting to the chosen production library directory, retaining other plugin paths or copying only this production library alongside existing plugins when necessary. Restart Logic and confirm the analyzer appears and opens an existing test capture. Roll back by selecting the previous directory/library. Do not copy `Analyzer.dll`, `libAnalyzer.so`, or `libAnalyzer.dylib` from the SDK into Logic.

Windows packages use the normal Release MSVC runtime; a compatible Microsoft Visual C++ runtime must be available. Linux builds use Ubuntu 22.04 and its C++/glibc baseline; compatibility with older distributions is not established. macOS packages are built on macOS 15 with the runner's default deployment target; compatibility with older macOS versions is not established by CI. Package metadata describes these limits. There are no signing/notarization credentials in the workflow: binaries are unsigned/not notarized, and macOS Gatekeeper may require the documented custom-analyzer import procedure. Do not disable security features globally to install a plugin.

## Maintainer checks and references

Run `python tests/test_package.py` for binary-format/architecture, content allowlist, provenance, deterministic ZIP, corruption, no-overwrite and failure cleanup tests, plus the actual inline publisher against an in-memory HTTP substitute (existing draft refusal, corrupt-upload rejection and complete-set publication order). CI additionally checks the full package set. No test invents a passing Logic host initialization or publishes a test release.

Primary references: [GitHub release API](https://docs.github.com/en/rest/releases/releases), [release asset upload/download API](https://docs.github.com/en/rest/releases/assets), [workflow permissions](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax), and [Saleae custom analyzer import](https://www.saleae.com/support/extensions-api/protocol-analyzer-sdk/setting-up-developer-directory).
