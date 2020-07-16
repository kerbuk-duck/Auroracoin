// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Copyright (c) 2014-2019 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>
#include <util/strencodings.h>
#include <crypto/common.h>
#include <crypto/groestl.h>
#include <crypto/qubit.h>
#include <crypto/scrypt.h>
#include <crypto/skein.h>
#include <consensus/consensus.h>
#include <chainparams.h>
#include <util/system.h>
#include <hash.h>
#include <tinyformat.h>
#include <crypto/common.h>
#include <arith_uint256.h>

uint256 CBlockHeader::GetHash() const
{
    return SerializeHash(*this);
}

int CBlockHeader::GetAlgo() const
{
    switch (nVersion & BLOCK_VERSION_ALGO)
    {
        case BLOCK_VERSION_SCRYPT:
            return ALGO_SCRYPT;
        case BLOCK_VERSION_SHA256D:
            return ALGO_SHA256D;
        case BLOCK_VERSION_GROESTL:
            return ALGO_GROESTL;
        case BLOCK_VERSION_SKEIN:
            return ALGO_SKEIN;
        case BLOCK_VERSION_QUBIT:
            return ALGO_QUBIT;
    }
    return ALGO_UNKNOWN;
}

uint256 CBlockHeader::GetPoWAlgoHash(const Consensus::Params& params) const
{
    uint256 thash;

    switch (GetAlgo())
    {
        case ALGO_SHA256D:
        {
            return GetHash();
        }
        case ALGO_SCRYPT:
        {
            scrypt_1024_1_1_256(BEGIN(nVersion), END(thash));
            break;
        }
        case ALGO_GROESTL:
        {
            groestl(BEGIN(nVersion), BEGIN(thash));
            break;
        }
        case ALGO_SKEIN:
        {
            skein(BEGIN(nVersion), BEGIN(thash));
            break;
        }
        case ALGO_QUBIT:
        {
            thash = qubit(BEGIN(nVersion));
            break;
        }
        case ALGO_UNKNOWN:
            // This block will be rejected anyway, but returning an always-invalid
            // PoW hash will allow it to be rejected sooner.
            thash = ArithToUint256(~arith_uint256(0));
            break;
    }
    return thash;
}

std::string CBlock::ToString(const Consensus::Params& params) const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, pow_algo=%d, pow_hash=%s, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        GetAlgo(),
        GetPoWAlgoHash(params).ToString(),
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}

std::string GetAlgoName(int Algo)
{
    switch (Algo)
    {
        case ALGO_SHA256D:
            return std::string("sha256d");
        case ALGO_SCRYPT:
            return std::string("scrypt");
        case ALGO_GROESTL:
            return std::string("groestl");
        case ALGO_SKEIN:
            return std::string("skein");
        case ALGO_QUBIT:
            return std::string("qubit");
    }
    return std::string("unknown");
}

int GetAlgoByName(std::string strAlgo, int fallback)
{
    transform(strAlgo.begin(),strAlgo.end(),strAlgo.begin(),::tolower);
    if (strAlgo == "sha" || strAlgo == "sha256" || strAlgo == "sha256d")
        return ALGO_SHA256D;
    else if (strAlgo == "scrypt")
        return ALGO_SCRYPT;
    else if (strAlgo == "groestl" || strAlgo == "groestlsha2")
        return ALGO_GROESTL;
    else if (strAlgo == "skein" || strAlgo == "skeinsha2")
        return ALGO_SKEIN;
    else if (strAlgo == "q2c" || strAlgo == "qubit")
        return ALGO_QUBIT;
    else
        return fallback;
}

int64_t GetBlockWeight(const CBlock& block)
{
    // This implements the weight = (stripped_size * 4) + witness_size formula,
    // using only serialization with and without witness data. As witness_size
    // is equal to total_size - stripped_size, this formula is identical to:
    // weight = (stripped_size * 3) + total_size.
    return ::GetSerializeSize(block, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) * (WITNESS_SCALE_FACTOR - 1) + ::GetSerializeSize(block, PROTOCOL_VERSION);
}
