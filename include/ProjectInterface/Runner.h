#pragma once

#include "Types.h"

#include "ProjectInterface/Conf.h"

MAA_PROJECT_INTERFACE_NS_BEGIN

class Runner
{
public:
    static bool run_pretasks(const std::vector<RuntimeParam::Pretask>& pretasks);
    static bool run(const RuntimeParam& param);
};

MAA_PROJECT_INTERFACE_NS_END
