#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include "modbus_tcp_client_module.h"
#include "modbus_tcp_client_plugin.h"
#include "modbus_device_discovery.h"

#include <modbus/modbus.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace {
using namespace DARTWIC::API;

bool waitForConnection(int socket) {
    if (socket < 0) return false;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout{3, 0};
#ifdef _WIN32
    return select(0, &readable, nullptr, nullptr, &timeout) > 0;
#else
    return select(socket + 1, &readable, nullptr, nullptr, &timeout) > 0;
#endif
}

double numeric(const ChannelValue& value) {
    return std::visit([](const auto& item) -> double {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, double>) return item;
        if constexpr (std::is_same_v<T, int>) return static_cast<double>(item);
        if constexpr (std::is_same_v<T, bool>) return item ? 1.0 : 0.0;
        return 0.0;
    }, value);
}

class MockRuntime final : public TaskRuntime {
public:
    MockRuntime(std::string name, std::string type, nlohmann::json arguments)
        : name_(std::move(name)), type_(std::move(type)), arguments_(std::move(arguments)) {}
    const std::string& getTaskName() const override { return name_; }
    const std::string& getTaskType() const override { return type_; }
    const nlohmann::json& getMetadata() const override { return metadata_; }
    const nlohmann::json& getArguments() const override { return arguments_; }
    void setArguments(nlohmann::json arguments) { arguments_ = std::move(arguments); }
    double getElapsedSeconds() const override { return 0.0; }
    bool isStopRequested() const override { return false; }
    void setFixedInputChannels(std::vector<std::string> channels) override {
        fixed_input_channels_ = std::move(channels);
    }
    void setRuntimeContext(const std::string& key, std::shared_ptr<void> value) override {
        contexts_[key] = std::move(value);
    }
    std::shared_ptr<void> getRuntimeContext(const std::string& key) const override {
        const auto found = contexts_.find(key);
        return found == contexts_.end() ? nullptr : found->second;
    }
    void removeRuntimeContext(const std::string& key) override { contexts_.erase(key); }
    void clearRuntimeContext() override { contexts_.clear(); }
    void recordWorkerCycle() override {}
private:
    std::string name_;
    std::string type_;
    nlohmann::json metadata_ = nlohmann::json::object();
    nlohmann::json arguments_;
    mutable std::unordered_map<std::string, std::shared_ptr<void>> contexts_;
public:
    std::vector<std::string> fixed_input_channels_;
};

