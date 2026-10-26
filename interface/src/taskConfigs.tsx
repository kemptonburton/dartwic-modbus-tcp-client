import React from "@dartwic/interface-sdk/react";
import {defineTaskConfig} from "@dartwic/interface-sdk/tasks";
import {ConfigMap} from "@dartwic/interface-sdk/config-maps";
import {readMap, writeMap} from "./configMaps";
function ModbusTaskConfig(props: any) {
  return <ConfigMap definition={props.task.task_type.endsWith(".read") ? readMap : writeMap}
    resourceKey={`task:${props.task.name}`} context={props}/>;
}
export const taskConfigs = [
  defineTaskConfig({taskType: "modbus_tcp_client.read", component: ModbusTaskConfig, configMap: readMap}),
  defineTaskConfig({taskType: "modbus_tcp_client.write", component: ModbusTaskConfig, configMap: writeMap}),
];
