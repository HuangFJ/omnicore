#ifndef BITCOIN_OMNICORE_NFTDB_H
#define BITCOIN_OMNICORE_NFTDB_H

#include <omnicore/dbbase.h>

#include <omnicore/log.h>
#include <omnicore/persistence.h>

#include <fs.h>
#include <stdint.h>

enum NonFungibleStorage : uint8_t
{
    None       = 0,
    RangeIndex = 'R',
    GrantData  = 'G',
    IssuerData = 'I',
    HolderData = 'H',
};

struct CRollbackData {
    enum RollbackType {
        DELETE_KEY = 0,
        PERSIST_KEY
    };
    uint8_t type;
    std::string data;
};

/** LevelDB based storage for non-fungible tokens, with uid range (propertyid_tokenidstart-tokenidend) as key and token owner (address) as value.
 */
class CMPNonFungibleTokensDB : public CDBBase
{
    std::unordered_map<std::string, CRollbackData> blockData;
    // Sanity checks the token counts
    void SanityCheck();
    // Store writes to block in cache to be used in rollbacks
    void StoreBlockCache(const std::string& key);

public:
    CMPNonFungibleTokensDB(const fs::path& path, bool fWipe)
    {
        leveldb::Status status = Open(path, fWipe);
        PrintToConsole("Loading non-fungible tokens database: %s\n", status.ToString());
    }

    virtual ~CMPNonFungibleTokensDB()
    {
        if (msc_debug_persistence) PrintToLog("CMPNonFungibleTokensDB closed\n");
    }

    void printStats();
    void printAll();

    // Gets the data set in a non-fungible token
    std::string GetNonFungibleTokenValue(const uint32_t &propertyId, const int64_t &tokenId, const NonFungibleStorage type);
    // Checks if the range of tokens is contiguous (ie owned by a single address)
    std::string GetNonFungibleTokenValueInRange(const uint32_t &propertyId, const int64_t &rangeStart, const int64_t &rangeEnd);
    // Counts the highest token range end (which is thus the total number of tokens)
    int64_t GetHighestRangeEnd(const uint32_t &propertyId);
    // Creates a range of non-fungible tokens
    std::pair<int64_t,int64_t> CreateNonFungibleTokens(const uint32_t &propertyId, const int64_t &amount, const std::string &owner, const std::string &info);
    // Gets the range a non-fungible token is in
    std::pair<int64_t,int64_t> GetRange(const uint32_t &propertyId, const int64_t &tokenId, const NonFungibleStorage type);
    // Deletes a range of non-fungible tokens
    void DeleteRange(const uint32_t &propertyId, const int64_t &tokenIdStart, const int64_t &tokenIdEnd, const NonFungibleStorage type);
    // Moves a range of non-fungible tokens
    bool MoveNonFungibleTokens(const uint32_t &propertyId, const int64_t &tokenIdStart, const int64_t &tokenIdEnd, const std::string &from, const std::string &to);
    // Sets token data on non-fungible tokens
    bool ChangeNonFungibleTokenData(const uint32_t &propertyId, const int64_t &tokenIdStart, const int64_t &tokenIdEnd, const std::string &data, const NonFungibleStorage type);
    // Adds a range of non-fungible tokens
    void AddRange(const uint32_t &propertyId, const int64_t &tokenIdStart, const int64_t &tokenIdEnd, const std::string &owner, const NonFungibleStorage type);
    // Gets the non-fungible token ranges for a property ID and address
    std::map<uint32_t, std::vector<std::pair<int64_t, int64_t>>> GetAddressNonFungibleTokens(const uint32_t &propertyId, const std::string &address);
    // Gets the non-fungible token ranges for a property ID
    std::vector<std::pair<std::string,std::pair<int64_t,int64_t>>> GetNonFungibleTokenRanges(const uint32_t &propertyId);
    // Write block cache
    void WriteBlockCache(int height, bool sanityCheck = false);
    // Rollback records prior given height
    void RollBackAboveBlock(int height);
};

namespace mastercore
{
    extern CMPNonFungibleTokensDB *pDbNFT;
}

#endif // BITCOIN_OMNICORE_NFTDB_H
