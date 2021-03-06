/* router_runner.cc
   Jeremy Barnes, 13 December 2012
   Copyright (c) 2012 Datacratic.  All rights reserved.

   Tool to run the router.
*/

#include "router_runner.h"

#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/positional_options.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include "rtbkit/common/bidder_interface.h"
#include "rtbkit/core/router/router.h"
#include "rtbkit/core/banker/slave_banker.h"
#include "soa/service/process_stats.h"
#include "jml/arch/timers.h"
#include "jml/utils/file_functions.h"

using namespace std;
using namespace ML;
using namespace Datacratic;
using namespace RTBKIT;

static inline Json::Value loadJsonFromFile(const std::string & filename)
{
    ML::File_Read_Buffer buf(filename);
    return Json::parse(std::string(buf.start(), buf.end()));
}


/*****************************************************************************/
/* ROUTER RUNNER                                                             */
/*****************************************************************************/


RouterRunner::
RouterRunner() :
    exchangeConfigurationFile("rtbkit/examples/router-config.json"),
    bidderConfigurationFile("rtbkit/examples/bidder-config.json"),
    lossSeconds(15.0),
    noPostAuctionLoop(false),
    logAuctions(false),
    logBids(false),
    maxBidPrice(200),
    slowModeTimeout(MonitorClient::DefaultCheckTimeout),
    useHttpBanker(false)
{
}

void
RouterRunner::
doOptions(int argc, char ** argv,
          const boost::program_options::options_description & opts)
{
    using namespace boost::program_options;

    options_description router_options("Router options");
    router_options.add_options()
        ("loss-seconds,l", value<float>(&lossSeconds),
         "number of seconds after which a loss is assumed")
        ("slowModeTimeout", value<int>(&slowModeTimeout),
         "number of seconds after which the system consider to be in SlowMode")
        ("no-post-auction-loop", bool_switch(&noPostAuctionLoop),
         "don't connect to the post auction loop")
        ("log-uri", value<vector<string> >(&logUris),
         "URI to publish logs to")
        ("exchange-configuration,x", value<string>(&exchangeConfigurationFile),
         "configuration file with exchange data")
        ("bidder,b", value<string>(&bidderConfigurationFile),
         "configuration file with bidder interface data")
        ("use-http-banker", bool_switch(&useHttpBanker),
         "Communicate with the MasterBanker over http")
        ("log-auctions", value<bool>(&logAuctions)->zero_tokens(),
         "log auction requests")
        ("log-bids", value<bool>(&logBids)->zero_tokens(),
         "log bid responses")
        ("max-bid-price", value(&maxBidPrice),
         "maximum bid price accepted by router")
        ("spend-rate", value<string>(&spendRate)->default_value("100000USD/1M"),
         "Amount of budget in USD to be periodically re-authorized (default 100000USD/1M)");

    options_description all_opt = opts;
    all_opt
        .add(serviceArgs.makeProgramOptions())
        .add(router_options);
    all_opt.add_options()
        ("help,h", "print this message");
    
    variables_map vm;
    store(command_line_parser(argc, argv)
          .options(all_opt)
          //.positional(p)
          .run(),
          vm);
    notify(vm);

    if (vm.count("help")) {
        cerr << all_opt << endl;
        exit(1);
    }
}

void
RouterRunner::
init()
{
    auto proxies = serviceArgs.makeServiceProxies();
    auto serviceName = serviceArgs.serviceName("router");

    exchangeConfig = loadJsonFromFile(exchangeConfigurationFile);
    bidderConfig = loadJsonFromFile(bidderConfigurationFile);

    auto connectPostAuctionLoop = !noPostAuctionLoop;
    router = std::make_shared<Router>(proxies, serviceName, lossSeconds,
                                      connectPostAuctionLoop,
                                      logAuctions, logBids,
                                      USD_CPM(maxBidPrice),
                                      slowModeTimeout);
    router->initBidderInterface(bidderConfig);
    router->init();

    const auto amount = Amount::parse(spendRate);
    banker = std::make_shared<SlaveBanker>(router->serviceName() + ".slaveBanker",
                                           CurrencyPool(amount));
    std::shared_ptr<ApplicationLayer> layer;
    if (useHttpBanker) {
        auto bankerUri = proxies->bankerUri;
        ExcCheck(!bankerUri.empty(),
                "the banker-uri must be specified in the bootstrap.json");
        layer = make_application_layer<HttpLayer>(bankerUri);
        std::cerr << "using http interface for the MasterBanker" << std::endl;
    }
    else {
        layer = make_application_layer<ZmqLayer>(proxies);
        std::cerr << "using zmq interface for the MasterBanker" << std::endl;
    }
    banker->setApplicationLayer(layer);

    router->setBanker(banker);
    router->bindTcp();
}

void
RouterRunner::
start()
{
    banker->start();
    router->start();

    // Start all exchanges
    for (auto & exchange: exchangeConfig)
        router->startExchange(exchange);
}

void
RouterRunner::
shutdown()
{
    router->shutdown();
    banker->shutdown();
}

int main(int argc, char ** argv)
{
    RouterRunner runner;

    runner.doOptions(argc, argv);
    runner.init();
    runner.start();

    runner.router->forAllExchanges([](std::shared_ptr<ExchangeConnector> const & item) {
        item->enableUntil(Date::positiveInfinity());
    });

    ProcessStats lastStats;
    auto onStat = [&] (std::string key, double val) {
        runner.router->recordStableLevel(val, key);
    };

    for (;;) {
        ML::sleep(1.0);

        ProcessStats curStats;
        ProcessStats::logToCallback(onStat, lastStats, curStats, "process");
        lastStats = curStats;
    }
}
