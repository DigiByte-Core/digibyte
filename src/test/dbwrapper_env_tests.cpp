// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <dbwrapper_env.h>
#include <test/util/setup_common.h>
#include <util/fs.h>

#include <leveldb/env.h>
#include <leveldb/options.h>
#include <leveldb/slice.h>
#include <leveldb/status.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <string>

namespace {

void WriteContents(const fs::path& path, const std::string& contents)
{
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(contents.data(), contents.size());
    stream.close();
    BOOST_REQUIRE(stream.good());
}

std::unique_ptr<leveldb::RandomAccessFile> OpenRandomFile(leveldb::Env& env, const fs::path& path)
{
    leveldb::RandomAccessFile* raw{nullptr};
    const auto status = env.NewRandomAccessFile(fs::PathToString(path), &raw);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.ToString());
    BOOST_REQUIRE(raw != nullptr);
    return std::unique_ptr<leveldb::RandomAccessFile>{raw};
}

#ifdef __linux__
bool HasMapping(const fs::path& path)
{
    std::ifstream mappings{"/proc/self/maps"};
    BOOST_REQUIRE(mappings.good());
    const auto path_string = fs::PathToString(path);
    std::string line;
    while (std::getline(mappings, line)) {
        if (line.find(path_string) != std::string::npos) return true;
    }
    return false;
}
#endif

} // namespace

