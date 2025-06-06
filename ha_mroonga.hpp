/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright (C) 2010  Tetsuro IKEDA
  Copyright (C) 2010-2013  Kentoku SHIBA
  Copyright (C) 2011-2025  Sutou Kouhei <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#pragma once

#ifdef USE_PRAGMA_INTERFACE
#  pragma interface
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <groonga.h>
#include "mrn_mysql_compat.h"
#include <mrn_operations.hpp>
#include <mrn_database.hpp>
#include <mrn_buffers.hpp>

#if !(defined(MRN_MARIADB_P) && MYSQL_VERSION_ID < 100600)
#  define MRN_HANDLER_HAVE_TABLE_TYPE
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_INDEX_TYPE
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_INDEX_READ_LAST_MAP
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_DELETE_TABLE_HAVE_HTON_DROP_TABLE
#endif

#ifdef MRN_MARIADB_P
#  define MRN_BIG_TABLES
#elif defined(BIG_TABLES)
#  define MRN_BIG_TABLES
#elif MYSQL_VERSION_ID >= 80021
#  define MRN_BIG_TABLES
#endif

#ifdef MRN_BIG_TABLES
#  define MRN_HA_ROWS_FORMAT "llu"
#else
#  define MRN_HA_ROWS_FORMAT "lu"
#endif

#ifdef MRN_MARIADB_P
#  define MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_CACHE
#  define MRN_HAVE_HA_EXTRA_NO_CACHE
#  define MRN_HAVE_HA_EXTRA_KEY_CACHE
#  define MRN_HAVE_HA_EXTRA_NO_KEY_CACHE
#  define MRN_HAVE_HA_EXTRA_WRITE_CACHE
#  define MRN_HAVE_HA_EXTRA_FLUSH_CACHE
#  define MRN_HAVE_HA_EXTRA_REINIT_CACHE
#  define MRN_HAVE_HA_EXTRA_MMAP
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_DETACH_CHILD
#  define MRN_HAVE_HA_EXTRA_PREPARE_FOR_FORCED_CLOSE
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_NO_AUTOINC_LOCKING
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80022)
#  define MRN_HAVE_HA_EXTRA_NO_READ_LOCKING
#endif

#if (defined(MRN_MARIADB_P) &&                                                 \
     ((MYSQL_VERSION_ID >= 100600 && MYSQL_VERSION_ID < 100616) ||             \
      (MYSQL_VERSION_ID >= 101100 && MYSQL_VERSION_ID < 101106)))
#  define MRN_HAVE_HA_EXTRA_IGNORE_INSERT
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_EXPORT
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_SECONDARY_SORT_ROWID
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_PREPARE_FOR_ALTER_TABLE
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_HA_EXTRA_STARTING_ORDERED_INDEX_SCAN
#endif

#if MYSQL_VERSION_ID >= 80017 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER
#  define MRN_HAVE_HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER
#endif

#if MYSQL_VERSION_ID >= 80017 && !defined(MRN_MARIADB_P)
#  define MRN_HAVE_MYSQL_TYPE_TYPED_ARRAY
#endif

#ifdef MRN_MARIADB_P
#  define MRN_TIMESTAMP_USE_MY_TIME_T
#  define MRN_TIMESTAMP_USE_MY_TIME_T_AND_POS
#elif MYSQL_VERSION_ID >= 80028
#  define MRN_TIMESTAMP_USE_MY_TIMEVAL
#else
#  define MRN_TIMESTAMP_USE_TIMEVAL
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_TL_WRITE_DELAYED
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HAVE_TL_WRITE_CONCURRENT_DEFAULT
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_AUTO_REPAIR_HAVE_ERROR
#  define MRN_HANDLER_AUTO_REPAIR_ERROR_OVERRIDE override
#  define MRN_HANDLER_AUTO_REPAIR_VOID_OVERRIDE
#else
#  define MRN_HANDLER_AUTO_REPAIR_ERROR_OVERRIDE
#  define MRN_HANDLER_AUTO_REPAIR_VOID_OVERRIDE override
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HAVE_POINT_XY
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HAVE_TDC_LOCK_TABLE_SHARE
#  define MRN_TABLE_SHARE_TDC_IS_POINTER
#endif

#ifndef MRN_MARIADB_P
#  define MRN_SUPPORT_PARTITION
#endif

#ifndef MRN_MARIADB_P
#  define MRN_FLUSH_LOGS_HAVE_BINLOG_GROUP_FLUSH
#endif

#ifdef MRN_MARIADB_P
#  define MRN_FOREIGN_KEY_USE_METHOD_ENUM
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_RESET_AUTO_INCREMENT
#endif

#ifdef MRN_MARIADB_P
typedef alter_table_operations mrn_alter_flags;
typedef alter_table_operations mrn_alter_table_flags;
#  define MRN_ALTER_INPLACE_INFO_FLAG(alter_inplace_info_name, name)           \
    ALTER_##name
#  define MRN_ALTER_INPLACE_INFO_ALTER_FLAG(name) ALTER_##name
#  define MRN_ALTER_INFO_FLAG(name)               ALTER_##name
#else
typedef Alter_inplace_info::HA_ALTER_FLAGS mrn_alter_flags;
#  define MRN_ALTER_INPLACE_INFO_FLAG(alter_inplace_info_name, name)           \
    alter_inplace_info_name
#  define MRN_ALTER_INPLACE_INFO_ALTER_FLAG(name)                              \
    Alter_inplace_info::ALTER_##name
#  define MRN_ALTER_INFO_FLAG(name) Alter_info::ALTER_##name
#endif

#ifdef MRN_MARIADB_P
typedef alter_table_operations mrn_alter_flags;
#else
typedef uint mrn_alter_table_flags;
#endif

#ifdef MRN_MARIADB_P
#  define MRN_ALTER_INPLACE_INFO_ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX           \
    MRN_ALTER_INPLACE_INFO_ALTER_FLAG(ADD_NON_UNIQUE_NON_PRIM_INDEX)
#  define MRN_ALTER_INPLACE_INFO_ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX          \
    MRN_ALTER_INPLACE_INFO_ALTER_FLAG(DROP_NON_UNIQUE_NON_PRIM_INDEX)
#else
#  define MRN_ALTER_INPLACE_INFO_ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX           \
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_INDEX, ADD_INDEX)
#  define MRN_ALTER_INPLACE_INFO_ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX          \
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_INDEX, DROP_INDEX)
#endif

#ifdef MRN_MARIADB_P
#  ifdef ALTER_INDEX_ORDER
#    define MRN_ALTER_INPLACE_INFO_ALTER_INDEX_ORDER ALTER_INDEX_ORDER
#  else
#    define MRN_ALTER_INPLACE_INFO_ALTER_INDEX_ORDER 0
#  endif
#else
#  define MRN_ALTER_INPLACE_INFO_ALTER_INDEX_ORDER 0
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLER_RECORDS_RETURN_ERROR
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_KEYS_TO_USE_FOR_SCANNING
#endif

#if MYSQL_VERSION_ID < 80011 || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_TABLE_CACHE_TYPE
#endif

#ifndef MRN_MARIADB_P
#  define MRN_ST_MYSQL_PLUGIN_HAVE_CHECK_UNINSTALL
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
#  define MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
#endif

#if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80011)
#  define MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
#  define MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
#  define MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLERTON_CREATE_HAVE_PARTITIONED
#endif

#ifdef MRN_HANDLERTON_CREATE_HAVE_PARTITIONED
#  define MRN_HANDLERTON_CREATE(hton, table, patitioned, mem_root)             \
    (hton)->create((hton), (table), (patitioned), (mem_root))
#else
#  define MRN_HANDLERTON_CREATE(hton, table, patitioned, mem_root)             \
    (hton)->create((hton), (table), (mem_root))
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  define mrn_get_new_handler(share, partitioned, alloc, db_type)              \
    get_new_handler((share), (partitioned), (alloc), (db_type))
#else
#  define mrn_get_new_handler(share, partitioned, alloc, db_type)              \
    get_new_handler((share), (alloc), (db_type))
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  define MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  define MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
#  define MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
#  define MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_HA_MULTI_RANGE_READ_NEXT
#  define MRN_HANDLER_HAVE_GET_REAL_ROW_TYPE
#  define MRN_HANDLER_HAVE_GET_DEFAULT_INDEX_ALGORITHM
#  define MRN_HANDLER_HAVE_IS_INDEX_ALGORITHM_SUPPORTED
#  define MRN_HANDLER_HAVE_GET_SE_PRIVATE_DATA
#  define MRN_HANDLER_HAVE_UPGRADE_TABLE
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_GET_MEMORY_BUFFER_SIZE
#  define MRN_HANDLER_HAVE_GET_FOREIGN_DUP_KEY
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLER_HAVE_NOTIFY_TABLE_CHANGED
#  define MRN_HANDLER_HAVE_PRIMARY_KEY_IS_CLUSTERED
#endif

