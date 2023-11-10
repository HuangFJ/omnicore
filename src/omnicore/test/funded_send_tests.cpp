
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <omnicore/createpayload.h>
#include <omnicore/omnicore.h>
#include <omnicore/script.h>
#include <omnicore/utilsbitcoin.h>
#include <omnicore/wallettxbuilder.h>

#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <script/standard.h>
#include <thread>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <vector>

using namespace mastercore;

class FundedSendTestingSetup : public TestChain100Setup
{
public:
    void ProccessAndSyncBlocks(const std::vector<CMutableTransaction>& txns)
    {
        auto height = GetActiveChain().Height();
        CreateAndProcessBlock(txns, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        for (int i = 0; i < 100 && GetActiveChain().Height() != height + 1; i++) {
            std::this_thread::sleep_for(1ms);
        }
    }
    FundedSendTestingSetup()
    {
        m_wallet_loader = interfaces::MakeWalletLoader(*m_node.chain, *Assert(m_node.args));
        wallet = std::make_shared<wallet::CWallet>(m_node.chain.get(), "", m_args, wallet::CreateMockWalletDatabase());
        interface_wallet = interfaces::MakeWallet(*m_wallet_loader->context(), wallet);
        ProccessAndSyncBlocks({});
        {
            LOCK(wallet->cs_wallet);
            wallet->SetLastBlockProcessed(GetActiveChain().Height(), GetActiveChain().Tip()->GetBlockHash());
        }
        wallet->LoadWallet();
        auto spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        {
            LOCK2(wallet->cs_wallet, spk_man->cs_KeyStore);
            spk_man->AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
        }
        wallet::WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        wallet->ScanForWalletTransactions(GetActiveChain().Genesis()->GetBlockHash(), 0, {}, reserver, false, false);
        wallet->m_fallback_fee = CFeeRate(1000);
    }

    ~FundedSendTestingSetup()
    {
        wallet.reset();
    }

    void AddTx(std::vector<wallet::CRecipient>& recipients)
    {
        int changePos = -1;
        wallet::CCoinControl dummy;
        auto result = wallet::CreateTransaction(*wallet, recipients, changePos, dummy);
        BOOST_CHECK(result.has_value());
        auto tx = result->tx;
        wallet->CommitTransaction(tx, {}, {});
        CMutableTransaction blocktx;
        {
            LOCK(wallet->cs_wallet);
            blocktx = CMutableTransaction(*wallet->mapWallet.at(tx->GetHash()).tx);
        }
        ProccessAndSyncBlocks({blocktx});

        std::unordered_map<uint256, wallet::CWalletTx, SaltedTxidHasher>::iterator it;
        {
            LOCK(wallet->cs_wallet);
            it = wallet->mapWallet.find(tx->GetHash());
        }

        it->second.m_state = wallet::TxStateConfirmed{GetActiveChain().Tip()->GetBlockHash(), GetActiveChain().Height(), 1};
        {
            LOCK(wallet->cs_wallet);
            wallet->SetLastBlockProcessed(GetActiveChain().Height(), GetActiveChain().Tip()->GetBlockHash());
        }
    }

    // For dust set entry in amounts to -1
    std::vector<CTxDestination> CreateDestinations(std::vector<CAmount> amounts) {
        std::vector<wallet::CRecipient> recipients;
        std::vector<CTxDestination> destinations;
        for (auto amount : amounts) {
            CTxDestination dest;
            {
                LOCK(wallet->cs_wallet);
                dest = wallet->GetNewDestination(OutputType::LEGACY, "").value();
            }
            destinations.push_back(dest);
            if (amount > 0) {
                recipients.push_back({GetScriptForDestination(dest), amount, false});
            } else if (amount == -1) {
                recipients.push_back({GetScriptForDestination(dest), OmniGetDustThreshold(GetScriptForDestination(dest)), false});
            }
        }
        AddTx(recipients);

        BOOST_CHECK_EQUAL(destinations.size(), amounts.size());

        return destinations;
    }

    std::unique_ptr<interfaces::WalletLoader> m_wallet_loader;
    std::shared_ptr<wallet::CWallet> wallet;
    std::unique_ptr<interfaces::Wallet> interface_wallet;
};

static std::vector<unsigned char> dummy_payload() {
    return CreatePayload_SimpleSend(1, 1);
}

static void check_outputs(uint256& hash, int expected_number) {
    CTransactionRef tx;
    int blockHeight;
    bool tx_succeed = false;
    for (int i = 0; i < 100 && !tx_succeed; i++) {
        std::this_thread::sleep_for(1ms);
        tx_succeed = GetTransaction(hash, tx, blockHeight);
    }
    BOOST_REQUIRE(tx_succeed);
    BOOST_CHECK_EQUAL(tx->vout.size(), expected_number);
}

BOOST_FIXTURE_TEST_SUITE(omnicore_funded_send_tests, FundedSendTestingSetup)

BOOST_AUTO_TEST_CASE(create_token_funded_by_source)
{
    std::vector<CTxDestination> destinations = CreateDestinations({1 * COIN, 0});

    uint256 hash;
    wallet->SetBroadcastTransactions(true);
    BOOST_CHECK_EQUAL(CreateFundedTransaction(EncodeDestination(destinations[0] /* source */), EncodeDestination(destinations[1] /* receiver */), EncodeDestination(destinations[1] /* receiver */), dummy_payload(), hash, interface_wallet.get()), 0);

    // Expect two outputs
    check_outputs(hash, 2);
}

BOOST_AUTO_TEST_CASE(create_token_funded_by_receiver_address)
{
    std::vector<CTxDestination> destinations = CreateDestinations({-1 /* Dust */, 1 * COIN});

    uint256 hash;
    wallet->SetBroadcastTransactions(true);
    BOOST_CHECK_EQUAL(CreateFundedTransaction(EncodeDestination(destinations[0] /* source */), EncodeDestination(destinations[1] /* receiver */), EncodeDestination(destinations[1] /* receiver */), dummy_payload(), hash, interface_wallet.get()), 0);

    // Expect two outputs
    check_outputs(hash, 2);
}

BOOST_AUTO_TEST_CASE(create_token_funded_by_fee_address)
{
    std::vector<CTxDestination> destinations = CreateDestinations({-1 /* Dust */, 0, 1 * COIN});

    uint256 hash;
    wallet->SetBroadcastTransactions(true);
    BOOST_CHECK_EQUAL(CreateFundedTransaction(EncodeDestination(destinations[0] /* source */), EncodeDestination(destinations[1] /* receiver */), EncodeDestination(destinations[2] /* fee */), dummy_payload(), hash, interface_wallet.get()), 0);

    // Expect three outputs
    check_outputs(hash, 3);
}

BOOST_AUTO_TEST_SUITE_END()
