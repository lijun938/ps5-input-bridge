# Chiaki / Chiaki-ng Import Implementation Plan

**Goal:** Import existing PS5 pairing credentials from desktop Chiaki and Chiaki-ng without changing Helios imports, account serialization, or the native input/session backend.

**Architecture:** Add an independent Qt settings importer in Core and a Windows registry reader in App. One new UI entry first reads local Chiaki settings and falls back to an INI/CONF file picker. Match manual host addresses to pairing records by console MAC; ask for a LAN address when the source has no usable address. Save the full import through the existing encrypted AccountStore only after all prompts succeed.

**Tech Stack:** .NET 9, C#, WPF, Microsoft.Win32.Registry, xUnit; no new dependencies.

## Behavior and compatibility

- WHEN a PS5 registration contains valid registration/morning keys and a matching manual host, THEN import it as an existing AccountProfile.
- WHEN a valid PS5 registration has no address, THEN request one without asking the user to pair again.
- WHEN credentials already exist (including credentials imported from Helios), THEN skip them without changing the saved account or address.
- WHEN a record is PS4 or incomplete, THEN skip it with a count and field names, never credential values.
- WHEN a file is unrelated/malformed or the user cancels, THEN preserve the original account file and current selection/session.
- Support the shared Qt desktop settings schema: registered_hosts, manual_hosts, target, server_nickname, server_mac, rp_regist_key, and rp_key. Do not import PSN tokens, account passwords, or stored console PINs.
- Preserve raw bytes, including embedded nulls in registration keys. Decode Qt INI quoting, escapes, and @ByteArray values; read registry values using Qt's native representation.
- Support local default and named profiles, with file selection available when local settings are unavailable.

## Task 1: Core importer (test first)

Create `tests/Ps5Bridge.Tests/ChiakiImporterTests.cs` and `src/Ps5Bridge.Core/ChiakiImporter.cs`.

1. Write tests for Qt INI credentials, escaped bytes/quotes, MAC-based manual-host matching, missing hosts, duplicate keys (within/across imports), PS4 filtering, malformed keys, and unrelated files.
2. Run `dotnet test tests/Ps5Bridge.Tests/Ps5Bridge.Tests.csproj -c Release --filter FullyQualifiedName~ChiakiImporterTests`; confirm the missing feature fails.
3. Implement INI parsing and a normalized settings entry point usable by the registry reader. Return ready profiles, host requests, and skip diagnostics.
4. Run the importer tests and the unchanged Helios importer tests.

## Task 2: Local Windows settings

Create `src/Ps5Bridge.App/ChiakiSettingsReader.cs` and matching tests (link the reader into the existing test project).

1. Confirm Qt Windows registry serialization from public Qt source.
2. Test a synthetic registry tree under a unique temporary HKCU test key, including default/named profiles and nested registered_hosts/manual_hosts.
3. Implement read-only local discovery, flatten supported settings, and reuse Core for conversion and deduplication.

## Task 3: WPF integration

Modify `src/Ps5Bridge.App/MainWindow.xaml` and `.xaml.cs`; add `ChiakiImportWindow.xaml` and `.xaml.cs`.

1. Add a separate Chiaki import button and allow account action buttons to wrap at the minimum window width.
2. Read local settings first; provide explicit INI/CONF selection as an alternative source.
3. Preview all candidates in one dialog with editable addresses; collect missing addresses, validate them, and honor cancellation before saving anything.
4. Guard all four account mutation entry points with a shared UI busy state, including asynchronous waits, to prevent overlapping snapshots from overwriting another import. Recheck duplicate keys before one AccountStore save; append profiles only after saving succeeds. Avoid changing an active selection.
5. Disable the new entry if loading the account store failed. Use the existing lifetime cancellation and notification patterns.

## Task 4: Verification and delivery

1. Build the WPF application in Release with warnings treated as errors.
2. Run all existing and new managed/native integration tests.
3. Run `dotnet run --project tests/Ps5Bridge.UiChecks/UiChecks.csproj -c Release` to check WPF layout at normal/minimum widths, bindings, address validation, cancel, and mutual exclusion of account mutation handlers. Use synthetic settings without reading or modifying real credentials or connecting to a PS5.
4. Update README with local import, exported settings, missing addresses, and supported desktop scope.
5. Publish the updated self-contained application and corresponding source package with `tools/publish.ps1 -SkipNativeBuild` if the existing output is available.
6. Review the diff and record verification results. Real-console connection remains a user-side verification.
