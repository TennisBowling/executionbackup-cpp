#include <cpr/cpr.h>
#include <iostream>
#include "json.hpp"
using json = nlohmann::json;

int main()
{
    json body = {
        {"jsonrpc", 2.0},
        {"method", "eth_syncing"},
        {"params", {}},
        {"id", 1}};
    std::cout << body.dump() << std::endl;

    cpr::Response res = cpr::Post(cpr::Url{"http://192.168.86.36:8545"}, cpr::Body{body.dump()}, cpr::Header{{"Content-type", "application/json"}});
    // std::cout << res.text;

    return 0;
}
