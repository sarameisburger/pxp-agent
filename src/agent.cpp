#include "src/agent.h"
#include "src/modules/echo.h"
#include "src/modules/inventory.h"
#include "src/modules/ping.h"
#include "src/modules/status.h"
#include "src/external_module.h"
#include "src/schemas.h"
#include "src/errors.h"
#include "src/log.h"
#include "src/uuid.h"
#include "src/string_utils.h"
#include "src/websocket/errors.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <vector>
#include <memory>

LOG_DECLARE_NAMESPACE("agent");

namespace CthunAgent {

static const uint CONNECTION_STATE_CHECK_INTERVAL { 15 };
static const int DEFAULT_MSG_TIMEOUT_SEC { 10 };

Agent::Agent(std::string bin_path) {
    // declare internal modules
    modules_["echo"] = std::shared_ptr<Module>(new Modules::Echo);
    modules_["inventory"] = std::shared_ptr<Module>(new Modules::Inventory);
    modules_["ping"] = std::shared_ptr<Module>(new Modules::Ping);
    modules_["status"] = std::shared_ptr<Module>(new Modules::Status);

    // TODO(ale): CTH-76 - this doesn't work if bin_path (argv[0]) has
    // only the name of the executable, neither when cthun_agent is
    // called by a symlink. The only safe way to refer to external
    // modules is to store them in a known location (ex. ~/cthun/).

    boost::filesystem::path module_path {
        boost::filesystem::canonical(
            boost::filesystem::system_complete(
                boost::filesystem::path(bin_path)).parent_path().parent_path())
    };
    module_path += "/modules";

    if (boost::filesystem::is_directory(module_path)) {
        boost::filesystem::directory_iterator end;

        for (auto file = boost::filesystem::directory_iterator(module_path);
                file != end; ++file) {
            if (!boost::filesystem::is_directory(file->status())) {
                LOG_INFO(file->path().string());

                try {
                    ExternalModule* external = new ExternalModule(file->path().string());
                    modules_[external->module_name] = std::shared_ptr<Module>(external);
                } catch (...) {
                    LOG_ERROR("failed to load: %1%", file->path().string());
                }
            }
        }
    } else {
        LOG_WARNING("failed to locate the modules directory; external modules "
                    "will not be loaded");
    }
}

Agent::~Agent() {
    if (ws_endpoint_ptr_) {
        // reset callbacks to avoid breaking the WebSocket Endpoint
        // with invalid reference context
        LOG_INFO("Resetting the WebSocket event callbacks");
        ws_endpoint_ptr_->resetCallbacks();
    }
}

void Agent::startAgent(std::string url,
                       std::string ca_crt_path,
                       std::string client_crt_path,
                       std::string client_key_path) {
    listModules();

    try {
        ws_endpoint_ptr_.reset(new WebSocket::Endpoint(url,
                                                       ca_crt_path,
                                                       client_crt_path,
                                                       client_key_path));
    } catch (WebSocket::websocket_error& e) {
        LOG_WARNING(e.what());
        throw fatal_error { "failed to initialize" };
    }

    setConnectionCallbacks();

    try {
        ws_endpoint_ptr_->connect();
    } catch (WebSocket::connection_error& e) {
        LOG_WARNING(e.what());
        throw fatal_error { "failed to connect" };
    }
    monitorConnectionState();
}

//
// Agent - private
//

void Agent::listModules() {
    LOG_INFO("Loaded modules:");
    for (auto module : modules_) {
        LOG_INFO("   %1%", module.first);
        for (auto action : module.second->actions) {
            LOG_INFO("       %1%", action.first);
        }
    }
}

void Agent::setConnectionCallbacks() {
    ws_endpoint_ptr_->setOnOpenCallback(
        [this]() {
            sendLogin();
        });

    ws_endpoint_ptr_->setOnMessageCallback(
        [this](std::string message) {
            processMessageAndSendResponse(message);
        });
}

// onOpen callback
void Agent::sendLogin() {
    DataContainer envelope_stuff {};
    std::string login_id { UUID::getUUID() };
    envelope_stuff.set<std::string>(login_id, "id");
    envelope_stuff.set<std::string>("1", "version");
    envelope_stuff.set<std::string>(StringUtils::getISO8601Time(DEFAULT_MSG_TIMEOUT_SEC),
                                    "expires");
    envelope_stuff.set<std::string>(ws_endpoint_ptr_->identity(), "sender");
    std::vector<std::string> endpoints { "cth://server" };
    envelope_stuff.set<std::vector<std::string>>(endpoints, "endpoints");
    std::vector<std::string> hops {};
    envelope_stuff.set<std::vector<std::string>>(hops, "hops");
    envelope_stuff.set<std::string>("http://puppetlabs.com/loginschema",
                                    "data_schema");
    // envelope_stuff.set<std::string>("agent", "data", "type");

    DataContainer data_stuff {};
    data_stuff.set<std::string>("agent", "type");

    // TODO(ale): use tokens for descriptors

    MessageChunk envelope { 0x01, envelope_stuff.toString() };
    MessageChunk data { 0x02, data_stuff.toString() };

    LOG_INFO("Sending login message with id: %1%", login_id);
    LOG_DEBUG("Login message data: %1%", data.data_portion);

    try {
        Message msg { envelope };
        msg.setDataChunk(data);
        auto serialized_msg = msg.getSerialized();


         ws_endpoint_ptr_->send(&serialized_msg[0], serialized_msg.size());


         // ws_endpoint_ptr_->send(envelope_data.toString());
    }  catch(WebSocket::message_error& e) {
        LOG_WARNING(e.what());
        throw e;
    }
}

// TODO: use the new Message class

DataContainer Agent::parseAndValidateMessage(std::string message) {
    DataContainer msg { message };
    valijson::Schema message_schema = Schemas::network_message();
    std::vector<std::string> errors;

    if (!msg.validate(message_schema, errors)) {
        std::string error_message { "message schema validation failed:\n" };
        for (auto error : errors) {
            error_message += error + "\n";
        }

        throw message_validation_error { error_message };
    }

    if (std::string("http://puppetlabs.com/cncschema").compare(
            msg.get<std::string>("data_schema")) != 0) {
        throw message_validation_error { "message is not of cnc schema" };
    }

    valijson::Schema data_schema { Schemas::cnc_data() };
    if (!msg.validate(data_schema, errors, "data")) {
        std::string error_message { "data schema validation failed:\n" };
        for (auto error : errors) {
            error_message += error + "\n";
        }

        throw message_validation_error { error_message };
    }

    return msg;
}

// TODO: use the new Message class

void Agent::sendResponse(std::string receiver_endpoint,
                         std::string request_id,
                         DataContainer response_output) {
    DataContainer msg {};
    std::string response_id { UUID::getUUID() };
    msg.set<std::string>(response_id, "id");
    msg.set<std::string>("1", "version");
    msg.set<std::string>(StringUtils::getISO8601Time(DEFAULT_MSG_TIMEOUT_SEC),
                         "expires");
    msg.set<std::string>(ws_endpoint_ptr_->identity(), "sender");
    std::vector<std::string> endpoints { receiver_endpoint };
    msg.set<std::vector<std::string>>(endpoints, "endpoints");
    std::vector<std::string> hops {};
    msg.set<std::vector<std::string>>(hops, "hops");
    msg.set<std::string>("http://puppetlabs.com/cncresponseschema", "data_schema");
    msg.set<DataContainer>(response_output, "data", "response");

    try {
        std::string response_txt = msg.toString();
        LOG_INFO("Responding to request %1%; response %2%, size %3%",
                  request_id, response_id, response_txt.size());
        LOG_DEBUG("Response %1%:\n%2%", response_id, response_txt);
        ws_endpoint_ptr_->send(response_txt);
    }  catch(WebSocket::message_error& e) {
        LOG_ERROR("Failed to send %1%: %2%", response_id, e.what());
    }
}

// TODO: use the new Message class

// onMessage callback
void Agent::processMessageAndSendResponse(std::string message) {
    LOG_INFO("Received message:\n%1%", message);
    std::unique_ptr<DataContainer> msg;

    try {
        auto m = parseAndValidateMessage(message);
        msg.reset(&m);


        // msg = std::move(parseAndValidateMessage(message));
    } catch (message_validation_error& e) {
        LOG_ERROR("Invalid message: %1%", e.what());
        return;
    }

    std::string request_id = msg->get<std::string>("id");
    std::string module_name = msg->get<std::string>("data", "module");
    std::string action_name = msg->get<std::string>("data", "action");
    std::string sender_endpoint = msg->get<std::string>("sender");

    try {
        if (modules_.find(module_name) != modules_.end()) {
            std::shared_ptr<Module> module = modules_[module_name];

            auto response_output = module->validateAndCallAction(action_name,
                                                                 *msg);

            sendResponse(sender_endpoint, request_id, response_output);
        } else {
            throw message_validation_error { "unknown module " + module_name };
        }
    } catch (message_error& e) {
        LOG_ERROR("Failed to perform '%1% %2%' for request %3%: %4%",
                  module_name, action_name, request_id, e.what());
        DataContainer err_result;
        err_result.set<std::string>(e.what(), "error");
        sendResponse(sender_endpoint, request_id, err_result);
    }
}

void Agent::monitorConnectionState() {
    for (;;) {
        sleep(CONNECTION_STATE_CHECK_INTERVAL);

        if (ws_endpoint_ptr_->getConnectionState()
                != WebSocket::ConnectionStateValues::open) {
            LOG_WARNING("Connection to Cthun server lost; retrying");
            ws_endpoint_ptr_->connect();
        } else {
            LOG_DEBUG("Sending heartbeat ping");
            ws_endpoint_ptr_->ping();
        }
    }
}

}  // namespace CthunAgent