class MockApi final : public SDK_API {
public:
    bool discovery_muted{false};
    bool mute_lookup_failed{false};
    int discovery_requests{0};
    int presence_updates{0};
    nlohmann::json discovery_request;
    bool isNotificationMuted(const std::string& id) override {
        if (id != "device-discovery:127.0.0.1:15099") throw std::runtime_error("Unexpected notification identity");
        if (mute_lookup_failed) throw std::runtime_error("Preference store unavailable");
        return discovery_muted;
    }
    nlohmann::json requestInterfaceUi(const std::string& ui, nlohmann::json payload, nlohmann::json options) override {
        if (options.value("reopen_completed", false)) ++discovery_requests;
        else ++presence_updates;
        discovery_request = {{"request_id", "discovery-test"}, {"ui_id", ui},
            {"payload", std::move(payload)}, {"options", std::move(options)}, {"status", "pending"}, {"muted", false}};
        return discovery_request;
    }
    nlohmann::json getInterfaceUiRequest(const std::string&) override { return discovery_request; }
    double queryChannelField(const std::string& channel, ChannelField,
        std::optional<ChannelValue> fallback) override {
        std::scoped_lock lock(mutex_);
        const auto found = values.find(channel);
        return found == values.end() ? (fallback ? numeric(*fallback) : 0.0) : numeric(found->second);
    }
    void insertChannelField(const std::string& channel, ChannelField, ChannelValue value,
        ChannelStorage storage) override {
        std::scoped_lock lock(mutex_);
        if (values.contains(channel)) throw std::runtime_error("duplicate insert");
        values[channel] = std::move(value);
        storages[channel] = storage;
    }
    void upsertChannelField(const std::string& channel, ChannelField field, ChannelValue value,
        ChannelStorage storage) override {
        std::scoped_lock lock(mutex_);
        if (field == ChannelField::VALUE) values[channel] = std::move(value);
        storages[channel] = storage;
    }
    FixedChannelHandle createFixedChannel(const std::string& channel, double initial_value) override {
        std::scoped_lock lock(mutex_);
        if (!values.contains(channel)) values[channel] = initial_value;
        storages[channel] = ChannelStorage::Fixed;
        return FixedChannelHandle{0, 1, std::make_shared<const std::string>(channel), this};
    }
    double queryChannel(const FixedChannelHandle& handle, double default_value) override {
        return queryChannelField(*handle.channel, ChannelField::VALUE, ChannelValue{default_value});
    }
    bool upsertChannel(const FixedChannelHandle& handle, double value,
        std::optional<uint64_t>) override {
        upsertChannelField(*handle.channel, ChannelField::VALUE, value, ChannelStorage::Fixed);
        return true;
    }
    bool removeChannel(const std::string& channel) override {
        std::scoped_lock lock(mutex_);
        storages.erase(channel);
        return values.erase(channel) > 0;
    }
    std::string registerModuleType(ModuleTypeDefinition definition) override {
        module_type = std::move(definition);
        return "dartwic.modbus-tcp-client." + module_type.id;
    }
    std::string registerPeer(PeerDefinition) override { return {}; }
    std::string registerTaskType(std::string local_id, std::string, TaskTypeDefinition definition) override {
        task_types[local_id] = std::move(definition);
        return "dartwic.modbus-tcp-client." + local_id;
    }
    std::string registerOperation(OperationDefinition) override { return {}; }
    std::string registerTelemetry(TelemetryDefinition) override { return {}; }
    std::string registerDCodeFunction(std::string, std::string, DCodeFunctionHandler, std::string,
        std::vector<DCodeFunctionArgument>, std::vector<DCodeFunctionArgument>,
        DCodePreparedFunctionHandler) override { return {}; }
    std::string registerLoop(std::string local_id, std::string, PluginLoopDefinition definition) override {
        loops[local_id] = std::move(definition);
        return "modbus_tcp_client." + local_id;
    }
    int consoleError(std::string title, std::string description, std::vector<std::string>, std::string, int auto_acknowledge,
        double separation_seconds) override {
        std::scoped_lock lock(mutex_);
        last_error_title = std::move(title);
        last_error_description = std::move(description);
        last_error_separation_seconds = separation_seconds;
        last_error_auto_acknowledge = auto_acknowledge;
        ++error_count;
        return error_count;
    }
    std::shared_ptr<DARTWIC::Modules::BaseModule> getModuleInstance(const std::string& name) override {
        const auto found = modules.find(name);
        return found == modules.end() ? nullptr : found->second;
    }
    std::vector<ModuleInstanceSummary> getModuleInstances(const std::string&) override { return module_summaries; }
    void upsertChannelValueBulk(const std::string&, const std::vector<std::pair<double, uint64_t>>&) override {}
    nlohmann::json callDCodeFunction(const std::string&, const nlohmann::json&) override { return {}; }
    void commandChannel(const std::string& channel, ChannelValue value) override {
        upsertChannelField(channel, ChannelField::VALUE, std::move(value), ChannelStorage::Fixed);
    }
    void setChannel(const std::string& channel, std::optional<ChannelValue> value) override {
        ++set_channel_count;
        observed_channels.push_back(channel);
        if (value) upsertChannelField(channel, ChannelField::VALUE, std::move(*value), ChannelStorage::Fixed);
    }
    void freeChannel(const std::string&) override {}
    void applyAuthorityByName(uint32_t operation, const std::string& channel,
        std::optional<ChannelValue> value) override {
        if (operation == 1) { setChannel(channel, std::move(value)); return; }
        if (value) upsertChannelField(channel, ChannelField::VALUE, std::move(*value), ChannelStorage::Fixed);
    }
    void applyAuthorityByHandle(uint32_t operation, const FixedChannelHandle& channel,
        std::optional<ChannelValue> value) override {
        if (!channel.channel) throw std::runtime_error("Invalid fixed channel handle");
        applyAuthorityByName(operation, *channel.channel, std::move(value));
    }


