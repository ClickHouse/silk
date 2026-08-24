#include "config.h"
#include "executor.h"
#include "pipeline.h"

#include <perf/util/parse.h>
#include <perf/util/report.h>
#include <silk/fibers/fiber.h>
#include <silk/util/assert.h>
#include <silk/util/crash-dumper.h>
#include <silk/util/init.h>
#include <silk/util/logger.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <cxxopts.hpp>

int main(int argc, char ** argv)
{
    silk::installCrashDumper();

    std::string configFile;
    std::string duration;
    std::string warmup;

    // cxxopts splits list values on commas, so one --param argument may carry several pairs.
    std::vector<std::string> paramArgs;
    bool countersRequested = false;
    bool cpuAdjustDisabled = false;
    bool verbose = false;

    cxxopts::Options cli("fibers-simulator", "fiber scheduler workload simulator");

    // clang-format off
    cli.add_options()
        ("config",         "pipeline config file",                            cxxopts::value<std::string>(configFile))
        ("duration",       "override the run duration",                       cxxopts::value<std::string>(duration))
        ("warmup",         "override the warmup",                             cxxopts::value<std::string>(warmup))
        ("param",          "override a config param (name=value, repeatable)", cxxopts::value<std::vector<std::string>>(paramArgs))
        ("print-counters", "print scheduler latency and counters",            cxxopts::value<bool>(countersRequested))
        ("disable-cpu-adjust", "pin the scheduler CPU width at full",    cxxopts::value<bool>(cpuAdjustDisabled))
        ("v,verbose",      "enable debug logging",                            cxxopts::value<bool>(verbose))
        ("h,help",         "print help");
    // clang-format on

    cli.parse_positional({"config"});
    cli.positional_help("<config>");

    try
    {
        cxxopts::ParseResult parsed = cli.parse(argc, argv);

        if (parsed.count("help") || configFile.empty())
        {
            std::cout << cli.help() << "\n";
            return parsed.count("help") ? 0 : 1;
        }
    }
    catch (const cxxopts::exceptions::exception & ex)
    {
        std::cerr << ex.what() << "\n" << cli.help() << "\n";
        return 1;
    }

    if (verbose)
    {
        silk::Logger::setLevel(silk::LogLevel::DEBUG);
    }

    silk::initialize();

    std::vector<std::pair<std::string, std::string>> paramOverrides;

    for (const std::string & paramArg : paramArgs)
    {
        size_t equals = paramArg.find('=');

        bool valid = equals != std::string::npos && equals != 0;
        SILK_ASSERT(valid, "invalid --param '%s' - expected name=value", paramArg.c_str());

        paramOverrides.emplace_back(paramArg.substr(0, equals), paramArg.substr(equals + 1));
    }

    silk::ConfigReader config;
    config.read(configFile.c_str(), paramOverrides);

    silk::Executor executor;
    executor.parseConfig(config.get("params"));

    if (!duration.empty())
    {
        executor.setDurationNs(parseDuration(duration));
    }

    if (!warmup.empty())
    {
        executor.setWarmupNs(parseDuration(warmup));
    }

    std::unique_ptr<silk::Step> pipeline = silk::makePipeline();
    silk::ConfigReader * pipelineConfig = config.get("pipeline");
    SILK_ASSERT(pipelineConfig, "config %s has no pipeline section", configFile.c_str());
    pipeline->parseConfig(pipelineConfig);

    config.verifyConsumed();

    silk::FiberScheduler::Options options{.enableProfiler = countersRequested, .disableCpuAdjust = cpuAdjustDisabled};
    silk::FiberScheduler::initialize(&options);

    executor.execute(pipeline.get());

    printf("{\n");
    printf("  \"config\": \"%s\",\n", configFile.c_str());

    if (!paramOverrides.empty())
    {
        printf("  \"params\": { ");

        for (size_t i = 0; i < paramOverrides.size(); ++i)
        {
            printf("%s\"%s\": \"%s\"", i ? ", " : "", paramOverrides[i].first.c_str(), paramOverrides[i].second.c_str());
        }

        printf(" },\n");
    }

    executor.printReport();

    if (countersRequested)
    {
        printf(",");
        printSchedulerLatency();
        printf(",");
        printCounters();
    }

    printf("}\n");

    silk::FiberScheduler::destroy();
    silk::destroy();
    return 0;
}
