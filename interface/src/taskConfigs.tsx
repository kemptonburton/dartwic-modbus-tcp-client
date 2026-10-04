import React from "@dartwic/interface-sdk/react";
import { defineTaskConfig, useTaskConfigBridge } from "@dartwic/interface-sdk/tasks";
import {Input, Label} from "@dartwic/interface-sdk/ui/general";
import {
  convertChannelReferenceToChannelName,
  TaskBindingTable,
} from "@dartwic/interface-sdk/ui/dartwic";
import {
  buildReadWritePayload, hasLegacyTaskArguments, normalizeReadMappings,
  normalizeReadbackInterval, normalizeWriteMappings, readRegisterTypes,
  stableStringify, writeRegisterTypes,
} from "./shared";

function ModbusTaskConfig({ task, operation, onSaved, onClose, taskEditor }: any) {
  const isReadTask = task.task_type === "modbus_tcp_client.read";
  const [selectedInstance, setSelectedInstance] = React.useState(task.arguments?.module_instance_name || "");
  const [readbackInterval, setReadbackInterval] = React.useState(() => normalizeReadbackInterval(task.arguments));
  const [readMappings, setReadMappings] = React.useState(() => normalizeReadMappings(task.arguments, convertChannelReferenceToChannelName));
  const [writeMappings, setWriteMappings] = React.useState(() => normalizeWriteMappings(task.arguments, convertChannelReferenceToChannelName));
  const [errorMessage, setErrorMessage] = React.useState("");
  const [isSaving, setIsSaving] = React.useState(false);
  const legacy = hasLegacyTaskArguments(task.arguments);

  const payload = React.useMemo(() => {
    const combined = buildReadWritePayload(selectedInstance, readbackInterval, readMappings, writeMappings);
    return isReadTask
      ? { module_instance_name: combined.module_instance_name, read_mappings: combined.read_mappings }
      : { module_instance_name: combined.module_instance_name, write_mappings: combined.write_mappings,
          readback_interval_seconds: combined.readback_interval_seconds };
  }, [isReadTask, selectedInstance, readbackInterval, readMappings, writeMappings]);
  const initialPayload = React.useMemo(() => {
    const combined = buildReadWritePayload(task.arguments?.module_instance_name || "", normalizeReadbackInterval(task.arguments),
      normalizeReadMappings(task.arguments, convertChannelReferenceToChannelName),
      normalizeWriteMappings(task.arguments, convertChannelReferenceToChannelName));
    return isReadTask
      ? { module_instance_name: combined.module_instance_name, read_mappings: combined.read_mappings }
      : { module_instance_name: combined.module_instance_name, write_mappings: combined.write_mappings,
          readback_interval_seconds: combined.readback_interval_seconds };
  }, [isReadTask, task]);
  const isDirty = stableStringify(payload) !== stableStringify(initialPayload);
  const moduleConnection = React.useMemo(() => ({
    pluginId: "modbus_tcp_client",
    moduleTypeIds: ["tcp_client"],
    value: selectedInstance,
    onValueChange: setSelectedInstance,
    placeholder: "SELECT ONE MODBUS CONNECTION",
    description: "Each connection allows one read task and one write task.",
    showConnectionStatus: true,
  }), [selectedInstance]);

  async function saveTask() {
    if (!selectedInstance) return setErrorMessage("SELECT A MODBUS MODULE INSTANCE.");
    if (isReadTask ? payload.read_mappings.length === 0 : payload.write_mappings.length === 0) {
      return setErrorMessage(isReadTask ? "ADD AT LEAST ONE READ MAPPING." : "ADD AT LEAST ONE WRITE MAPPING.");
    }
    setIsSaving(true);
    setErrorMessage("");
    try {
      const result = await operation("dartwic/create-task", {
        portal_name: task.portal,
        task_name: task.name,
        task_type: task.task_type,
        arguments: payload,
      }, 30000);
      if (result?.error) return setErrorMessage((result?.payload?.error || "FAILED TO SAVE TASK.").toUpperCase());
      await onSaved?.();
      await onClose?.();
    } finally {
      setIsSaving(false);
    }
  }

  useTaskConfigBridge(taskEditor, {
    isDirty, isSaving, canSave: !legacy, errorMessage,
    saveLabel: "SAVE", cancelLabel: "CANCEL", onSave: saveTask, onCancel: onClose,
    moduleConnection,
  });

  return (
    <div className="flex h-full min-h-0 flex-col gap-4">
      {legacy ? <div className="rounded-md border border-destructive p-3 text-sm text-destructive">
        Legacy Modbus task arguments are unsupported. Create a new Modbus read or write task.
      </div> : null}
      {!isReadTask ? <div className="space-y-2">
        <Label>READBACK INTERVAL SECONDS</Label>
        <Input type="number" min="0" step="0.1" value={readbackInterval}
          onChange={(event: any) => setReadbackInterval(event.target.value)} />
      </div> : null}
      {isReadTask ? <TaskBindingTable
        title="READ MAPPINGS (DEVICE → RAPID)"
        bindings={readMappings}
        onBindingsChange={setReadMappings}
        bindingTypes={readRegisterTypes}
        channelMode="write"
        normalizeChannelValue={convertChannelReferenceToChannelName}
        createBinding={(sequence: number) => ({
          id: `read-${sequence}`,
          registerType: readRegisterTypes[0].value,
          register: "",
          channel: "",
        })}
      /> : null}
      {!isReadTask ? <TaskBindingTable
        title="WRITE MAPPINGS (RAPID → DEVICE)"
        bindings={writeMappings}
        onBindingsChange={setWriteMappings}
        bindingTypes={writeRegisterTypes}
        channelMode="read"
        normalizeChannelValue={convertChannelReferenceToChannelName}
        createBinding={(sequence: number) => ({
          id: `write-${sequence}`,
          registerType: writeRegisterTypes[0].value,
          register: "",
          channel: "",
        })}
      /> : null}
    </div>
  );
}

export const taskConfigs = [
  defineTaskConfig({ taskType: "modbus_tcp_client.read", component: ModbusTaskConfig }),
  defineTaskConfig({ taskType: "modbus_tcp_client.write", component: ModbusTaskConfig }),
];
