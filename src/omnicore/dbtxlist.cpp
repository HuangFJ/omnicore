
#include <cstdint>
#include <omnicore/activation.h>
#include <omnicore/convert.h>
#include <omnicore/dbtxlist.h>
#include <omnicore/dex.h>
#include <omnicore/log.h>
#include <omnicore/notifications.h>
#include <omnicore/omnicore.h>
#include <omnicore/tx.h>
#include <omnicore/utilsbitcoin.h>

#include <chain.h>
#include <chainparams.h>
#include <fs.h>
#include <validation.h>
#include <sync.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/system.h>
#include <util/strencodings.h>

#include <leveldb/iterator.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

CMPTxList::CMPTxList(const fs::path& path, bool fWipe)
{
    leveldb::Status status = Open(path, fWipe);
    PrintToConsole("Loading tx meta-info database: %s\n", status.ToString());
}

CMPTxList::~CMPTxList()
{
    if (msc_debug_persistence) PrintToLog("CMPTxList closed\n");
}

struct CBlockTxKey {
    static constexpr uint8_t prefix = 'b';
    uint32_t block = ~0u;
    uint256 txid;

    SERIALIZE_METHODS(CBlockTxKey, obj) {
        READWRITE(Using<BigEndian32Inv>(obj.block));
        READWRITE(obj.txid);
    }
};

struct CMPTxList::CTxKey {
    static constexpr uint8_t prefix = 't';
    uint256 txid;
    int block = 0;
    uint8_t valid = 0;
    uint32_t type = 0;

    SERIALIZE_METHODS(CTxKey, obj) {
        READWRITE(obj.txid);
        READWRITE(Using<VarintSigned>(obj.block));
        READWRITE(obj.valid);
        READWRITE(Using<Varint>(obj.type));
    }

    bool operator==(const CTxKey& other) const {
        return txid == other.txid
            && block == other.block
            && valid == other.valid
            && type == other.type;
    }

    bool operator!=(const CTxKey& other) const {
        return !(*this == other);
    }
};

void CMPTxList::recordTX(const uint256 &txid, bool fValid, int nBlock, unsigned int type, uint64_t nValue)
{
    // overwrite detection, we should never be overwriting a tx, as that means we have redone something a second time
    // reorgs delete all txs from levelDB above reorg_chain_height
    int64_t old_value;
    CTxKey old_key, key{txid, nBlock, fValid, type};
    if (getTX(txid, old_key, old_value) && (old_key == key && old_value == nValue)) {
        PrintToLog("LEVELDB TX OVERWRITE DETECTION - %s\n", txid.ToString());
    }

    PrintToLog("%s(%s, valid=%s, block= %d, type= %d, value= %lu)\n",
            __func__, txid.ToString(), fValid ? "YES" : "NO", nBlock, type, nValue);

    Write(CBlockTxKey{uint32_t(nBlock), txid}, "");
    Write(key, nValue);
    ++nWritten;
}

struct CPaymentTxKey {
    static constexpr uint8_t prefix = 'p';
    uint256 txid;
    uint32_t payments = ~0u;
    int block = 0;
    uint8_t valid = 0;

    SERIALIZE_METHODS(CPaymentTxKey, obj) {
        READWRITE(obj.txid);
        READWRITE(Using<BigEndian32Inv>(obj.payments));
        READWRITE(Using<VarintSigned>(obj.block));
        READWRITE(obj.valid);
    }
};

struct CPaymentTxValue {
    uint32_t vout;
    std::string buyer;
    std::string seller;
    uint32_t propertyId;
    uint64_t amount;

    SERIALIZE_METHODS(CPaymentTxValue, obj) {
        READWRITE(Using<Varint>(obj.vout));
        READWRITE(obj.buyer);
        READWRITE(obj.seller);
        READWRITE(Using<Varint>(obj.propertyId));
        READWRITE(obj.amount);
    }
};

