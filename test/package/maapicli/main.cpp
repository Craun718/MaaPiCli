#include "ProjectInterface/Parser.h"
#include <iostream>
#include <meojson/json.hpp>

int main()
{
    std::cerr << "package test: entering main\n";
    std::cerr.flush();

    auto value = json::parse("{}");
    if (!value) {
        std::cerr << "failed to parse test JSON\n";
        return 1;
    }

    std::cerr << "package test: parsed JSON\n";
    std::cerr.flush();

    if (MAA_PROJECT_INTERFACE_NS::Parser::parse_interface(*value)) {
        std::cerr << "an empty object must not be a valid interface\n";
        return 1;
    }

    std::cerr << "package test: rejected empty interface\n";
    std::cerr.flush();

    return 0;
}
