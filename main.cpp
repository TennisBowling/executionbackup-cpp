#include <cpr/cpr.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "util.hpp"
#include "rust_jwt/rust_jwt.hpp"
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include "Simple-Web-Server/server_http.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include <vector>
#include <map>
#include <future>
#include <thread>
#include <csignal>
using json = nlohmann::json;
using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

cpr::Header APPLICATIONJSON = {{"Content-Type", "application/json"}};
std::string SYNCING_JSON = "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"eth_syncing\",\"params\":[]}";

boost::asio::thread_pool pool(std::thread::hardware_concurrency());

void signal_handler(int signal)
{
    spdlog::info("Caught signal {}, stopping.", signal);
    exit(signal);
}

struct request_result
{
    int status;
    std::string body;
    cpr::Header headers;
};

class NodeInstance;

struct check_alive_result
{
    NodeInstance *node;
    int status;
    long long resp_time;
};

class NodeInstance
{
public:
    cpr::Url url;
    std::string url_string;
    int status; // 0 = offline, 1 = online, 2 = online but not synced
    std::string jwt_secret;

    NodeInstance(std::string url, std::string jwt_secret)
    {
        this->url = cpr::Url{url};
        this->url_string = url;
        // this->status = 0;
        this->jwt_secret = jwt_secret;
    }

    void set_online()
    {
        if (this->status == 1)
        {
            return;
        }
        this->status = 1;
        spdlog::info("Node {} is online", this->url_string);
    }

    void set_offline()
    {
        if (this->status == 0)
        {
            return;
        }
        this->status = 0;
        spdlog::info("Node {} is offline", this->url_string);
    }

    void set_alive_but_syncing()
    {
        if (this->status == 2)
        {
            return;
        }
        this->status = 2;
        spdlog::info("Node {} is alive but currently syncing", this->url_string);
    }

    check_alive_result check_alive()
    {

        // we need to use jwt here since we're directly talking to the auth port

        // create jwt object
        const int64_t timestamp = static_cast<int64_t>(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());

        std::string token = make_jwt(this->jwt_secret.data(), &timestamp);

        auto start = std::chrono::high_resolution_clock::now();
        auto response = this->do_request_jwt(SYNCING_JSON, APPLICATIONJSON, token);
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        if (response.status == 0)
        {
            return {this, 0, elapsed};
        }

        json jsondata;
        try
        {
            jsondata = json::parse(response.body);
        }
        catch (const json::parse_error &e)
        {
            this->set_offline();
            spdlog::error("Error parsing json: {}", e.what());
            spdlog::error("Response body: {}", response.body);
            return check_alive_result{this, 0, elapsed};
        }

        if (jsondata["result"].is_boolean())
        {
            this->set_online();
            return check_alive_result{this, 1, elapsed};
        }
        else if (!jsondata["error"].is_null())
        {
            this->set_offline();
            spdlog::error("Error while checking node {}: {}", this->url_string, jsondata["error"].get<std::string>());
            return check_alive_result{this, 0, elapsed};
        }
        else
        {
            this->set_alive_but_syncing();
            return check_alive_result{this, 2, elapsed};
        }
    }
    // function for without jwt (routed requests call this, and have the jwt in the header already)
    request_result do_request(std::string data, cpr::Header headers)
    {
        std::string response;
        cpr::Header response_headers;
        int status;
        // replace whatever content encoding accept there is with identity
        headers.erase("Accept-Encoding");
        headers.emplace("Accept-Encoding", "identity");

        auto r = cpr::Post(
            this->url,
            cpr::Body{data},
            headers);
        response = r.text;
        status = r.status_code;
        response_headers = r.header;
        response_headers.erase("transfer-encoding"); // crow will auto set this header depending if it wants to chunk the response or not.
                                                     // leaving it here will mean the client expects it to be chunked but crow doesn't chunk.

        if (status == 0)
        {
            this->set_offline();
            spdlog::error("Error: {}", r.error.message);
        }
        return request_result{status, response, response_headers};
    }

    // function for with jwt (requests created from recheck call this)
    request_result do_request_jwt(std::string data, cpr::Header headers, std::string token)
    {
        std::string response;
        cpr::Header response_headers;
        int status;
        // replace whatever content encoding accept there is with identity
        headers.erase("Accept-Encoding");
        headers.emplace("Accept-Encoding", "identity");

        auto r = cpr::Post(
            this->url,
            cpr::Body{data},
            headers,
            cpr::Bearer{token});
        response = r.text;
        status = r.status_code;
        response_headers = r.header;

        if (status == 0)
        {
            this->set_offline();
            spdlog::error("Error: {}", r.error.message);
        }
        return request_result{status, response, response_headers};
    }
};

class NodeRouter
{
public:
    std::vector<NodeInstance> nodes;
    std::vector<NodeInstance> alive;
    std::vector<NodeInstance> dead;
    std::vector<NodeInstance> alive_but_syncing;
    double fcU_invalid_threshold;
    int index;

