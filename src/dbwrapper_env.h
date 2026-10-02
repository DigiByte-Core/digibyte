// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_DBWRAPPER_ENV_H
#define DIGIBYTE_DBWRAPPER_ENV_H

#include <memory>

namespace leveldb {
class Env;
}

namespace dbwrapper_private {

/** Create a disk environment whose random reads use caller buffers, never mmap.
 * Other operations use LevelDB's default environment. Destroy the environment
 * after its database. Each open random-access file owns its underlying handle.
 */
std::unique_ptr<leveldb::Env> MakeNoMmapEnv();

} // namespace dbwrapper_private

#endif // DIGIBYTE_DBWRAPPER_ENV_H
