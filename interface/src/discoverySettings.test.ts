import test from 'node:test';
import assert from 'node:assert/strict';
import {persistDiscovery} from './discoverySettings.ts';

test('discovery edits use the existing workspace override and retain unknown fields', async () => {
  const writes: any[] = [];
  const discovery = {unknown: 'keep', network_scan: {future_option: 42, enabled: false},
    targets: [{host: 'device', start_address: 1, end_address: 2, future_target: true}]};
  const operation = async (name: string, payload: any) => {
    if (name === 'dartwic/plugins/list-installed-engine-plugins') return {payload: {plugins: [{id: 'modbus_tcp_client', folder: 'plugins/modbus'}]}};
    if (name === 'dartwic/get-file') return {payload: {content: '{}'}};
    if (name === 'dartwic/settings/get') return {payload: {revision: 'observed-revision', effective: {plugins: {modbus_tcp_client: {device_discovery: discovery}}}}};
    assert.equal(name, 'dartwic/settings/configure');
    writes.push(payload); return {payload: {}};
  };
  await persistDiscovery(operation, 3, 20, {enabled: true, include_local_subnets: true, subnets: [], ports: [502],
    unit_ids: [1], probe_timeout_ms: 50, hosts_per_tick: 8, max_hosts_per_subnet: 254,
    minimum_prefix_length: 24, rescan_interval_seconds: 300});
  assert.equal(writes.length, 1);
  assert.equal(writes[0].scope, 'workspace');
  assert.equal(writes[0].revision, 'observed-revision');
  const saved = writes[0].patch.plugins.modbus_tcp_client.device_discovery;
  assert.equal(saved.unknown, 'keep');
  assert.equal(saved.network_scan.future_option, 42); assert.equal(saved.network_scan.enabled, true);
  assert.deepEqual(saved.targets, [{host: 'device', start_address: 3, end_address: 20, future_target: true}]);
  assert.equal(discovery.network_scan.enabled, false);
});
