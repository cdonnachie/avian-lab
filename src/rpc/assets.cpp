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
#include <wallet/asset_tx.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#endif

#include <assets/ans.h>

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

//! Validate IPFS hash or txid message data
static void CheckIPFSTxidMessage(const std::string& message, int64_t expireTime)
{
    if (message.empty()) return;
    size_t msglen = message.length();

    // ANS checks
    bool fHasANS = (msglen >= CAvianNameSystemID::prefix.size() + 1) &&
                   (message.substr(0, CAvianNameSystemID::prefix.length()) == CAvianNameSystemID::prefix) &&
                   (msglen <= 64);
    if (fHasANS && !IsAvianNameSystemDeployed())
        throw JSONRPCError(RPC_INVALID_PARAMS, "ANS IDs not allowed when they are not deployed.");
    if (fHasANS && !CAvianNameSystemID::IsValidID(message))
        throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid ANS ID");

    // IPFS CID or txid
    if (msglen == 46 || msglen == 64) {
        return; // Valid lengths for IPFS CID (Qm...) or txid
    }
    if (fHasANS) return;

    throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid IPFS hash or txid (must be 46 or 64 characters)");
}

static RPCHelpMan issue()
{
    return RPCHelpMan{
        "issue",
        "Issue an asset, subasset or unique asset.\n"
        "Asset name must not conflict with any existing asset.\n"
        "Unit as the number of decimals precision for the asset (0 for whole units (\"1\"), 8 for max precision (\"1.00000000\"))\n"
        "Reissuable is true/false for whether additional units can be issued by the original issuer.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "a unique name"},
            {"qty", RPCArg::Type::NUM, RPCArg::Default{1}, "the number of units to be issued"},
            {"to_address", RPCArg::Type::STR, RPCArg::Default{""}, "address asset will be sent to, if empty address will be generated"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address the AVN change will be sent to, if empty change address will be generated"},
            {"units", RPCArg::Type::NUM, RPCArg::Default{0}, "the number of decimals precision (0-8)"},
            {"reissuable", RPCArg::Type::BOOL, RPCArg::Default{true}, "whether future reissuance is allowed (false for unique assets)"},
            {"has_ipfs", RPCArg::Type::BOOL, RPCArg::Default{false}, "whether an ipfs hash is going to be added"},
            {"ipfs_hash", RPCArg::Type::STR, RPCArg::Default{""}, "an ipfs hash or txid hash (required if has_ipfs = true)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("issue", "\"ASSET_NAME\" 1000")
            + HelpExampleCli("issue", "\"ASSET_NAME\" 1000 \"myaddress\" \"changeaddress\" 8 false true \"QmTqu3Lk3gmTsQVtjU7rYYM37EAW4xNmbuEAp2Mjr4AV7E\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            wallet::EnsureWalletIsUnlocked(*pwallet);

            std::string assetName = request.params[0].get_str();
            AssetType assetType;
            std::string assetError;
            if (!IsAssetNameValid(assetName, assetType, assetError))
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid asset name: %s\nError: %s", assetName, assetError));

            if (assetType == AssetType::RESTRICTED)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use the issuerestricted RPC to issue a restricted asset");
            if (assetType == AssetType::QUALIFIER || assetType == AssetType::SUB_QUALIFIER)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Use the issuequalifierasset RPC to issue a qualifier asset");
            if (assetType == AssetType::VOTE || assetType == AssetType::REISSUE || assetType == AssetType::OWNER || assetType == AssetType::INVALID)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Unsupported asset type");

            CAmount nAmount = COIN;
            if (!request.params[1].isNull())
                nAmount = AmountFromValue(request.params[1]);

            std::string address;
            if (!request.params[2].isNull())
                address = request.params[2].get_str();
            if (!address.empty()) {
                CTxDestination destination = DecodeDestination(address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + address);
            } else {
                auto op_dest = pwallet->GetNewDestination(OutputType::LEGACY, "");
                if (!op_dest) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(op_dest).original);
                address = EncodeDestination(*op_dest);
            }

            std::string change_address;
            if (!request.params[3].isNull())
                change_address = request.params[3].get_str();
            if (!change_address.empty()) {
                CTxDestination destination = DecodeDestination(change_address);
                if (!IsValidDestination(destination))
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid change address: ") + change_address);
            }

            int units = 0;
            if (!request.params[4].isNull())
                units = request.params[4].getInt<int>();

            bool reissuable = (assetType != AssetType::UNIQUE && assetType != AssetType::MSGCHANNEL);
            if (!request.params[5].isNull())
                reissuable = request.params[5].get_bool();

            bool has_ipfs = false;
            if (!request.params[6].isNull())
                has_ipfs = request.params[6].get_bool();

            std::string ipfs_hash;
            if (!request.params[7].isNull() && has_ipfs) {
                ipfs_hash = request.params[7].get_str();
                int64_t expireTime = 0;
                CheckIPFSTxidMessage(ipfs_hash, expireTime);
            }

            // Validate unique/msgchannel constraints
            if ((assetType == AssetType::UNIQUE || assetType == AssetType::MSGCHANNEL) && (nAmount != COIN || units != 0 || reissuable))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameters for issuing a unique asset.");

            CNewAsset asset(assetName, nAmount, units, reissuable ? 1 : 0, has_ipfs ? 1 : 0, DecodeAssetData(ipfs_hash));

            wallet::CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(change_address);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!wallet::CreateAssetTransaction(*pwallet, ctrl, asset, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!wallet::SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

static RPCHelpMan transfer()
{
    return RPCHelpMan{
        "transfer",
        "Transfers a quantity of an owned asset to a given address.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of asset"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of assets you want to send to the address"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"message", RPCArg::Type::STR, RPCArg::Default{""}, "once messaging is enabled, ipfs hash or txid hash to send along with the transfer"},
            {"expire_time", RPCArg::Type::NUM, RPCArg::Default{0}, "UTC timestamp of when the message expires"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's AVN change will be sent to this address"},
            {"asset_change_address", RPCArg::Type::STR, RPCArg::Default{""}, "the transaction's asset change will be sent to this address"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("transfer", "\"ASSET_NAME\" 20 \"address\"")
            + HelpExampleCli("transfer", "\"ASSET_NAME\" 20 \"address\" \"\" 0 \"change_address\" \"asset_change_address\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            wallet::EnsureWalletIsUnlocked(*pwallet);

            std::string asset_name = request.params[0].get_str();

            if (IsAssetNameAQualifier(asset_name))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Please use the transferqualifier RPC to send qualifier assets from this wallet.");

            CAmount nAmount = AmountFromValue(request.params[1]);

            std::string to_address = request.params[2].get_str();
            CTxDestination to_dest = DecodeDestination(to_address);
            if (!IsValidDestination(to_dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Avian address: ") + to_address);

            std::string message;
            if (!request.params[3].isNull())
                message = request.params[3].get_str();

            int64_t expireTime = 0;
            if (!request.params[4].isNull())
                expireTime = request.params[4].getInt<int64_t>();

            if (!message.empty()) {
                if (!AreMessagesDeployed())
                    throw JSONRPCError(RPC_INVALID_PARAMS, "Unable to send messages until messaging is enabled");
                CheckIPFSTxidMessage(message, expireTime);
            }

            std::string avn_change_address;
            if (!request.params[5].isNull())
                avn_change_address = request.params[5].get_str();

            std::string asset_change_address;
            if (!request.params[6].isNull())
                asset_change_address = request.params[6].get_str();

            CTxDestination avn_change_dest = DecodeDestination(avn_change_address);
            if (!avn_change_address.empty() && !IsValidDestination(avn_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("AVN change address must be a valid address. Invalid address: ") + avn_change_address);

            CTxDestination asset_change_dest = DecodeDestination(asset_change_address);
            if (!asset_change_address.empty() && !IsValidDestination(asset_change_dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Asset change address must be a valid address. Invalid address: ") + asset_change_address);

            std::vector<std::pair<CAssetTransfer, std::string>> vTransfers;
            CAssetTransfer assetTransfer(asset_name, nAmount, DecodeAssetData(message), expireTime);
            vTransfers.emplace_back(std::make_pair(assetTransfer, to_address));

            wallet::CCoinControl ctrl;
            ctrl.destChange = avn_change_dest;
            ctrl.destAssetChange = asset_change_dest;

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!wallet::CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!wallet::SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

static RPCHelpMan reissue()
{
    return RPCHelpMan{
        "reissue",
        "Reissues a quantity of an asset to an owned address if you own the Owner Token.\n"
        "Can change the reissuable flag during reissuance.\n"
        "Can change the ipfs hash during reissuance.\n",
        {
            {"asset_name", RPCArg::Type::STR, RPCArg::Optional::NO, "name of asset that is being reissued"},
            {"qty", RPCArg::Type::NUM, RPCArg::Optional::NO, "number of assets to reissue"},
            {"to_address", RPCArg::Type::STR, RPCArg::Optional::NO, "address to send the asset to"},
            {"change_address", RPCArg::Type::STR, RPCArg::Default{""}, "address that the change of the transaction will be sent to"},
            {"reissuable", RPCArg::Type::BOOL, RPCArg::Default{true}, "whether future reissuance is allowed"},
            {"new_units", RPCArg::Type::NUM, RPCArg::Default{-1}, "the new units that will be associated with the asset"},
            {"new_ipfs", RPCArg::Type::STR, RPCArg::Default{""}, "whether to update the current ipfs hash or txid"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::STR, "txid", "The transaction id"}}},
        RPCExamples{
            HelpExampleCli("reissue", "\"ASSET_NAME\" 20 \"address\"")
            + HelpExampleCli("reissue", "\"ASSET_NAME\" 20 \"address\" \"change_address\" true 8 \"Qmd286K6pohQcTKYqnS1YhWrCiS4gz7Xi34sdwMe9USZ7u\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const std::shared_ptr<wallet::CWallet> pwallet = wallet::GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            wallet::EnsureWalletIsUnlocked(*pwallet);

            std::string asset_name = request.params[0].get_str();
            CAmount nAmount = AmountFromValue(request.params[1]);
            std::string address = request.params[2].get_str();

            std::string changeAddress;
            if (!request.params[3].isNull())
                changeAddress = request.params[3].get_str();

            bool reissuable = true;
            if (!request.params[4].isNull())
                reissuable = request.params[4].get_bool();

            int newUnits = -1;
            if (!request.params[5].isNull())
                newUnits = request.params[5].getInt<int>();

            std::string newipfs;
            if (!request.params[6].isNull()) {
                newipfs = request.params[6].get_str();
                if (!newipfs.empty()) {
                    int64_t expireTime = 0;
                    CheckIPFSTxidMessage(newipfs, expireTime);
                }
            }

            CReissueAsset reissueAsset(asset_name, nAmount, newUnits, reissuable ? 1 : 0, DecodeAssetData(newipfs), "");

            wallet::CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(changeAddress);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!wallet::CreateReissueAssetTransaction(*pwallet, ctrl, reissueAsset, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            // Additional validation against resulting tx
            std::string strError;
            if (!ContextualCheckReissueAsset(passets, reissueAsset, strError, *tx))
                throw JSONRPCError(RPC_INVALID_REQUEST, strError);

            std::string txid;
            if (!wallet::SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
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
        {"assets", &issue},
        {"assets", &transfer},
        {"assets", &reissue},
#endif
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
