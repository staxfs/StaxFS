#pragma once

#include "../common/metadata_types.h"

namespace dfs {

class AbstractInodeStorage {
public:
  virtual ~AbstractInodeStorage() = 0;

  // CRUD functions

  /**
   * @brief Retrieves an inode from storage.
   *
   * @param inode_id The id of the inode to retrieve.
   * @param result_buffer Where to put the inode if it's found.
   * @return true The inode was found.
   * @return false The inode was not found.
   */
  auto GetInode(uint64_t inode_id, Inode &result_buffer) -> bool;

  /**
   * @brief Stores an inode into storage.
   *
   * @param inode_id The id of the inode to store.
   * @param source Inode to store.
   * @param cache Whether to update cache or not.
   */
  void PutInode(uint64_t inode_id, const Inode &source, bool cache = false);

  /**
   * @brief Removes an inode from storage.
   *
   * @param inode_id The id of the inode to remove. Deleting a non-existent
   * inode is a no-op.
   */
  void DeleteInode(uint64_t inode_id);

  /**
   * @brief Retrieves a directory table from storage.
   *
   * @param inode_id The DIRECTORY's inode_id you wish to find.
   * @param result_buffer Where to put the table if it's found.
   * @param size Buffer size in bytes. Pass -1 if you want to read all entries.
   * @param offset The offset to start reading.
   * @return int The number of bytes read. 0 indicates EOF and -1 error.
   */
  auto GetDents(uint64_t inode_id, Dirent &result_buffer, uint32_t size,
                int offset) -> int;

  /**
   * @brief Stores a Dirent into storage.
   *
   * @param inode_id The DIRECTORY's inode_id you wish to store.
   * @param source Pointer to the START of the directory table to be stored.
   * @param offset Where to put the entries. Give me -1 for append.
   */
  void PutDent(uint64_t inode_id, Dirent &source, int offset);

  /**
   * @brief Removes a directory table from storage.
   *
   * @param inode_id The DIRECTORY's inode_id you wish to remove.
   */
  void DeleteDents(uint64_t inode_id);
};

class AbstractPosixMetadata {
public:
  virtual ~AbstractPosixMetadata() = 0;

  /**
   * @brief Retrieves an inode from storage.
   *
   * @param path The path of the inode to retrieve.
   * @param buffer Where to put the inode if it's found.
   * @return int 0 if found, -1 if not found.
   */
  virtual auto Stat(std::string_view path, Inode &buffer) -> int = 0;

  /**
   * @brief Delete file or directory.
   *
   * @param path The path of the file/directory to delete.
   * @param is_dir Whether the path is a directory or not.
   * @return int 0 if success, -1 if error.
   */
  virtual auto Unlink(std::string_view path, bool is_dir) -> int = 0;

  /**
   * @brief Remove a directory.
   *
   * @param path The path of the directory to remove.
   * @return int 0 if success, -1 if error.
   */
  virtual auto Rmdir(std::string_view path) -> int = 0;

  /**
   * @brief Test for access permissions.
   *
   * @param path The path of the file/directory to test.
   * @param mode The mode to test.
   * @param uid The user id.
   * @param gid The group id.
   * @return int 0 if success, -1 if error.
   */
  virtual auto Access(std::string_view path, uint32_t mode, uint32_t uid,
                      uint32_t gid) -> int = 0;

  /**
   * @brief Create a file or directory.
   *
   * @param path The path of the file/directory to create.
   * @param mode The mode to create.
   * @param uid The user id.
   * @param gid The group id.
   * @param buf If null, create a directory. If not null, create a file.
   * @return int 0 if success, -1 if error.
   */
  virtual auto Create(std::string_view path, uint32_t mode, uint32_t uid,
                      uint32_t gid, Inode *buf) -> int = 0;

  /**
   * @brief Create a directory.
   *
   * @param path The path of the directory to create.
   * @param mode The mode to create.
   * @param uid The user id.
   * @param gid The group id.
   * @return int 0 if success, -1 if error.
   */
  virtual auto Mkdir(std::string_view path, uint32_t mode, uint32_t uid,
                     uint32_t gid) -> int = 0;

  /**
   * @brief Rename a file or directory.
   *
   * @param oldpath The old path.
   * @param newpath The new path.
   * @param link Whether to link or not.
   * @return int 0 if success, -1 if error.
   */
  virtual auto Rename(std::string_view oldpath, std::string_view newpath,
                      bool link) -> int = 0;

  /**
   * @brief Link a file.
   *
   * @param oldpath The old path.
   * @param newpath The new path.
   * @return int 0 if success, -1 if error.
   */
  virtual auto Link(std::string_view oldpath, std::string_view newpath)
      -> int = 0;

  virtual auto OpenDir(std::string_view path, int alloc) -> DirHandle = 0;

  // fsync(fd) -> 0 | -1
  virtual auto MetaSync(uint64_t id) -> int = 0;
};

} // namespace dfs
