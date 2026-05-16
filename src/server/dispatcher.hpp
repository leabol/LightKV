#pragma once 

#include "request.hpp"
#include "response.hpp"
#include <functional>
#include <unordered_map>

using namespace protocol;
using Storage =  std::unordered_map<std::string, std::string>;
using Handler = std::function<Response(const Request&, Storage&)>;

namespace server {
class Dispatcher{
public:
    Dispatcher(std::unordered_map<std::string,std::string> *pStorage)
    {
        storage_ = pStorage;
    };

    void registerHandler(const CommandType cmd, Handler handler) {
        handlers_[static_cast<int>(cmd)] = std::move(handler);
    }

    Response dispatch(const Request &req) {
        auto it = handlers_.find(static_cast<int>(req.cmd));
        if (it == handlers_.end()) {
            return {false, "unsupported"};
        }
        return it->second(req, *storage_);
    }

private:
    Storage *storage_{nullptr};
    std::unordered_map<int, Handler> handlers_;
};
} // namespace server