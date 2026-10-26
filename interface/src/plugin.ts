import {moduleMap, readMap, writeMap, pluginMap} from "./configMaps";
import { definePlugin } from "@dartwic/interface-sdk";
import { taskCards } from "./taskCards";

export default definePlugin({
  id: "modbus_tcp_client",
  name: "Modbus TCP Client",
  register(registry) {
    registry.addTaskUi({
      id: "read",
      name: "Modbus Read",
      configMap: readMap,
      card: taskCards[0].component,
    });
    registry.addTaskUi({
      id: "write",
      name: "Modbus Write",
      configMap: writeMap,
      card: taskCards[1].component,
    });
    registry.addModuleUi({
      id: "tcp_client",
      name: "Modbus TCP Client",
      configMap: moduleMap,
      connection: ({instanceConfig}) => ({
        channel: `${instanceConfig.name}.info.connected`,
        label: "Modbus TCP",
        endpoint: `${instanceConfig.parameters?.server_ip || "127.0.0.1"}:${instanceConfig.parameters?.server_port ?? 502}`,
        connectedValue: 1,
      }),
    });
    registry.addSettingsPanel({
      id: "discovery",
      name: "Discovery Settings",
      configMap: pluginMap,
    });
  },
});
