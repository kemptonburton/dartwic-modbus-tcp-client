import {createStorageClient} from "@dartwic/interface-sdk/storage";

const pluginId = "modbus_tcp_client";

function resultError(result: any, fallback: string) {
  if (!result?.error) return "";
  return String(result?.payload?.error || result?.message || fallback).toUpperCase();
}

export function validIpv4Cidr(value: string) {
  const [address, prefixText, ...rest] = value.split("/");
  if (rest.length > 0) return false;
  const octets = address.split(".");
  if (octets.length !== 4 || octets.some((octet) => !/^\d{1,3}$/u.test(octet) || Number(octet) > 255)) {
    return false;
  }
  if (prefixText === undefined) return true;
  const prefix = Number(prefixText);
  return /^\d{1,2}$/u.test(prefixText) && Number.isInteger(prefix) && prefix >= 0 && prefix <= 32;
}

async function installedPluginFolder(operation: any) {
  const result = await operation("dartwic/plugins/list-installed-engine-plugins", {}, 10000);
  const error = resultError(result, "Could not locate the installed engine plugin.");
  if (error) throw new Error(error);
  const plugins = Array.isArray(result?.payload?.plugins) ? result.payload.plugins : [];
  const record = plugins.find((entry: any) => entry?.id === pluginId);
  if (!record?.folder) throw new Error("THE INSTALLED MODBUS ENGINE PLUGIN FOLDER WAS NOT FOUND.");
  return String(record.folder);
}

type NetworkScanSettings = {
  enabled: boolean;
  include_local_subnets: boolean;
  subnets: string[];
  ports: number[];
  unit_ids: number[];
  probe_timeout_ms: number;
  hosts_per_tick: number;
  max_hosts_per_subnet: number;
  minimum_prefix_length: number;
  rescan_interval_seconds: number;
};

export async function persistDiscovery(operation: any, startAddress: number, endAddress: number,
  networkScan: NetworkScanSettings) {
  const folder = await installedPluginFolder(operation);
  const read = await operation("dartwic/get-file", {
    rootDir: folder,
    filePath: "plugin.json",
  }, 10000);
  const readError = resultError(read, "Could not read the engine plugin manifest.");
  if (readError) throw new Error(readError);
  const manifest = JSON.parse(String(read?.payload?.content || "{}"));
  const storage = createStorageClient(operation);
  const snapshot = await storage.readEffectiveSettings({plugins: {[pluginId]: manifest}});
  const discovery = snapshot.effective.plugins[pluginId].device_discovery || {};
  const existingTargets = Array.isArray(discovery.targets) ? discovery.targets : [];
  const firstTarget = existingTargets[0] && typeof existingTargets[0] === "object"
    ? existingTargets[0]
    : {host: "127.0.0.1", port: 502, unit_ids: [1, 255]};
  const deviceDiscovery = {
    ...discovery,
    enabled: discovery.enabled !== false,
    network_scan: {...discovery.network_scan, ...networkScan},
    targets: [
      {...firstTarget, start_address: startAddress, end_address: endAddress},
      ...existingTargets.slice(1).map((target: any) => ({
        ...target,
        start_address: startAddress,
        end_address: endAddress,
      })),
    ],
  };
  await storage.writeSettingsOverride("workspace", {plugins: {[pluginId]: {
    device_discovery: deviceDiscovery,
  }}}, {revision: snapshot.revision});
}

