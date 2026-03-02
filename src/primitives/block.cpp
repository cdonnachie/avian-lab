// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Copyright (c) 2017 The Raven Core developers
// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>

#include <algo/minotaurx/minotaurx.h>
#include <algo/x16r/x16r.h>

#include <cstdint>
#include <cstring>
#include <unordered_map>

#define TIME_MASK 0xffffff80

// Global PoW algorithm timestamps, set during initialization
static uint32_t g_nX16rtTimestamp = 0;
static uint32_t g_nDualAlgoTimestamp = 0;
static bool g_bPoWParamsSet = false;

// ========================================================================
// PowCache: global cache mapping SHA256d header hash -> PoW hash
// Avoids recomputing expensive X16R/X16RT/MinotaurX hashes
// ========================================================================
static constexpr size_t DEFAULT_POW_CACHE_SIZE = 10000000;

struct Uint256Hasher {
    size_t operator()(const uint256& v) const {
        // Use first 8 bytes as hash (good distribution for block hashes)
        return ReadLE64(v.data());
    }
};

static RecursiveMutex cs_powcache;
static std::unordered_map<uint256, uint256, Uint256Hasher> g_pow_cache GUARDED_BY(cs_powcache);
static size_t g_pow_cache_max_size = DEFAULT_POW_CACHE_SIZE;

void SetPoWHashParams(uint32_t nX16rtTimestamp, uint32_t nDualAlgoTimestamp)
{
    g_nX16rtTimestamp = nX16rtTimestamp;
    g_nDualAlgoTimestamp = nDualAlgoTimestamp;
    g_bPoWParamsSet = true;
}

bool ArePoWHashParamsSet()
{
    return g_bPoWParamsSet;
}

uint256 CBlockHeader::GetSHA256Hash() const
{
    return (HashWriter{} << *this).GetHash();
}

uint256 CBlockHeader::ComputePoWHash(uint32_t nX16rtTimestamp, uint32_t nDualAlgoTimestamp) const
{
    // Serialize the header to get the raw 80 bytes
    DataStream ss{};
    ss << *this;
    const unsigned char* pbegin = reinterpret_cast<const unsigned char*>(ss.data());
    const unsigned char* pend = pbegin + ss.size();

    if (nTime > nX16rtTimestamp) {
        if (nTime > nDualAlgoTimestamp) {
            // Multi algo (x16rt + MinotaurX)
            switch (GetPoWType()) {
            case POW_TYPE_X16RT: {
                int32_t nTimeX16r = nTime & TIME_MASK;
                uint256 hashTime = Hash(std::span{reinterpret_cast<const unsigned char*>(&nTimeX16r), sizeof(nTimeX16r)});
                return HashX16R(pbegin, pend, hashTime);
            }
            case POW_TYPE_MINOTAURX: {
                return Minotaurx(pbegin, pend, true);
            }
            default:
                // Don't crash on invalid blockType, just return a bad hash
                return HIGH_HASH;
            }
        } else {
            // x16rt before dual-algo
            int32_t nTimeX16r = nTime & TIME_MASK;
            uint256 hashTime = Hash(std::span{reinterpret_cast<const unsigned char*>(&nTimeX16r), sizeof(nTimeX16r)});
            return HashX16R(pbegin, pend, hashTime);
        }
    } else {
        // x16r (uses hashPrevBlock for hash selection)
        return HashX16R(pbegin, pend, hashPrevBlock);
    }
}

uint256 CBlockHeader::GetHash() const
{
    if (!g_bPoWParamsSet) {
        // Before PoW params are initialized (e.g., during genesis block creation),
        // fall back to SHA256d. This is safe because:
        // - Genesis block hashGenesisBlock is set to the known PoW hash constant
        // - No block validation occurs before params are initialized
        return GetSHA256Hash();
    }

    // Per-object cache (fast path, no lock needed)
    if (m_hasPoWHash) {
        return m_cachedPoWHash;
    }

    // Global PowCache lookup (keyed by SHA256d header hash)
    uint256 headerHash = GetSHA256Hash();
    {
        LOCK(cs_powcache);
        auto it = g_pow_cache.find(headerHash);
        if (it != g_pow_cache.end()) {
            m_cachedPoWHash = it->second;
            m_hasPoWHash = true;
            return m_cachedPoWHash;
        }
    }

    // Cache miss: compute the expensive PoW hash
    m_cachedPoWHash = ComputePoWHash(g_nX16rtTimestamp, g_nDualAlgoTimestamp);
    m_hasPoWHash = true;

    // Store in global cache
    {
        LOCK(cs_powcache);
        if (g_pow_cache.size() >= g_pow_cache_max_size * 2) {
            // Simple eviction: clear when we hit 2x max size
            g_pow_cache.clear();
        }
        g_pow_cache[headerHash] = m_cachedPoWHash;
    }

    return m_cachedPoWHash;
}

uint256 CBlockHeader::GetX16RHash() const
{
    DataStream ss{};
    ss << *this;
    const unsigned char* pbegin = reinterpret_cast<const unsigned char*>(ss.data());
    const unsigned char* pend = pbegin + ss.size();
    return HashX16R(pbegin, pend, hashPrevBlock);
}

// MinotaurX hash of arbitrary data
uint256 CBlockHeader::MinotaurxHashArbitrary(const char* data)
{
    return Minotaurx(data, data + strlen(data), true);
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
