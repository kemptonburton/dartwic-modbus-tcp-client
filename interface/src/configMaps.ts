import {defineConfigMap, createModuleConfigAdapter, createTaskConfigAdapter, type ConfigMapField} from '@dartwic/interface-sdk/config-maps';
import {persistDiscovery, validIpv4Cidr} from './discoverySettings';
import {readRegisterTypes, writeRegisterTypes} from './shared';

async function request(operation: any, name: string, payload: any = {}) {
  const result = await operation(name, payload, 30000);
  if (!result || result.error) throw new Error(result?.payload?.error || `Failed: ${name}`);
  return result.payload;
}
const number = (path: string, label: string, min: number, max: number, defaultValue?: number): ConfigMapField =>
  ({path, label, type: 'number', required: true, integer: true, min, max, defaultValue});
const text = (path: string, label: string, defaultValue = ''): ConfigMapField => ({path, label, type: 'text', defaultValue});

export const moduleMap = defineConfigMap({
  ...createModuleConfigAdapter(),
  title: 'Modbus connection',
  groups: [{id: 'connection', label: 'Connection', fields: [
    {...text('parameters.server_ip', 'Address', '127.0.0.1'), required: true},
    number('parameters.server_port', 'Port', 1, 65535, 502),
    {...number('parameters.unit_id', 'Unit ID', 0, 255, 255),
      validate: (value) => value > 247 && value !== 255 ? 'Use unit ID 0–247 or 255.' : ''},
    number('parameters.tv_sec', 'Timeout seconds', 0, 3600, 3),
    number('parameters.tv_usec', 'Timeout microseconds', 0, 999999, 0),
    text('parameters.event_system', 'Event system', 'Software'),
    text('parameters.event_subsystem', 'Event subsystem', 'Modbus'),
  ]}],

});

function mappings(read: boolean) {
  const columns: ConfigMapField[] = [
    {path: 'register_type', label: 'Type', type: 'select', required: true, options: read ? readRegisterTypes : writeRegisterTypes},
    number('register', 'Address', 0, 65535),
    {path: 'channel', label: read ? 'Publish channel' : 'Command channel', type: 'channel', channelMode: read ? 'write' : 'read', required: true},
  ];
  if (!read) columns.push({path: 'readback_channel', label: 'Readback channel', type: 'channel', channelMode: 'write',
    defaultValue: (_document, row) => row?.channel ? `${row.channel}_state` : '',
    description: 'Device state read immediately after the writes in every task cycle.'});
  return {path: read ? 'arguments.read_mappings' : 'arguments.write_mappings',
    label: read ? 'Read mappings · Device → RAPID' : 'Write mappings · RAPID → Device', columns,
    createRow: () => ({register_type: read ? 'input_register' : 'coil'})};
}

export function taskMap(read: boolean) {
  return defineConfigMap({
    ...createTaskConfigAdapter(),
    title: read ? 'Modbus read' : 'Modbus write',
    groups: [{id: 'task', label: 'Task configuration', fields: [
      {path: 'arguments.module_instance_name', label: 'Module connection', type: 'module', required: true,
        pluginId: 'modbus_tcp_client', moduleTypeIds: ['tcp_client'], maxTasksPerModule: 1, description: 'One read and one write task per connection.'},
    ], tables: [mappings(read)]}],
    validate(value) {
      if (Array.isArray(value.arguments?.mappings)) return 'Legacy task arguments are unsupported; create a new read or write task.';
      const rows = value.arguments?.[read ? 'read_mappings' : 'write_mappings'];
      return !rows?.length ? 'Configure at least one mapping.' : '';
    },

  });
}
export const readMap = taskMap(true);
export const writeMap = taskMap(false);

// Lists remain text drafts until Enter; only the committed text is parsed.
const list = (path: string, label: string, numeric = false): ConfigMapField => ({path, label, type: 'text',
  render: undefined,
  // Parsing is supplied to the shared cell rather than to each editor.
  parse: (raw: string) => raw.split(',').map((value) => value.trim()).filter(Boolean).map((value) => numeric ? Number(value) : value),
});
export const pluginMap = defineConfigMap({
  title: 'Modbus discovery settings',
  groups: [{id: 'discovery', label: 'Device discovery', fields: [
    number('start_address', 'Start address', 0, 65535, 0), number('end_address', 'End address', 0, 65535, 255),
    {path: 'network_scan.enabled', label: 'Network discovery', type: 'toggle'},
    {path: 'network_scan.include_local_subnets', label: 'Scan local subnets', type: 'toggle'},
    list('network_scan.subnets', 'Additional IPv4 subnets'), list('network_scan.ports', 'Ports', true),
    list('network_scan.unit_ids', 'Unit IDs', true),
    number('network_scan.probe_timeout_ms', 'Port timeout (ms)', 10, 2000, 50),
    number('network_scan.hosts_per_tick', 'Hosts per pass', 1, 64, 8),
    number('network_scan.max_hosts_per_subnet', 'Maximum hosts per subnet', 1, 4096, 254),
    number('network_scan.minimum_prefix_length', 'Minimum prefix length', 16, 30, 24),
    number('network_scan.rescan_interval_seconds', 'Rescan interval (seconds)', 10, 86400, 300),
  ]}],
  validate(value) {
    if (value.end_address < value.start_address || value.end_address - value.start_address >= 4096) return 'Use an ordered range of at most 4096 addresses.';
    const scan = value.network_scan;
    if (!scan.ports.length || scan.ports.some((port: number) => !Number.isInteger(port) || port < 1 || port > 65535)) return 'Use ports 1–65535.';
    if (!scan.unit_ids.length || scan.unit_ids.some((id: number) => !Number.isInteger(id) || id < 0 || (id > 247 && id !== 255))) return 'Use unit IDs 0–247 or 255.';
    if (scan.subnets.some((value: string) => !validIpv4Cidr(value))) return 'Use valid IPv4 subnets.';
    return '';
  },
  async load(context) { return request(context.operation, 'modbus_tcp_client.get_discovery_settings'); },
  async apply(next, previous, context) {
    const network = {...next.network_scan}; delete network.queued_endpoints;
    // Persist first, then apply live. Failure after persistence is reported as such.
    await persistDiscovery(context.operation, next.start_address, next.end_address, network);
    try {
      await request(context.operation, 'modbus_tcp_client.configure_discovery', {...next, network_scan: network});
    } catch (error: any) {
      throw new Error(`Settings saved; live discovery update failed: ${error.message}. Retry to reconcile.`);
    }
    void previous;
    return next;
  },
});
