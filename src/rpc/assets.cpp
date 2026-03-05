// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>

#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/restricteddb.h>
#include <core_io.h>
#include <validation.h>

#ifdef ENABLE_WALLET
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#endif

#include <univalue.h>

static UniValue UnitValueFromAmount(const CAmount& amount, int8_t units)
{
    // Format amount according to asset units (0-8 decimals)
    bool sign = amount < 0;
    int64_t n_abs = (sign ? -amount : amount);
    int64_t quotient = n_abs;
    int64_t remainder = 0;

    if (units > 0) {
        int64_t divisor = 1;
        for (int i = 0; i < units; i++) divisor *= 10;
        quotient = n_abs / divisor;
        remainder = n_abs % divisor;
    }

    if (units == 0) {
        return UniValue(UniValue::VNUM, strprintf("%s%d", sign ? "-" : "", quotient));
    }

    return UniValue(UniValue::VNUM, strprintf("%s%d.%0*d", sign ? "-" : "", quotient, units, remainder));
}

static UniValue AssetUnitValueFromAmount(const CAmount& amount, const std::string& assetName)
{
    uint8_t units = MAX_UNIT;
    if (IsAssetNameAnOwner(assetName)) {
        units = OWNER_UNITS;
    } else if (passets) {
        CNewAsset assetData;
        if (passets->GetAssetMetaDataIfExists(assetName, assetData)) {
            units = assetData.units;
        }
    }
    return UnitValueFromAmount(amount, units);
}

static RPCHelpMan listassets()
{
    return RPCHelpMan{
        "listassets",
        "Returns a list of all assets.\n"
        "This could be a slow/expensive operation as it reads from the database.\n",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Default{"*"}, "Filters results -- must be an asset name or a partial asset name followed by '*' ('*' matches all trailing characters)"},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "When false result is just a list of asset names -- when true results are asset name mapped to metadata"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "Truncates results to include only the first count assets found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "Results skip over the first start assets found (if negative it skips back from the end)"},
        },
        {
            RPCResult{"verbose=false",
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::STR, "", "asset name"},
                }
            },
            RPCResult{"verbose=true",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::OBJ, "asset_name", "",
                    {
                        {RPCResult::Type::STR, "name", "the asset name"},
                        {RPCResult::Type::NUM, "amount", "the total amount issued"},
                        {RPCResult::Type::NUM, "units", "the number of decimal places"},
                        {RPCResult::Type::NUM, "reissuable", "1 if reissuable"},
                        {RPCResult::Type::NUM, "has_ipfs", "1 if has IPFS data"},
                        {RPCResult::Type::NUM, "block_height", "the block height the asset was created"},
                        {RPCResult::Type::STR_HEX, "blockhash", "the block hash the asset was created"},
                    }},
                }
            },
        },
        RPCExamples{
            HelpExampleCli("listassets", "")
          + HelpExampleCli("listassets", "\"ASSET*\" true 10 20")
          + HelpExampleRpc("listassets", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!passetsdb)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset db unavailable.");

            std::string filter = "*";
            if (!request.params[0].isNull())
                filter = request.params[0].get_str();
            if (filter.empty())
                filter = "*";

            bool verbose = false;
            if (!request.params[1].isNull())
                verbose = request.params[1].get_bool();

            size_t count = INT_MAX;
            if (!request.params[2].isNull()) {
                if (request.params[2].getInt<int>() < 1)
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be greater than 1.");
                count = request.params[2].getInt<int>();
            }

            long start = 0;
            if (!request.params[3].isNull()) {
                start = request.params[3].getInt<int>();
            }

            std::vector<CDatabasedAssetData> assets;
            if (!passetsdb->AssetDir(assets, filter, count, start))
                throw JSONRPCError(RPC_INTERNAL_ERROR, "couldn't retrieve asset directory.");

            UniValue result;
            result = verbose ? UniValue(UniValue::VOBJ) : UniValue(UniValue::VARR);

            for (const auto& data : assets) {
                const CNewAsset& asset = data.asset;
                if (verbose) {
                    UniValue detail(UniValue::VOBJ);
                    detail.pushKV("name", asset.strName);
                    detail.pushKV("amount", UnitValueFromAmount(asset.nAmount, asset.units));
                    detail.pushKV("units", asset.units);
                    detail.pushKV("reissuable", asset.nReissuable);
                    detail.pushKV("has_ipfs", asset.nHasIPFS);
                    detail.pushKV("block_height", data.nHeight);
                    detail.pushKV("blockhash", data.blockHash.GetHex());
                    if (asset.nHasIPFS) {
                        if (asset.strIPFSHash.size() == 32) {
                            detail.pushKV("txid_hash", EncodeAssetData(asset.strIPFSHash));
                        } else {
                            detail.pushKV("ipfs_hash", EncodeAssetData(asset.strIPFSHash));
                        }
                    }
                    result.pushKV(asset.strName, detail);
                } else {
                    result.push_back(asset.strName);
                }
            }

            return result;
        },
    };
}