BOOST_FIXTURE_TEST_SUITE(dbwrapper_env_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(random_reads_use_caller_storage)
{
    const auto path = m_args.GetDataDirBase() / fs::u8path("random_reads-\u00e9");
    WriteContents(path, "0123456789");
    auto env = dbwrapper_private::MakeNoMmapEnv();
    auto file = OpenRandomFile(*env, path);
    std::array<char, 16> scratch{};
    leveldb::Slice result;

    auto status = file->Read(3, 4, &result, scratch.data());
    BOOST_REQUIRE_MESSAGE(status.ok(), status.ToString());
    BOOST_CHECK(result.data() == scratch.data());
    BOOST_CHECK_EQUAL(result.ToString(), "3456");
    BOOST_CHECK_EQUAL(file->GetName(), fs::PathToString(path));
    status = file->Read(0, 2, &result, scratch.data());
    BOOST_REQUIRE_MESSAGE(status.ok(), status.ToString());
    BOOST_CHECK(result.data() == scratch.data());
    BOOST_CHECK_EQUAL(result.ToString(), "01");
    status = file->Read(7, 0, &result, scratch.data());
    BOOST_CHECK(status.ok());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(short_reads_and_offset_overflow)
{
    const auto path = m_args.GetDataDirBase() / "short_reads";
    WriteContents(path, "0123456789");
    auto env = dbwrapper_private::MakeNoMmapEnv();
    auto file = OpenRandomFile(*env, path);
    std::array<char, 16> scratch{};
    leveldb::Slice result;

    auto status = file->Read(8, 8, &result, scratch.data());
    BOOST_REQUIRE_MESSAGE(status.ok(), status.ToString());
    BOOST_CHECK_EQUAL(result.ToString(), "89");
    status = file->Read(10, 1, &result, scratch.data());
    BOOST_CHECK(status.ok());
    BOOST_CHECK(result.empty());
    status = file->Read(std::numeric_limits<uint64_t>::max(), 1, &result, scratch.data());
    BOOST_CHECK(!status.ok());
    BOOST_CHECK(result.empty());
    status = file->Read(std::numeric_limits<int64_t>::max(), 2, &result, scratch.data());
    BOOST_CHECK(!status.ok());
    BOOST_CHECK(result.empty());
}

BOOST_AUTO_TEST_CASE(missing_files_are_explicit_and_open_files_own_their_source)
{
    const auto path = m_args.GetDataDirBase() / "file_lifetime";
    auto env = dbwrapper_private::MakeNoMmapEnv();
    leveldb::RandomAccessFile* raw{nullptr};
    const auto missing = env->NewRandomAccessFile(fs::PathToString(path), &raw);
    BOOST_CHECK(missing.IsNotFound());
    BOOST_CHECK(raw == nullptr);

    WriteContents(path, "original contents");
    auto file = OpenRandomFile(*env, path);
    env.reset();
#ifndef WIN32
    // An already-open file must not reopen a different file at the same path.
    BOOST_REQUIRE(fs::remove(path));
    WriteContents(path, "replacement data");
#endif
    std::array<char, 32> scratch{};
    leveldb::Slice result;
    const auto status = file->Read(0, 17, &result, scratch.data());
    BOOST_REQUIRE_MESSAGE(status.ok(), status.ToString());
    BOOST_CHECK_EQUAL(result.ToString(), "original contents");
}

BOOST_AUTO_TEST_CASE(concurrent_random_reads_keep_independent_offsets)
{
    const auto path = m_args.GetDataDirBase() / "concurrent_reads";
    std::string contents;
    for (int i = 0; i < 256; ++i) contents += static_cast<char>(i);
    WriteContents(path, contents);
    auto env = dbwrapper_private::MakeNoMmapEnv();
    auto file = OpenRandomFile(*env, path);

    auto read_range = [&](size_t base) {
        std::array<char, 8> scratch{};
        for (size_t i = 0; i < 1024; ++i) {
            const size_t offset = base + (i % 32);
            leveldb::Slice result;
            const auto status = file->Read(offset, scratch.size(), &result, scratch.data());
            if (!status.ok() || result.data() != scratch.data() ||
                result.ToString() != contents.substr(offset, scratch.size())) return false;
        }
        return true;
    };
    auto first = std::async(std::launch::async, read_range, 0);
    auto second = std::async(std::launch::async, read_range, 128);
    BOOST_CHECK(first.get());
    BOOST_CHECK(second.get());
}

BOOST_AUTO_TEST_CASE(database_policy_is_scoped_and_survives_restart)
{
    const auto bounded_path = m_args.GetDataDirBase() / "bounded_tables";
    const auto default_path = m_args.GetDataDirBase() / "default_tables";
    const std::string value(4096, 'v');
    for (const auto& path : {bounded_path, default_path}) {
        CDBWrapper writer{{.path = path, .cache_bytes = 1 << 20}};
        CDBBatch batch{writer};
        for (uint32_t key = 0; key < 256; ++key) batch.Write(key, value);
        BOOST_REQUIRE(writer.WriteBatch(batch, true));
    }

    CDBWrapper bounded{{.path = bounded_path, .cache_bytes = 1 << 20,
                        .options = {.force_compact = true}, .use_mmap = false, .max_open_files = 74}};
    CDBWrapper ordinary{{.path = default_path, .cache_bytes = 1 << 20,
                         .options = {.force_compact = true}}};
    const auto& bounded_options = dbwrapper_private::GetOptions(bounded);
    const auto& ordinary_options = dbwrapper_private::GetOptions(ordinary);
    BOOST_CHECK(bounded_options.env != leveldb::Env::Default());
    BOOST_CHECK(ordinary_options.env == leveldb::Env::Default());
    BOOST_CHECK_EQUAL(bounded_options.max_open_files, 74);
    int expected_default{1000};
#ifndef WIN32
    if (sizeof(void*) < 8) expected_default = 64;
#endif
    BOOST_CHECK_EQUAL(ordinary_options.max_open_files, expected_default);

    for (uint32_t key = 0; key < 256; ++key) {
        std::string actual;
        BOOST_REQUIRE(bounded.Read(key, actual));
        BOOST_CHECK_EQUAL(actual, value);
        BOOST_REQUIRE(ordinary.Read(key, actual));
        BOOST_CHECK_EQUAL(actual, value);
    }
#ifdef __linux__
    BOOST_CHECK(!HasMapping(bounded_path));
    if (sizeof(void*) >= 8) BOOST_CHECK(HasMapping(default_path));
#endif

    CDBWrapper memory{{.path = m_args.GetDataDirBase() / "memory_tables",
                       .cache_bytes = 1 << 20, .memory_only = true,
                       .use_mmap = false, .max_open_files = 74}};
    BOOST_REQUIRE(memory.Write(uint32_t{1}, value));
    std::string actual;
    BOOST_REQUIRE(memory.Read(uint32_t{1}, actual));
    BOOST_CHECK_EQUAL(actual, value);
    BOOST_CHECK(!fs::exists(m_args.GetDataDirBase() / "memory_tables"));
}

BOOST_AUTO_TEST_CASE(table_corruption_still_fails_checksum_validation)
{
    const auto path = m_args.GetDataDirBase() / "corrupt_table";
    {
        CDBWrapper writer{{.path = path, .cache_bytes = 1 << 20, .use_mmap = false}};
        CDBBatch batch{writer};
        for (uint32_t key = 0; key < 256; ++key) batch.Write(key, std::string(4096, 'v'));
        BOOST_REQUIRE(writer.WriteBatch(batch, true));
    }
    {
        CDBWrapper compact{{.path = path, .cache_bytes = 1 << 20,
                            .options = {.force_compact = true}, .use_mmap = false}};
    }
    size_t corrupted{0};
    for (const auto& entry : fs::directory_iterator{path}) {
        if (entry.path().extension() != ".ldb" && entry.path().extension() != ".sst") continue;
        std::fstream stream{entry.path(), std::ios::in | std::ios::out | std::ios::binary};
        char value{0};
        stream.get(value);
        BOOST_REQUIRE(stream.good());
        stream.seekp(0);
        stream.put(value ^ 1);
        stream.close();
        BOOST_REQUIRE(stream.good());
        ++corrupted;
    }
    BOOST_REQUIRE_GT(corrupted, 0U);
    const auto read_corrupt = [&] {
        CDBWrapper reader{{.path = path, .cache_bytes = 1 << 20, .use_mmap = false}};
        for (uint32_t key = 0; key < 256; ++key) {
            std::string value;
            reader.Read(key, value);
        }
    };
    BOOST_CHECK_THROW(read_corrupt(), dbwrapper_error);
}

BOOST_AUTO_TEST_SUITE_END()