void CMPTxList::recordPaymentTX(const uint256& txid, bool fValid, int nBlock, unsigned int vout, unsigned int propertyId, uint64_t nValue, const std::string& buyer, const std::string& seller)
{
    // Prep - setup vars
    uint32_t numberOfPayments = 1;

    // Step 1 - Check TXList to see if this cancel TXID exists
    // Step 2a - If doesn't exist leave number of affected txs & ref set to 1
    // Step 2b - If does exist add +1 to existing ref and set this ref as new number of affected
    CDBaseIterator it{NewIterator(), PartialKey<CPaymentTxKey>(txid)};
    if (it.Valid()) {
        numberOfPayments = it.Key<CPaymentTxKey>().payments + 1;
    }

    // Step 3 - Create new/update master record for payment tx in TXList
    Write(CBlockTxKey{uint32_t(nBlock), txid}, "");
    PrintToLog("DEXPAYDEBUG : Writing master record %s(%s, valid=%s, block= %d, number of payments= %d)\n", __func__, txid.ToString(), fValid ? "YES" : "NO", nBlock, numberOfPayments);

    // Step 4 - Write sub-record with payment details
    CPaymentTxKey key{txid, numberOfPayments, nBlock, fValid};
    CPaymentTxValue value{vout, buyer, seller, propertyId, nValue};
    Write(key, value);
    PrintToLog("DEXPAYDEBUG : Writing sub-record %s-%d with value %d:%s:%s:%d:%d\n", txid.ToString(), numberOfPayments, vout, buyer, seller, propertyId, nValue);
}

struct CDexCancelTxKey {
    static constexpr uint8_t prefix = 'c';
    uint256 txid;
    uint32_t affected = ~0u;
    int block = 0;
    uint8_t valid = 0;

    SERIALIZE_METHODS(CDexCancelTxKey, obj) {
        READWRITE(obj.txid);
        READWRITE(Using<BigEndian32Inv>(obj.affected));
        READWRITE(Using<VarintSigned>(obj.block));
        READWRITE(obj.valid);
    }
};

struct CDexCancelTxValue {
    uint32_t propertyId;
    uint64_t amount;

    SERIALIZE_METHODS(CDexCancelTxValue, obj) {
        READWRITE(Using<Varint>(obj.propertyId));
        READWRITE(obj.amount);
    }
};

struct CDexTxToCancelKey {
    static constexpr uint8_t prefix = 'e';
    const uint256& txid;

    SERIALIZE_METHODS(CDexTxToCancelKey, obj) {
        READWRITE(obj.txid);
    }
};

void CMPTxList::recordMetaDExCancelTX(const uint256& txid, const uint256& txidSub, bool fValid, int nBlock, unsigned int propertyId, uint64_t nValue)
{
    // Prep - setup vars
    uint32_t numerOfAffected = 1;

    // Step 1 - Check TXList to see if this cancel TXID exists
    // Step 2a - If doesn't exist leave number of affected txs & ref set to 1
    // Step 2b - If does exist add +1 to existing ref and set this ref as new number of affected
    CDBaseIterator it{NewIterator(), PartialKey<CDexCancelTxKey>(txid)};
    if (it.Valid()) {
        numerOfAffected = it.Key<CDexCancelTxKey>().affected + 1;
    }

    // Step 3 - Create new/update master record for cancel tx in TXList
    Write(CBlockTxKey{uint32_t(nBlock), txid}, "");
    PrintToLog("METADEXCANCELDEBUG : Writing master record %s(%s, valid=%s, block= %d, number of affected transactions= %d)\n", __func__, txid.ToString(), fValid ? "YES" : "NO", nBlock, numerOfAffected);

    Write(CDexTxToCancelKey{txidSub}, txid);
    // Step 4 - Write sub-record with cancel details
    CDexCancelTxValue value{propertyId, nValue};
    Write(CDexCancelTxKey{txid, numerOfAffected, nBlock, fValid}, value);
    PrintToLog("METADEXCANCELDEBUG : Writing sub-record %d-%d with value %s:%d:%d\n", txid.ToString(), numerOfAffected, txidSub.ToString(), propertyId, nValue);
}

struct CSendAllTxKey {
    static constexpr uint8_t prefix = 's';
    uint256 txid;
    uint32_t num = ~0u;

    SERIALIZE_METHODS(CSendAllTxKey, obj) {
        READWRITE(obj.txid);
        READWRITE(Using<BigEndian32Inv>(obj.num));
    }
};

struct CSendAllTxValue {
    uint32_t propertyId;
    int64_t amount;

    SERIALIZE_METHODS(CSendAllTxValue, obj) {
        READWRITE(Using<Varint>(obj.propertyId));
        READWRITE(obj.amount);
    }
};

/**
 * Records a "send all" sub record.
 */
