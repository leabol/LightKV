#pragma once 
#include <string>

namespace protocol{

struct Response {
    bool ok;
    std::string value;
};

}//namespace protocol