static RPCHelpMan getassetdata()
{
    return RPCHelpMan{
        "getassetdata",
        "Returns asset metadata if that asset exists.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the name of the asset"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "name", "the asset name"},
                {RPCResult::Type::NUM, "amount", "the total amount issued"},
                {RPCResult::Type::NUM, "units", "the number of decimal places"},
                {RPCResult::Type::NUM, "reissuable", "1 if reissuable"},
                {RPCResult::Type::NUM, "has_ipfs", "1 if has IPFS data"},
                {RPCResult::Type::STR, "ipfs_hash", /*optional=*/true, "the IPFS hash (only if has_ipfs = 1)"},
                {RPCResult::Type::STR, "verifier_string", /*optional=*/true, "the verifier string for restricted assets"},
            }
        },
        RPCExamples{
            HelpExampleCli("getassetdata", "\"ASSET_NAME\"")
          + HelpExampleRpc("getassetdata", "\"ASSET_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string asset_name = request.params[0].get_str();

            LOCK(cs_main);
            UniValue result(UniValue::VOBJ);

            if (passets) {
                CNewAsset asset;
                if (!passets->GetAssetMetaDataIfExists(asset_name, asset))
                    return UniValue::VNULL;

                result.pushKV("name", asset.strName);
                result.pushKV("amount", UnitValueFromAmount(asset.nAmount, asset.units));
                result.pushKV("units", asset.units);
                result.pushKV("reissuable", asset.nReissuable);
                result.pushKV("has_ipfs", asset.nHasIPFS);

                if (asset.nHasIPFS) {
                    if (asset.strIPFSHash.size() == 32) {
                        result.pushKV("txid_hash", EncodeAssetData(asset.strIPFSHash));
                    } else {
                        result.pushKV("ipfs_hash", EncodeAssetData(asset.strIPFSHash));
                    }
                }

                CNullAssetTxVerifierString verifier;
                if (passets->GetAssetVerifierStringIfExists(asset.strName, verifier)) {
                    result.pushKV("verifier_string", verifier.verifier_string);
                }

                return result;
            }

            return UniValue::VNULL;
        },
    };
}

static RPCHelpMan getcacheinfo()
{
    return RPCHelpMan{
        "getcacheinfo",
        "Returns information about the asset cache.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "asset_total_cache_size", "total size of asset caches"},
                {RPCResult::Type::NUM, "asset_address_map_size", "size of address-to-asset amount map"},
            }
        },
        RPCExamples{
            HelpExampleCli("getcacheinfo", "")
          + HelpExampleRpc("getcacheinfo", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            LOCK(cs_main);
            UniValue result(UniValue::VOBJ);

            if (passets) {
                result.pushKV("asset_total_cache_size", (int)passets->DynamicMemoryUsage());
                result.pushKV("asset_address_map_size", (int)passets->mapAssetsAddressAmount.size());
            } else {
                result.pushKV("asset_total_cache_size", 0);
                result.pushKV("asset_address_map_size", 0);
            }

            return result;
        },
    };
}

