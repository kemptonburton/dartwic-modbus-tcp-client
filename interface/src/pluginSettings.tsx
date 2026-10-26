import React from "@dartwic/interface-sdk/react";
import {ConfigMap} from "@dartwic/interface-sdk/config-maps";
import {pluginMap} from "./configMaps";
export function ModbusPluginSettings(props: any) {
  return <ConfigMap definition={pluginMap} resourceKey="plugin:modbus_tcp_client:discovery" context={props}/>;
}