#if defined(HAVE_PSI_INTERFACE) && defined(MRN_MARIADB_P)
#  define MRN_HAVE_PSI_SERVER
#endif

#ifdef MRN_MARIADB_P
typedef const uchar* mrn_update_row_new_data_t;
#else
typedef uchar* mrn_update_row_new_data_t;
#endif

#ifdef MRN_MARIADB_P
using mrn_write_row_buf_t = const uchar*;
#else
typedef uchar* mrn_write_row_buf_t;
#endif

#ifndef MRN_MARIADB_P
#  define MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
#endif

#ifdef MRN_PERCONA_P
#  define MRN_HANDLER_HAVE_HAS_GAP_LOCKS
#  if MYSQL_VERSION_ID >= 80018
#    define MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT noexcept
#  else
#    define MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT
#  endif
#endif

#if MYSQL_VERSION_ID < 80013 || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_CAN_SWITCH_ENGINES
#endif

#if !defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80027
/* nothing */
#elif !defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80016
#  define MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
#else
#  define MRN_HANDLER_HAVE_COND_POP
#endif

#if MYSQL_VERSION_ID >= 80017 && !defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_HA_EXTRA
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_RESTORE_AUTO_INCREMENT_OVERRIDE override
#  define MRN_HANDLER_RELEASE_AUTO_INCREMENT_OVERRIDE override
#else
#  define MRN_HANDLER_RESTORE_AUTO_INCREMENT_OVERRIDE
#  define MRN_HANDLER_RELEASE_AUTO_INCREMENT_OVERRIDE
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST
#else
#  define MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST const
#endif

#if MYSQL_VERSION_ID < 80019 || defined(MRN_MARIADB_P)
#  define MRN_HANDLER_HAVE_FOREIGN_KEY_INFO
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_RECORDS_IN_RANGE_HAVE_PAGE_RANGE
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLER_NEED_OVERRIDE_UNBIND_PSI
#  define MRN_HANDLER_NEED_OVERRIDE_REBIND_PSI
#endif

#if defined(MRN_MARIADB_P) &&                                                  \
  ((MYSQL_VERSION_ID >= 100525 && MYSQL_VERSION_ID < 100600) ||                \
   (MYSQL_VERSION_ID >= 100618 && MYSQL_VERSION_ID < 100700) ||                \
   (MYSQL_VERSION_ID >= 101108 && MYSQL_VERSION_ID < 101200) ||                \
   (MYSQL_VERSION_ID >= 110402))
#  define MRN_HANDLER_ENABLE_INDEXES_PARAMETERS       key_map map, bool persist
#  define MRN_HANDLER_DISABLE_INDEXES_PARAMETERS      key_map map, bool persist
#  define MRN_HANDLER_ENABLE_INDEXES_ALL_ARGS         key_map(table->s->keys), false
#  define MRN_HANDLER_DISABLE_INDEXES_ALL_ARGS        key_map(table->s->keys), false
#  define MRN_HANDLER_ENABLE_INDEXES_FORWARD_ARGS     map, persist
#  define MRN_HANDLER_DISABLE_INDEXES_FORWARD_ARGS    map, persist
#  define MRN_HANDLER_ENABLE_INDEXES_NEED_TO_EXECUTE  (true)
#  define MRN_HANDLER_DISABLE_INDEXES_NEED_TO_EXECUTE (true)
// keys_in_use excludes keys disabled by ALTER TABLE ... DISABLE KEYS.
// ALTER TABLE ... DISABLE KEYS can not disable unique keys.
// If keys_in_use.is_clear_all() is false, keys_in_use includes unique
// keys. So we need to skip unique keys explicitly.
#  define MRN_HANDLER_ENABLE_INDEXES_SKIP_UNIQUE_KEY                           \
    (!table->s->keys_in_use.is_clear_all())
#  define MRN_HANDLER_ENABLE_INDEXES_IS_TARGET_KEY(skip_unique_key,            \
                                                   key_info,                   \
                                                   i)                          \
    (map.is_set(i))
#  define MRN_HANDLER_DISABLE_INDEXES_IS_TARGET_KEY(key_info, i)               \
    !(map.is_set(i))
#else
#  define MRN_HANDLER_ENABLE_INDEXES_PARAMETERS    uint mode
#  define MRN_HANDLER_DISABLE_INDEXES_PARAMETERS   uint mode
#  define MRN_HANDLER_ENABLE_INDEXES_ALL_ARGS      HA_KEY_SWITCH_ALL
#  define MRN_HANDLER_DISABLE_INDEXES_ALL_ARGS     HA_KEY_SWITCH_ALL
#  define MRN_HANDLER_ENABLE_INDEXES_FORWARD_ARGS  mode
#  define MRN_HANDLER_DISABLE_INDEXES_FORWARD_ARGS mode
#  define MRN_HANDLER_ENABLE_INDEXES_NEED_TO_EXECUTE                           \
    (mode == HA_KEY_SWITCH_NONUNIQ_SAVE || mode == HA_KEY_SWITCH_ALL)
#  define MRN_HANDLER_DISABLE_INDEXES_NEED_TO_EXECUTE                          \
    (mode == HA_KEY_SWITCH_NONUNIQ_SAVE || mode == HA_KEY_SWITCH_ALL)
#  define MRN_HANDLER_ENABLE_INDEXES_SKIP_UNIQUE_KEY                           \
    (mode == HA_KEY_SWITCH_NONUNIQ_SAVE)
#  define MRN_HANDLER_DISABLE_INDEXES_SKIP_UNIQUE_KEY                          \
    (mode == HA_KEY_SWITCH_NONUNIQ_SAVE)
#  define MRN_HANDLER_ENABLE_INDEXES_IS_TARGET_KEY(skip_unique_key,            \
                                                   key_info,                   \
                                                   i)                          \
    !(skip_unique_key && (key_info[i].flags & HA_NOSAME))
#  define MRN_HANDLER_DISABLE_INDEXES_IS_TARGET_KEY(key_info, i)               \
    !(MRN_HANDLER_DISABLE_INDEXES_SKIP_UNIQUE_KEY &&                           \
      (key_info[i].flags & HA_NOSAME))
#endif

#if defined(MRN_MARIADB_P) && (MYSQL_VERSION_ID >= 110400)
using mrn_io_and_cpu_cost = IO_AND_CPU_COST;
#  define MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
#  define MRN_HANDLER_HAVE_KEYREAD_TIME
#else
using mrn_io_and_cpu_cost = double;
#  define MRN_HANDLER_HAVE_READ_TIME
#endif

#if defined(MRN_MARIADB_P) &&                                                  \
  ((MYSQL_VERSION_ID >= 100527 && MYSQL_VERSION_ID < 100600) ||                \
   (MYSQL_VERSION_ID >= 100620 && MYSQL_VERSION_ID < 100700) ||                \
   (MYSQL_VERSION_ID >= 101110 && MYSQL_VERSION_ID < 101200) ||                \
   (MYSQL_VERSION_ID >= 110400))
using mrn_handler_referenced_by_foreign_key_bool = bool;
#  define MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT const noexcept
#else
using mrn_handler_referenced_by_foreign_key_bool = uint;
#  define MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT
#endif

class ha_mroonga;

/* structs */
struct st_mrn_ft_info {
  struct _ft_vft* please;
  struct _ft_vft_ext* could_you;
  grn_ctx* ctx;
  grn_encoding encoding;
  uint flags;
  uint active_index;
  grn_obj query;
  grn_obj* table;
  grn_obj* index_column;
  grn_obj* expression;
  grn_obj* match_columns;
  grn_obj* result;
  grn_obj* sorted_result;
  grn_obj* score_column;
  grn_obj key;
  grn_obj score;
  KEY* key_info;
  KEY* primary_key_info;
  grn_obj* cursor;
  grn_obj* id_accessor;
  grn_obj* key_accessor;
  ha_mroonga* mroonga;
};

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
struct ha_field_option_struct {
  const char* groonga_type;
  const char* flags;
};

struct ha_index_option_struct {
  const char* tokenizer;
  const char* normalizer;
  const char* token_filters;
  const char* flags;
  const char* lexicon;
  const char* lexicon_flags;
};
#endif