static RPCHelpMan listassetbalancesbyaddress()
{
    return RPCHelpMan{
        "listassetbalancesbyaddress",
        "Returns a list of all asset balances for an address.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "a valid Avian address"},
            {"onlytotal", RPCArg::Type::BOOL, RPCArg::Default{false}, "when false result is just a list of assets balances -- when true only the number of assets is returned"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "truncates results to include only the first count assets found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "results skip over the first start assets found"},
        },
        RPCResult{
            RPCResult::Type::OBJ_DYN, "", "",
            {
                {RPCResult::Type::NUM, "asset_name", "asset balance"},
            }
        },
        RPCExamples{
            HelpExampleCli("listassetbalancesbyaddress", "\"RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
          + HelpExampleRpc("listassetbalancesbyaddress", "\"RXissueAssetXXXXXXXXXXXXXXXXZFGHWo\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "This rpc call is not functional unless -assetindex is enabled.");

            std::string address = request.params[0].get_str();

            bool onlytotal = false;
            if (!request.params[1].isNull())
                onlytotal = request.params[1].get_bool();

            size_t count = INT_MAX;
            if (!request.params[2].isNull()) {
                if (request.params[2].getInt<int>() < 1)
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be greater than 1.");
                count = request.params[2].getInt<int>();
            }

            long start = 0;
            if (!request.params[3].isNull()) {
                start = request.params[3].getInt<int>();
            }

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            UniValue result(UniValue::VOBJ);

            if (onlytotal) {
                // Just count the number of assets at this address
                int count = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.second == address && amount > 0) {
                        count++;
                    }
                }
                result.pushKV("total", count);
            } else {
                size_t found = 0;
                long skipped = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.second == address && amount > 0) {
                        if (skipped < start) {
                            skipped++;
                            continue;
                        }
                        result.pushKV(pair.first, AssetUnitValueFromAmount(amount, pair.first));
                        found++;
                        if (found >= count)
                            break;
                    }
                }
            }

            return result;
        },
    };
}

static RPCHelpMan listaddressesbyasset()
{
    return RPCHelpMan{
        "listaddressesbyasset",
        "Returns a list of all addresses that hold the given asset.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of the asset"},
            {"onlytotal", RPCArg::Type::BOOL, RPCArg::Default{false}, "when false result is just a list of addresses with balances -- when true only the number of addresses is returned"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "truncates results to include only the first count addresses found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "results skip over the first start addresses found"},
        },
        RPCResult{
            RPCResult::Type::OBJ_DYN, "", "",
            {
                {RPCResult::Type::NUM, "address", "balance"},
            }
        },
        RPCExamples{
            HelpExampleCli("listaddressesbyasset", "\"ASSET_NAME\"")
          + HelpExampleRpc("listaddressesbyasset", "\"ASSET_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            if (!fAssetIndex)
                throw JSONRPCError(RPC_MISC_ERROR, "This rpc call is not functional unless -assetindex is enabled.");

            std::string assetName = request.params[0].get_str();

            bool onlytotal = false;
            if (!request.params[1].isNull())
                onlytotal = request.params[1].get_bool();

            size_t count = INT_MAX;
            if (!request.params[2].isNull()) {
                if (request.params[2].getInt<int>() < 1)
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be greater than 1.");
                count = request.params[2].getInt<int>();
            }

            long start = 0;
            if (!request.params[3].isNull()) {
                start = request.params[3].getInt<int>();
            }

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            UniValue result(UniValue::VOBJ);

            // Check if asset exists
            CNewAsset asset;
            if (!passets->GetAssetMetaDataIfExists(assetName, asset))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Asset not found: " + assetName);

            if (onlytotal) {
                int count = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.first == assetName && amount > 0) {
                        count++;
                    }
                }
                result.pushKV("total", count);
            } else {
                size_t found = 0;
                long skipped = 0;
                for (const auto& [pair, amount] : passets->mapAssetsAddressAmount) {
                    if (pair.first == assetName && amount > 0) {
                        if (skipped < start) {
                            skipped++;
                            continue;
                        }
                        result.pushKV(pair.second, AssetUnitValueFromAmount(amount, assetName));
                        found++;
                        if (found >= count)
                            break;
                    }
                }
            }

            return result;
        },
    };
}

