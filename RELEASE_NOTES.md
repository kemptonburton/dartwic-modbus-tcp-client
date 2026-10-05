# Modbus TCP Client 1.0.2

- Refreshed and bundled Engine and Interface SDK snapshots; interface builds no longer need a private neighboring checkout.
- Added SDK hash verification before packaging.
- Saved discovery settings under `plugins.modbus_tcp_client.device_discovery` in workspace settings instead of mutating installed plugin defaults.
- Rebuilt Windows x64 Release/Debug engine binaries and the interface bundle; interface type checking passed.
- Applied available non-breaking dependency security fixes.

The settings UI requires the updated storage-enabled development Engine; original
public beta.3 lacks the settings operations. No live Modbus hardware was exercised.
The build toolchain still reports eight npm audit findings in the Vite/esbuild and
Tailwind glob dependency chains; resolving them needs a separate tooling migration.
Do not expose a plugin development server to untrusted networks or build untrusted source.