/* handler class */
class ha_mroonga : public handler {
public:
#ifdef MRN_ENABLE_WRAPPER_MODE
  handler* wrap_handler;
  bool is_clone;
  ha_mroonga* parent_for_clone;
  MEM_ROOT* mem_root_for_clone;
#endif
  grn_obj key_buffer;
  grn_id record_id;
  grn_id* key_id;
  grn_id* del_key_id;
  MY_BITMAP multiple_column_key_bitmap;

private:
  THR_LOCK_DATA thr_lock_data;

#ifdef MRN_ENABLE_WRAPPER_MODE
  // for wrapper mode (TODO: need to be confirmed)
  uint wrap_ft_init_count;
#endif
  MRN_SHARE* share;
#ifdef MRN_ENABLE_WRAPPER_MODE
  KEY* wrap_key_info;
  KEY* base_key_info;
  key_part_map pk_keypart_map;
  MEM_ROOT mem_root;
  /// for create table and alter table
  mutable bool analyzed_for_create;
  mutable TABLE table_for_create;
  mutable MRN_SHARE share_for_create;
  mutable TABLE_SHARE table_share_for_create;
  mutable MEM_ROOT mem_root_for_create;
  mutable handler* wrap_handler_for_create;
  mrn_alter_flags alter_handler_flags;
  KEY* alter_key_info_buffer;
  uint alter_key_count;
  uint alter_index_drop_count;
  KEY* alter_index_drop_buffer;
  uint alter_index_add_count;
  uint* alter_index_add_buffer;
  TABLE* wrap_altered_table;
  KEY* wrap_altered_table_key_info;
  TABLE_SHARE* wrap_altered_table_share;
  KEY* wrap_altered_table_share_key_info;
#endif
  int mrn_lock_type;

  // for groonga objects
  grn_ctx ctx_entity_;
  grn_ctx* ctx;
  grn_obj* grn_table;
  grn_obj** grn_columns;
  grn_column_cache** grn_column_caches;
  grn_obj** grn_column_ranges;
  grn_obj** grn_index_tables;
  grn_obj** grn_index_columns;

  // buffers
  grn_obj encoded_key_buffer;
  grn_obj old_value_buffer;
  grn_obj new_value_buffer;
  grn_obj top_left_point;
  grn_obj bottom_right_point;
  grn_obj source_point;
  double top_left_longitude_in_degree;
  double bottom_right_longitude_in_degree;
  double bottom_right_latitude_in_degree;
  double top_left_latitude_in_degree;

  // for search
  grn_obj* grn_source_column_geo;
  grn_obj* cursor_geo;
  grn_table_cursor* cursor;
  grn_table_cursor* index_table_cursor;
  grn_obj* empty_value_records;
  grn_table_cursor* empty_value_records_cursor;
  grn_obj* condition_push_down_result;
  grn_obj* sorted_condition_push_down_result_;
  grn_table_cursor* condition_push_down_result_cursor;
  mrn::Buffers blob_buffers_;

  // for error report
  uint dup_key;

  // for optimization
  bool count_skip;
  bool fast_order_limit;
  bool fast_order_limit_with_index;

  // for context
  bool ignoring_duplicated_key;
  bool inserting_with_update;
  bool fulltext_searching;
  bool ignoring_no_key_columns;
  bool replacing_;
  uint written_by_row_based_binlog;

  // for ft in where clause test
  Item_func_match* current_ft_item;

  mrn::Operations* operations_;

public:
  ha_mroonga(handlerton* hton, TABLE_SHARE* share_arg);
  ~ha_mroonga();

  THD* current_thread();

#ifdef MRN_HANDLER_HAVE_TABLE_TYPE
  const char* table_type() const override;
#endif
#ifdef MRN_HANDLER_HAVE_INDEX_TYPE
  const char* index_type(uint inx) override;
#endif
  const char** bas_ext() const; // required

  ulonglong table_flags() const override; // required
  ulong index_flags(uint idx,
                    uint part,
                    bool all_parts) const override; // required

  // required
  int create(const char* name,
             TABLE* form,
             HA_CREATE_INFO* info
#ifdef MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
             ,
             dd::Table* table_def
#endif
             ) override;
#ifdef MRN_HANDLER_HAVE_GET_SE_PRIVATE_DATA
  bool get_se_private_data(dd::Table* dd_table, bool reset) override;
#endif
  // required
  int open(const char* name,
           int mode,
           uint open_options
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
           ,
           const dd::Table* table_def
#endif
           ) override;
  int info(uint flag); // required

  uint lock_count() const;
  THR_LOCK_DATA** store_lock(THD* thd, // required
                             THR_LOCK_DATA** to,
                             enum thr_lock_type lock_type);
  int external_lock(THD* thd, int lock_type);

  int rnd_init(bool scan); // required
  int rnd_end();
  void position(const uchar* record); // required
  int extra_opt(enum ha_extra_function operation, ulong cache_size);

  int delete_table(const char* name
#ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
                   ,
                   const dd::Table* table_def
#endif
                   ) override;
  int write_row(mrn_write_row_buf_t buf) override;
  int update_row(const uchar* old_data,
                 mrn_update_row_new_data_t new_data) override;
  int delete_row(const uchar* buf);

  uint max_supported_record_length() const override;
  uint max_supported_keys() const override;
  uint max_supported_key_parts() const override;
  uint max_supported_key_length() const override;
  uint max_supported_key_part_length(
#ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
    HA_CREATE_INFO* create_info
#endif
  ) const override;

#ifdef MRN_HANDLER_RECORDS_IN_RANGE_HAVE_PAGE_RANGE
  ha_rows records_in_range(uint inx,
                           const key_range* min_key,
                           const key_range* max_key,
                           page_range* pages) override;
#else
  ha_rows
  records_in_range(uint inx, key_range* min_key, key_range* max_key) override;
#endif
  int index_init(uint idx, bool sorted) override;
  int index_end() override;
#ifdef MRN_HANDLER_HAVE_INDEX_READ_LAST_MAP
  int index_read_last_map(uchar* buf,
                          const uchar* key,
                          key_part_map keypart_map) override;
#endif
  int index_next_same(uchar* buf, const uchar* key, uint keylen) override;

  int ft_init() override;
  FT_INFO* ft_init_ext(uint flags, uint inx, String* key) override;
  int ft_read(uchar* buf) override;

  const Item* cond_push(const Item* cond
#ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                        ,
                        bool other_tables_ok
#endif
                        ) override;
#ifdef MRN_HANDLER_HAVE_COND_POP
  void cond_pop() override;
#endif

  int reset() override;

