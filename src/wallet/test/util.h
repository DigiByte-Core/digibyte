// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_WALLET_TEST_UTIL_H
#define DIGIBYTE_WALLET_TEST_UTIL_H

#include <addresstype.h>
#include <wallet/db.h>

#include <functional>
#include <memory>
#include <optional>

class ArgsManager;
class CChain;
class CKey;
enum class OutputType;
namespace interfaces {
class Chain;
} // namespace interfaces

namespace wallet {
class CWallet;
class WalletDatabase;
struct WalletContext;

static const DatabaseFormat DATABASE_FORMATS[] = {
#ifdef USE_SQLITE
       DatabaseFormat::SQLITE,
#endif
#ifdef USE_BDB
       DatabaseFormat::BERKELEY,
#endif
};

const std::string ADDRESS_BCRT1_UNSPENDABLE = "bcrt1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq3xueyj";

std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, CChain& cchain, const CKey& key);

std::shared_ptr<CWallet> TestLoadWallet(WalletContext& context);
std::shared_ptr<CWallet> TestLoadWallet(std::unique_ptr<WalletDatabase> database, WalletContext& context, uint64_t create_flags);
void TestUnloadWallet(std::shared_ptr<CWallet>&& wallet);

// Creates a copy of the provided database
std::unique_ptr<WalletDatabase> DuplicateMockDatabase(WalletDatabase& database);

/** Returns a new encoded destination from the wallet (hardcoded to BECH32) */
std::string getnewaddress(CWallet& w);
/** Returns a new destination, of an specific type, from the wallet */
CTxDestination getNewDestination(CWallet& w, OutputType output_type);

using MockableData = std::map<SerializeData, SerializeData, std::less<>>;

class MockableCursor: public DatabaseCursor
{
public:
    MockableData::const_iterator m_cursor;
    MockableData::const_iterator m_cursor_end;
    bool m_pass;

    explicit MockableCursor(const MockableData& records, bool pass) : m_cursor(records.begin()), m_cursor_end(records.end()), m_pass(pass) {}
    MockableCursor(const MockableData& records, bool pass, Span<const std::byte> prefix);
    ~MockableCursor() {}

    Status Next(DataStream& key, DataStream& value) override;
};

class MockableBatch : public DatabaseBatch
{
private:
    MockableData& m_records;
    bool& m_pass;
    std::optional<size_t>* m_fail_write_at;
    size_t* m_write_count;
    bool* m_fail_commit;
    //! Whether this batch can group its writes into one transaction. A test sets
    //! this false to stand for a wallet file that takes single writes but cannot
    //! group them. See MockableDatabase::m_txn_pass.
    bool m_txn_pass;
    //! Called with the key of each row this batch writes, when a test asked to
    //! be told. See MockableDatabase::m_on_write.
    const std::function<void(Span<const std::byte>)>* m_on_write;
    //! Asked whether to refuse the write of a row, when a test asked to decide.
    //! See MockableDatabase::m_refuse_write.
    const std::function<bool(Span<const std::byte>)>* m_refuse_write;
    //! Holds the rows as they were when a database transaction began, so an
    //! abort can put them back. Set only while a transaction is open. Like
    //! Berkeley DB, the transaction belongs to this batch.
    std::optional<MockableData> m_rows_before_txn;

    DatabaseReadStatus ReadKey(DataStream&& key, DataStream& value) override;
    bool WriteKey(DataStream&& key, DataStream&& value, bool overwrite=true) override;
    bool EraseKey(DataStream&& key) override;
    bool HasKey(DataStream&& key) override;
    bool ErasePrefix(Span<const std::byte> prefix) override;

public:
    explicit MockableBatch(MockableData& records, bool& pass,
                           const std::function<void(Span<const std::byte>)>* on_write = nullptr,
                           const std::function<bool(Span<const std::byte>)>* refuse_write = nullptr,
                           bool txn_pass = true,
                           std::optional<size_t>* fail_write_at = nullptr,
                           size_t* write_count = nullptr,
                           bool* fail_commit = nullptr)
        : m_records(records), m_pass(pass), m_fail_write_at(fail_write_at),
          m_write_count(write_count), m_fail_commit(fail_commit), m_txn_pass(txn_pass),
          m_on_write(on_write), m_refuse_write(refuse_write) {}
    ~MockableBatch() { Close(); }

