import React from "@dartwic/interface-sdk/react";
import {defineModuleConfig} from "@dartwic/interface-sdk/module-configs";
import {ConfigMap, getConfigMapChannels} from "@dartwic/interface-sdk/config-maps";
import {ModuleRuntimeOverview} from "@dartwic/interface-sdk/ui/dartwic";
import {moduleMap, readMap, writeMap} from "./configMaps";
const taskChannels = (task: any) => getConfigMapChannels(task.task_type.endsWith('.read') ? readMap : writeMap, task);
function ModbusModuleConfig(props: any) {
  return <div className="space-y-6">
    <ConfigMap definition={moduleMap} resourceKey={`module:${props.instanceConfig.name}`} context={props}/>
    <ModuleRuntimeOverview instanceName={props.instanceConfig.name} resolveTaskChannels={taskChannels}/>
  </div>;
}
export const moduleConfigs = [defineModuleConfig({component: ModbusModuleConfig})];
