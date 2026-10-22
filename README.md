# dartwic-modbus-tcp-client

Modbus TCP device discovery, read/write tasks, and module configuration for DARTWIC.

The Engine and Interface SDK snapshots are bundled under `engine/include/sdk` and
`interface/sdk`. Run `npm ci` after cloning; no private DARTWIC checkout is needed
for the interface build. Packaging verifies `sdk-lock.json` before building.

## Settings storage

Version 2.0.0 saves discovery overrides to the workspace's
`settings.json`, under `plugins.modbus_tcp_client.device_discovery`.
The installed `plugin.json` remains the default configuration. This settings UI
requires DARTWIC Engine and Interface 2.0.0 or newer. Core 2.0.0 packages
will be published separately from this plugin release. The read/write tasks
require the 2.0 fixed-channel handle API; use the matching engine release.

## Layout

- `plugin.json`
  - single source manifest for the engine and interface sides
  - declares `minEngineVersion` and `minInterfaceVersion`
- `versions.json`
  - generated compatibility history keyed by released plugin version
- `engine/`
  - source for the engine plugin DLL
  - `src/module_config.json` describes the module name, title, icon, and description
  - `src/default_parameters.json` contains the default parameter values for newly created module instances
- `interface/`
  - source for the interface plugin
- `plugin/`
  - generated release output only

## Build

- Initialize the bundled `vcpkg` submodule after clone:
  - `git submodule update --init --recursive`
- Interface: run `npm run build`
- Engine: configure and build the root `CMakeLists.txt`, or use the provided package/deploy commands

The outputs are written to:

- `plugin/engine/modbus_tcp_client`
- `plugin/engine-debug/modbus_tcp_client` when built in Debug or during `npm run package`
- `plugin/interface/modbus_tcp_client`

## Windows Modbus dependency

The repository's `vcpkg-configuration.json` selects the bundled upstream libmodbus 3.2.0 port in `vcpkg-overlays/libmodbus`. That official vcpkg port includes the Windows `FD_SETSIZE` fix: Windows socket handles are not POSIX descriptor indexes and must not be rejected solely because their numeric value exceeds 64. The pinned source revision is recorded in the overlay's `UPSTREAM.md`; keep its port files and patches together when updating. Native dependency changes require rebuilding and redeploying the engine plugin and its runtime DLL, then restarting the engine.

## Versioning and packaging

- `npm version patch|minor|major|<exact-version>` updates `package.json`, syncs `plugin.json`, and records the current compatibility floor in `versions.json`
- `npm run package` builds release engine, builds debug engine, builds interface when present, and writes the current platform ZIP with both native configurations
- On Linux, `npm run package` uses the `linux-clang-release` CMake preset. Set `VCPKG_ROOT` to a vcpkg checkout or initialize the repository's `vcpkg` submodule. The archive includes the plugin's native library; verify libmodbus and other dynamic dependencies on the target distribution.
- `npm run package -- 0.8.1` bumps to that exact version before packaging

`deployment-settings.json` controls explicit deploy targets:

- `engine_dir`: engine root that receives `plugins/<plugin-id>`
- `interface_dir`: interface root that receives `plugins/<plugin-id>`

Deploy commands:

- `npm run deploy` builds and deploys the release engine variant plus the interface plugin
- `npm run deploy:engine-debug` builds and deploys only the debug engine variant
- `npm run deploy:debug` builds and deploys the debug engine variant plus the interface plugin
- `npm run deploy:interface` builds and deploys only the interface plugin

Build behavior:

- plain `cmake` engine builds never copy into external DARTWIC plugin directories
- plain `npm run build` never copies into external DARTWIC plugin directories
- only the explicit `deploy*` commands copy to paths from `deployment-settings.json`

## Runtime Model

- The engine plugin is a singleton plugin object loaded from `plugins/modbus_tcp_client`.
- Its one Caesar discovery loop checks explicit `device_discovery.targets` and can also sweep bounded local or configured IPv4 CIDRs from `device_discovery.network_scan`. Closed ports are rejected by a short TCP probe before any read-only Modbus requests are sent.
- Automatic local scanning defaults to port `502`, a `/24` minimum prefix, and at most 254 hosts per subnet. Additional CIDRs, ports, unit IDs, timeout, and host limit are configurable from the plugin settings page. No discovery request writes to a Modbus device.
- It registers the Modbus task types on plugin load.
- Setting `device_discovery.enabled` to `false` disables explicit-target discovery and local-network scanning. Existing modules keep their independent connection monitoring and read/write tasks.
- It exposes local module type `tcp_client`, qualified by DARTWIC as
  `modbus_tcp_client.tcp_client`.
