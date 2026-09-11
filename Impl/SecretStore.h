#pragma once

#include <string>

#include "ProjectInterface/Conf.h"

MAA_PROJECT_INTERFACE_NS_BEGIN

class SecretStore
{
public:
    // Values without the current platform prefix are treated as legacy plaintext
    // while loading and are encrypted again when the configuration is saved.
    static std::string encrypt(const std::string& context, const std::string& value);
    static std::string decrypt(const std::string& context, const std::string& value);
};

MAA_PROJECT_INTERFACE_NS_END