    NodeRouter(std::vector<std::string> urls, double fcU_invalid_threshold, std::string jwt)
    {
        this->fcU_invalid_threshold = fcU_invalid_threshold;
        this->index = 0;
        for (auto url : urls)
        {
            this->nodes.push_back(NodeInstance(url, jwt));
        }
        spdlog::info("Initialized with {} nodes", this->nodes.size());
    }

    void recheck()
    {
        std::vector<std::future<check_alive_result>> results;
        for (auto &node : this->nodes)
        {
            results.push_back(std::async(&NodeInstance::check_alive, &node));
        }

        // set the alive nodes to the ones that are online, and sort them by their response time
        this->alive.clear();
        this->dead.clear();
        this->alive_but_syncing.clear();
        std::vector<check_alive_result> alive_node_results;

        for (auto &fut : results)
        {
            auto result = fut.get();

            if (result.status == 1)
            {
                alive_node_results.push_back(result);
            }
            else if (result.status == 2)
            {
                this->dead.push_back(*result.node);
            }
            else
            {
                this->alive_but_syncing.push_back(*result.node);
            }
        }

        // sort the alive nodes by their response time. lower index = faster
        std::sort(alive_node_results.begin(), alive_node_results.end(), [](check_alive_result &a, check_alive_result &b)
                  { return a.resp_time < b.resp_time; });

        for (auto &result : alive_node_results)
        {
            this->alive.push_back(*result.node);
        }
    }

    // get the same node on each call, if node ofline, add 1 to the index and try again
    NodeInstance get_execution_node()
    {
        if (this->alive.size() == 0)
        {
            this->recheck();
            return this->get_execution_node();
        }
        else
        {
            auto node = this->alive[this->index];
            if (node.status == 0)
            {
                this->index++;
                if (this->index >= this->alive.size())
                {
                    this->index = 0;
                }
                return this->get_execution_node();
            }
            else
            {
                return node;
            }
        }
    }

    // a function that gets the majority response from a vector of request_results
    // then checks if it is at least this->fcU_invalid_threshold (which is a decimal representation of a percentage)
    // if not, return empty string
    std::string fcU_majority(std::vector<request_result> &results)
    {
        std::map<std::string, int> responses;
        for (auto &result : results)
        {
            responses[result.body]++;
        }
        auto max_response = std::max_element(responses.begin(), responses.end(), [](const std::pair<std::string, int> &a, const std::pair<std::string, int> &b)
                                             { return a.second < b.second; });
        if (max_response->second > results.size() * this->fcU_invalid_threshold)
        {
            return max_response->first;
        }
        else
        {
            return "";
        }
    }

    /*
    logic for forkchoiceUpdated
    there are three possible statuses: VALID, INVALID, SYNCING (SYNCING is safe and will stall the CL)
    ONLY return VALID when all nodes return VALID
    if any node returns INVALID, return SYNCING

    to return SYNCING, the body of the response should be: '{"jsonrpc":"2.0","id":1,"result":{"payloadStatus":{"status":"SYNCING","latestValidHash":null,"validationError":null},"payloadId":null}}'

    BUT if fcU_majority returns INVALID, return it
    if there is a resonse of INVALID and no majority, return SYNCING
    */

    request_result fcU_logic(std::vector<request_result> &resps)
    {
        auto maj = this->fcU_majority(resps);

        if (maj.empty()) // if there is no majority, return SYNCING
        {
            return request_result{200, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"payloadStatus\":{\"status\":\"SYNCING\",\"latestValidHash\":null,\"validationError\":null},\"payloadId\":null}}", cpr::Header{{"Content-Type", "application/json"}, {"Content-Length", "135"}, {"Content-Type", "application/json"}}};
        }

        if (json::parse(maj)["result"]["payloadStatus"]["status"] == "INVALID") // here we see if most responses are INVALID, if so, return INVALID
        {
            return request_result{200, maj, cpr::Header{{"Content-Type", "application/json"}, {"Content-Length", std::to_string(maj.size())}, {"Content-Type", "application/json"}}};
        }

        for (auto &resp : resps) // and here we see if just one response is INVALID, if so, return SYNCING (since it isn't a majority, we can't be sure to reject the block by returning INVALID)
        {
            json j = json::parse(resp.body);
            if (j["result"]["payloadStatus"]["status"] == "INVALID" || j["result"]["payloadStatus"]["status"] == "SYNCING")
            {
                return request_result{200, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"payloadStatus\":{\"status\":\"SYNCING\",\"latestValidHash\":null,\"validationError\":null},\"payloadId\":null}}", cpr::Header{{"Content-Type", "application/json"}, {"Content-Length", "135"}, {"Content-Type", "application/json"}}}; // we cannot use the response's headers since we are replacing the body with our own (invalidating the content type)
            }
        }

        // if we get here, all responses are VALID or SYNCING
        // send to syncing nodes using the first response
        boost::asio::post(pool, [this, resps]()
                          {
                            auto data = resps[0].body;
                            auto headers = resps[0].headers;
                            for (auto &node : this->alive_but_syncing)
                            {
                                boost::asio::post(pool, [&node, data, headers]()
                                                { node.do_request(data, headers); });
                            } });
        return resps[0];
    }

