/* slave_banker.cc
   Jeremy Barnes, 8 November 2012
   Copyright (c) 2012 Datacratic Inc.  All rights reserved.

   Slave banker implementation.
*/

#include "slave_banker.h"
#include "jml/utils/vector_utils.h"

using namespace std;

static constexpr int MaximumFailSyncSeconds = 3;

namespace RTBKIT {


/*****************************************************************************/
/* SLAVE BUDGET CONTROLLER                                                   */
/*****************************************************************************/

SlaveBudgetController::
SlaveBudgetController()
{
}


void
SlaveBudgetController::
addAccount(const AccountKey & account,
           const OnBudgetResult & onResult)
{
    applicationLayer->addAccount(account, onResult);
}


void
SlaveBudgetController::
topupTransfer(const AccountKey & account,
              CurrencyPool amount,
              const OnBudgetResult & onResult)
{
    applicationLayer->topupTransfer(account, AT_BUDGET, amount, onResult);
}

void
SlaveBudgetController::
setBudget(const std::string & topLevelAccount,
          CurrencyPool amount,
          const OnBudgetResult & onResult)
{
    applicationLayer->setBudget(topLevelAccount, amount, onResult);
}

void
SlaveBudgetController::
addBudget(const std::string & topLevelAccount,
          CurrencyPool amount,
          const OnBudgetResult & onResult)
{
    throw ML::Exception("addBudget no good any more");
}

void
SlaveBudgetController::
getAccountList(const AccountKey & account,
               int depth,
               std::function<void (std::exception_ptr,
                                   std::vector<AccountKey> &&)>)
{
    throw ML::Exception("getAccountList not needed anymore");
}

void
SlaveBudgetController::
getAccountSummary(const AccountKey & account,
                  int depth,
                  std::function<void (std::exception_ptr,
                                      AccountSummary &&)> onResult)
{
    applicationLayer->getAccountSummary(account, depth, onResult);
}

void
SlaveBudgetController::
getAccount(const AccountKey & accountKey,
           std::function<void (std::exception_ptr,
                               Account &&)> onResult)
{
    applicationLayer->getAccount(accountKey, onResult);
}


/*****************************************************************************/
/* SLAVE BANKER                                                              */
/*****************************************************************************/

SlaveBanker::
SlaveBanker(const std::string & accountSuffix, CurrencyPool spendRate)
    : createdAccounts(128), reauthorizing(false), numReauthorized(0)
{
    init(accountSuffix, spendRate);
}

void
SlaveBanker::
init(const std::string & accountSuffix, CurrencyPool spendRate)
{
    if (accountSuffix.empty()) {
        throw ML::Exception("'accountSuffix' cannot be empty");
    }

    if (spendRate.isZero()) {
        throw ML::Exception("'spendRate' can not be zero");
    }

    // When our account manager creates an account, it will call this
    // function.  We can't do anything from it (because the lock could
    // be held), but we *can* push a message asynchronously to be
    // handled later...
    accounts.onNewAccount = [=] (const AccountKey & accountKey)
        {
            //cerr << "((((1)))) new account " << accountKey << endl;
            createdAccounts.push(accountKey);
        };

    // ... here.  Now we know that no lock is held and so we can
    // perform the work we need to synchronize the account with
    // the server.
    createdAccounts.onEvent = [=] (const AccountKey & accountKey)
        {
            //cerr << "((((2)))) new account " << accountKey << endl;

            auto onDone = [=] (std::exception_ptr exc,
                               ShadowAccount && account)
            {
#if 0
                cerr << "((((3)))) new account " << accountKey << endl;

                cerr << "got back shadow account " << account
                << " for " << accountKey << endl;

                cerr << "current status is " << accounts.getAccount(accountKey)
                << endl;
#endif
            };

            addSpendAccount(accountKey, USD(0), onDone);
        };

    addSource("SlaveBanker::createdAccounts", createdAccounts);

    this->accountSuffix = accountSuffix;
    this->spendRate = spendRate;

    lastSync = lastReauthorize = Date::now();
    
    addPeriodic("SlaveBanker::reportSpend", 1.0,
                std::bind(&SlaveBanker::reportSpend,
                          this,
                          std::placeholders::_1),
                true /* single threaded */);
    addPeriodic("SlaveBanker::reauthorizeBudget", 1.0,
                std::bind(&SlaveBanker::reauthorizeBudget,
                          this,
                          std::placeholders::_1),
                true /* single threaded */);
}

ShadowAccount
SlaveBanker::
syncAccountSync(const AccountKey & account)
{
    BankerSyncResult<ShadowAccount> result;
    syncAccount(account, result);
    return result.get();
}

void
SlaveBanker::
onSyncResult(const AccountKey & accountKey,
               std::function<void (std::exception_ptr,
                                   ShadowAccount &&)> onDone,
               std::exception_ptr exc,
               Account&& masterAccount)
{
    ShadowAccount result;

    try {
        if (exc) {
            onDone(exc, std::move(result));
            return;
        }

        //cerr << "got result from master for " << accountKey
        //     << " which is "
        //     << masterAccount << endl;
        
        result = accounts.syncFromMaster(accountKey, masterAccount);
    } catch (...) {
        onDone(std::current_exception(), std::move(result));
    }

    try {
        onDone(nullptr, std::move(result));
    } catch (...) {
        cerr << "warning: onDone handler threw" << endl;
    }
}

void
SlaveBanker::
onInitializeResult(const AccountKey & accountKey,
                   std::function<void (std::exception_ptr,
                                       ShadowAccount &&)> onDone,
                   std::exception_ptr exc,
                   Account&& masterAccount)
{
    ShadowAccount result;

    try {
        if (exc) {
            onDone(exc, std::move(result));
            return;
        }

        result = accounts.initializeAndMergeState(accountKey, masterAccount);
    } catch (...) {
        onDone(std::current_exception(), std::move(result));
    }
    
    try {
        onDone(nullptr, std::move(result));
    } catch (...) {
        cerr << "warning: onDone handler threw" << endl;
    }
}


void
SlaveBanker::
syncAccount(const AccountKey & accountKey,
            std::function<void (std::exception_ptr,
                                ShadowAccount &&)> onDone)
{
    auto onDone2
        = std::bind(&SlaveBanker::onSyncResult,
                    this,
                    accountKey,
                    onDone,
                    std::placeholders::_1,
                    std::placeholders::_2);

    //cerr << "syncing account " << accountKey << ": "
    //     << accounts.getAccount(accountKey) << endl;
    applicationLayer->syncAccount(
                          accounts.getAccount(accountKey),
                          getShadowAccountStr(accountKey),
                          onDone2);
}

void
SlaveBanker::
syncAllSync()
{
    BankerSyncResult<void> result;
    syncAll(result);
    result.get();
}

void
SlaveBanker::
syncAll(std::function<void (std::exception_ptr)> onDone)
{
    auto allKeys = accounts.getAccountKeys();

    vector<AccountKey> filteredKeys;
    for (auto k: allKeys)
    	if (accounts.isInitialized(k))
    		filteredKeys.push_back(k);

    allKeys.swap(filteredKeys);

    if (allKeys.empty()) {
        // We need some kind of synchronization here because the lastSync
        // member variable will also be read in the context of an other
        // MessageLoop (the MonitorProviderClient). Thus, if we want to avoid
        // data-race here, we grab a lock.
        std::lock_guard<Lock> guard(syncLock);
        lastSync = Date::now();
        if (onDone)
            onDone(nullptr);
        return;
    }

    struct Aggregator {

        Aggregator(SlaveBanker *self, int numTotal,
                   std::function<void (std::exception_ptr)> onDone)
            : itl(new Itl())
        {
            itl->self = self;
            itl->numTotal = numTotal;
            itl->numFinished = 0;
            itl->exc = nullptr;
            itl->onDone = onDone;
        }

        struct Itl {
            SlaveBanker *self;
            int numTotal;
            int numFinished;
            std::exception_ptr exc;
            std::function<void (std::exception_ptr)> onDone;
        };

        std::shared_ptr<Itl> itl;
        
        void operator () (std::exception_ptr exc, ShadowAccount && account)
        {
            if (exc)
                itl->exc = exc;
            int nowDone = __sync_add_and_fetch(&itl->numFinished, 1);
            if (nowDone == itl->numTotal) {
                if (!itl->exc) {
                    std::lock_guard<Lock> guard(itl->self->syncLock);
                    itl->self->lastSync = Date::now();
                }

                if (itl->onDone)
                    itl->onDone(itl->exc);
                else {
                    if (itl->exc)
                        cerr << "warning: async callback aggregator ate "
                             << "exception" << endl;
                }
            }
        }               
    };
    
    Aggregator aggregator(const_cast<SlaveBanker *>(this), allKeys.size(), onDone);

    //cerr << "syncing " << allKeys.size() << " keys" << endl;

    for (auto & key: allKeys) {
        // We take its parent since syncAccount assumes nothing was added
        if (accounts.isInitialized(key))
            syncAccount(key, aggregator);
    }
}

void
SlaveBanker::
addSpendAccount(const AccountKey & accountKey,
                CurrencyPool accountFloat,
                std::function<void (std::exception_ptr, ShadowAccount&&)> onDone)
{
    bool first = accounts.createAccountAtomic(accountKey);
    if(!first) {
        // already done
        if (onDone) {
            auto account = accounts.getAccount(accountKey);
            onDone(nullptr, std::move(account));
        }
    }
    else {
        // TODO: record float
        //accountFloats[accountKey] = accountFloat;

        // Now kick off the initial synchronization step
        auto onDone2 = std::bind(&SlaveBanker::onInitializeResult,
                                 this,
                                 accountKey,
                                 onDone,
                                 std::placeholders::_1,
                                 std::placeholders::_2);

        cerr << "********* calling addSpendAccount for " << accountKey
             << " for SlaveBanker " << accountSuffix << endl;

        applicationLayer->addSpendAccount(getShadowAccountStr(accountKey), onDone2);

    }
}

void
SlaveBanker::
reportSpend(uint64_t numTimeoutsExpired)
{
    if (numTimeoutsExpired > 1) {
        cerr << "warning: slave banker missed " << numTimeoutsExpired
             << " timeouts" << endl;
    }

    if (reportSpendSent != Date())
        cerr << "warning: report spend still in progress" << endl;

    //cerr << "started report spend" << endl;

    auto onDone = [=] (std::exception_ptr exc)
        {
            //cerr << "finished report spend" << endl;
            reportSpendSent = Date();
            if (exc)
                cerr << "reportSpend got exception" << endl;
        };
    
    syncAll(onDone);
}

void
SlaveBanker::
reauthorizeBudget(uint64_t numTimeoutsExpired)
{
    if (numTimeoutsExpired > 1) {
        cerr << "warning: slave banker missed " << numTimeoutsExpired
             << " timeouts" << endl;
    }

    //std::unique_lock<Lock> guard(lock);
    if (reauthorizing) {
        cerr << "warning: reauthorize budget still in progress" << endl;
        return;
    }

    accountsLeft = 0;

    // For each of our accounts, we report back what has been spent
    // and re-up to our desired float
    auto onAccount = [&] (const AccountKey & key,
                          const ShadowAccount & account)
        {
            Json::Value payload = spendRate.toJson();

            auto onDone = std::bind(&SlaveBanker::onReauthorizeBudgetMessage, this,
                                    key,
                                    std::placeholders::_1, std::placeholders::_2,
                                    std::placeholders::_3);

            accountsLeft++;

            // Finally, send it out
            applicationLayer->request(
                            "POST", "/v1/accounts/" + getShadowAccountStr(key) + "/balance",
                            { { "accountType", "spend" } },
                            payload.toString(),
                            onDone);
        };
    accounts.forEachInitializedAccount(onAccount);
    
    if (accountsLeft > 0) {
        reauthorizing = true;
        reauthorizeDate = Date::now();
    }
}

void
SlaveBanker::
onReauthorizeBudgetMessage(const AccountKey & accountKey,
                           std::exception_ptr exc,
                           int responseCode,
                           const std::string & payload)
{
    if (exc) {
        cerr << "reauthorize budget got exception" << payload << endl;
        cerr << "accountKey = " << accountKey << endl;
        return;
    }
    else if (responseCode == 200) {
        Account masterAccount = Account::fromJson(Json::parse(payload));
        accounts.syncFromMaster(accountKey, masterAccount);
    }
    reauthorizeBudgetSent = Date();
    accountsLeft--;
    if (accountsLeft == 0) {
        lastReauthorizeDelay = Date::now() - reauthorizeDate;
        numReauthorized++;
        reauthorizing = false;

        std::lock_guard<Lock> guard(syncLock);
        lastReauthorize = Date::now();
    }
}

void
SlaveBanker::
waitReauthorized()
    const
{
    while (reauthorizing) {
        ML::sleep(0.2);
    }
}

MonitorIndicator
SlaveBanker::
getProviderIndicators() const
{
    Date now = Date::now();

    // See syncAll for the reason of this lock
    std::lock_guard<Lock> guard(syncLock);
    bool syncOk = now < lastSync.plusSeconds(MaximumFailSyncSeconds) &&
                  now < lastReauthorize.plusSeconds(MaximumFailSyncSeconds);

    MonitorIndicator ind;
    ind.serviceName = accountSuffix;
    ind.status = syncOk;
    ind.message = string() + "Sync with MasterBanker: " + (syncOk ? "OK" : "ERROR");

    return ind;
}

const CurrencyPool SlaveBanker::DefaultSpendRate = CurrencyPool(USD(0.10));

} // namespace RTBKIT