static RPCHelpMan checkaddressrestriction()
{
    return RPCHelpMan{
        "checkaddressrestriction",
        "Checks to see if an address has been frozen by a restricted asset.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "the Avian address to search"},
            {"restricted_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name to search"},
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "true if the address is frozen"
        },
        RPCExamples{
            HelpExampleCli("checkaddressrestriction", "\"address\" \"$RESTRICTED_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string address = request.params[0].get_str();
            std::string restricted_name = request.params[1].get_str();

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            return passets->CheckForAddressRestriction(restricted_name, address, true);
        },
    };
}

static RPCHelpMan checkglobalrestriction()
{
    return RPCHelpMan{
        "checkglobalrestriction",
        "Checks to see if a restricted asset is globally frozen.\n",
        {
            {"restricted_name", RPCArg::Type::STR, RPCArg::Optional::NO, "the restricted asset name to search"},
        },
        RPCResult{
            RPCResult::Type::BOOL, "", "true if the asset is globally frozen"
        },
        RPCExamples{
            HelpExampleCli("checkglobalrestriction", "\"$RESTRICTED_NAME\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::string restricted_name = request.params[0].get_str();

            LOCK(cs_main);

            if (!passets)
                throw JSONRPCError(RPC_INTERNAL_ERROR, "asset cache unavailable.");

            return passets->CheckForGlobalRestriction(restricted_name, true);
        },
    };
}

#ifdef ENABLE_WALLET
template <typename It>
static void safe_advance(It& it, It end, size_t n) {
    while (n-- > 0 && it != end)
        ++it;
};

static RPCHelpMan listmyassets()
{
    return RPCHelpMan{
        "listmyassets",
        "Returns a list of all assets that are owned by this wallet.\n",
        {
            {"asset", RPCArg::Type::STR, RPCArg::Default{"*"}, "Filters results -- must be an asset name or a partial asset name followed by '*' ('*' matches all trailing characters)"},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "When false result is just a list of asset balances -- when true results include outpoints"},
            {"count", RPCArg::Type::NUM, RPCArg::DefaultHint{"all"}, "Truncates results to include only the first count assets found"},
            {"start", RPCArg::Type::NUM, RPCArg::Default{0}, "Results skip over the first start assets found (if negative it skips back from the end)"},
            {"confs", RPCArg::Type::NUM, RPCArg::Default{0}, "Results are skipped if they don't have this number of confirmations"},
        },
        {
            RPCResult{"verbose=false",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::NUM, "asset_name", "asset balance"},
                }
            },
            RPCResult{"verbose=true",
                RPCResult::Type::OBJ_DYN, "", "",
                {
                    {RPCResult::Type::OBJ, "asset_name", "",
                    {
                        {RPCResult::Type::NUM, "balance", "the asset balance"},
                        {RPCResult::Type::ARR, "outpoints", "",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "the txid"},
                                {RPCResult::Type::NUM, "vout", "the vout"},
                                {RPCResult::Type::NUM, "amount", "the amount"},
                            }},
                        }},
                    }},
                }
            },
        },
        RPCExamples{
            HelpExampleCli("listmyassets", "")
          + HelpExampleCli("listmyassets", "\"ASSET*\" true 10 20")
          + HelpExampleRpc("listmyassets", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<const wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            std::string filter = "*";
            if (!request.params[0].isNull())
                filter = request.params[0].get_str();
            if (filter.empty())
                filter = "*";

            bool verbose = false;
            if (!request.params[1].isNull())
                verbose = request.params[1].get_bool();

            size_t count = INT_MAX;
            if (!request.params[2].isNull()) {
                if (request.params[2].getInt<int>() < 1)
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "count must be greater than 1.");
                count = request.params[2].getInt<int>();
            }

            long start = 0;
            if (!request.params[3].isNull()) {
                start = request.params[3].getInt<int>();
            }

            int confs = 0;
            if (!request.params[4].isNull()) {
                confs = request.params[4].getInt<int>();
            }

            // Get available asset coins from wallet
            LOCK(pwallet->cs_wallet);
            wallet::CoinFilterParams coin_params;
            coin_params.min_amount = 0;
            wallet::CoinsResult available = wallet::AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, coin_params);

            // Build balances per asset
            std::string prefix;
            bool matchAll = (filter == "*");
            if (!matchAll && filter.back() == '*') {
                prefix = filter.substr(0, filter.size() - 1);
                matchAll = false;
            } else if (!matchAll) {
                prefix = filter;
            }

            std::map<std::string, CAmount> balances;
            std::map<std::string, std::vector<wallet::COutput>> assetOutputs;

            for (const auto& [assetName, outputs] : available.mapAssetCoins) {
                // Apply filter
                if (!matchAll) {
                    if (filter.back() == '*' || filter != assetName) {
                        // Prefix match
                        if (!prefix.empty() && assetName.find(prefix) != 0)
                            continue;
                        if (prefix.empty() && filter != assetName)
                            continue;
                    }
                }

                CAmount balance = 0;
                std::vector<wallet::COutput> filteredOutputs;
                for (const auto& output : outputs) {
                    // Check confirmations
                    if (confs > 0 && output.depth < confs)
                        continue;

                    CAssetOutputEntry data;
                    if (GetAssetData(output.txout.scriptPubKey, data)) {
                        balance += data.nAmount;
                        filteredOutputs.push_back(output);
                    }
                }
                if (balance > 0 || !filteredOutputs.empty()) {
                    balances[assetName] = balance;
                    if (verbose)
                        assetOutputs[assetName] = std::move(filteredOutputs);
                }
            }

            // Pagination
            auto bal = balances.begin();
            if (start >= 0)
                safe_advance(bal, balances.end(), (size_t)start);
            else
                safe_advance(bal, balances.end(), balances.size() + start);
            auto end = bal;
            safe_advance(end, balances.end(), count);

            // Generate output
            UniValue result(UniValue::VOBJ);
            if (verbose) {
                for (; bal != end && bal != balances.end(); bal++) {
                    UniValue asset(UniValue::VOBJ);
                    asset.pushKV("balance", AssetUnitValueFromAmount(bal->second, bal->first));

                    UniValue outpoints(UniValue::VARR);
                    if (assetOutputs.count(bal->first)) {
                        for (const auto& out : assetOutputs.at(bal->first)) {
                            UniValue tempOut(UniValue::VOBJ);
                            tempOut.pushKV("txid", out.outpoint.hash.GetHex());
                            tempOut.pushKV("vout", (int)out.outpoint.n);

                            CAssetOutputEntry data;
                            if (GetAssetData(out.txout.scriptPubKey, data)) {
                                tempOut.pushKV("amount", AssetUnitValueFromAmount(data.nAmount, bal->first));
                            }
                            outpoints.push_back(std::move(tempOut));
                        }
                    }
                    asset.pushKV("outpoints", std::move(outpoints));
                    result.pushKV(bal->first, std::move(asset));
                }
            } else {
                for (; bal != end && bal != balances.end(); bal++) {
                    result.pushKV(bal->first, AssetUnitValueFromAmount(bal->second, bal->first));
                }
            }

            return result;
        },
    };
}
#endif // ENABLE_WALLET

void RegisterAssetRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"assets", &listassets},
        {"assets", &getassetdata},
        {"assets", &getcacheinfo},
        {"assets", &listassetbalancesbyaddress},
        {"assets", &listaddressesbyasset},
        {"restricted assets", &checkaddressrestriction},
        {"restricted assets", &checkglobalrestriction},
#ifdef ENABLE_WALLET
        {"assets", &listmyassets},
#endif
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