    request_result do_engine_route(std::string data, json j, cpr::Header headers)
    {
        if (j["method"] == "engine_getPayloadV1") // getPayloadV1 is for getting a block to be proposed, so no use in getting from multiple nodes
        {
            auto node = this->get_execution_node();
            spdlog::debug("getPayload request sent to {}", node.url_string);
            return node.do_request(data, headers);
        }
        else if (j["method"] == "engine_forkchoiceUpdatedV1")
        {
            spdlog::debug("forkchoiceUpdated request sent to {} nodes", this->alive.size());
            std::vector<std::future<request_result>> futures;
            for (auto &node : this->alive)
            {
                futures.push_back(std::async(std::launch::async, &NodeInstance::do_request, &node, data, headers)); // call do_request on each node
            }
            std::vector<request_result> resps = join_all_async<request_result>(futures); // wait for all responses to come back
            return this->fcU_logic(resps);                                               // do the fcU_logic
        }
        else
        {
            // wait for the primary node's response, but send to all other nodes
            auto node = this->get_execution_node();
            auto fut = std::async(std::launch::async, &NodeInstance::do_request, &node, data, headers);
            boost::asio::post(pool, [this, data, headers, node]()
                              {
                for (auto &alivenode : this->alive)
                {
                    if (alivenode.url_string != node.url_string)
                    {
                        boost::asio::post(pool, [&alivenode, data, headers]()
                                          { alivenode.do_request(data, headers); });
                    }
                }
                for (auto &alivenode : this->alive_but_syncing)
                {
                    if (node.url_string != node.url_string)
                    {
                        boost::asio::post(pool, [&alivenode, data, headers]()
                                          { alivenode.do_request(data, headers); });
                    }
                } });
            return fut.get();
        }
    }

    request_result route_normal(std::string data, cpr::Header headers)
    {
        auto node = this->get_execution_node();
        spdlog::debug("Routing normal request to {}", node.url_string);
        return node.do_request(data, headers);
    }
};

int main(int argc, char *argv[])
{
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] - %v"); // nice style that i like

    // parse arguments
    auto vm = parse_args(argc, argv);
    // get vm["nodes"] into a vector of strings
    std::vector<std::string> urls;
    // urls.push_back("http://192.168.86.109:8881"); // my personal geth node
    csv_to_vec(vm["nodes"].as<std::string>(), urls);

    auto jwt = read_jwt(vm["jwt-secret"].as<std::string>());
    // auto jwt = read_jwt("C:\\Users\\FASTS\\OneDrive\\Documents\\github\\executionbackup\\jwt.txt");

    boost::asio::post(pool, []()
                      { spdlog::info("Starting threadpool with {} threads", std::thread::hardware_concurrency()); });

    int port;

    if (vm.count("port") == 0)
    {
        port = 8000;
    }
    else
    {
        port = vm["port"].as<int>();
    }

    double fcuinvalidthreshold;

    if (vm.count("fcu-invalid-threshold") == 0)
    {
        fcuinvalidthreshold = 0.6;
    }
    else
    {
        fcuinvalidthreshold = vm["fcu-invalid-threshold"].as<double>();
    }

    std::string listenaddr;

    if (vm.count("listen-addr") == 0)
    {
        listenaddr = std::string("0.0.0.0");
    }
    else
    {
        listenaddr = vm["listen-addr"].as<std::string>();
    }

    HttpServer server;
    server.config.port = port;
    server.config.address = listenaddr;

    // create a node router
    NodeRouter router(urls, fcuinvalidthreshold, jwt);

    router.recheck();

    // setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // call recheck every 30s
    auto _ = std::async(std::launch::async, [&router]()
                        {
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                router.recheck();
            } });

    server.resource["/"]["POST"] = [&router](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request)
    {
        boost::asio::post(pool, [&router, response, request]()
                          {
            std::string body = request->content.string();
        cpr::Header headers;
        // req->header is a unordered_map with only lowercase
        for (auto &header : request->header)
        {
            headers[header.first] = header.second;
        }

        json j = json::parse(body);
        spdlog::debug("Received engine request: {}", j.dump(4));
        if (j["method"].get<std::string>().starts_with("engine_"))
        {
            spdlog::trace("Routing to engine route");
            auto router_resp = router.do_engine_route(body, j, headers);

            SimpleWeb::CaseInsensitiveMultimap headermap;
            for (auto &header : router_resp.headers)
            {
                headermap.emplace(header.first, header.second);
            }
            response->write(status_code_to_enum[router_resp.status], router_resp.body, headermap);
        }
        else
        {
            spdlog::trace("Routing to normal route");
            auto router_resp = router.route_normal(body, headers);

            SimpleWeb::CaseInsensitiveMultimap headermap;
            for (auto &header : router_resp.headers)
            {
                headermap.emplace(header.first, header.second);
            }
            response->write(status_code_to_enum[router_resp.status], router_resp.body, headermap);
        } });
    };

    spdlog::info("Listening on {}:{}", listenaddr, port);
    server.start();

    return 0;
}