void CMPTxList::recordSendAllSubRecord(const uint256& txid, int nBlock, int subRecordNumber, uint32_t propertyId, int64_t nValue)
{
    bool status = Write(CSendAllTxKey{txid, uint32_t(subRecordNumber)}, CSendAllTxValue{propertyId, nValue});
    Write(CBlockTxKey{uint32_t(nBlock), txid}, "");
    ++nWritten;
    if (msc_debug_txdb) PrintToLog("%s(): store: %s:%d=%d:%d, status: %s\n", __func__, txid.ToString(), subRecordNumber, propertyId, nValue, status ? "OK" : "NOK");
}

uint256 CMPTxList::findMetaDExCancel(const uint256& txid)
{
    uint256 cancelTxId;
    return Read(CDexTxToCancelKey{txid}, cancelTxId) ? cancelTxId : uint256{};
}

/**
 * Returns the number of sub records.
 */
int CMPTxList::getNumberOfSubRecords(const uint256& txid)
{
    CDBaseIterator it{NewIterator(), PartialKey<CPaymentTxKey>(txid)};
    if (it.Valid()) return it.Key<CPaymentTxKey>().payments;
    it.Seek(PartialKey<CSendAllTxKey>(txid));
    return it.Valid() ? it.Key<CSendAllTxKey>().num : 0;
}

int CMPTxList::getNumberOfMetaDExCancels(const uint256& txid)
{
    CDBaseIterator it{NewIterator(), PartialKey<CDexCancelTxKey>(txid)};
    return it.ValueOr<uint32_t>(0);
}

bool CMPTxList::getPurchaseDetails(const uint256& txid, int purchaseNumber, std::string* buyer, std::string* seller, uint64_t* vout, uint64_t* propertyId, uint64_t* nValue)
{
    CDBaseIterator it{NewIterator(), PartialKey<CPaymentTxKey>(txid)};
    for (; it; ++it) {
        if (it.Key<CPaymentTxKey>().payments != purchaseNumber) continue;
        auto value = it.Value<CPaymentTxValue>();
        *vout = value.vout;
        *buyer = value.buyer;
        *seller = value.seller;
        *propertyId = value.propertyId;
        *nValue = value.amount;
        return true;
    }
    return false;
}

/**
 * Retrieves details about a "metadex cancel" record.
 */
bool CMPTxList::getMetaDExCancelDetails(const uint256& txid, int subSend, uint32_t& propertyId, int64_t& amount)
{
    CDexCancelTxValue value;
    if (Read(CDexCancelTxKey{txid, uint32_t(subSend)}, value)) {
        propertyId = value.propertyId;
        amount = value.amount;
        return true;
    }
    return false;
}

/**
 * Retrieves details about a "send all" record.
 */
bool CMPTxList::getSendAllDetails(const uint256& txid, int subSend, uint32_t& propertyId, int64_t& amount)
{
    CSendAllTxValue value;
    if (Read(CSendAllTxKey{txid, uint32_t(subSend)}, value)) {
        propertyId = value.propertyId;
        amount = value.amount;
        return true;
    }
    return false;
}

int CMPTxList::getMPTransactionCountTotal()
{
    int count = 0;
    for (CDBaseIterator it{NewIterator()}; it; ++it) {
        switch(it.Key()[0]) {
            case CTxKey::prefix:
            case CPaymentTxKey::prefix:
            case CDexCancelTxKey::prefix:
            case CSendAllTxKey::prefix:
                ++count;
            break;
        }
    }
    return count;
}

int CMPTxList::getMPTransactionCountBlock(int block)
{
    int count = 0;
    CDBaseIterator it{NewIterator(), CBlockTxKey{uint32_t(block)}};
    for (; it; ++it) {
        auto key = it.Key<CBlockTxKey>();
        if (key.block != block) break;
        ++count;
    }
    return count;
}

/** Returns a list of all Omni transactions in the given block range. */
int CMPTxList::GetOmniTxsInBlockRange(int blockFirst, int blockLast, std::set<uint256>& retTxs)
{
    CDBaseIterator it{NewIterator(), CBlockTxKey{uint32_t(blockLast)}};
    for (; it; ++it) {
        auto key = it.Key<CBlockTxKey>();
        if (key.block < blockFirst) break;
        retTxs.insert(key.txid);
    }
    return retTxs.size();
}

struct CDBVersionKey {
    static constexpr uint8_t prefix = 'D';
    SERIALIZE_METHODS(CDBVersionKey, obj) {}
};

/*
 * Gets the DB version from txlistdb
 *
 * Returns the current version
 */
int CMPTxList::getDBVersion()
{
    uint8_t version;
    bool status = Read(CDBVersionKey{}, version);
    if (msc_debug_txdb) PrintToLog("%s(): dbversion %d status %s\n", __func__, version, status ? "OK" : "NOK");
    return status ? version : 0;
}