- It exposes task types `modbus_tcp_client.read` and
  `modbus_tcp_client.write`.
- Module instances live separately under `modules/*.json`.

### Periodic I/O and diagnostics

Configuration groups adjacent registers, allocates reusable transaction buffers,
declares the write task's fixed command inputs, and retains fixed RAPID handles
for mapped channels and task diagnostics. Each write tick reads its command
handles before taking the module connection mutex. Device transactions run
synchronously under that mutex; completed reads and readbacks are published
after it is released. The task does not parse mappings or resolve channel names
on each tick. Network timeout and reconnect behavior still bound the achievable
period, so a configured task period is not a Modbus response guarantee.

Connection and transaction errors use ARGUS's explicit 10-second firing
separation. Repeated accepted reports refresh one occurrence; the separation
does not replace the Modbus reconnect delay or the request timeout.

## Compatibility

- engine loading is gated by `minEngineVersion`
- interface loading is gated by `minInterfaceVersion`
- both generated plugin output folders copy the root `plugin.json`
- release DARTWIC consumes only `plugin/engine/modbus_tcp_client`
- debug DARTWIC consumes only `plugin/engine-debug/modbus_tcp_client`
- there is no fallback between release and debug engine variants

## Discovery setup presentation

Discovery uses the shared SDK resource descriptors: module connection status is listed under the module, while each read/write task owns four diagnostic channels. Address selections expose channel pickers in an expandable name list; required channels use checked, disabled rows. Review stays compact. The connection monitor is a shared plugin loop rather than a loop created per module. Channel name choices remain in the provisioning draft until Apply.

The connection monitor owns and republishes `{instance}.info.connected` each second, with a three-second stale timeout. Task transactions update internal health without writing a loop-owned channel. A TCP connect alone stays unverified: successful transactions or valid Modbus exception replies establish protocol liveness. When no response has been verified for two seconds, the monitor performs a read-only holding-register probe at address zero using the configured transaction timeout. A mapping exception can coexist with a healthy connection; timeout or socket loss clears health.

## Release package contract

Run `npm run package` on each supported native host. It must build both Release and Debug successfully before producing `plugin-win32-x64.zip` on Windows x64 or `plugin-linux-x64.zip` on Linux x64. Windows Debug PDB symbols are required. The ZIP includes `plugin/engine/modbus_tcp_client/`, `plugin/engine-debug/modbus_tcp_client/`, and `plugin/interface/modbus_tcp_client/`; each contains the matching root `plugin.json`, native libraries live in each native tree's `bin/`, and the Interface entry point is `ui/index.js`. Required root configuration and runtime dependencies remain in the package; supporting files ship inside each plugin side's `assets/` directory.

Upload the root `plugin.json` and one or both exact platform ZIPs to the same GitHub release; keep that manifest in the tagged source too. The tag, package, and every manifest must identify the same version. `release-manifest.json` provides generated SHA-256 checksums. Generic `plugin.zip` and separate debug ZIPs are not accepted. The Engine selects its OS and Release/Debug variant; it never falls back to a different OS.

See the [example plugin's full release contract](https://github.com/kemptonburton/dartwic-example-plugin#github-release-contract) for the complete manifest shape and archive tree. Native packaging requires `VCPKG_ROOT`; optionally set `DARTWIC_VCPKG_INSTALLED_DIR` to reuse installed dependencies. Packaging does not deploy, push, or publish.

## Asset ownership

Managed files in this repository's `assets/` ship inside the installed plugin and
are replaced with it. Keep custom shared files in
`workspace/assets/plugins/<plugin-id>/` using the SDK helpers. Project settings
remain project-scoped; local secrets and runtime data belong to Engine instances.
Workspaces contain authored Git-trackable content; installed Engine and Interface
directories contain replaceable software and local state. Plugin installation
does not copy files into workspaces. See `assets/README.md` for the storage rules.
