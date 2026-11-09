// Copyright 2026 TGS

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Internal POSIX deletion machinery for local-store teardown. Not part of
// the public API: consumers include mdio/dataset.h, which pulls this in for
// the kCreateClean fast path.

#ifndef MDIO_INTERNAL_LOCAL_STORE_DELETE_H_
#define MDIO_INTERNAL_LOCAL_STORE_DELETE_H_

#if !defined(_WIN32)

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// clang-format off
#include <nlohmann/json.hpp>  // NOLINT
// clang-format on

namespace mdio {
namespace internal {

/**
 * @brief Single-pass depth-first removal of a directory subtree, relative
 * to an open parent directory.
 *
 * Enumerates each directory exactly once: entries are unlinked as they
 * are read (trusting readdir d_type, so no per-entry probe openat),
 * sub-directories recurse and are unlinked bottom-up on unwind. This
 * avoids both tensorstore's per-file ENOTDIR probe openat and
 * libstdc++'s remove_all second pass (which re-opens and re-enumerates
 * every directory before removing it).
 *
 * The named directory itself is kept: callers remove it with an explicit
 * unlinkat when they want it gone.
 *
 * Best effort: entries that cannot be removed are left in place; the
 * stock per-variable DeleteRange that follows remains the backstop.
 */
inline bool SinglePassRemoveTree(int parent_fd, const char* name) {
  int fd = ::openat(parent_fd, name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return errno == ENOENT;  // Already gone: nothing to remove.
  }
  ::DIR* dir = ::fdopendir(fd);  // Takes ownership of fd on success.
  if (dir == nullptr) {
    ::close(fd);
    return false;
  }
  bool ok = true;
  while (true) {
    errno = 0;
    struct ::dirent* entry = ::readdir(dir);
    if (entry == nullptr) {
      if (errno != 0) {
        ok = false;
      }
      break;
    }
    if (::strcmp(entry->d_name, ".") == 0 ||
        ::strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (entry->d_type == DT_DIR) {
      ok = SinglePassRemoveTree(fd, entry->d_name) && ok;
      if (::unlinkat(fd, entry->d_name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        ok = false;
      }
    } else if (entry->d_type == DT_UNKNOWN) {
      // d_type not populated: try file first, then directory.
      if (::unlinkat(fd, entry->d_name, 0) != 0) {
        if (errno == EISDIR) {
          ok = SinglePassRemoveTree(fd, entry->d_name) && ok;
          if (::unlinkat(fd, entry->d_name, AT_REMOVEDIR) != 0 &&
              errno != ENOENT) {
            ok = false;
          }
        } else if (errno != ENOENT) {
          ok = false;
        }
      }
    } else {
      if (::unlinkat(fd, entry->d_name, 0) != 0 && errno != ENOENT) {
        ok = false;
      }
    }
  }
  ::closedir(dir);
  return ok;
}

/**
 * @brief Best-effort fast teardown of the variable subtrees a kCreateClean
 * open is about to replace.
 *
 * tensorstore's `delete_existing` pass deletes a file-kvstore range by
 * walking it with one `openat(O_DIRECTORY)` per entry: every chunk file
 * costs one failed (ENOTDIR) openat plus its unlinkat, and on NFS that
 * probe dominated the recreate wall time (measured: ~8.1k openat for a
 * 6.1k-file store, recreate ~13x a fresh create). This helper empties
 * each variable's own kvstore subtree first with SinglePassRemoveTree,
 * which trusts readdir d_type and enumerates each directory exactly
 * once, so the subsequent stock DeleteRange finds nothing left to walk.
 *
 * The deletion scope mirrors the stock pass, which deletes each in-spec
 * variable's subtree contents and nothing else: the variable directory
 * itself is kept (its inode stays valid, matching stock), and entries
 * under the common parent that are not an in-spec variable's own
 * subtree -- unrelated sibling directories, loose files -- are never
 * touched. The dataset-root metadata files the create flow rewrites
 * (.zmetadata/.zgroup/.zattrs for V2, zarr.json for V3) are deliberately
 * NOT removed: the stock flow only ever overwrites them, and removing
 * them here would turn a failure after the fast pre-delete (e.g. a
 * per-variable Open error) into an unrecognizable store -- zarr
 * version detection keys on exactly those root markers, so without
 * them a recoverable partial store looks like "not an MDIO store".
 *
 * Anything the fast pass could not remove is still handled by the stock
 * per-variable DeleteRange that follows, which therefore remains the
 * semantic backstop. Symlinked variable directories are not followed
 * (O_NOFOLLOW) and are left for the stock path.
 *
 * Applies only when every variable spec is a local-file kvstore whose
 * paths share one parent directory; anything else (cloud stores,
 * unexpected layout) keeps the stock behavior untouched.
 */
inline void BestEffortPreDeleteLocalStore(
    const std::vector<::nlohmann::json>& json_variables) {
  std::optional<std::string> store_root;
  std::vector<std::string> variable_names;
  for (const auto& json : json_variables) {
    if (!json.contains("kvstore")) {
      return;
    }
    const auto& kvstore = json["kvstore"];
    if (!kvstore.contains("driver") || !kvstore.contains("path") ||
        !kvstore["driver"].is_string() || !kvstore["path"].is_string() ||
        kvstore["driver"].get<std::string>() != "file") {
      return;  // Cloud or malformed spec: keep the stock delete path.
    }
    std::string path_text = kvstore["path"].get<std::string>();
    // A trailing slash would make filename() empty and parent_path()
    // resolve to the variable directory itself; strip it first.
    while (!path_text.empty() && path_text.back() == '/') {
      path_text.pop_back();
    }
    if (path_text.empty()) {
      return;  // Unexpected layout; keep the stock delete path.
    }
    const std::filesystem::path var_path(path_text);
    const std::string name = var_path.filename().string();
    const std::string parent = var_path.parent_path().string();
    // "." and ".." must never be treated as a variable directory: removing
    // one would walk outside the store.
    if (name.empty() || name == "." || name == ".." || parent.empty()) {
      return;  // Unexpected layout; keep the stock delete path.
    }
    if (!store_root) {
      store_root = parent;
    } else if (*store_root != parent) {
      return;  // Variables disagree on the dataset root; keep stock path.
    }
    variable_names.push_back(name);
  }
  if (!store_root || variable_names.empty()) {
    return;
  }
  int root_fd = ::open(store_root->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root_fd < 0) {
    return;  // Root missing: nothing to delete; stock path preserves
             // the open semantics.
  }
  ::DIR* root_dir = ::fdopendir(root_fd);
  if (root_dir == nullptr) {
    ::close(root_fd);
    return;
  }
  // Empty each in-spec variable's own subtree, keeping the variable
  // directory itself (matching the stock per-variable DeleteRange, which
  // never deletes the kvstore directory, only its contents). Unrelated
  // entries under the root -- including the dataset-root metadata files
  // the create flow rewrites -- are left untouched.
  for (const auto& name : variable_names) {
    SinglePassRemoveTree(root_fd, name.c_str());
  }
  ::closedir(root_dir);
}

}  // namespace internal
}  // namespace mdio

#endif  // !defined(_WIN32): Windows keeps an empty header (call sites are
        // gated on the same condition).

#endif  // MDIO_INTERNAL_LOCAL_STORE_DELETE_H_
