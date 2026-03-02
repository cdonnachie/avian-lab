// Copyright (c) 2017-2020 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <assets/rewards.h>
#include <assets/assetsnapshotdb.h>
#include <assets/assets.h>
#include <hash.h>
#include <key_io.h>
#include <logging.h>
#include <chainparams.h>
#include <sstream>

std::map<uint256, CRewardSnapshot> mapRewardSnapshots;

uint256 CRewardSnapshot::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH);
}

bool AddDistributeRewardSnapshot(CRewardSnapshot& p_rewardSnapshot)
{
    auto hash = p_rewardSnapshot.GetHash();
    CRewardSnapshot temp;
    if (pDistributeSnapshotDb->RetrieveDistributeSnapshotRequest(hash, temp)) {
        return false;
    }

    if (pDistributeSnapshotDb->AddDistributeSnapshot(hash, p_rewardSnapshot)) {
        mapRewardSnapshots[hash] = p_rewardSnapshot;
    }

    return true;
}

bool GenerateDistributionList(const CRewardSnapshot& p_rewardSnapshot, std::vector<OwnerAndAmount>& vecDistributionList)
{
    vecDistributionList.clear();

    if (passets == nullptr) {
        LogPrint(BCLog::REWARDS, "%s: Invalid assets cache!\n", __func__);
        return false;
    }
    if (pSnapshotRequestDb == nullptr) {
        LogPrint(BCLog::REWARDS, "%s: Invalid Snapshot Request cache!\n", __func__);
        return false;
    }
    if (pAssetSnapshotDb == nullptr) {
        LogPrint(BCLog::REWARDS, "%s: Invalid asset snapshot cache!\n", __func__);
        return false;
    }

    //  Get details on the specified source asset
    CNewAsset distributionAsset;
    [[maybe_unused]] bool srcIsIndivisible = false;
    CAmount srcUnitDivisor = COIN;  //  Default to divisor for AVN
    const int8_t COIN_DIGITS_PAST_DECIMAL = 8;

    //  This value is in indivisible units of the source asset
    CAmount modifiedPaymentInAssetUnits = p_rewardSnapshot.nDistributionAmount;

    if (p_rewardSnapshot.strDistributionAsset != "AVN") {
        if (!passets->GetAssetMetaDataIfExists(p_rewardSnapshot.strDistributionAsset, distributionAsset)) {
            LogPrint(BCLog::REWARDS, "%s: Failed to retrieve asset details for '%s'\n", __func__, p_rewardSnapshot.strDistributionAsset.c_str());
            return false;
        }

        //  If the token is indivisible, signal this to later code with a zero divisor
        if (distributionAsset.units == 0) {
            srcIsIndivisible = true;
        }

        srcUnitDivisor = static_cast<CAmount>(pow(10, distributionAsset.units));

        CAmount srcDivisor = pow(10, COIN_DIGITS_PAST_DECIMAL - distributionAsset.units);
        modifiedPaymentInAssetUnits /= srcDivisor;

        LogPrint(BCLog::REWARDS, "%s: Distribution asset '%s' has units %d and divisor %d\n", __func__,
                 p_rewardSnapshot.strDistributionAsset.c_str(), distributionAsset.units, srcUnitDivisor);
    }
    else {
        LogPrint(BCLog::REWARDS, "%s: Distribution is AVN with divisor %d\n", __func__, srcUnitDivisor);
    }

    LogPrint(BCLog::REWARDS, "%s: Scaled payment amount in %s is %d\n", __func__,
             p_rewardSnapshot.strDistributionAsset.c_str(), modifiedPaymentInAssetUnits);

    //  Get details on the ownership asset
    CNewAsset ownershipAsset;
    CAmount tgtUnitDivisor = 0;
    if (!passets->GetAssetMetaDataIfExists(p_rewardSnapshot.strOwnershipAsset, ownershipAsset)) {
        LogPrint(BCLog::REWARDS, "%s: Failed to retrieve asset details for '%s'\n", __func__, p_rewardSnapshot.strOwnershipAsset.c_str());
        return false;
    }

    //  Save the ownership asset's divisor
    tgtUnitDivisor = static_cast<CAmount>(pow(10, COIN_DIGITS_PAST_DECIMAL - ownershipAsset.units));

    LogPrint(BCLog::REWARDS, "%s: Ownership asset '%s' has units %d and divisor %d\n", __func__,
             p_rewardSnapshot.strOwnershipAsset.c_str(), ownershipAsset.units, tgtUnitDivisor);

    //  Remove exception addresses & amounts from the list
    std::set<std::string> exceptionAddressSet;
    {
        std::istringstream iss(p_rewardSnapshot.strExceptionAddresses);
        std::string token;
        while (std::getline(iss, token, ',')) {
            if (!token.empty()) exceptionAddressSet.insert(token);
        }
    }

    std::set<OwnerAndAmount> nonExceptionOwnerships;
    CAmount totalAmtOwned = 0;

    CAssetSnapshotDBEntry snapshotEntry;
    if (!pAssetSnapshotDb->RetrieveOwnershipSnapshot(p_rewardSnapshot.strOwnershipAsset, p_rewardSnapshot.nHeight, snapshotEntry)) {
        LogPrint(BCLog::REWARDS, "%s: Failed to retrieve ownership snapshot list!\n", __func__);
        return false;
    }

    for (auto const & currPair : snapshotEntry.ownersAndAmounts) {
        //  Ignore exception and burn addresses
        if (
                exceptionAddressSet.find(currPair.first) == exceptionAddressSet.end()
                && !Params().IsBurnAddress(currPair.first)
                ) {
            //  Address is valid so add it to the payment list
            nonExceptionOwnerships.insert(OwnerAndAmount(currPair.first, currPair.second));
            totalAmtOwned += currPair.second;
        }
    }

    //  Make sure we have some addresses to pay to
    if (nonExceptionOwnerships.size() == 0) {
        LogPrint(BCLog::REWARDS, "%s: Ownership of '%s' includes only exception/burn addresses.\n", __func__,
                 p_rewardSnapshot.strOwnershipAsset.c_str());
        return false;
    }

    LogPrint(BCLog::REWARDS, "%s: Total amount owned %d\n", __func__,
             totalAmtOwned);

    LogPrint(BCLog::REWARDS, "%s: Total payout amount %d\n", __func__,
             modifiedPaymentInAssetUnits);

    CAmount totalSentAsRewards = 0;
    //  Loop through asset owners
    for (auto & ownership : nonExceptionOwnerships) {
        // Get percentage of total ownership
        long double percent = (long double)ownership.amount / (long double)totalAmtOwned;
        // Caculate the reward with potentional unit inaccurancies e.g with units 4, 90054100 satoshis = 0.90054100
        CAmount rewardAmt = percent * modifiedPaymentInAssetUnits * static_cast<CAmount>(pow(10, COIN_DIGITS_PAST_DECIMAL - distributionAsset.units));
        // Remove all none accurate units e.g with units 4 90054100 => 9005
        rewardAmt /= static_cast<CAmount>(pow(10, COIN_DIGITS_PAST_DECIMAL - distributionAsset.units));
        // Replace all none accurate units back with zeros e.g with units 4 9005 => 90050000 satoshis = 0.90050000
        rewardAmt *= static_cast<CAmount>(pow(10, COIN_DIGITS_PAST_DECIMAL - distributionAsset.units));

        totalSentAsRewards += rewardAmt;

        LogPrint(BCLog::REWARDS, "%s: Found ownership address for '%s': '%s' owns %d => reward %d\n", __func__,
                 p_rewardSnapshot.strOwnershipAsset.c_str(), ownership.address.c_str(),
                 ownership.amount, rewardAmt);

        //  Save it into our list if the reward payment is above zero
        if (rewardAmt > 0)
            vecDistributionList.push_back(OwnerAndAmount(ownership.address, rewardAmt));
    }

    CAmount change = totalAmtOwned - totalSentAsRewards;
    if (change > 0) {
        LogPrint(BCLog::REWARDS, "%s: Found change amount of %u\n", __func__, change);
    }

    return true;
}
