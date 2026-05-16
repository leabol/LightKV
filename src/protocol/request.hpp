#pragma once
#include <string>

namespace protocol{

enum class CommandType {
    GET,
    SET,
    DEL
};

struct Request {
    CommandType cmd;
    std::string key;
    std::string value;
};

}//namespace protocol