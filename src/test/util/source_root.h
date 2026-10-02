// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_TEST_UTIL_SOURCE_ROOT_H
#define DIGIBYTE_TEST_UTIL_SOURCE_ROOT_H

#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>

/**
 * Reading a file out of the checkout from a unit test.
 *
 * A few tests read a source file and check what it still contains, for example
 * that a log line was not dropped. Those tests used to open the file with a
 * path like "src/digidollar/validation.cpp", which only finds the file when the
 * test program is started from the top of the checkout. Started from anywhere
 * else the test failed, and the message looked like a fault in the code when
 * really it was only the wrong working directory.
 *
 * The build already knows where the checkout is, so it passes that path to the
 * test program as DIGIBYTE_TEST_SOURCE_ROOT (see src/Makefile.test.include).
 * The helpers below use that first and then fall back to a search, so a
 * checkout that was moved or copied after the build still works.
 *
 * A tester who moves the checkout can point the tests at it by setting the
 * environment variable DIGIBYTE_TEST_SOURCE_ROOT to the new location.
 */

namespace source_root_detail {

//! True when this directory holds a DigiByte checkout.
inline bool LooksLikeRepositoryRoot(const fs::path& dir)
{
    return fs::exists(dir / "configure.ac") && fs::exists(dir / "src" / "validation.cpp");
}

//! Walk up from a starting directory and return the first checkout found.
inline fs::path SearchUpwards(const fs::path& start)
{
    fs::path dir{fs::absolute(start)};
    while (!dir.empty()) {
        if (LooksLikeRepositoryRoot(dir)) return dir;
        const fs::path parent{dir.parent_path()};
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

//! Where the running test program lives, or an empty path if that is unknown.
inline fs::path ProgramPath()
{
    try {
        const auto& master{boost::unit_test::framework::master_test_suite()};
        if (master.argc > 0 && master.argv != nullptr && master.argv[0] != nullptr && master.argv[0][0] != '\0') {
            return fs::absolute(fs::u8path(master.argv[0]));
        }
    } catch (const std::exception&) {
        // Boost only knows the command line once the test module is running.
    }
    return {};
}

inline fs::path FindRepositoryRootOnce()
{
    try {
        // The tester says where the checkout is. This wins so a moved checkout
        // can be used without rebuilding.
        if (const char* from_environment{std::getenv("DIGIBYTE_TEST_SOURCE_ROOT")};
            from_environment != nullptr && from_environment[0] != '\0') {
            const fs::path dir{fs::u8path(from_environment)};
            if (LooksLikeRepositoryRoot(dir)) return dir;
        }
        // The checkout the build ran in. This is the normal answer.
#ifdef DIGIBYTE_TEST_SOURCE_ROOT
        {
            const fs::path dir{fs::u8path(DIGIBYTE_TEST_SOURCE_ROOT)};
            if (LooksLikeRepositoryRoot(dir)) return dir;
        }
#endif
        // Above the test program itself, which normally sits in src/test/.
        if (const fs::path program{ProgramPath()}; !program.empty()) {
            if (const fs::path dir{SearchUpwards(program.parent_path())}; !dir.empty()) return dir;
        }
        // Above the directory the test program was started from.
        if (const fs::path dir{SearchUpwards(fs::current_path())}; !dir.empty()) return dir;
    } catch (const std::exception&) {
        // Any filesystem error just means the checkout was not found.
    }
    return {};
}

inline std::string CannotReadMessage(const char* path_from_repository_root, const fs::path& root)
{
    std::string where{root.empty() ? std::string{"no checkout was found"}
                                   : "looked in the checkout at " + root.u8string()};
    return std::string{"cannot read "} + path_from_repository_root + " (" + where + "). "
           "This test reads a file out of the checkout, so it needs the checkout the test program was "
           "built from. This is not a fault in the code under test. If the checkout has moved, set the "
           "environment variable DIGIBYTE_TEST_SOURCE_ROOT to it and run the test again.";
}

} // namespace source_root_detail

/**
 * The checkout this test program belongs to, or an empty path if it cannot be
 * found. The answer is worked out once and reused.
 */
inline fs::path FindRepositoryRoot()
{
    static const fs::path root{source_root_detail::FindRepositoryRootOnce()};
    return root;
}

/**
 * Read a whole file out of the checkout. The path is given from the top of the
 * checkout, for example "src/digidollar/validation.cpp". Fails the running test
 * with a message that explains what is missing if the file cannot be read.
 */
inline std::string ReadRepositoryFile(const char* path_from_repository_root)
{
    const fs::path root{FindRepositoryRoot()};
    std::string contents;
    if (!root.empty()) {
        std::ifstream file{root / path_from_repository_root, std::ios::binary};
        if (file) {
            contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
    }
    BOOST_REQUIRE_MESSAGE(!contents.empty(),
                          source_root_detail::CannotReadMessage(path_from_repository_root, root));
    return contents;
}

#endif // DIGIBYTE_TEST_UTIL_SOURCE_ROOT_H
