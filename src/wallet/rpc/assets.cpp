// Copyright (c) 2017-2019 The Raven Core developers
// Copyright (c) 2020-2024 The Avian developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>
#include <rpc/util.h>

#include <assets/assets.h>
#include <assets/assetdb.h>
#include <assets/ans.h>
#include <core_io.h>
#include <key_io.h>
#include <validation.h>

#include <wallet/asset_tx.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

namespace wallet {

template <typename It>
static void safe_advance(It& it, It end, size_t n) {
    while (n-- > 0 && it != end)
        ++it;
};

RPCHelpMan listmyassets()
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
            const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
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
            CoinFilterParams coin_params;
            coin_params.min_amount = 0;
            CoinsResult available = AvailableCoinsWithAssets(*pwallet, nullptr, std::nullopt, coin_params);

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
            std::map<std::string, std::vector<COutput>> assetOutputs;

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
                std::vector<COutput> filteredOutputs;
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

RPCHelpMan issue()
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
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

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

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(change_address);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateAssetTransaction(*pwallet, ctrl, asset, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan transfer()
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
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

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

            CCoinControl ctrl;
            ctrl.destChange = avn_change_dest;
            ctrl.destAssetChange = asset_change_dest;

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateTransferAssetTransaction(*pwallet, ctrl, vTransfers, "", error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

RPCHelpMan reissue()
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
            const std::shared_ptr<CWallet> pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);

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

            CCoinControl ctrl;
            ctrl.destChange = DecodeDestination(changeAddress);

            CTransactionRef tx;
            CAmount nFeeRequired;
            std::pair<int, std::string> error;

            if (!CreateReissueAssetTransaction(*pwallet, ctrl, reissueAsset, address, error, tx, nFeeRequired))
                throw JSONRPCError(error.first, error.second);

            // Additional validation against resulting tx
            std::string strError;
            if (!ContextualCheckReissueAsset(passets, reissueAsset, strError, *tx))
                throw JSONRPCError(RPC_INVALID_REQUEST, strError);

            std::string txid;
            if (!SendAssetTransaction(*pwallet, tx, error, txid))
                throw JSONRPCError(error.first, error.second);

            UniValue result(UniValue::VARR);
            result.push_back(txid);
            return result;
        },
    };
}

} // namespace wallet