/*
 * Sets the DB version for txlistdb
 *
 * Returns the current version after update
 */
int CMPTxList::setDBVersion()
{
    bool status = Write(CDBVersionKey{}, uint8_t(DB_VERSION));
    if (msc_debug_txdb) PrintToLog("%s(): dbversion %d status %s\n", __func__, DB_VERSION, status ? "OK" : "NOK");
    return getDBVersion();
}

struct CNonFugibleKey {
    static constexpr uint8_t prefix = 'n';
    uint256 txid;

    SERIALIZE_METHODS(CNonFugibleKey, obj) {
        READWRITE(obj.txid);
    }
};

std::pair<int64_t,int64_t> CMPTxList::GetNonFungibleGrant(const uint256& txid)
{
    std::pair<int64_t, int64_t> value;
    return Read(CNonFugibleKey{txid}, value) ? value : std::pair<int64_t, int64_t>{0, 0};
}

void CMPTxList::RecordNonFungibleGrant(const uint256& txid, int64_t start, int64_t end)
{
    bool status = Write(CNonFugibleKey{txid}, std::make_pair(start, end));
    PrintToLog("%s(): Writing Non-Fungible Grant range %s:%d-%d (%s)\n", __FUNCTION__, txid.ToString(), start, end, status ? "OK" : "NOK");
}

bool CMPTxList::getTX(const uint256& txid, CTxKey& key, int64_t& value)
{
    CDBaseIterator it{NewIterator(), PartialKey<CTxKey>(txid)};
    bool status = it.Valid() && it.Key(key) && it.Value(value);
    ++nRead;
    return status;
}

bool CMPTxList::existsMPTX(const uint256& txid)
{
    CDBaseIterator it{NewIterator()};
    return (it.Seek(PartialKey<CTxKey>(txid)), it)
        || (it.Seek(PartialKey<CPaymentTxKey>(txid)), it);
}

// call it like so (variable # of parameters):
// int block = 0;
// ...
// uint64_t nNew = 0;
//
// if (getValidMPTX(txid, &block, &type, &nNew)) // if true -- the TX is a valid MP TX
//
bool CMPTxList::getValidMPTX(const uint256& txid, int* block, unsigned int* type, uint64_t* nAmended)
{
    if (msc_debug_txdb) PrintToLog("%s()\n", __func__);

    CTxKey key;
    int64_t value = 0;
    bool validity = false;
    if (getTX(txid, key, value)) {
        // parse the string returned, find the validity flag/bit & other parameters
        validity = key.valid > 0;
        block && (*block = key.block);
        type && (*type = key.type);
        nAmended && (*nAmended = value);
    } else {
        CPaymentTxKey key;
        CDBaseIterator it{NewIterator(), PartialKey<CPaymentTxKey>(txid)};
        if (it.Valid() && it.Key(key) && it.Value(value)) {
            validity = key.valid > 0;
            block && (*block = key.block);
            type && (*type = 0);
            nAmended && (*nAmended = value);
        }
    }
    if (msc_debug_txdb) printStats();
    return validity;
}

std::set<int> CMPTxList::GetSeedBlocks(int startHeight, int endHeight)
{
    std::set<int> setSeedBlocks;
    CDBaseIterator it{NewIterator(), CBlockTxKey{uint32_t(endHeight)}};
    for (; it; ++it) {
        auto key = it.Key<CBlockTxKey>();
        if (key.block < startHeight) break;
        setSeedBlocks.insert(key.block);
    }
    return setSeedBlocks;
}

std::map<uint256, int> CMPTxList::LoadValidTxs(int blockHeight, const std::set<int>& txtypes)
{
    std::map<uint256, int> txs;
    for (CDBaseIterator it{NewIterator(), CTxKey{}}; it; ++it) {
        auto key = it.Key<CTxKey>();
        if (!key.valid) continue;
        if (key.block > blockHeight) continue;
        if (!txtypes.empty() && !txtypes.count(key.type)) continue;
        txs.emplace(key.txid, key.block);
    }
    return txs;
}

bool CMPTxList::CheckForFreezeTxs(int blockHeight)
{
    for (CDBaseIterator it{NewIterator(), CTxKey{}}; it; ++it) {
        auto key = it.Key<CTxKey>();
        if (key.block < blockHeight) continue;
        auto txtype = key.type;
        if (txtype == MSC_TYPE_FREEZE_PROPERTY_TOKENS || txtype == MSC_TYPE_UNFREEZE_PROPERTY_TOKENS ||
            txtype == MSC_TYPE_ENABLE_FREEZING || txtype == MSC_TYPE_DISABLE_FREEZING) {
            return true;
        }
    }
    return false;
}