  handler* clone(const char* name, MEM_ROOT* mem_root) override;
  void print_error(int error, myf flag) override;
  bool get_error_message(int error, String* buffer) override;
#ifdef MRN_HANDLER_HAVE_GET_FOREIGN_DUP_KEY
  bool get_foreign_dup_key(char* child_table_name,
                           uint child_table_name_len,
                           char* child_key_name,
                           uint child_key_name_len) override;
#endif
  void change_table_ptr(TABLE* table_arg, TABLE_SHARE* share_arg) override;
  mrn_io_and_cpu_cost scan_time() override;
#if defined(MRN_HANDLER_HAVE_READ_TIME) && defined(MRN_ENABLE_WRAPPER_MODE)
  double read_time(uint index, uint ranges, ha_rows rows) override;
#endif
#ifdef MRN_HANDLER_HAVE_GET_MEMORY_BUFFER_SIZE
  longlong get_memory_buffer_size() const override;
#endif
#ifdef MRN_HANDLER_HAVE_TABLE_CACHE_TYPE
  uint8 table_cache_type() override;
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  ha_rows multi_range_read_info_const(uint keyno,
                                      RANGE_SEQ_IF* seq,
                                      void* seq_init_param,
                                      uint n_ranges,
                                      uint* bufsz,
                                      uint* flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                      ha_rows limit,
#  endif
                                      Cost_estimate* cost) override;
#endif
  ha_rows multi_range_read_info(uint keyno,
                                uint n_ranges,
                                uint keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                uint key_parts,
#endif
                                uint* bufsz,
                                uint* flags,
                                Cost_estimate* cost) override;
  int multi_range_read_init(RANGE_SEQ_IF* seq,
                            void* seq_init_param,
                            uint n_ranges,
                            uint mode,
                            HANDLER_BUFFER* buf) override;
  int multi_range_read_next(range_id_t* range_info) override;
#ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
  void start_bulk_insert(ha_rows rows, uint flags) override;
#else
  void start_bulk_insert(ha_rows rows) override;
#endif
  int end_bulk_insert() override;
#ifdef MRN_HANDLER_HAVE_GET_SE_PRIVATE_DATA
  bool upgrade_table(THD* thd,
                     const char* db_name,
                     const char* table_name,
                     dd::Table* dd_table) override;
#endif
  int delete_all_rows() override;
  int truncate(
#ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
    dd::Table* table_def
#endif
    ) override;
#ifdef MRN_HANDLER_HAVE_KEYS_TO_USE_FOR_SCANNING
  const key_map* keys_to_use_for_scanning() override;
#endif
  ha_rows estimate_rows_upper_bound() override;
#ifdef MRN_HANDLER_HAVE_GET_REAL_ROW_TYPE
  enum row_type
  get_real_row_type(const HA_CREATE_INFO* create_info) const override;
#endif
#ifdef MRN_HANDLER_HAVE_GET_DEFAULT_INDEX_ALGORITHM
  enum ha_key_alg get_default_index_algorithm() const override;
#endif
#ifdef MRN_HANDLER_HAVE_IS_INDEX_ALGORITHM_SUPPORTED
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override;
#endif
  void update_create_info(HA_CREATE_INFO* create_info) override;
  int rename_table(const char* from,
                   const char* to
#ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
                   ,
                   const dd::Table* from_table_def,
                   dd::Table* to_table_def
#endif
                   ) override;
  bool is_crashed() const override;
  bool auto_repair(int error) const MRN_HANDLER_AUTO_REPAIR_ERROR_OVERRIDE;
  bool auto_repair() const MRN_HANDLER_AUTO_REPAIR_VOID_OVERRIDE;
  int disable_indexes(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS) override;
  int enable_indexes(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS) override;
  int check(THD* thd, HA_CHECK_OPT* check_opt) override;
  int repair(THD* thd, HA_CHECK_OPT* check_opt) override;
  bool check_and_repair(THD* thd) override;
  int analyze(THD* thd, HA_CHECK_OPT* check_opt) override;
  int optimize(THD* thd, HA_CHECK_OPT* check_opt) override;
  bool is_fatal_error(int error_num
#ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                      ,
                      uint flags
#endif
                      ) override;
  bool check_if_incompatible_data(HA_CREATE_INFO* create_info,
                                  uint table_changes) override;
  enum_alter_inplace_result
  check_if_supported_inplace_alter(TABLE* altered_table,
                                   Alter_inplace_info* ha_alter_info) override;
  int update_auto_increment();
  void set_next_insert_id(ulonglong id);
  void get_auto_increment(ulonglong offset,
                          ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong* first_value,
                          ulonglong* nb_reserved_values) override;
  void restore_auto_increment(ulonglong prev_insert_id)
    MRN_HANDLER_RESTORE_AUTO_INCREMENT_OVERRIDE;
#ifdef MRN_HANDLER_HAVE_RESTORE_AUTO_INCREMENT_NO_ARGUMENT
  void restore_auto_increment() MRN_HANDLER_RESTORE_AUTO_INCREMENT_OVERRIDE;
#endif
  void release_auto_increment() MRN_HANDLER_RELEASE_AUTO_INCREMENT_OVERRIDE;
  int check_for_upgrade(HA_CHECK_OPT* check_opt) override;
#ifdef MRN_HANDLER_HAVE_RESET_AUTO_INCREMENT
  int reset_auto_increment(ulonglong value) override;
#endif
  bool was_semi_consistent_read() override;
  void try_semi_consistent_read(bool yes) override;
  void unlock_row() override;
  int start_stmt(THD* thd, thr_lock_type lock_type) override;

#ifdef MRN_HANDLER_HAVE_HAS_GAP_LOCKS
  bool has_gap_locks() const MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT override;
#endif

protected:
#ifdef MRN_HANDLER_RECORDS_RETURN_ERROR
  int records(ha_rows* num_rows) override;
#else
  ha_rows records() override;
#endif
  int rnd_next(uchar* buf) override;
  int rnd_pos(uchar* buf, uchar* pos) override;
  int index_read_map(uchar* buf,
                     const uchar* key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_next(uchar* buf) override;
  int index_prev(uchar* buf) override;
  int index_first(uchar* buf) override;
  int index_last(uchar* buf) override;
#ifdef MRN_HANDLER_HAVE_PRIMARY_KEY_IS_CLUSTERED
  bool primary_key_is_clustered()
    MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST override;
#endif
#ifdef MRN_HANDLER_HAVE_CAN_SWITCH_ENGINES
  bool can_switch_engines() override;
#endif
#ifdef MRN_HANDLER_HAVE_FOREIGN_KEY_INFO
  bool is_fk_defined_on_table_or_index(uint index) override;
  char* get_foreign_key_create_info() override;
  int get_foreign_key_list(THD* thd,
                           List<FOREIGN_KEY_INFO>* f_key_list) override;
  int get_parent_foreign_key_list(THD* thd,
                                  List<FOREIGN_KEY_INFO>* f_key_list) override;
  mrn_handler_referenced_by_foreign_key_bool referenced_by_foreign_key()
    MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT override;
  void free_foreign_key_create_info(char* str) override;
#endif
  void init_table_handle_for_HANDLER() override;
#ifdef MRN_HANDLER_NEED_OVERRIDE_UNBIND_PSI
  void unbind_psi() override;
#endif
#ifdef MRN_HANDLER_NEED_OVERRIDE_REBIND_PSI
  void rebind_psi() override;
#endif
  bool prepare_inplace_alter_table(TABLE* altered_table,
                                   Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                   ,
                                   const dd::Table* old_table_def,
                                   dd::Table* new_table_def
#endif
                                   ) override;
  bool inplace_alter_table(TABLE* altered_table,
                           Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                           ,
                           const dd::Table* old_table_def,
                           dd::Table* new_table_def
#endif
                           ) override;
  bool commit_inplace_alter_table(TABLE* altered_table,
                                  Alter_inplace_info* ha_alter_info,
                                  bool commit
#ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                  ,
                                  const dd::Table* old_table_def,
                                  dd::Table* new_table_def
#endif
                                  ) override;
#ifdef MRN_HANDLER_HAVE_NOTIFY_TABLE_CHANGED
  void notify_table_changed(
#  ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
    Alter_inplace_info* ha_alter_info
#  endif
    ) override;
#endif

#if defined(MRN_HANDLER_HAVE_KEYREAD_TIME) && defined(MRN_ENABLE_WRAPPER_MODE)
  IO_AND_CPU_COST keyread_time(uint index,
                               ulong ranges,
                               ha_rows rows,
                               ulonglong blocks) override;
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override;
#endif

private:
  bool have_unique_index();

  bool is_foreign_key_field(const char* table_name,
                            const char* field_name,
                            size_t field_name_length);
  bool is_foreign_key_field(const char* table_name, const char* field_name);
  bool is_foreign_key_field(const char* table_name, LEX_CSTRING& field_name);

  void
  push_warning_unsupported_spatial_index_search(enum ha_rkey_function flag);
  void clear_cursor();
  void clear_cursor_geo();
  void clear_empty_value_records();
  void clear_search_result();
  void clear_search_result_geo();
  void clear_indexes();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int add_wrap_hton(const char* path, handlerton* wrap_handlerton);
#endif
  void remove_related_files(const char* base_path);
  void remove_grn_obj_force(const char* name);
  int drop_index(MRN_SHARE* target_share, uint key_index);
  int drop_indexes_normal(const char* table_name, grn_obj* table);
  int drop_indexes_multiple(const char* table_name,
                            grn_obj* table,
                            const char* index_table_name_separator);
  int drop_indexes(const char* table_name);
  bool find_table_flags(HA_CREATE_INFO* info,
                        MRN_SHARE* mrn_share,
                        grn_table_flags* table_flags);
  bool find_column_flags(Field* field,
                         MRN_SHARE* mrn_share,
                         int i,
                         grn_column_flags* column_flags);
  grn_obj*
  find_column_type(Field* field, MRN_SHARE* mrn_share, int i, int error_code);
  void set_tokenizer(grn_obj* lexicon, KEY* key);
  void set_tokenizer(grn_obj* lexicon, const char* name, size_t name_length);
  bool have_custom_normalizer(KEY* key) const;
  void set_normalizer(grn_obj* lexicon, KEY* key);
  void set_normalizer(grn_obj* lexicon,
                      KEY* key,
                      const char* normalizer,
                      size_t normalizer_length);
  bool find_index_column_flags(KEY* key, grn_column_flags* index_column_flags);
  void set_token_filters(grn_obj* lexicon, KEY* key);
  void set_token_filters(grn_obj* lexicon,
                         const char* token_filters,
                         size_t token_filters_length);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_get_record(uchar* buf, const uchar* key);
  int wrapper_get_next_geo_record(uchar* buf);
#endif
  int storage_get_next_record(uchar* buf);
  void geo_store_rectangle(const uchar* rectangle, bool reverse);
  int generic_geo_open_cursor(const uchar* key,
                              enum ha_rkey_function find_flag);

  int close() override;
  int extra(enum ha_extra_function operation) override;

  bool is_dry_write();
  bool is_enable_optimization();
  bool should_normalize(Field* field, bool is_fulltext_index) const;
  void check_count_skip(key_part_map target_key_part_map);
  bool is_grn_zero_column_value(grn_obj* column, grn_obj* value);
  bool is_primary_key_field(Field* field) const;

public:
  bool check_fast_order_limit(grn_obj* result_set,
                              grn_table_sort_key** sort_keys,
                              int* n_sort_keys,
                              longlong* limit);

private:
  int generic_store_bulk_fixed_size_string(Field* field, grn_obj* buf);
  int generic_store_bulk_variable_size_string(Field* field, grn_obj* buf);
  int generic_store_bulk_integer(Field* field, grn_obj* buf);
  int generic_store_bulk_unsigned_integer(Field* field, grn_obj* buf);
  int generic_store_bulk_float(Field* field, grn_obj* buf);
  int generic_store_bulk_timestamp(Field* field, grn_obj* buf);
  int generic_store_bulk_date(Field* field, grn_obj* buf);
  int generic_store_bulk_time(Field* field, grn_obj* buf);
  int generic_store_bulk_datetime(Field* field, grn_obj* buf);
  int generic_store_bulk_year(Field* field, grn_obj* buf);
  int generic_store_bulk_new_date(Field* field, grn_obj* buf);
  int generic_store_bulk_timestamp2(Field* field, grn_obj* buf);
  int generic_store_bulk_datetime2(Field* field, grn_obj* buf);
  int generic_store_bulk_time2(Field* field, grn_obj* buf);
  int generic_store_bulk_new_decimal(Field* field, grn_obj* buf);
  int generic_store_bulk_blob(Field* field, grn_obj* buf);
  int generic_store_bulk_geometry(Field* field, grn_obj* buf);
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
  int generic_store_bulk_json(Field* field, grn_obj* buf);
#endif
  int generic_store_bulk(Field* field, grn_obj* buf);

  void storage_store_field_string(Field* field,
                                  const char* value,
                                  uint value_length);
  void storage_store_field_integer(Field* field,
                                   const char* value,
                                   uint value_length);
  void storage_store_field_unsigned_integer(Field* field,
                                            const char* value,
                                            uint value_length);
  void
  storage_store_field_float(Field* field, const char* value, uint value_length);
  void storage_store_field_timestamp(Field* field,
                                     const char* value,
                                     uint value_length);
  void
  storage_store_field_date(Field* field, const char* value, uint value_length);
  void
  storage_store_field_time(Field* field, const char* value, uint value_length);
  void storage_store_field_datetime(Field* field,
                                    const char* value,
                                    uint value_length);
  void
  storage_store_field_year(Field* field, const char* value, uint value_length);
  void storage_store_field_new_date(Field* field,
                                    const char* value,
                                    uint value_length);
  void storage_store_field_datetime2(Field* field,
                                     const char* value,
                                     uint value_length);
  void
  storage_store_field_time2(Field* field, const char* value, uint value_length);
  void
  storage_store_field_blob(Field* field, const char* value, uint value_length);
#ifdef MRN_HAVE_MYSQL_TYPE_BLOB_COMPRESSED
  void storage_store_field_blob_compressed(Field* field,
                                           const char* value,
                                           uint value_length);
#endif
  bool geo_need_reverse(Field_geom* field);
  void storage_store_field_geometry(Field* field,
                                    const char* value,
                                    uint value_length);
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
  void
  storage_store_field_json(Field* field, const char* value, uint value_length);
#endif
  void storage_store_field(Field* field, const char* value, uint value_length);
  void
  storage_get_column_value(int nth_column, grn_id record_id, grn_obj* value);
  void storage_store_field_column(Field* field,
                                  bool is_primary_key,
                                  int nth_column,
                                  grn_id record_id);
  void storage_store_fields(TABLE* target_table, uchar* buf, grn_id record_id);
  void storage_store_fields_for_prep_update(const uchar* old_data,
                                            const uchar* new_data,
                                            grn_id record_id);
  void storage_store_fields_by_index(uchar* buf);

  int storage_encode_key_normalize_min_sort_chars(Field* field,
                                                  uchar* buf,
                                                  uint size);
  int storage_encode_key_fixed_size_string(Field* field,
                                           const uchar* key,
                                           uchar* buf,
                                           uint* size);
  int storage_encode_key_variable_size_string(Field* field,
                                              const uchar* key,
                                              uchar* buf,
                                              uint* size);
  int storage_encode_key_timestamp(Field* field,
                                   const uchar* key,
                                   uchar* buf,
                                   uint* size);
  int storage_encode_key_time(Field* field,
                              const uchar* key,
                              uchar* buf,
                              uint* size);
  int storage_encode_key_year(Field* field,
                              const uchar* key,
                              uchar* buf,
                              uint* size);
  int storage_encode_key_datetime(Field* field,
                                  const uchar* key,
                                  uchar* buf,
                                  uint* size);
  int storage_encode_key_timestamp2(Field* field,
                                    const uchar* key,
                                    uchar* buf,
                                    uint* size);
  int storage_encode_key_datetime2(Field* field,
                                   const uchar* key,
                                   uchar* buf,
                                   uint* size);
  int storage_encode_key_time2(Field* field,
                               const uchar* key,
                               uchar* buf,
                               uint* size);
  int storage_encode_key_enum(Field* field,
                              const uchar* key,
                              uchar* buf,
                              uint* size);
  int storage_encode_key_set(Field* field,
                             const uchar* key,
                             uchar* buf,
                             uint* size);
  int storage_encode_key(
    Field* field, const uchar* key, uchar* buf, uint* size, mrn_bool* is_null);
  int storage_encode_multiple_column_key(KEY* key_info,
                                         const uchar* key,
                                         uint key_length,
                                         uchar* buffer,
                                         uint* encoded_length);
  int storage_encode_multiple_column_key_range(KEY* key_info,
                                               const uchar* start,
                                               uint start_size,
                                               const uchar* end,
                                               uint end_size,
                                               uchar* min_buffer,
                                               uint* min_encoded_size,
                                               uchar* max_buffer,
                                               uint* max_encoded_size);
  int storage_encode_multiple_column_key_range(KEY* key_info,
                                               const key_range* start,
                                               const key_range* end,
                                               uchar* min_buffer,
                                               uint* min_encoded_size,
                                               uchar* max_buffer,
                                               uint* max_encoded_size);

  void set_pk_bitmap();
  int create_share_for_create() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_create(const char* name,
                     TABLE* table,
                     HA_CREATE_INFO* info,
#  ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
                     dd::Table* table_def,
#  endif
                     MRN_SHARE* tmp_share);
#endif
  int storage_create(const char* name,
                     TABLE* table,
                     HA_CREATE_INFO* info,
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
                     dd::Table* table_def,
#endif
                     MRN_SHARE* tmp_share);
#ifdef MRN_HANDLER_HAVE_GET_SE_PRIVATE_DATA
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_get_se_private_data(dd::Table* dd_table, bool reset);
#  endif
  bool storage_get_se_private_data(dd::Table* dd_table, bool reset);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_create_index_fulltext_validate(KEY* key_info);
  int wrapper_create_index_fulltext(const char* grn_table_name,
                                    int i,
                                    KEY* key_info,
                                    grn_obj** index_tables,
                                    grn_obj** index_columns);
  int wrapper_create_index_geo(const char* grn_table_name,
                               int i,
                               KEY* key_info,
                               grn_obj** index_tables,
                               grn_obj** index_columns);
  int wrapper_create_index(const char* name,
                           TABLE* table,
                           HA_CREATE_INFO* info,
                           MRN_SHARE* tmp_share);
#endif
  int storage_create_validate_pseudo_column(TABLE* table);
  bool storage_create_foreign_key(TABLE* table,
                                  const char* grn_table_name,
                                  Field* field,
                                  grn_obj* table_obj,
#ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
                                  const dd::Table* table_def,
#endif
                                  int& error);
  int storage_validate_key(KEY* key);
  int storage_create_validate_index(TABLE* table);
  bool find_lexicon_flags(KEY* key, grn_table_flags* lexicon_flags);
  int storage_create_index_table(TABLE* table,
                                 const char* grn_table_name,
                                 grn_obj* grn_table,
                                 KEY* key_info,
                                 grn_obj** index_tables,
                                 uint i);
  int storage_create_index(TABLE* table,
                           const char* grn_table_name,
                           grn_obj* grn_table,
                           KEY* key_info,
                           grn_obj** index_tables,
                           grn_obj** index_columns,
                           uint i);
  int storage_create_indexes(TABLE* table,
                             const char* grn_table_name,
                             grn_obj* grn_table,
                             MRN_SHARE* tmp_share);
  int close_databases();
  int ensure_database_open(const char* name, mrn::Database** db = NULL);
  int ensure_database_remove(const char* name);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_delete_table(const char* name,
#  ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
                           const dd::Table* table_def,
#  endif
                           handlerton* wrap_handlerton,
                           const char* table_name);
#endif
  int generic_delete_table(const char* name,
#ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
                           const dd::Table* table_def,
#endif
                           const char* table_name);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_open(const char* name,
                   int mode,
                   uint open_options
#  ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
                   ,
                   const dd::Table* table_def
#  endif
  );
  int wrapper_open_indexes(const char* name);
#endif
  int storage_reindex();
  int storage_open(const char* name,
                   int mode,
                   uint open_options
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
                   ,
                   const dd::Table* table_def
#endif
  );
  int open_table(const char* name);
  int storage_open_columns(void);
  void storage_close_columns(void);
  int storage_open_indexes(const char* name);
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_overwrite_index_bits();
  int wrapper_close();
#endif
  int storage_close();
  int generic_extra(enum ha_extra_function operation);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_extra(enum ha_extra_function operation);
#endif
  int storage_extra(enum ha_extra_function operation);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_extra_opt(enum ha_extra_function operation, ulong cache_size);
#endif
  int storage_extra_opt(enum ha_extra_function operation, ulong cache_size);
  int generic_reset();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_reset();
#endif
  int storage_reset();
#ifdef MRN_ENABLE_WRAPPER_MODE
  uint wrapper_lock_count() const;
#endif
  uint storage_lock_count() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  THR_LOCK_DATA** wrapper_store_lock(THD* thd,
                                     THR_LOCK_DATA** to,
                                     enum thr_lock_type lock_type);
#endif
  THR_LOCK_DATA** storage_store_lock(THD* thd,
                                     THR_LOCK_DATA** to,
                                     enum thr_lock_type lock_type);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_external_lock(THD* thd, int lock_type);
#endif
  int storage_external_lock(THD* thd, int lock_type);
#ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
#  ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_start_bulk_insert(ha_rows rows, uint flags);
#  endif
  void storage_start_bulk_insert(ha_rows rows, uint flags);
#else
#  ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_start_bulk_insert(ha_rows rows);
#  endif
  void storage_start_bulk_insert(ha_rows rows);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_end_bulk_insert();
#endif
  int storage_end_bulk_insert();
#ifdef MRN_HANDLER_HAVE_GET_SE_PRIVATE_DATA
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_upgrade_table(THD* thd,
                             const char* db_name,
                             const char* table_name,
                             dd::Table* dd_table);
#  endif
  bool storage_upgrade_table(THD* thd,
                             const char* db_name,
                             const char* table_name,
                             dd::Table* dd_table);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_is_target_index(KEY* key_info);
  bool wrapper_have_target_index();
  int wrapper_write_row(mrn_write_row_buf_t buf);
  int wrapper_write_row_index(mrn_write_row_buf_t buf);
#endif
  int storage_write_row(mrn_write_row_buf_t buf);
  int storage_write_row_multiple_column_index(mrn_write_row_buf_t buf,
                                              grn_id record_id,
                                              KEY* key_info,
                                              grn_obj* index_column);
  int storage_write_row_multiple_column_indexes(mrn_write_row_buf_t buf,
                                                grn_id record_id);
  int storage_write_row_unique_index(const uchar* buf,
                                     KEY* key_info,
                                     grn_obj* index_table,
                                     grn_obj* index_column,
                                     grn_id* key_id);
  int storage_write_row_unique_indexes(const uchar* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_get_record_id(uchar* data,
                            grn_id* record_id,
                            const char* context);
  int wrapper_update_row(const uchar* old_data,
                         mrn_update_row_new_data_t new_data);
  int wrapper_update_row_index(const uchar* old_data,
                               mrn_update_row_new_data_t new_data);
#endif
  int storage_update_row(const uchar* old_data,
                         mrn_update_row_new_data_t new_data);
  int storage_update_row_index(const uchar* old_data,
                               mrn_update_row_new_data_t new_data);
  int storage_update_row_unique_indexes(mrn_update_row_new_data_t new_data);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_delete_row(const uchar* buf);
  int wrapper_delete_row_index(const uchar* buf);
#endif
  int storage_delete_row(const uchar* buf);
  int storage_delete_row_index(const uchar* buf);
  int storage_delete_row_unique_index(grn_obj* index_table, grn_id del_key_id);
  int storage_delete_row_unique_indexes();
  int storage_prepare_delete_row_unique_index(const uchar* buf,
                                              grn_id record_id,
                                              KEY* key_info,
                                              grn_obj* index_table,
                                              grn_obj* index_column,
                                              grn_id* del_key_id);
  int storage_prepare_delete_row_unique_indexes(const uchar* buf,
                                                grn_id record_id);
#ifdef MRN_ENABLE_WRAPPER_MODE
  uint wrapper_max_supported_record_length() const;
#endif
  uint storage_max_supported_record_length() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  uint wrapper_max_supported_keys() const;
#endif
  uint storage_max_supported_keys() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  uint wrapper_max_supported_key_parts() const;
#endif
  uint storage_max_supported_key_parts() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  uint wrapper_max_supported_key_length() const;
#endif
  uint storage_max_supported_key_length() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  uint wrapper_max_supported_key_part_length(
#  ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
    HA_CREATE_INFO* create_info
#  endif
  ) const;
#endif
  uint storage_max_supported_key_part_length(
#ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
    HA_CREATE_INFO* create_info
#endif
  ) const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  ulonglong wrapper_table_flags() const;
#endif
  ulonglong storage_table_flags() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  ulong wrapper_index_flags(uint idx, uint part, bool all_parts) const;
#endif
  ulong storage_index_flags(uint idx, uint part, bool all_parts) const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_info(uint flag);
#endif
  int storage_info(uint flag);
  void storage_info_variable();
  void storage_info_variable_records();
  void storage_info_variable_data_file_length();
#ifdef MRN_HANDLER_RECORDS_RETURN_ERROR
#  ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_records(ha_rows* num_rows);
#  endif
  int storage_records(ha_rows* num_rows);
#else
#  ifdef MRN_ENABLE_WRAPPER_MODE
  ha_rows wrapper_records();
#  endif
  ha_rows storage_records();
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_rnd_init(bool scan);
#endif
  int storage_rnd_init(bool scan);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_rnd_end();
#endif
  int storage_rnd_end();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_rnd_next(uchar* buf);
#endif
  int storage_rnd_next(uchar* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_rnd_pos(uchar* buf, uchar* pos);
#endif
  int storage_rnd_pos(uchar* buf, uchar* pos);
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_position(const uchar* record);
#endif
  void storage_position(const uchar* record);
#ifdef MRN_ENABLE_WRAPPER_MODE
#  ifdef MRN_HANDLER_RECORDS_IN_RANGE_HAVE_PAGE_RANGE
  ha_rows wrapper_records_in_range(uint key_nr,
                                   const key_range* range_min,
                                   const key_range* range_max,
                                   page_range* pages);
#  else
  ha_rows wrapper_records_in_range(uint key_nr,
                                   key_range* range_min,
                                   key_range* range_max);
#  endif
#endif
  ha_rows storage_records_in_range(uint key_nr,
                                   const key_range* range_min,
                                   const key_range* range_max);
  ha_rows generic_records_in_range_geo(uint key_nr,
                                       const key_range* range_min,
                                       const key_range* range_max);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_init(uint idx, bool sorted);
#endif
  int storage_index_init(uint idx, bool sorted);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_end();
#endif
  int storage_index_end();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_read_map(uchar* buf,
                             const uchar* key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag);
#endif
  int storage_index_read_map(uchar* buf,
                             const uchar* key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag);
#ifdef MRN_HANDLER_HAVE_INDEX_READ_LAST_MAP
#  ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_read_last_map(uchar* buf,
                                  const uchar* key,
                                  key_part_map keypart_map);
#  endif
  int storage_index_read_last_map(uchar* buf,
                                  const uchar* key,
                                  key_part_map keypart_map);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_next(uchar* buf);
#endif
  int storage_index_next(uchar* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_prev(uchar* buf);
#endif
  int storage_index_prev(uchar* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_first(uchar* buf);
#endif
  int storage_index_first(uchar* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_last(uchar* buf);
#endif
  int storage_index_last(uchar* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_index_next_same(uchar* buf, const uchar* key, uint keylen);
#endif
  int storage_index_next_same(uchar* buf, const uchar* key, uint keylen);
  int generic_ft_init();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_ft_init();
#endif
  int storage_ft_init();
#ifdef MRN_ENABLE_WRAPPER_MODE
  FT_INFO* wrapper_ft_init_ext(uint flags, uint key_nr, String* key);
#endif
  FT_INFO* storage_ft_init_ext(uint flags, uint key_nr, String* key);
  grn_rc generic_ft_init_ext_prepare_expression_in_boolean_mode(
    struct st_mrn_ft_info* info,
    String* key,
    grn_obj* index_column,
    grn_obj* match_columns,
    grn_obj* expression);
  grn_rc generic_ft_init_ext_prepare_expression_in_natural_language_mode(
    struct st_mrn_ft_info* info,
    String* key,
    grn_obj* index_column,
    grn_obj* match_columns,
    grn_obj* expression);
  struct st_mrn_ft_info*
  generic_ft_init_ext_select(uint flags, uint key_nr, String* key);
  FT_INFO* generic_ft_init_ext(uint flags, uint key_nr, String* key);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_ft_read(uchar* buf);
#endif
  int storage_ft_read(uchar* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  const Item* wrapper_cond_push(const Item* cond
#  ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                ,
                                bool other_tables_ok
#  endif
  );
#endif
  const Item* storage_cond_push(const Item* cond
#ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                ,
                                bool other_tables_ok
#endif
  );
#ifdef MRN_HANDLER_HAVE_COND_POP
#  ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_cond_pop();
#  endif
  void storage_cond_pop();
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  handler* wrapper_clone(const char* name, MEM_ROOT* mem_root);
#endif
  handler* storage_clone(const char* name, MEM_ROOT* mem_root);
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_print_error(int error, myf flag);
#endif
  void storage_print_error(int error, myf flag);
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_get_error_message(int error, String* buffer);
#endif
  bool storage_get_error_message(int error, String* buffer);
#ifdef MRN_HANDLER_HAVE_GET_FOREIGN_DUP_KEY
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_get_foreign_dup_key(char* child_table_name,
                                   uint child_table_name_len,
                                   char* child_key_name,
                                   uint child_key_name_len);
#  endif
  bool storage_get_foreign_dup_key(char* child_table_name,
                                   uint child_table_name_len,
                                   char* child_key_name,
                                   uint child_key_name_len);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_change_table_ptr(TABLE* table_arg, TABLE_SHARE* share_arg);
#endif
  void storage_change_table_ptr(TABLE* table_arg, TABLE_SHARE* share_arg);
#ifdef MRN_ENABLE_WRAPPER_MODE
  mrn_io_and_cpu_cost wrapper_scan_time();
#endif
  mrn_io_and_cpu_cost storage_scan_time();

#if defined(MRN_HANDLER_HAVE_READ_TIME) && defined(MRN_ENABLE_WRAPPER_MODE)
  double wrapper_read_time(uint index, uint ranges, ha_rows rows);
  double storage_read_time(uint index, uint ranges, ha_rows rows);
#endif

#if defined(MRN_HANDLER_HAVE_KEYREAD_TIME) && defined(MRN_ENABLE_WRAPPER_MODE)
  IO_AND_CPU_COST wrapper_keyread_time(uint index,
                                       ulong ranges,
                                       ha_rows rows,
                                       ulonglong blocks);
  IO_AND_CPU_COST storage_keyread_time(uint index,
                                       ulong ranges,
                                       ha_rows rows,
                                       ulonglong blocks);
  IO_AND_CPU_COST wrapper_rnd_pos_time(ha_rows rows);
  IO_AND_CPU_COST storage_rnd_pos_time(ha_rows rows);
#endif

#ifdef MRN_HANDLER_HAVE_GET_MEMORY_BUFFER_SIZE
#  ifdef MRN_ENABLE_WRAPPER_MODE
  longlong wrapper_get_memory_buffer_size() const;
#  endif
  longlong storage_get_memory_buffer_size() const;
#endif
#ifdef MRN_HANDLER_HAVE_TABLE_CACHE_TYPE
#  ifdef MRN_ENABLE_WRAPPER_MODE
  uint8 wrapper_table_cache_type();
#  endif
  uint8 storage_table_cache_type();
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  ha_rows wrapper_multi_range_read_info_const(uint keyno,
                                              RANGE_SEQ_IF* seq,
                                              void* seq_init_param,
                                              uint n_ranges,
                                              uint* bufsz,
                                              uint* flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                              ha_rows limit,
#  endif
                                              Cost_estimate* cost);
  ha_rows storage_multi_range_read_info_const(uint keyno,
                                              RANGE_SEQ_IF* seq,
                                              void* seq_init_param,
                                              uint n_ranges,
                                              uint* bufsz,
                                              uint* flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                              ha_rows limit,
#  endif
                                              Cost_estimate* cost);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  ha_rows wrapper_multi_range_read_info(uint keyno,
                                        uint n_ranges,
                                        uint keys,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                        uint key_parts,
#  endif
                                        uint* bufsz,
                                        uint* flags,
                                        Cost_estimate* cost);
#endif
  ha_rows storage_multi_range_read_info(uint keyno,
                                        uint n_ranges,
                                        uint keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                        uint key_parts,
#endif
                                        uint* bufsz,
                                        uint* flags,
                                        Cost_estimate* cost);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_multi_range_read_init(RANGE_SEQ_IF* seq,
                                    void* seq_init_param,
                                    uint n_ranges,
                                    uint mode,
                                    HANDLER_BUFFER* buf);
#endif
  int storage_multi_range_read_init(RANGE_SEQ_IF* seq,
                                    void* seq_init_param,
                                    uint n_ranges,
                                    uint mode,
                                    HANDLER_BUFFER* buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_multi_range_read_next(range_id_t* range_info);
#endif
  int storage_multi_range_read_next(range_id_t* range_info);
  int generic_delete_all_rows(grn_obj* target_grn_table,
                              const char* function_name);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_delete_all_rows();
#endif
  int storage_delete_all_rows();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_truncate(
#  ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
    dd::Table* table_def
#  endif
  );
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_truncate_index();
#endif
  int storage_truncate(
#ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
    dd::Table* table_def
#endif
  );
  int storage_truncate_index();
#ifdef MRN_HANDLER_HAVE_KEYS_TO_USE_FOR_SCANNING
#  ifdef MRN_ENABLE_WRAPPER_MODE
  const key_map* wrapper_keys_to_use_for_scanning();
#  endif
  const key_map* storage_keys_to_use_for_scanning();
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  ha_rows wrapper_estimate_rows_upper_bound();
#endif
  ha_rows storage_estimate_rows_upper_bound();
#ifdef MRN_HANDLER_HAVE_GET_REAL_ROW_TYPE
#  ifdef MRN_ENABLE_WRAPPER_MODE
  enum row_type
  wrapper_get_real_row_type(const HA_CREATE_INFO* create_info) const;
#  endif
  enum row_type
  storage_get_real_row_type(const HA_CREATE_INFO* create_info) const;
#endif
#ifdef MRN_HANDLER_HAVE_GET_DEFAULT_INDEX_ALGORITHM
#  ifdef MRN_ENABLE_WRAPPER_MODE
  enum ha_key_alg wrapper_get_default_index_algorithm() const;
#  endif
  enum ha_key_alg storage_get_default_index_algorithm() const;
#endif
#ifdef MRN_HANDLER_HAVE_IS_INDEX_ALGORITHM_SUPPORTED
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_is_index_algorithm_supported(enum ha_key_alg key_alg) const;
#  endif
  bool storage_is_index_algorithm_supported(enum ha_key_alg key_alg) const;
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_update_create_info(HA_CREATE_INFO* create_info);
#endif
  void storage_update_create_info(HA_CREATE_INFO* create_info);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_rename_table(const char* from,
                           const char* to,
                           MRN_SHARE* tmp_share,
                           const char* from_table_name,
                           const char* to_table_name
#  ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
                           ,
                           const dd::Table* from_table_def,
                           dd::Table* to_table_def
#  endif
  );
  int wrapper_rename_index(const char* from,
                           const char* to,
                           MRN_SHARE* tmp_share,
                           const char* from_table_name,
                           const char* to_table_name);
#endif
  int storage_rename_table(const char* from,
                           const char* to,
                           MRN_SHARE* tmp_share,
                           const char* from_table_name,
                           const char* to_table_name
#ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
                           ,
                           const dd::Table* from_table_def,
                           dd::Table* to_table_def
#endif
  );
  int storage_rename_foreign_key(MRN_SHARE* tmp_share,
                                 const char* from_table_name,
                                 const char* to_table_name);
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_is_crashed() const;
#endif
  bool storage_is_crashed() const;
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_auto_repair(int error) const;
#endif
  bool storage_auto_repair(int error) const;
  int generic_disable_index(int i, KEY* key_info);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_disable_indexes_mroonga(MRN_HANDLER_DISABLE_INDEXES_PARAMETERS);
  int wrapper_disable_indexes(MRN_HANDLER_DISABLE_INDEXES_PARAMETERS);
#endif
  int storage_disable_indexes(MRN_HANDLER_DISABLE_INDEXES_PARAMETERS);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_enable_indexes_mroonga(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS);
  int wrapper_enable_indexes(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS);
#endif
  int storage_enable_indexes(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_check(THD* thd, HA_CHECK_OPT* check_opt);
#endif
  int storage_check(THD* thd, HA_CHECK_OPT* check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_fill_indexes(THD* thd,
                           KEY* key_info,
                           grn_obj** index_columns,
                           uint n_keys);
  int wrapper_recreate_indexes(THD* thd);
#endif
  int storage_recreate_indexes(THD* thd);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_repair(THD* thd, HA_CHECK_OPT* check_opt);
#endif
  int storage_repair(THD* thd, HA_CHECK_OPT* check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_check_and_repair(THD* thd);
#endif
  bool storage_check_and_repair(THD* thd);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_analyze(THD* thd, HA_CHECK_OPT* check_opt);
#endif
  int storage_analyze(THD* thd, HA_CHECK_OPT* check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_optimize(THD* thd, HA_CHECK_OPT* check_opt);
#endif
  int storage_optimize(THD* thd, HA_CHECK_OPT* check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_is_fatal_error(int error_num
#  ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                              ,
                              uint flags
#  endif
  );
#endif
  bool storage_is_fatal_error(int error_num
#ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                              ,
                              uint flags
#endif
  );
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_is_comment_changed(TABLE* table1, TABLE* table2);
  bool wrapper_check_if_incompatible_data(HA_CREATE_INFO* create_info,
                                          uint table_changes);
#endif
  bool storage_check_if_incompatible_data(HA_CREATE_INFO* create_info,
                                          uint table_changes);
  int storage_add_index_multiple_columns(TABLE* target_table,
                                         KEY* key_info,
                                         uint num_of_keys,
                                         grn_obj** index_tables,
                                         grn_obj** index_columns,
                                         bool skip_unique_key);
#ifdef MRN_ENABLE_WRAPPER_MODE
  enum_alter_inplace_result
  wrapper_check_if_supported_inplace_alter(TABLE* altered_table,
                                           Alter_inplace_info* ha_alter_info);
#endif
  enum_alter_inplace_result
  storage_check_if_supported_inplace_alter(TABLE* altered_table,
                                           Alter_inplace_info* ha_alter_info);
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_prepare_inplace_alter_table(TABLE* altered_table,
                                           Alter_inplace_info* ha_alter_info
#  ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                           ,
                                           const dd::Table* old_table_def,
                                           dd::Table* new_table_def
#  endif
  );
#endif
  bool storage_prepare_inplace_alter_table(TABLE* altered_table,
                                           Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                           ,
                                           const dd::Table* old_table_def,
                                           dd::Table* new_table_def
#endif
  );
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_inplace_alter_table(TABLE* altered_table,
                                   Alter_inplace_info* ha_alter_info
#  ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                   ,
                                   const dd::Table* old_table_def,
                                   dd::Table* new_table_def
#  endif
  );
#endif
  bool storage_inplace_alter_table_add_index(TABLE* altered_table,
                                             Alter_inplace_info* ha_alter_info);
  bool
  storage_inplace_alter_table_drop_index(TABLE* altered_table,
                                         Alter_inplace_info* ha_alter_info);
  bool
  storage_inplace_alter_table_add_column(TABLE* altered_table,
                                         Alter_inplace_info* ha_alter_info);
  bool
  storage_inplace_alter_table_drop_column(TABLE* altered_table,
                                          Alter_inplace_info* ha_alter_info);
  bool
  storage_inplace_alter_table_rename_column(TABLE* altered_table,
                                            Alter_inplace_info* ha_alter_info);
  bool storage_inplace_alter_table(TABLE* altered_table,
                                   Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                   ,
                                   const dd::Table* old_table_def,
                                   dd::Table* new_table_def
#endif
  );
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_commit_inplace_alter_table(TABLE* altered_table,
                                          Alter_inplace_info* ha_alter_info,
                                          bool commit
#  ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                          ,
                                          const dd::Table* old_table_def,
                                          dd::Table* new_table_def
#  endif
  );
#endif
  bool storage_commit_inplace_alter_table(TABLE* altered_table,
                                          Alter_inplace_info* ha_alter_info,
                                          bool commit
#ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                          ,
                                          const dd::Table* old_table_def,
                                          dd::Table* new_table_def
#endif
  );
#ifdef MRN_HANDLER_HAVE_NOTIFY_TABLE_CHANGED
#  ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_notify_table_changed(
#    ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
    Alter_inplace_info* ha_alter_info
#    endif
  );
#  endif
  void storage_notify_table_changed(
#  ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
    Alter_inplace_info* ha_alter_info
#  endif
  );
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_update_auto_increment();
#endif
  int storage_update_auto_increment();
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_set_next_insert_id(ulonglong id);
#endif
  void storage_set_next_insert_id(ulonglong id);
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_get_auto_increment(ulonglong offset,
                                  ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong* first_value,
                                  ulonglong* nb_reserved_values);
#endif
  void storage_get_auto_increment(ulonglong offset,
                                  ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong* first_value,
                                  ulonglong* nb_reserved_values);
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_restore_auto_increment(ulonglong prev_insert_id);
#endif
  void storage_restore_auto_increment(ulonglong prev_insert_id);
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_release_auto_increment();
#endif
  void storage_release_auto_increment();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_check_for_upgrade(HA_CHECK_OPT* check_opt);
#endif
  int storage_check_for_upgrade(HA_CHECK_OPT* check_opt);
#ifdef MRN_HANDLER_HAVE_RESET_AUTO_INCREMENT
#  ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_reset_auto_increment(ulonglong value);
#  endif
  int storage_reset_auto_increment(ulonglong value);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_was_semi_consistent_read();
#endif
  bool storage_was_semi_consistent_read();
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_try_semi_consistent_read(bool yes);
#endif
  void storage_try_semi_consistent_read(bool yes);
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_unlock_row();
#endif
  void storage_unlock_row();
#ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_start_stmt(THD* thd, thr_lock_type lock_type);
#endif
  int storage_start_stmt(THD* thd, thr_lock_type lock_type);
#ifdef MRN_HANDLER_HAVE_HAS_GAP_LOCKS
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_has_gap_locks() const MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT;
#  endif
  bool storage_has_gap_locks() const MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT;
#endif
#ifdef MRN_HANDLER_HAVE_PRIMARY_KEY_IS_CLUSTERED
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool
  wrapper_primary_key_is_clustered() MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST;
#  endif
  bool
  storage_primary_key_is_clustered() MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST;
#endif
#ifdef MRN_HANDLER_HAVE_CAN_SWITCH_ENGINES
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_can_switch_engines();
#  endif
  bool storage_can_switch_engines();
#endif
#ifdef MRN_HANDLER_HAVE_FOREIGN_KEY_INFO
#  ifdef MRN_ENABLE_WRAPPER_MODE
  bool wrapper_is_fk_defined_on_table_or_index(uint index);
#  endif
  bool storage_is_fk_defined_on_table_or_index(uint index);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  char* wrapper_get_foreign_key_create_info();
#  endif
  char* storage_get_foreign_key_create_info();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  int wrapper_get_foreign_key_list(THD* thd,
                                   List<FOREIGN_KEY_INFO>* f_key_list);
  int wrapper_get_parent_foreign_key_list(THD* thd,
                                          List<FOREIGN_KEY_INFO>* f_key_list);
#  endif
  int storage_get_foreign_key_list(THD* thd,
                                   List<FOREIGN_KEY_INFO>* f_key_list);
  int storage_get_parent_foreign_key_list(THD* thd,
                                          List<FOREIGN_KEY_INFO>* f_key_list);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  mrn_handler_referenced_by_foreign_key_bool wrapper_referenced_by_foreign_key()
    MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT;
#  endif
  mrn_handler_referenced_by_foreign_key_bool storage_referenced_by_foreign_key()
    MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_free_foreign_key_create_info(char* str);
#  endif
  void storage_free_foreign_key_create_info(char* str);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_init_table_handle_for_HANDLER();
#endif
  void storage_init_table_handle_for_HANDLER();
#ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_set_keys_in_use();
#endif
  void storage_set_keys_in_use();
#ifdef MRN_HANDLER_NEED_OVERRIDE_UNBIND_PSI
#  ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_unbind_psi();
#  endif
  void storage_unbind_psi();
#endif
#ifdef MRN_HANDLER_NEED_OVERRIDE_REBIND_PSI
#  ifdef MRN_ENABLE_WRAPPER_MODE
  void wrapper_rebind_psi();
#  endif
  void storage_rebind_psi();
#endif
};

#ifdef __cplusplus
}
#endif