    ChannelValue value(const std::string& channel) {
        std::scoped_lock lock(mutex_);
        return values.at(channel);
    }
    ChannelStorage storage(const std::string& channel) {
        std::scoped_lock lock(mutex_);
        return storages.at(channel);
    }
    int errorCount() {
        std::scoped_lock lock(mutex_);
        return error_count;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, ChannelValue> values;
    std::unordered_map<std::string, ChannelStorage> storages;
    std::unordered_map<std::string, std::shared_ptr<DARTWIC::Modules::BaseModule>> modules;
    ModuleTypeDefinition module_type;
    std::unordered_map<std::string, TaskTypeDefinition> task_types;
    std::unordered_map<std::string, PluginLoopDefinition> loops;
    std::vector<ModuleInstanceSummary> module_summaries;
    int error_count{0};
    int set_channel_count{0};
    std::vector<std::string> observed_channels;
    std::string last_error_title;
    std::string last_error_description;
    double last_error_separation_seconds{0.0};
    int last_error_auto_acknowledge{-1};
};

class Simulator {
public:
    explicit Simulator(int port) : port_(port) {
        context_ = modbus_new_tcp("127.0.0.1", port_);
        mapping_ = modbus_mapping_new(64, 64, 64, 64);
        if (!context_ || !mapping_) throw std::runtime_error("Unable to create Modbus simulator");
        mapping_->tab_input_registers[10] = 321;
        worker_ = std::thread([this]() {
            int socket = modbus_tcp_listen(context_, 1);
            if (socket < 0 || !waitForConnection(socket) || modbus_tcp_accept(context_, &socket) < 0) {
                failed_.store(true);
                return;
            }
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH]{};
            for (int request = 0; request < 17; ++request) {
                if (!waitForConnection(modbus_get_socket(context_))) {
                    failed_.store(true);
                    return;
                }
                const int length = modbus_receive(context_, query);
                if (length <= 0) {
                    failed_.store(true);
                    return;
                }
                // The device can report a state different from the command.
                // On the last cycle its coil read fails after writes succeed.
                const auto function = query[modbus_get_header_length(context_)];
                if (request >= 9 && function == 1) mapping_->tab_bits[0] = 0;
                if (request >= 9 && function == 3) mapping_->tab_registers[20] = 777;
                const int replied = request >= 13 && function == 1
                    ? modbus_reply_exception(context_, query, MODBUS_EXCEPTION_SLAVE_OR_SERVER_FAILURE)
                    : modbus_reply(context_, query, length, mapping_);
                if (replied < 0) {
                    failed_.store(true);
                    return;
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~Simulator() {
        if (worker_.joinable()) worker_.join();
        if (mapping_) modbus_mapping_free(mapping_);
        if (context_) { modbus_close(context_); modbus_free(context_); }
    }
    uint16_t holding(int address) const { return mapping_->tab_registers[address]; }
    bool failed() const { return failed_.load(); }
private:
    int port_;
    modbus_t* context_{};
    modbus_mapping_t* mapping_{};
    std::thread worker_;
    std::atomic<bool> failed_{false};
};

class TimeoutRecoverySimulator {
public:
    explicit TimeoutRecoverySimulator(int port) : port_(port) {
        context_ = modbus_new_tcp("127.0.0.1", port_);
        mapping_ = modbus_mapping_new(8, 8, 8, 8);
        if (!context_ || !mapping_) throw std::runtime_error("Unable to create timeout-recovery simulator");
        mapping_->tab_input_registers[0] = 777;
        worker_ = std::thread([this]() {
            int server_socket = modbus_tcp_listen(context_, 3);
            if (server_socket < 0) {
                failed_.store(true);
                return;
            }
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH]{};
            for (int connection = 0; connection < 3; ++connection) {
                if (!waitForConnection(server_socket) || modbus_tcp_accept(context_, &server_socket) < 0) {
                    failed_.store(true);
                    return;
                }
                const int length = modbus_receive(context_, query);
                if (length <= 0) {
                    failed_.store(true);
                    return;
                }
                // Deliberately drop two responses. A timed-out synchronous
                // Modbus TCP stream is unsafe to reuse because the late reply
                // could be mistaken for the next transaction.
                if (connection < 2) {
                    modbus_close(context_);
                } else if (modbus_reply(context_, query, length, mapping_) < 0) {
                    failed_.store(true);
                    return;
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~TimeoutRecoverySimulator() {
        if (worker_.joinable()) worker_.join();
        if (mapping_) modbus_mapping_free(mapping_);
        if (context_) { modbus_close(context_); modbus_free(context_); }
    }
    bool failed() const { return failed_.load(); }
private:
    int port_{};
    modbus_t* context_{};
    modbus_mapping_t* mapping_{};
    std::thread worker_;
    std::atomic<bool> failed_{false};
};

class AbruptDisconnectSimulator {
public:
    explicit AbruptDisconnectSimulator(int port) {
        context_ = modbus_new_tcp("127.0.0.1", port);
        mapping_ = modbus_mapping_new(8, 8, 8, 8);
        if (!context_ || !mapping_) throw std::runtime_error("Unable to create disconnect simulator");
        mapping_->tab_input_registers[0] = 909;
        const int listening_socket = modbus_tcp_listen(context_, 1);
        if (listening_socket < 0) throw std::runtime_error("Unable to listen for disconnect test");
        worker_ = std::thread([this, server_socket = listening_socket]() mutable {
            if (!waitForConnection(server_socket) || modbus_tcp_accept(context_, &server_socket) < 0) {
                failed_.store(true);
                return;
            }
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH]{};
            const int length = modbus_receive(context_, query);
            if (length <= 0 || modbus_reply(context_, query, length, mapping_) < 0) {
                failed_.store(true);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            const int client_socket = modbus_get_socket(context_);
            linger reset_linger{};
            reset_linger.l_onoff = 1;
            reset_linger.l_linger = 0;
            if (setsockopt(client_socket, SOL_SOCKET, SO_LINGER,
                    reinterpret_cast<const char*>(&reset_linger), sizeof(reset_linger)) != 0) {
                failed_.store(true);
            }
            modbus_close(context_);
            dropped_.store(true, std::memory_order_release);
        });
    }
    ~AbruptDisconnectSimulator() {
        if (worker_.joinable()) worker_.join();
        if (mapping_) modbus_mapping_free(mapping_);
        if (context_) modbus_free(context_);
    }
    bool waitForDrop() const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!dropped_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return dropped_.load(std::memory_order_acquire);
    }
    bool failed() const { return failed_.load(); }
private:
    modbus_t* context_{};
    modbus_mapping_t* mapping_{};
    std::thread worker_;
    std::atomic<bool> dropped_{false};
    std::atomic<bool> failed_{false};
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class ConnectionProbeSimulator {
public:
    enum class Mode { Responsive, Silent, Exception };
    ConnectionProbeSimulator(int port, Mode mode) {
        context_ = modbus_new_tcp("127.0.0.1", port);
        mapping_ = modbus_mapping_new(1, 1, 1, 1);
        if (!context_ || !mapping_) throw std::runtime_error("Unable to create connection probe simulator");
        worker_ = std::thread([this, mode]() {
            int server = modbus_tcp_listen(context_, 1);
            if (server < 0 || !waitForConnection(server) || modbus_tcp_accept(context_, &server) < 0) return;
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH]{};
            if (!waitForConnection(modbus_get_socket(context_))) return;
            const int length = modbus_receive(context_, query);
            if (length <= 0) return;
            if (mode == Mode::Responsive) modbus_reply(context_, query, length, mapping_);
            if (mode == Mode::Exception) modbus_reply_exception(context_, query, MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
            while (!stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~ConnectionProbeSimulator() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        modbus_mapping_free(mapping_);
        modbus_close(context_);
        modbus_free(context_);
    }
private:
    modbus_t* context_{};
    modbus_mapping_t* mapping_{};
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

void testConnectionMonitor() {
    const int base_port = 23000 + static_cast<int>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
    using Mode = ConnectionProbeSimulator::Mode;
    for (const auto mode : {Mode::Responsive, Mode::Silent, Mode::Exception}) {
        const int port = base_port + static_cast<int>(mode);
        ConnectionProbeSimulator simulator(port, mode);
        MockApi api;
        ModbusTCPClientPlugin plugin(nlohmann::json::object(), &api);
        auto module = std::shared_ptr<DARTWIC::Modules::BaseModule>(plugin.createModule("tcp_client", {
            {"name", "probe"}, {"parameters", {{"server_ip", "127.0.0.1"}, {"server_port", port},
                {"unit_id", 1}, {"tv_sec", 0}, {"tv_usec", 50000}}}
        }, &api));
        auto typed = std::dynamic_pointer_cast<ModbusTCPClientModule>(module);
        require(static_cast<bool>(typed), "Connection probe module was not created");
        typed->monitorConnection();
        const double expected = mode == Mode::Silent ? 0.0 : 1.0;
        require(numeric(api.value("probe.info.connected")) == expected,
            "TCP acceptance without a Modbus response must not publish connected");
        const int publications = api.set_channel_count;
        typed->monitorConnection();
        require(api.set_channel_count == publications + 1,
            "Every monitor tick must publish its current health, including unchanged health");
        require(numeric(api.value("probe.info.connected")) == expected,
            "The monitor restored connected health without a verified response");
        if (mode != Mode::Silent) {
            // Model a task detecting a loss after the monitor owns the channel.
            // The next loop tick must publish the new internal health itself.
            typed->getTCPClient().disconnect();
            typed->monitorConnection();
            require(numeric(api.value("probe.info.connected")) == 0.0,
                "The monitor retained the last green value after task-side disconnect");
        }
    }
    std::cout << "Connection monitor publication and protocol verification passed." << std::endl;
}

void testDiscoveryLifecycle() {
    for (const bool configured_targets : {false, true}) {
        nlohmann::json disabled = {{"device_discovery", {{"enabled", false}}}};
        if (configured_targets) {
            disabled["device_discovery"]["network_scan"] = {{"enabled", true}};
            disabled["device_discovery"]["targets"] = nlohmann::json::array({
                {{"host", "127.0.0.1"}, {"port", 15099}}
            });
        }
        MockApi disabled_api;
        int disabled_scans = 0;
        ModbusDeviceFinder disabled_finder(&disabled_api, disabled, [&](const nlohmann::json&) {
            ++disabled_scans;
            return nlohmann::json::object();
        });
        const auto settings = disabled_finder.settings();
        require(!settings.at("enabled").get<bool>() &&
            !settings.at("network_scan").at("enabled").get<bool>(),
            "Disabled discovery retained enabled targets or network scanning");
        disabled_finder.tick();
        disabled_finder.tick();
        require(disabled_scans == 0 && disabled_api.discovery_requests == 0 &&
            disabled_api.presence_updates == 0 &&
            disabled_finder.settings().at("network_scan").at("queued_endpoints") == 0,
            "Disabled discovery scanned or announced a device");
    }
    MockApi api;
    int scans = 0;
    bool mute_during_scan = false;
    bool online = true;
    std::int64_t now = 100000;
    const nlohmann::json config = {{"device_discovery", {
        {"network_scan", {{"enabled", false}}},
        {"targets", nlohmann::json::array({{{"host", "127.0.0.1"}, {"port", 15099}, {"unit_ids", {1}},
            {"start_address", 0}, {"end_address", 1}}})}
    }}};
    ModbusDeviceFinder finder(&api, config, [&](const nlohmann::json&) {
        ++scans;
        if (!online) throw std::runtime_error("Simulator is offline");
        if (mute_during_scan) api.discovery_muted = true;
        const nlohmann::json table = {{"supported", true}, {"available_count", 2},
            {"ranges", nlohmann::json::array({{{"start", 0}, {"end", 1}}})}};
        return nlohmann::json{{"scanned", {{"start_address", 0}, {"end_address", 1}}},
            {"coils", table}, {"discrete_inputs", table},
            {"holding_registers", table}, {"input_registers", table}};
    }, [&] { return now; });
    api.discovery_muted = true;
    finder.tick();
    finder.tick();
    require(scans == 0 && api.discovery_requests == 0, "Muted discovery must not scan or announce");
    api.discovery_muted = false;
    finder.tick();
    require(scans == 1 && api.discovery_requests == 1, "Unmute must offer the device exactly once");
    const auto& provisioning = api.discovery_request.at("payload").at("provisioning");
    for (const auto& group : provisioning.at("channel_suggestion_editor").at("groups")) {
        for (const auto& option : group.at("task_options")) {
            require(option.value("readback_suffix", std::string{}) ==
                (option.at("task_id") == "write" ? "_state" : ""),
                "Editable discovery write mappings must derive observed-state channels");
        }
    }
    const auto& channels = api.discovery_request.at("payload").at("channels");
    require(channels.size() == 12, "Discovery must include command and readback channels for both writable tables");
    size_t write_count = 0;
    for (const auto& task : provisioning.at("tasks")) {
        if (task.at("task_type") != "write") continue;
        for (const auto& mapping : task.at("arguments").at("write_mappings")) {
            ++write_count;
            const std::string readback = mapping.at("channel").get<std::string>() + "_state";
            require(mapping.at("readback_channel") == readback,
                "Default discovery write mappings must retain their readback channel");
            require(std::any_of(channels.begin(), channels.end(), [&](const auto& channel) {
                return channel.at("name") == readback && channel.at("direction") == "input";
            }), "Each suggested readback must have an input-channel declaration");
        }
    }
    require(write_count == 4, "Discovery must suggest both coil and holding-register writes");
    const auto& resources = provisioning.at("resources");
    require(resources.at("channels").size() == 1 && resources.at("loops").empty() &&
        resources.at("channels").at(0).at("name") == "{instance}.info.connected",
        "Discovery must separate module connection status from task diagnostics");
    for (const auto& task : provisioning.at("channel_suggestion_editor").at("tasks")) {
        const auto& diagnostics = task.at("resources").at("channels");
        require(diagnostics.size() == 4 && diagnostics.at(0).at("name") ==
            "{instance}" + task.at("name_suffix").get<std::string>() + ".modbus.transaction_time_ms",
            "Each Modbus task must preview its own diagnostic channels");
    }
    const auto& options = api.discovery_request["options"];
    require(options["silenceable"] == true && options["mute_scope"] == "engine",
        "Discovery must expose engine-scoped mute");
    require(options["notification_id"] == "modbus_tcp_client:device-discovery:127.0.0.1:15099",
        "Discovery and mute lookup identities must match");
    finder.tick();
    require(scans == 1 && api.discovery_requests == 1, "Pending discovery must not repeat");
    api.discovery_muted = true;
    finder.tick();
    finder.tick();
    require(scans == 1 && api.discovery_requests == 1, "Mute callback must suppress subsequent work");
    require(api.discovery_request["payload"]["presence"]["available"] == false,
        "Mute must withdraw the live offer, not keep claiming a device exists");
    api.discovery_muted = false;
    finder.tick();
    require(scans == 2 && api.discovery_requests == 2, "Unmute callback must permit one fresh offer");
    api.discovery_request["status"] = "completed";
    api.modules["renamed"] = std::make_shared<DARTWIC::Modules::BaseModule>(
        nlohmann::json{{"parameters", {{"server_ip", "127.0.0.1"}, {"server_port", 15099}, {"unit_id", 1}}}}, &api);
    api.module_summaries.push_back({"renamed", "modbus_tcp_client", "tcp_client", ""});
    finder.tick();
    require(scans == 2 && api.discovery_requests == 2, "Configured device must not be offered again");
    api.modules.clear();
    api.module_summaries.clear();
    mute_during_scan = true;
    finder.tick();
    require(scans == 3 && api.discovery_requests == 2, "Mute during scan must prevent announcement");
    api.discovery_muted = false;
    api.mute_lookup_failed = true;
    finder.tick();
    require(scans == 3 && api.discovery_requests == 2, "Unknown mute state must defer discovery");
    api.mute_lookup_failed = false;
    mute_during_scan = false;
    finder.tick();
    require(scans == 4 && api.discovery_requests == 3, "Removed device can be rediscovered after unmute");
    require(api.discovery_request["payload"]["presence"]["valid_until_unix_ms"] == now + 15000,
        "A discovery must carry a bounded presence lease");
    now += 5000;
    finder.tick();
    require(api.discovery_requests == 3 && scans == 5,
        "Pending discovery must revalidate without repeating the full announcement");
    require(api.discovery_request["payload"]["presence"]["valid_until_unix_ms"] == now + 15000,
        "Successful Modbus verification renews the presence lease");
    online = false;
    now += 5000;
    finder.tick();
    require(api.discovery_request["payload"]["presence"]["available"] == false,
        "Turning off the simulator must withdraw its pending offer");
    const auto requests_while_offline = api.discovery_requests;
    finder.tick();
    require(api.discovery_requests == requests_while_offline, "Offline devices cannot reannounce");
    api.discovery_muted = true;
    const auto scans_while_muted = scans;
    finder.tick();
    now += 120000;
    finder.tick();
    require(scans == scans_while_muted && api.discovery_requests == requests_while_offline,
        "Muted offline devices must remain absent without probing");
    api.discovery_muted = false;
    finder.tick();
    require(api.discovery_requests == requests_while_offline, "Unmuting an offline device must not resurrect it");
    online = true;
    finder.tick();
    require(api.discovery_requests == requests_while_offline + 1,
        "Only a new successful scan may rediscover a returning simulator");
    std::cout << "Discovery mute/configuration lifecycle passed." << std::endl;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--connection-only") {
            testConnectionMonitor();
            return 0;
        }
        testDiscoveryLifecycle();
        if (argc > 1 && std::string(argv[1]) == "--discovery-only") return 0;
        const int port = 15000 + static_cast<int>(
            std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
        Simulator simulator(port);
        MockApi api;
        ModbusTCPClientPlugin plugin(nlohmann::json::object(), &api);
        plugin.onPluginLoaded();
        auto module = std::shared_ptr<DARTWIC::Modules::BaseModule>(plugin.createModule("tcp_client", {
            {"name", "simulator"},
            {"parameters", {{"server_ip", "127.0.0.1"}, {"server_port", port}, {"unit_id", 1},
                {"tv_sec", 0}, {"tv_usec", 200000}, {"connection_monitor_enabled", false}}}
        }, &api));
        module->setPluginId("modbus_tcp_client");
        module->setModuleTypeId("tcp_client");
        api.modules["simulator"] = module;

        MockRuntime read_runtime("modbus_read", "modbus_tcp_client.read", {
            {"module_instance_name", "simulator"},
            {"read_mappings", nlohmann::json::array({
                {{"register", 10}, {"channel", "sensor.pressure"}, {"register_type", "input_register"}}
            })}
        });
        MockRuntime write_runtime("modbus_write", "modbus_tcp_client.write", {
            {"module_instance_name", "simulator"},
            {"write_mappings", nlohmann::json::array({
                {{"register", 20}, {"channel", "command.valve"}, {"register_type", "holding_register"}},
                {{"register", 0}, {"channel", "command.coil"}, {"register_type", "coil"},
                    {"readback_channel", "feedback.coil"}}
            })}
        });
        auto& read_type = api.task_types.at("read");
        auto& write_type = api.task_types.at("write");
        read_type.on_configure(read_type, read_runtime);
        write_type.on_configure(write_type, write_runtime);
        require(api.storage("sensor.pressure") == ChannelStorage::Fixed &&
            api.storage("command.valve") == ChannelStorage::Fixed,
            "Mapped Modbus channels were not configured Fixed");
        require(write_runtime.fixed_input_channels_ == std::vector<std::string>{"command.coil", "command.valve"},
            "Modbus configuration did not declare its fixed command snapshot");
        require(api.observed_channels.empty(), "Configuration must not acquire task authority");
        api.upsertChannelField("command.valve", ChannelField::VALUE, 55.0, ChannelStorage::Fixed);
        api.upsertChannelField("command.coil", ChannelField::VALUE, 1.0, ChannelStorage::Fixed);
        if (read_type.on_start) read_type.on_start(read_type, read_runtime);
        read_type.on_task(read_type, read_runtime, 0.01);
        write_type.on_start(write_type, write_runtime);
        write_type.on_task(write_type, write_runtime, 0.01);
        if (api.error_count > 0) {
            require(api.last_error_separation_seconds == 10.0,
                "Modbus connection errors need explicit ARGUS firing separation");
        }
        if (simulator.failed()) throw std::runtime_error("Modbus simulator transaction failed");
        require(simulator.holding(20) == 55, "Command snapshot was not written");
        require(numeric(api.value("command.valve_state")) == 55.0 &&
            numeric(api.value("feedback.coil")) == 1.0,
            "Write cycle must read actual holding-register and coil states without an interval setting");
        const auto observed = [&](const std::string& channel) {
            return std::find(api.observed_channels.begin(), api.observed_channels.end(), channel) != api.observed_channels.end();
        };
        require(observed("sensor.pressure") && observed("command.valve_state") && observed("feedback.coil") &&
            observed("modbus_write.modbus.stale") && !observed("command.valve") && !observed("command.coil"),
            "Starts must set measurements/readbacks/diagnostics only, never command inputs");
        for (int cycle = 1; cycle <= 3; ++cycle) {
            auto arguments = write_runtime.getArguments();
            arguments["readback_interval_seconds"] = cycle == 1 ? 0 : 3600;
            write_runtime.setArguments(arguments);
            write_type.on_configure(write_type, write_runtime);
            api.observed_channels.clear();
            write_type.on_start(write_type, write_runtime);
            require(observed("feedback.coil") && !observed("command.coil"),
                "Restart must reacquire only readback authority");
            api.upsertChannelField("command.valve", ChannelField::VALUE, 55.0 + cycle, ChannelStorage::Fixed);
            api.upsertChannelField("command.coil", ChannelField::VALUE, cycle == 1 ? 0.0 : 1.0, ChannelStorage::Fixed);
            write_type.on_task(write_type, write_runtime, 0.01);
            require(numeric(api.value("feedback.coil")) == 0.0,
                "Readback must follow device state and retain its last observation on read failure");
            require(numeric(api.value("command.valve_state")) == (cycle == 1 ? 56.0 : 777.0),
                "Legacy interval values must not gate same-cycle device readback");
            require(numeric(api.value("modbus_write.modbus.stale")) == (cycle == 3 ? 1.0 : 0.0),
                "Readback failure must mark the write cycle stale");
        }
        require(!simulator.failed(), "Same-cycle readback simulator failed");
        require(numeric(api.value("sensor.pressure")) == 321.0, "Sensor input was not published");
        require(numeric(api.value("modbus_read.modbus.stale")) == 0.0, "Successful read remained stale");
        std::cout << "Same-cycle coil/register readback and start authority passed." << std::endl;

        const int timeout_recovery_port = port + 1000;
        TimeoutRecoverySimulator timeout_recovery_simulator(timeout_recovery_port);
        auto recovery_module = std::shared_ptr<DARTWIC::Modules::BaseModule>(plugin.createModule("tcp_client", {
            {"name", "recovery"},
            {"parameters", {{"server_ip", "127.0.0.1"}, {"server_port", timeout_recovery_port},
                {"unit_id", 1}, {"tv_sec", 0}, {"tv_usec", 50000},
                {"connection_monitor_enabled", false}}}
        }, &api));
        auto typed_recovery_module = std::dynamic_pointer_cast<ModbusTCPClientModule>(recovery_module);
        require(static_cast<bool>(typed_recovery_module), "Timeout-recovery module was not created");
        std::vector<uint16_t> recovery_values(1, 0);
        auto& recovery_client = typed_recovery_module->getTCPClient();
        const bool first_recovery_read = recovery_client.readInputRegisters(0, recovery_values);
        require(!recovery_client.isConnected(),
            "Timed-out Modbus connection was retained with an ambiguous response stream");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const bool second_recovery_read = recovery_client.readInputRegisters(0, recovery_values);
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        const bool third_recovery_read = recovery_client.readInputRegisters(0, recovery_values);
        require(!first_recovery_read,
            "First deliberately dropped response unexpectedly succeeded");
        require(!second_recovery_read,
            "Second deliberately dropped response unexpectedly succeeded");
        require(recovery_client.reconnectCount() == 3,
            "Modbus timeout recovery did not use a fresh connection per ambiguous stream");
        require(third_recovery_read && recovery_values[0] == 777,
            "Modbus client did not recover after bounded reconnect backoff");
        require(!timeout_recovery_simulator.failed(), "Timeout-recovery simulator failed");
        recovery_client.publishConnectionState();
        require(numeric(api.value("recovery.info.connected")) == 1.0,
            "Protocol recovery did not publish connected health");

        const int disconnect_port = port + 2000;
        AbruptDisconnectSimulator disconnect_simulator(disconnect_port);
        auto disconnect_module = std::shared_ptr<DARTWIC::Modules::BaseModule>(plugin.createModule("tcp_client", {
            {"name", "disconnect"},
            {"parameters", {{"server_ip", "127.0.0.1"}, {"server_port", disconnect_port},
                {"unit_id", 1}, {"tv_sec", 0}, {"tv_usec", 200000},
                {"connection_monitor_enabled", false}}}
        }, &api));
        auto typed_disconnect_module = std::dynamic_pointer_cast<ModbusTCPClientModule>(disconnect_module);
        require(static_cast<bool>(typed_disconnect_module), "Disconnect-test module was not created");
        auto& disconnect_client = typed_disconnect_module->getTCPClient();
        std::vector<uint16_t> disconnect_values(1, 0);
        require(disconnect_client.readInputRegisters(0, disconnect_values) && disconnect_values[0] == 909,
            "Disconnect-test client did not establish initial protocol health");
        disconnect_client.publishConnectionState();
        require(numeric(api.value("disconnect.info.connected")) == 1.0,
            "Initial successful transaction did not publish connected health");
        require(disconnect_simulator.waitForDrop(), "Disconnect-test server did not close its client socket");
        const int errors_before_disconnect = api.errorCount();
        require(!disconnect_client.readInputRegisters(0, disconnect_values),
            "Transaction unexpectedly succeeded after the server closed its socket");
        disconnect_client.publishConnectionState();
        require(!disconnect_client.isConnected() && numeric(api.value("disconnect.info.connected")) == 0.0,
            "Server disconnect did not immediately clear client and RAPID connection state");
        require(api.errorCount() > errors_before_disconnect &&
            api.last_error_title.find("MODBUS CONNECTION ERROR") != std::string::npos &&
            api.last_error_separation_seconds == 10.0,
            "Server disconnect was not reported through the host console-error API");
        require(api.last_error_description.find("No error") == std::string::npos,
            "Server disconnect produced the misleading no-error message");
        require(!disconnect_simulator.failed(), "Disconnect simulator failed");

        const int idle_port = port + 3000;
        const int errors_before_idle_monitor = api.errorCount();
        auto idle_module = std::shared_ptr<DARTWIC::Modules::BaseModule>(plugin.createModule("tcp_client", {
            {"name", "idle"},
            {"parameters", {{"server_ip", "127.0.0.1"}, {"server_port", idle_port},
                {"unit_id", 1}, {"tv_sec", 0}, {"tv_usec", 50000}}}
        }, &api));
        api.modules["idle"] = idle_module;
        api.module_summaries.push_back({"idle", "modbus_tcp_client", "tcp_client", ""});
        for (int attempt = 0; attempt < 20 && api.errorCount() == errors_before_idle_monitor; ++attempt) {
            api.loops.at("connection_monitor").on_loop();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        require(numeric(api.value("idle.info.connected")) == 0.0,
            "Idle disconnected module did not retain disconnected channel state");
        require(api.errorCount() > errors_before_idle_monitor &&
            api.last_error_title.find("MODBUS CONNECTION ERROR [idle]") != std::string::npos &&
            api.last_error_separation_seconds == 10.0,
            "Shared connection monitor did not report an error while Modbus tasks were stopped");
        require(api.last_error_auto_acknowledge == 0,
            "Connection event unexpectedly auto-acknowledges while the fault is firing");

        MockRuntime duplicate("duplicate_read", "modbus_tcp_client.read", read_runtime.getArguments());
        bool duplicate_rejected = false;
        try { read_type.on_configure(read_type, duplicate); }
        catch (const std::exception&) { duplicate_rejected = true; }
        require(duplicate_rejected, "A second read task was allowed on one Modbus connection");

        // Failed preparation cannot leak a destination claim or release the old one.
        api.modules["alternate"] = module;
        auto invalid_arguments = read_runtime.getArguments();
        invalid_arguments["module_instance_name"] = "alternate";
        invalid_arguments["read_mappings"] = nlohmann::json::array();
        MockRuntime failed_reassignment("modbus_read", "modbus_tcp_client.read", invalid_arguments);
        bool reassignment_rejected = false;
        try { read_type.on_configure(read_type, failed_reassignment); }
        catch (const std::exception&) { reassignment_rejected = true; }
        require(reassignment_rejected, "Invalid reassignment was accepted");
        duplicate_rejected = false;
        try { read_type.on_configure(read_type, duplicate); }
        catch (const std::exception&) { duplicate_rejected = true; }
        require(duplicate_rejected, "Failed reassignment released the old module slot");
        auto alternate_arguments = read_runtime.getArguments();
        alternate_arguments["module_instance_name"] = "alternate";
        MockRuntime alternate("alternate_read", "modbus_tcp_client.read", alternate_arguments);
        read_type.on_configure(read_type, alternate);
        read_type.cleanup(alternate);

        MockRuntime reassigned("modbus_read", "modbus_tcp_client.read", alternate_arguments);
        read_type.on_configure(read_type, reassigned);
        read_type.on_configure(read_type, duplicate);
        read_type.cleanup(duplicate);
        read_type.cleanup(read_runtime);
        write_type.cleanup(write_runtime);

        std::vector<std::unique_ptr<MockRuntime>> contenders;
        std::vector<std::thread> configuring;
        std::atomic<int> winners{0};
        std::atomic<int> winner_index{-1};
        for (int index = 0; index < 8; ++index)
            contenders.push_back(std::make_unique<MockRuntime>("concurrent_read_" + std::to_string(index),
                "modbus_tcp_client.read", read_runtime.getArguments()));
        for (int index = 0; index < 8; ++index) configuring.emplace_back([&, index] {
            try {
                read_type.on_configure(read_type, *contenders[index]);
                ++winners;
                winner_index.store(index);
            } catch (const std::exception&) {}
        });
        for (auto& thread : configuring) thread.join();
        require(winners.load() == 1, "Concurrent module claims did not admit exactly one read task");
        read_type.cleanup(*contenders[winner_index.load()]);
        std::cout << "Modbus split read/write simulator integration passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