void CMPTxList::printStats()
{
    PrintToLog("CMPTxList stats: nWritten= %d , nRead= %d\n", nWritten, nRead);
}

void CMPTxList::printAll()
{
    int count = 0;
    std::string skey, svalue;
    for (CDBaseIterator it{NewIterator()}; it; ++it) {
        switch(it.Key()[0]) {
            case CTxKey::prefix: {
                auto key = it.Key<CTxKey>();
                skey = key.txid.ToString();
                auto value = it.Value<int64_t>();
                svalue = strprintf("%d:%d:%d:%d", key.block, key.valid, key.type, value);
            } break;
            case CPaymentTxKey::prefix: {
                auto key = it.Key<CPaymentTxKey>();
                skey = strprintf("%s-%d", key.txid.ToString(), key.payments);
                auto value = it.Value<CPaymentTxValue>();
                svalue = strprintf("%d:%d:%d:%s:%s:%d:%d", key.block, key.valid, value.vout, value.buyer, value.seller, value.propertyId, value.amount);
            } break;
            case CDexCancelTxKey::prefix: {
                auto key = it.Key<CDexCancelTxKey>();
                skey = strprintf("%s-%d", key.txid.ToString(), key.affected);
                auto value = it.Value<CDexCancelTxValue>();
                svalue = strprintf("%d:%d:%d:%d", key.block, key.valid, value.propertyId, value.amount);
            } break;
            case CSendAllTxKey::prefix: {
                auto key = it.Key<CSendAllTxKey>();
                skey = strprintf("%s-%d", key.txid.ToString(), key.num);
                auto value = it.Value<CSendAllTxValue>();
                svalue = strprintf("%d:%d", value.propertyId, value.amount);
            } break;
            default:
                continue;
        }
        PrintToConsole("entry #%8d= %s:%s\n", ++count, skey, svalue);
    }
}

template<typename T>
bool DeleteToBatch(CDBWriteBatch& batch, CDBaseIterator& it, const uint256& txid)
{
    bool found = false;
    for (it.Seek(PartialKey<T>(txid)); it; ++it) {
        found = true;
        batch.Delete(it.Key());
    }
    return found;
}

void CMPTxList::deleteTransactions(const std::set<uint256>& txs, int block)
{
    CDBWriteBatch batch;
    CDBaseIterator it{NewIterator()};
    std::set<uint256> cancelTxs;
    for (it.Seek(CBlockTxKey{}); it; ++it) {
        if (it.Key<CBlockTxKey>().block < block) break;
        batch.Delete(it.Key());
    }
    for (auto& txid : txs) {
        bool deleted = DeleteToBatch<CTxKey>(batch, it, txid) ||
                       DeleteToBatch<CPaymentTxKey>(batch, it, txid) ||
                       (DeleteToBatch<CDexCancelTxKey>(batch, it, txid) &&
                       cancelTxs.insert(txid).second);
        deleted |= DeleteToBatch<CSendAllTxKey>(batch, it, txid);
        deleted |= DeleteToBatch<CDexTxToCancelKey>(batch, it, txid);
        if (deleted) {
            PrintToLog("%s() DELETING: %s\n", __func__, txid.ToString());
        }
    }
    for (it.Seek(CDexTxToCancelKey{{}}); it && !cancelTxs.empty(); ++it) {
        auto txid = it.Value<uint256>();
        // if both cancel and payment txs are reverted don't put back payment one
        if (cancelTxs.erase(txid)) {
            batch.Delete(it.Key());
        }
    }
    WriteBatch(batch);
}

// figure out if there was at least 1 Master Protocol transaction within the block range, or a block if starting equals ending
// block numbers are inclusive
bool CMPTxList::isMPinBlockRange(int starting_block, int ending_block)
{
    unsigned int n_found = 0;
    CDBaseIterator it{NewIterator(), CBlockTxKey{uint32_t(ending_block)}};
    for (; it; ++it) {
        auto key = it.Key<CBlockTxKey>();
        if (key.block < starting_block) break;
        ++n_found;
    }
    PrintToLog("%s(%d, %d); n_found= %d\n", __func__, starting_block, ending_block, n_found);
    return (n_found);
}