    void Flush() override {}
    //! A batch that goes away with a database transaction still open loses what
    //! that transaction wrote, the same as both real wallet files do.
    void Close() override { TxnAbort(); }

    std::unique_ptr<DatabaseCursor> GetNewCursor() override
    {
        return std::make_unique<MockableCursor>(m_records, m_pass);
    }
    std::unique_ptr<DatabaseCursor> GetNewPrefixCursor(Span<const std::byte> prefix) override {
        return std::make_unique<MockableCursor>(m_records, m_pass, prefix);
    }
    bool TxnBegin() override
    {
        // A file that refuses everything cannot start a transaction either, a
        // file that cannot group its writes cannot start one while still taking
        // single writes, and one transaction at a time is all this batch does.
        if (!m_pass || !m_txn_pass || m_rows_before_txn.has_value()) return false;
        m_rows_before_txn = m_records;
        return true;
    }
    bool TxnCommit() override
    {
        if (!m_pass || !m_txn_pass || !m_rows_before_txn.has_value()) return false;
        // Paymaster tests also exercise failure at the atomic commit boundary.
        if (m_fail_commit && *m_fail_commit) {
            TxnAbort();
            return false;
        }
        // The rows written since the transaction began stay as they are.
        m_rows_before_txn.reset();
        return true;
    }
    bool TxnAbort() override
    {
        if (!m_rows_before_txn.has_value()) return false;
        // Put the rows back the way they were when the transaction began.
        m_records = *m_rows_before_txn;
        m_rows_before_txn.reset();
        return true;
    }
};

/** A WalletDatabase whose contents and return values can be modified as needed for testing
 **/
class MockableDatabase : public WalletDatabase
{
public:
    MockableData m_records;
    bool m_pass{true};
    std::optional<size_t> m_fail_write_at;
    size_t m_write_count{0};
    bool m_fail_commit{false};
    //! A test sets this to false to stand for a wallet file that takes single
    //! writes but cannot group them into one transaction. Writes still work, so
    //! this is how a refused TxnBegin is tested without failing every write.
    bool m_txn_pass{true};
    //! A test can ask to be told when the wallet writes a row; it is called
    //! with the raw key bytes of that row, on the thread that is writing it.
    //! Tests use this to change the chain, the oracle quote or the wallet at
    //! the exact moment a named record is saved, instead of hoping to win a
    //! race between two threads.
    std::function<void(Span<const std::byte>)> m_on_write;
    //! A test can refuse the write of one named row, to stand for a wallet file
    //! that takes some rows and then stops taking them. It is given the raw key
    //! bytes of the row and returns true to refuse that write.
    std::function<bool(Span<const std::byte>)> m_refuse_write;

    MockableDatabase(MockableData records = {}) : WalletDatabase(), m_records(records) {}
    ~MockableDatabase() {};

    void Open() override {}
    void AddRef() override {}
    void RemoveRef() override {}

    bool Rewrite(const char* pszSkip=nullptr) override { return m_pass; }
    bool Backup(const std::string& strDest) const override { return m_pass; }
    void Flush() override {}
    void Close() override {}
    bool PeriodicFlush() override { return m_pass; }
    void IncrementUpdateCounter() override {}
    void ReloadDbEnv() override {}

    std::string Filename() override { return "mockable"; }
    std::string Format() override { return "mock"; }
    void FailWriteAt(size_t write_index)
    {
        m_write_count = 0;
        m_fail_write_at = write_index;
    }
    void FailCommit() { m_fail_commit = true; }
    void ClearFailureInjection()
    {
        m_fail_write_at.reset();
        m_write_count = 0;
        m_fail_commit = false;
    }

    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override { return std::make_unique<MockableBatch>(m_records, m_pass, &m_on_write, &m_refuse_write, m_txn_pass, &m_fail_write_at, &m_write_count, &m_fail_commit); }
};

std::unique_ptr<WalletDatabase> CreateMockableWalletDatabase(MockableData records = {});

MockableDatabase& GetMockableDatabase(CWallet& wallet);
} // namespace wallet

#endif // DIGIBYTE_WALLET_TEST_UTIL_H
