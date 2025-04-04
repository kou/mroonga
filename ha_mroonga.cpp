/* -*- c-basic-offset: 2; indent-tabs-mode: nil -*- */
/*
  Copyright (C) 2010 Tetsuro IKEDA
  Copyright (C) 2010-2013 Kentoku SHIBA
  Copyright (C) 2011-2025 Sutou Kouhei <kou@clear-code.com>
  Copyright (C) 2013 Kenji Maruyama <mmmaru777@gmail.com>
  Copyright (C) 2020-2021 Horimoto Yasuhiro <horimoto@clear-code.com>

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

#include "mrn.hpp"
#include "mrn_mysql.h"
#include "mrn_mysql_compat.h"

#ifdef USE_PRAGMA_IMPLEMENTATION
#  pragma implementation
#endif

#include <ft_global.h>
#include <item_sum.h>
#include <key.h>
#include <myisampack.h>
#include <mysql.h>
#include <mysql/plugin.h>
#include <mysqld.h>
#include <spatial.h>
#include <sql_base.h>
#include <sql_select.h>
#include <sql_show.h>
#include <tztime.h>

#ifdef MRN_HAVE_BINLOG_H
#  include <binlog.h>
#endif
#ifdef MRN_HAVE_CREATE_FIELD_H
#  include <create_field.h>
#endif
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
#  include <create_options.h>
#endif
#include <my_byteorder.h>
#ifdef MRN_HAVE_MYSQL_PSI_MYSQL_MEMORY_H
#  include <mysql/psi/mysql_memory.h>
#endif
#ifdef MRN_HAVE_SQL_OPTIMIZER_H
#  include <sql_optimizer.h>
#endif
#include <sql_table.h>
#ifdef MRN_HAVE_SQL_DERROR_H
#  include <derror.h>
#endif
#ifdef MRN_HAVE_SQL_TYPE_GEOM_H
#  include <sql_type_geom.h>
#endif
#ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
#  include <dd/dictionary.h>
#  include <dd/cache/dictionary_client.h>
#endif
#ifdef MRN_HAVE_SRS
#  include <srs_fetcher.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

#ifdef WIN32
#  include <math.h>
#  include <direct.h>
#  define MRN_TABLE_SHARE_LOCK_SHARE_PROC   "?key_TABLE_SHARE_LOCK_share@@3IA"
#  define MRN_TABLE_SHARE_LOCK_HA_DATA_PROC "?key_TABLE_SHARE_LOCK_ha_data@@3IA"
#  ifdef _WIN64
#    define MRN_BINLOG_FILTER_PROC "?binlog_filter@@3PEAVRpl_filter@@EA"
#    define MRN_MY_TZ_UTC_PROC     "?my_tz_UTC@@3PEAVTime_zone@@EA"
#  else
#    define MRN_BINLOG_FILTER_PROC "?binlog_filter@@3PAVRpl_filter@@A"
#    define MRN_MY_TZ_UTC_PROC     "?my_tz_UTC@@3PAVTime_zone@@A"
#  endif
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

#include <cstring>
#include <string>

#include "mrn_err.h"
#include "mrn_table.hpp"
#include <groonga/plugin.h>
#include "ha_mroonga.hpp"
#include <mrn_path_mapper.hpp>
#include <mrn_index_table_name.hpp>
#include <mrn_index_column_name.hpp>
#include <mrn_debug_column_access.hpp>
#include <mrn_auto_increment_value_lock.hpp>
#include <mrn_external_lock.hpp>
#include <mrn_match_escalation_threshold_scope.hpp>
#include <mrn_multiple_column_key_codec.hpp>
#include <mrn_field_normalizer.hpp>
#include <mrn_encoding.hpp>
#include <mrn_parameters_parser.hpp>
#include <mrn_lock.hpp>
#include <mrn_condition_converter.hpp>
#include <mrn_time_converter.hpp>
#include <mrn_smart_grn_obj.hpp>
#include <mrn_database_manager.hpp>
#include <mrn_context_pool.hpp>
#include <mrn_grn.hpp>
#include <mrn_value_decoder.hpp>
#include <mrn_database_repairer.hpp>
#include <mrn_operation.hpp>
#include <mrn_column_name.hpp>
#include <mrn_count_skip_checker.hpp>
#include <mrn_variables.hpp>
#include <mrn_query_parser.hpp>
#include <mrn_smart_bitmap.hpp>
#include <mrn_table_data_switcher.hpp>
#include <mrn_timestamp_field_value_converter.hpp>

// for debug
#define MRN_CLASS_NAME      "ha_mroonga"

#define MRN_SHORT_TEXT_SIZE (1 << 12) //  4Kbytes
#define MRN_TEXT_SIZE       (1 << 16) // 64Kbytes
#define MRN_LONG_TEXT_SIZE  (1 << 31) //  2Gbytes

#ifdef MRN_HAVE_TDC_LOCK_TABLE_SHARE
#  ifdef MRN_TABLE_SHARE_TDC_IS_POINTER
#    define mrn_open_mutex(share) &((share)->tdc->LOCK_table_share)
#  else
#    define mrn_open_mutex(share) &((share)->tdc.LOCK_table_share)
#  endif
#  define mrn_open_mutex_lock(share)                                           \
    do {                                                                       \
      TABLE_SHARE* share_ = share;                                             \
      if (share_ && share_->tmp_table == NO_TMP_TABLE) {                       \
        mysql_mutex_lock(mrn_open_mutex(share_));                              \
      }                                                                        \
    } while (0)
#  define mrn_open_mutex_unlock(share)                                         \
    do {                                                                       \
      TABLE_SHARE* share_ = share;                                             \
      if (share_ && share_->tmp_table == NO_TMP_TABLE) {                       \
        mysql_mutex_unlock(mrn_open_mutex(share_));                            \
      }                                                                        \
    } while (0)
#else
#  ifdef DBUG_OFF
#    ifndef _WIN32
extern mysql_mutex_t LOCK_open;
#    endif
#  endif
static mysql_mutex_t* mrn_LOCK_open;
#  define mrn_open_mutex_lock(share)   mysql_mutex_lock(mrn_LOCK_open)
#  define mrn_open_mutex_unlock(share) mysql_mutex_unlock(mrn_LOCK_open)
#endif

#ifdef MRN_MARIADB_P
#  if MYSQL_VERSION_ID >= 100200
#    define MRN_ORDER_IS_ASC(order) ((order)->direction == ORDER::ORDER_ASC)
#  else
#    define MRN_ORDER_IS_ASC(order) ((order)->asc)
#  endif
#else
#  if MYSQL_VERSION_ID >= 80011
#    define MRN_ORDER_IS_ASC(order) ((order)->direction == ORDER_ASC)
#  else
#    define MRN_ORDER_IS_ASC(order) ((order)->direction == ORDER::ORDER_ASC)
#  endif
#endif

#define MRN_STRINGIFY(macro_or_string)         MRN_STRINGIFY_ARG(macro_or_string)
#define MRN_STRINGIFY_ARG(contents)            #contents

#define MRN_PLUGIN_NAME                        mroonga
#define MRN_PLUGIN_NAME_STRING                 "Mroonga"
#define MRN_STATUS_VARIABLE_NAME_PREFIX_STRING "Mroonga"

#ifdef MRN_MARIADB_P
#  define st_mysql_plugin          st_maria_plugin
#  define mrn_declare_plugin(NAME) maria_declare_plugin(NAME)
#  define mrn_declare_plugin_end   maria_declare_plugin_end
#  define MRN_PLUGIN_LAST_VALUES                                               \
    MRN_VERSION_FULL, MariaDB_PLUGIN_MATURITY_STABLE
#else
#  define mrn_declare_plugin(NAME) mysql_declare_plugin(NAME)
#  define mrn_declare_plugin_end   mysql_declare_plugin_end
#  define MRN_PLUGIN_LAST_VALUES   NULL, 0
#endif

#if MYSQL_VERSION_ID >= 100007 && defined(MRN_MARIADB_P)
#  define MRN_THD_GET_AUTOINC(thd, off, inc) thd_get_autoinc(thd, off, inc)
#else
#  define MRN_THD_GET_AUTOINC(thd, off, inc)                                   \
    {                                                                          \
      *(off) = thd->variables.auto_increment_offset;                           \
      *(inc) = thd->variables.auto_increment_increment;                        \
    }
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  define MRN_LEX_GET_CREATE_INFO(lex) ((lex)->create_info)
#  define MRN_LEX_GET_ALTER_INFO(lex)  ((lex)->alter_info)
#else
#  define MRN_LEX_GET_CREATE_INFO(lex) &((lex)->create_info)
#  define MRN_LEX_GET_ALTER_INFO(lex)  &((lex)->alter_info)
#endif

#ifdef MRN_MARIADB_P
#  define MRN_KEYTYPE_FOREIGN Key::FOREIGN_KEY
#else
#  define MRN_KEYTYPE_FOREIGN KEYTYPE_FOREIGN
#endif

#ifdef MRN_MARIADB_P
#  define mrn_calculate_key_len(table, key_index, buffer, keypart_map)         \
    calculate_key_len(table, key_index, buffer, keypart_map)
#else
#  define mrn_calculate_key_len(table, key_index, buffer, keypart_map)         \
    calculate_key_len(table, key_index, keypart_map)
#endif

#ifdef MRN_MARIADB_P
#  define MRN_TABLE_LIST_DERIVED_QUERY_EXPRESSION(table_list)                  \
    ((table_list)->derived)
#elif MYSQL_VERSION_ID >= 80024
#  define MRN_TABLE_LIST_DERIVED_QUERY_EXPRESSION(table_list)                  \
    ((table_list)->is_derived() ? (table_list)->derived_query_expression()     \
                                : nullptr)
#else
#  define MRN_TABLE_LIST_DERIVED_QUERY_EXPRESSION(table_list)                  \
    ((table_list)->is_derived() ? (table_list)->derived_unit() : NULL)
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  define MRN_TABLE_SET_FOUND_ROW(table) table->set_found_row();
#  define MRN_TABLE_SET_NO_ROW(table)    table->set_no_row();
#else
#  define MRN_TABLE_SET_FOUND_ROW(table) table->status = 0;
#  define MRN_TABLE_SET_NO_ROW(table)    table->status = STATUS_NOT_FOUND;
#endif

#ifdef MRN_MARIADB_P
#  define MRN_GEOMETRY_FREE(geometry) delete (geometry)
#else
#  define MRN_GEOMETRY_FREE(geometry)
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  include <thd_raii.h>
#  define MRN_DISABLE_BINLOG_BEGIN(thd)                                        \
    do {                                                                       \
      Disable_binlog_guard guard(thd);
#  define MRN_DISABLE_BINLOG_END(thd)                                          \
    }                                                                          \
    while (false)
#else
#  define MRN_DISABLE_BINLOG_BEGIN(thd)                                        \
    do {                                                                       \
      tmp_disable_binlog(thd);
#  define MRN_DISABLE_BINLOG_END(thd)                                          \
    reenable_binlog(thd);                                                      \
    }                                                                          \
    while (false)
#endif

#if MYSQL_VERSION_ID >= 100504 && defined(MRN_MARIADB_P)
#  define MRN_COLUMN(name, type_length, type_id, type_object)                  \
    Show::Column((name), (type_object), NOT_NULL)
#  define MRN_COLUMN_END() Show::CEnd()
#else
#  define MRN_COLUMN(name, type_length, type_id, type_object)                  \
    {                                                                          \
      (name), (type_length), (type_id), 0, 0, NULL, SKIP_OPEN_TABLE            \
    }
#  define MRN_COLUMN_END() MRN_COLUMN(NULL, 0, MYSQL_TYPE_LONG, )
#endif

#if !defined(MRN_MARIADB_P) ||                                                 \
  MYSQL_VERSION_ID < 100500 && defined(MRN_MARIADB_P)
#  define MRN_HANDLERTON_HAVE_STATE
#endif

#ifdef MRN_MARIADB_P
#  define MRN_HANDLERTON_HAVE_KILL_QUERY
#  define MRN_HANDLERTON_HAVE_ALTER_TABLE_FLAGS
#else
#  define MRN_HANDLERTON_HAVE_KILL_CONNECTION
#endif

Rpl_filter* mrn_binlog_filter;
Time_zone* mrn_my_tz_UTC;
#ifdef MRN_HAVE_TABLE_DEF_CACHE
mrn_table_def_cache_type* mrn_table_def_cache;
#endif

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
#  define PSI_INFO_ENTRY(key, name, flags, volatility, documentation)          \
    {                                                                          \
      key, name, flags, volatility, documentation                              \
    }
#else
#  define PSI_INFO_ENTRY(key, name, flags, volatility, documentation)          \
    {                                                                          \
      key, name, flags                                                         \
    }
#  define PSI_FLAG_SINGLETON PSI_FLAG_GLOBAL
#endif

PSI_memory_key mrn_memory_key;

#ifdef HAVE_PSI_MEMORY_INTERFACE
static PSI_memory_info mrn_all_memory_keys[] = {
  PSI_INFO_ENTRY(
    &mrn_memory_key, "Mroonga", 0, PSI_VOLATILITY_UNKNOWN, PSI_DOCUMENT_ME),
};
#endif

static const char* INDEX_COLUMN_NAME = "index";
static const char* MRN_PLUGIN_AUTHOR = "The Mroonga project";

#ifdef HAVE_PSI_INTERFACE
#  ifdef WIN32
#    ifdef MRN_TABLE_SHARE_HAVE_LOCK_SHARE
PSI_mutex_key* mrn_table_share_lock_share;
#    endif
PSI_mutex_key* mrn_table_share_lock_ha_data;
#  endif
static PSI_mutex_key mrn_open_tables_mutex_key;
static PSI_mutex_key mrn_long_term_shares_mutex_key;
static PSI_mutex_key mrn_allocated_thds_mutex_key;
PSI_mutex_key mrn_share_mutex_key;
PSI_mutex_key mrn_long_term_share_auto_inc_mutex_key;
static PSI_mutex_key mrn_db_manager_mutex_key;
static PSI_mutex_key mrn_context_pool_mutex_key;
static PSI_mutex_key mrn_operations_mutex_key;

#  if (!defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 80011)
#    define MRN_MUTEX_INFO_ENTRY(key, name, flags, volatility, document)       \
      {                                                                        \
        key, name, flags, volatility, document                                 \
      }
#  else
#    define MRN_MUTEX_INFO_ENTRY(key, name, flags, volatility, document)       \
      {                                                                        \
        key, name, flags                                                       \
      }
#  endif

static PSI_mutex_info mrn_mutexes[] = {
  MRN_MUTEX_INFO_ENTRY(&mrn_open_tables_mutex_key,
                       "mrn::open_tables",
                       PSI_FLAG_SINGLETON,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME),
  MRN_MUTEX_INFO_ENTRY(&mrn_long_term_shares_mutex_key,
                       "mrn::long_term_shares",
                       PSI_FLAG_SINGLETON,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME),
  MRN_MUTEX_INFO_ENTRY(&mrn_allocated_thds_mutex_key,
                       "mrn::allocated_thds",
                       PSI_FLAG_SINGLETON,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME),
  MRN_MUTEX_INFO_ENTRY(&mrn_share_mutex_key,
                       "mrn::share",
                       0,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME),
  MRN_MUTEX_INFO_ENTRY(&mrn_long_term_share_auto_inc_mutex_key,
                       "mrn::long_term_share::auto_inc",
                       0,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME),
  MRN_MUTEX_INFO_ENTRY(&mrn_db_manager_mutex_key,
                       "mrn::DatabaseManager",
                       PSI_FLAG_SINGLETON,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME),
  MRN_MUTEX_INFO_ENTRY(&mrn_context_pool_mutex_key,
                       "mrn::ContextPool",
                       PSI_FLAG_SINGLETON,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME),
  MRN_MUTEX_INFO_ENTRY(&mrn_operations_mutex_key,
                       "mrn::Operations",
                       PSI_FLAG_SINGLETON,
                       PSI_VOLATILITY_UNKNOWN,
                       PSI_DOCUMENT_ME)};

#  if !defined(MRN_MARIADB_P)
#    define MRN_REGISTER_MUTEXES(category, mutexes)                            \
      do {                                                                     \
        int n_mutexes = array_elements(mutexes);                               \
        mysql_mutex_register(category, mutexes, n_mutexes);                    \
      } while (false)
#  elif defined(MRN_HAVE_PSI_SERVER)
#    define MRN_REGISTER_MUTEXES(category, mutexes)                            \
      do {                                                                     \
        if (PSI_server) {                                                      \
          int n_mutexes = array_elements(mutexes);                             \
          PSI_server->register_mutex(category, mutexes, n_mutexes);            \
        }                                                                      \
      } while (false)
#  endif
#else
#  define MRN_REGISTER_MUTEXES(category, mutexes)
#endif

/* global variables */
bool mrn_initialized = false;
grn_ctx mrn_ctx;
handlerton* mrn_hton_ptr;
grn_hash* mrn_open_tables;
mysql_mutex_t mrn_open_tables_mutex;
grn_hash* mrn_long_term_shares;
mysql_mutex_t mrn_long_term_shares_mutex;
grn_hash* mrn_allocated_thds;
mysql_mutex_t mrn_allocated_thds_mutex;

/* internal variables */
static grn_obj* mrn_db;
static grn_ctx mrn_db_manager_ctx;
static mysql_mutex_t mrn_db_manager_mutex;
mrn::DatabaseManager* mrn_db_manager = NULL;
static mysql_mutex_t mrn_context_pool_mutex;
mrn::ContextPool* mrn_context_pool = NULL;
static mysql_mutex_t mrn_operations_mutex;

#ifdef MRN_MARIADB_P
static inline my_ptrdiff_t mrn_compute_ptr_diff_for_key(const uchar* data,
                                                        const uchar* base)
{
  return 0;
}
#else
static inline my_ptrdiff_t mrn_compute_ptr_diff_for_key(const uchar* data,
                                                        const uchar* base)
{
  return data - base;
}
#endif

#ifdef WIN32
static inline double round(double x) { return (floor(x + 0.5)); }
#endif

static void mrn_init_encoding_map() { mrn::encoding::init(); }

static int mrn_change_encoding(grn_ctx* ctx, const CHARSET_INFO* charset)
{
  return mrn::encoding::set(ctx, charset);
}

#if !defined(DBUG_OFF) && !defined(_lint)
static const char* mrn_inspect_extra_function(enum ha_extra_function operation)
{
  const char* inspected = "<unknown>";
  switch (operation) {
  case HA_EXTRA_NORMAL:
    inspected = "HA_EXTRA_NORMAL";
    break;
  case HA_EXTRA_QUICK:
    inspected = "HA_EXTRA_QUICK";
    break;
  case HA_EXTRA_NOT_USED:
    inspected = "HA_EXTRA_NOT_USED";
    break;
#  ifdef MRN_HAVE_HA_EXTRA_CACHE
  case HA_EXTRA_CACHE:
    inspected = "HA_EXTRA_CACHE";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_NO_CACHE
  case HA_EXTRA_NO_CACHE:
    inspected = "HA_EXTRA_NO_CACHE";
    break;
#  endif
  case HA_EXTRA_NO_READCHECK:
    inspected = "HA_EXTRA_NO_READCHECK";
    break;
  case HA_EXTRA_READCHECK:
    inspected = "HA_EXTRA_READCHECK";
    break;
  case HA_EXTRA_KEYREAD:
    inspected = "HA_EXTRA_KEYREAD";
    break;
  case HA_EXTRA_NO_KEYREAD:
    inspected = "HA_EXTRA_NO_KEYREAD";
    break;
  case HA_EXTRA_NO_USER_CHANGE:
    inspected = "HA_EXTRA_NO_USER_CHANGE";
    break;
#  ifdef MRN_HAVE_HA_EXTRA_KEY_CACHE
  case HA_EXTRA_KEY_CACHE:
    inspected = "HA_EXTRA_KEY_CACHE";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_NO_KEY_CACHE
  case HA_EXTRA_NO_KEY_CACHE:
    inspected = "HA_EXTRA_NO_KEY_CACHE";
    break;
#  endif
  case HA_EXTRA_WAIT_LOCK:
    inspected = "HA_EXTRA_WAIT_LOCK";
    break;
  case HA_EXTRA_NO_WAIT_LOCK:
    inspected = "HA_EXTRA_NO_WAIT_LOCK";
    break;
#  ifdef MRN_HAVE_HA_EXTRA_WRITE_CACHE
  case HA_EXTRA_WRITE_CACHE:
    inspected = "HA_EXTRA_WRITE_CACHE";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_FLUSH_CACHE
  case HA_EXTRA_FLUSH_CACHE:
    inspected = "HA_EXTRA_FLUSH_CACHE";
    break;
#  endif
  case HA_EXTRA_NO_KEYS:
    inspected = "HA_EXTRA_NO_KEYS";
    break;
  case HA_EXTRA_KEYREAD_CHANGE_POS:
    inspected = "HA_EXTRA_KEYREAD_CHANGE_POS";
    break;
  case HA_EXTRA_REMEMBER_POS:
    inspected = "HA_EXTRA_REMEMBER_POS";
    break;
  case HA_EXTRA_RESTORE_POS:
    inspected = "HA_EXTRA_RESTORE_POS";
    break;
#  ifdef MRN_HAVE_HA_EXTRA_REINIT_CACHE
  case HA_EXTRA_REINIT_CACHE:
    inspected = "HA_EXTRA_REINIT_CACHE";
    break;
#  endif
  case HA_EXTRA_FORCE_REOPEN:
    inspected = "HA_EXTRA_FORCE_REOPEN";
    break;
  case HA_EXTRA_FLUSH:
    inspected = "HA_EXTRA_FLUSH";
    break;
  case HA_EXTRA_NO_ROWS:
    inspected = "HA_EXTRA_NO_ROWS";
    break;
  case HA_EXTRA_RESET_STATE:
    inspected = "HA_EXTRA_RESET_STATE";
    break;
  case HA_EXTRA_IGNORE_DUP_KEY:
    inspected = "HA_EXTRA_IGNORE_DUP_KEY";
    break;
  case HA_EXTRA_NO_IGNORE_DUP_KEY:
    inspected = "HA_EXTRA_NO_IGNORE_DUP_KEY";
    break;
  case HA_EXTRA_PREPARE_FOR_DROP:
    inspected = "HA_EXTRA_PREPARE_FOR_DROP";
    break;
  case HA_EXTRA_PREPARE_FOR_UPDATE:
    inspected = "HA_EXTRA_PREPARE_FOR_UPDATE";
    break;
  case HA_EXTRA_PRELOAD_BUFFER_SIZE:
    inspected = "HA_EXTRA_PRELOAD_BUFFER_SIZE";
    break;
  case HA_EXTRA_CHANGE_KEY_TO_UNIQUE:
    inspected = "HA_EXTRA_CHANGE_KEY_TO_UNIQUE";
    break;
  case HA_EXTRA_CHANGE_KEY_TO_DUP:
    inspected = "HA_EXTRA_CHANGE_KEY_TO_DUP";
    break;
  case HA_EXTRA_KEYREAD_PRESERVE_FIELDS:
    inspected = "HA_EXTRA_KEYREAD_PRESERVE_FIELDS";
    break;
#  ifdef MRN_HAVE_HA_EXTRA_MMAP
  case HA_EXTRA_MMAP:
    inspected = "HA_EXTRA_MMAP";
    break;
#  endif
  case HA_EXTRA_IGNORE_NO_KEY:
    inspected = "HA_EXTRA_IGNORE_NO_KEY";
    break;
  case HA_EXTRA_NO_IGNORE_NO_KEY:
    inspected = "HA_EXTRA_NO_IGNORE_NO_KEY";
    break;
  case HA_EXTRA_MARK_AS_LOG_TABLE:
    inspected = "HA_EXTRA_MARK_AS_LOG_TABLE";
    break;
  case HA_EXTRA_WRITE_CAN_REPLACE:
    inspected = "HA_EXTRA_WRITE_CAN_REPLACE";
    break;
  case HA_EXTRA_WRITE_CANNOT_REPLACE:
    inspected = "HA_EXTRA_WRITE_CANNOT_REPLACE";
    break;
  case HA_EXTRA_DELETE_CANNOT_BATCH:
    inspected = "HA_EXTRA_DELETE_CANNOT_BATCH";
    break;
  case HA_EXTRA_UPDATE_CANNOT_BATCH:
    inspected = "HA_EXTRA_UPDATE_CANNOT_BATCH";
    break;
  case HA_EXTRA_INSERT_WITH_UPDATE:
    inspected = "HA_EXTRA_INSERT_WITH_UPDATE";
    break;
  case HA_EXTRA_PREPARE_FOR_RENAME:
    inspected = "HA_EXTRA_PREPARE_FOR_RENAME";
    break;
  case HA_EXTRA_ADD_CHILDREN_LIST:
    inspected = "HA_EXTRA_ADD_CHILDREN_LIST";
    break;
  case HA_EXTRA_ATTACH_CHILDREN:
    inspected = "HA_EXTRA_ATTACH_CHILDREN";
    break;
  case HA_EXTRA_IS_ATTACHED_CHILDREN:
    inspected = "HA_EXTRA_IS_ATTACHED_CHILDREN";
    break;
  case HA_EXTRA_DETACH_CHILDREN:
    inspected = "HA_EXTRA_DETACH_CHILDREN";
    break;
#  ifdef MRN_HAVE_HA_EXTRA_EXPORT
  case HA_EXTRA_EXPORT:
    inspected = "HA_EXTRA_EXPORT";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_SECONDARY_SORT_ROWID
  case HA_EXTRA_SECONDARY_SORT_ROWID:
    inspected = "HA_EXTRA_SECONDARY_SORT_ROWID";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_DETACH_CHILD
  case HA_EXTRA_DETACH_CHILD:
    inspected = "HA_EXTRA_DETACH_CHILD";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_PREPARE_FOR_FORCED_CLOSE
  case HA_EXTRA_PREPARE_FOR_FORCED_CLOSE:
    inspected = "HA_EXTRA_PREPARE_FOR_FORCED_CLOSE";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_NO_READ_LOCKING
  case HA_EXTRA_NO_READ_LOCKING:
    inspected = "HA_EXTRA_NO_READ_LOCKING";
    break;
#  endif
  case HA_EXTRA_BEGIN_ALTER_COPY:
    inspected = "HA_EXTRA_BEGIN_ALTER_COPY";
    break;
  case HA_EXTRA_END_ALTER_COPY:
    inspected = "HA_EXTRA_END_ALTER_COPY";
    break;
#  ifdef MRN_HAVE_HA_EXTRA_NO_AUTOINC_LOCKING
  case HA_EXTRA_NO_AUTOINC_LOCKING:
    inspected = "HA_EXTRA_NO_AUTOINC_LOCKING";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_PREPARE_FOR_ALTER_TABLE
  case HA_EXTRA_PREPARE_FOR_ALTER_TABLE:
    inspected = "HA_EXTRA_PREPARE_FOR_ALTER_TABLE";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_STARTING_ORDERED_INDEX_SCAN
  case HA_EXTRA_STARTING_ORDERED_INDEX_SCAN:
    inspected = "HA_EXTRA_STARTING_ORDERED_INDEX_SCAN";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER
  case HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER:
    inspected = "HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER
  case HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER:
    inspected = "HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER";
    break;
#  endif
#  ifdef MRN_HAVE_HA_EXTRA_IGNORE_INSERT
  case HA_EXTRA_IGNORE_INSERT:
    inspected = "HA_EXTRA_IGNORE_INSERT";
    break;
#  endif
  }
  return inspected;
}
#endif

/* status */
static long mrn_count_skip = 0;
static long mrn_fast_order_limit = 0;
static long mrn_condition_push_down = 0;
static long mrn_n_pooling_contexts = 0;

/* logging */
static char* mrn_log_file_path = NULL;
static grn_log_level mrn_log_level_default = GRN_LOG_DEFAULT_LEVEL;
static ulong mrn_log_level = mrn_log_level_default;
static char* mrn_query_log_file_path = NULL;

char* mrn_default_tokenizer = NULL;
char* mrn_default_wrapper_engine = NULL;
static int mrn_lock_timeout = grn_get_lock_timeout();
static int mrn_n_workers = grn_get_default_n_workers();
static char* mrn_libgroonga_version = const_cast<char*>(grn_get_version());
static char* mrn_version = const_cast<char*>(MRN_VERSION_FULL);
static char* mrn_vector_column_delimiter = NULL;
static mrn_bool mrn_libgroonga_support_zlib = false;
static mrn_bool mrn_libgroonga_support_lz4 = false;
static mrn_bool mrn_libgroonga_support_zstd = false;
static mrn_bool mrn_libgroonga_support_mecab = false;
static mrn_bool mrn_enable_operations_recording = false;
static const char* mrn_boolean_mode_sytnax_flag_names[] = {
  "DEFAULT",
  "SYNTAX_QUERY",
  "SYNTAX_SCRIPT",
  "ALLOW_COLUMN",
  "ALLOW_UPDATE",
  "ALLOW_LEADING_NOT",
  "QUERY_NO_SYNTAX_ERROR",
  NullS};
static TYPELIB mrn_boolean_mode_syntax_flags_typelib = {
  array_elements(mrn_boolean_mode_sytnax_flag_names) - 1,
  "",
  mrn_boolean_mode_sytnax_flag_names,
  NULL};
#ifdef MRN_GROONGA_EMBEDDED
static mrn_bool mrn_libgroonga_embedded = true;
#else
static mrn_bool mrn_libgroonga_embedded = false;
#endif
static mrn_bool mrn_enable_back_trace = true;
static mrn_bool mrn_enable_reference_count = false;

static mrn::variables::ActionOnError
  mrn_action_on_fulltext_query_error_default =
    mrn::variables::ACTION_ON_ERROR_ERROR_AND_LOG;

/* system functions */

static struct st_mysql_storage_engine storage_engine_structure = {
  MYSQL_HANDLERTON_INTERFACE_VERSION};

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
typedef SHOW_VAR mrn_show_var;
#else
typedef struct st_mysql_show_var mrn_show_var;
#endif

static int mrn_show_memory_map_size(THD* thd, mrn_show_var* var, char* buff)
{
  static size_t memory_map_size = 0;
#if GRN_VERSION_OR_LATER(12, 0, 4)
  memory_map_size = grn_get_memory_map_size();
#endif
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char*>(&memory_map_size);
  return 0;
}

#ifdef MRN_MARIADB_P
#  define MRN_STATUS_VARIABLE_ENTRY(name, value, type, scope)                  \
    {                                                                          \
      name, value, type                                                        \
    }
#else
#  define MRN_STATUS_VARIABLE_ENTRY(name, value, type, scope)                  \
    {                                                                          \
      name, value, type, scope                                                 \
    }
#endif

static mrn_show_var mrn_status_variables[] = {
  MRN_STATUS_VARIABLE_ENTRY(MRN_STATUS_VARIABLE_NAME_PREFIX_STRING
                            "_count_skip",
                            (char*)&mrn_count_skip,
                            SHOW_LONG,
                            SHOW_SCOPE_GLOBAL),
  MRN_STATUS_VARIABLE_ENTRY(MRN_STATUS_VARIABLE_NAME_PREFIX_STRING
                            "_fast_order_limit",
                            (char*)&mrn_fast_order_limit,
                            SHOW_LONG,
                            SHOW_SCOPE_GLOBAL),
  MRN_STATUS_VARIABLE_ENTRY(MRN_STATUS_VARIABLE_NAME_PREFIX_STRING
                            "_condition_push_down",
                            (char*)&mrn_condition_push_down,
                            SHOW_LONG,
                            SHOW_SCOPE_GLOBAL),
  MRN_STATUS_VARIABLE_ENTRY(MRN_STATUS_VARIABLE_NAME_PREFIX_STRING
                            "_n_pooling_contexts",
                            (char*)&mrn_n_pooling_contexts,
                            SHOW_LONG_NOFLUSH,
                            SHOW_SCOPE_GLOBAL),
  MRN_STATUS_VARIABLE_ENTRY(MRN_STATUS_VARIABLE_NAME_PREFIX_STRING
                            "_memory_map_size",
                            (char*)&mrn_show_memory_map_size,
                            SHOW_FUNC,
                            SHOW_SCOPE_GLOBAL),
  MRN_STATUS_VARIABLE_ENTRY(NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL)};

static const char* mrn_log_level_type_names[] = {"NONE",
                                                 "EMERG",
                                                 "ALERT",
                                                 "CRIT",
                                                 "ERROR",
                                                 "WARNING",
                                                 "NOTICE",
                                                 "INFO",
                                                 "DEBUG",
                                                 "DUMP",
                                                 NullS};
static TYPELIB mrn_log_level_typelib = {
  array_elements(mrn_log_level_type_names) - 1,
  "mrn_log_level_typelib",
  mrn_log_level_type_names,
  NULL};

#if MYSQL_VERSION_ID >= 80011 && !defined(MRN_MARIADB_P)
typedef SYS_VAR mrn_sys_var;
#else
typedef struct st_mysql_sys_var mrn_sys_var;
#endif

static void mrn_log_level_update(THD* thd,
                                 mrn_sys_var* var,
                                 void* var_ptr,
                                 const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulong new_value = *static_cast<const ulong*>(save);
  ulong old_value = mrn_log_level;
  mrn_log_level = new_value;
  grn_default_logger_set_max_level(static_cast<grn_log_level>(mrn_log_level));
  grn_ctx* ctx = grn_ctx_open(0);
  mrn_change_encoding(ctx, system_charset_info);
  GRN_LOG(ctx,
          GRN_LOG_NOTICE,
          "log level changed from '%s' to '%s'",
          mrn_log_level_type_names[old_value],
          mrn_log_level_type_names[new_value]);
  grn_ctx_fin(ctx);
  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_ENUM(log_level,
                         mrn_log_level,
                         PLUGIN_VAR_RQCMDARG,
                         "logging level",
                         NULL,
                         mrn_log_level_update,
                         static_cast<ulong>(mrn_log_level),
                         &mrn_log_level_typelib);

static void
mrn_log_file_update(THD* thd, mrn_sys_var* var, void* var_ptr, const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const char* new_value = *((const char**)save);
  char** old_value_ptr = (char**)var_ptr;

  grn_ctx* ctx = &mrn_ctx;
  mrn_change_encoding(ctx, system_charset_info);

  const char* new_log_file_name;
  new_log_file_name = *old_value_ptr;

  if (strcmp(*old_value_ptr, new_value) == 0) {
    GRN_LOG(ctx,
            GRN_LOG_NOTICE,
            "log file isn't changed "
            "because the requested path isn't different: <%s>",
            new_value);
  } else {
    GRN_LOG(ctx,
            GRN_LOG_NOTICE,
            "log file is changed: <%s> -> <%s>",
            *old_value_ptr,
            new_value);
    grn_default_logger_set_path(new_value);
    grn_logger_reopen(ctx);
    GRN_LOG(ctx,
            GRN_LOG_NOTICE,
            "log file is changed: <%s> -> <%s>",
            *old_value_ptr,
            new_value);
    new_log_file_name = new_value;
  }

#ifdef MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
  char* old_log_file_name = *old_value_ptr;
  *old_value_ptr = mrn_my_strdup(new_log_file_name, MYF(MY_WME));
  my_free(old_log_file_name);
#else
  *old_value_ptr = mrn_my_strdup(new_log_file_name, MYF(MY_WME));
#endif

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_STR(log_file,
                        mrn_log_file_path,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "log file for " MRN_PLUGIN_NAME_STRING,
                        NULL,
                        mrn_log_file_update,
                        MRN_LOG_FILE_PATH);

static void mrn_query_log_file_update(THD* thd,
                                      mrn_sys_var* var,
                                      void* var_ptr,
                                      const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const char* new_value = *((const char**)save);
  char** old_value_ptr = (char**)var_ptr;
  const char* normalized_new_value = NULL;

  grn_ctx* ctx = &mrn_ctx;
  mrn_change_encoding(ctx, system_charset_info);

  const char* new_query_log_file_name;
  new_query_log_file_name = *old_value_ptr;

  bool need_update = false;
  if (!*old_value_ptr) {
    if (new_value && new_value[0] != '\0') {
      GRN_LOG(ctx, GRN_LOG_NOTICE, "query log is enabled: <%s>", new_value);
      need_update = true;
      normalized_new_value = new_value;
    } else {
      GRN_LOG(ctx, GRN_LOG_NOTICE, "query log file is still disabled");
    }
  } else {
    if (!new_value || new_value[0] == '\0') {
      GRN_LOG(ctx,
              GRN_LOG_NOTICE,
              "query log file is disabled: <%s>",
              *old_value_ptr);
      need_update = true;
      normalized_new_value = NULL;
    } else if (strcmp(*old_value_ptr, new_value) == 0) {
      GRN_LOG(ctx,
              GRN_LOG_NOTICE,
              "query log file isn't changed "
              "because the requested path isn't different: <%s>",
              new_value);
    } else {
      GRN_LOG(ctx,
              GRN_LOG_NOTICE,
              "query log file is changed: <%s> -> <%s>",
              *old_value_ptr,
              new_value);
      need_update = true;
      normalized_new_value = new_value;
    }
  }

  if (need_update) {
    grn_default_query_logger_set_path(normalized_new_value);
    grn_query_logger_reopen(ctx);
    new_query_log_file_name = normalized_new_value;
  }

#ifdef MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
  char* old_query_log_file_name = *old_value_ptr;
#endif
  if (new_query_log_file_name) {
    *old_value_ptr = mrn_my_strdup(new_query_log_file_name, MYF(0));
  } else {
    *old_value_ptr = NULL;
  }
#ifdef MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
  my_free(old_query_log_file_name);
#endif

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_STR(query_log_file,
                        mrn_query_log_file_path,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "query log file for " MRN_PLUGIN_NAME_STRING,
                        NULL,
                        mrn_query_log_file_update,
                        NULL);

static void mrn_default_tokenizer_update(THD* thd,
                                         mrn_sys_var* var,
                                         void* var_ptr,
                                         const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const char* new_value = *((const char**)save);
  char** old_value_ptr = (char**)var_ptr;
  grn_ctx* ctx = &mrn_ctx;

  mrn_change_encoding(ctx, system_charset_info);
  if (strcmp(*old_value_ptr, new_value) == 0) {
    GRN_LOG(ctx,
            GRN_LOG_NOTICE,
            "default tokenizer for fulltext index isn't changed "
            "because the requested default tokenizer isn't different: <%s>",
            new_value);
  } else {
    GRN_LOG(ctx,
            GRN_LOG_NOTICE,
            "default tokenizer for fulltext index is changed: <%s> -> <%s>",
            *old_value_ptr,
            new_value);
  }

#ifdef MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
  my_free(*old_value_ptr);
  *old_value_ptr = mrn_my_strdup(new_value, MYF(MY_WME));
#else
  *old_value_ptr = (char*)new_value;
#endif

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_STR(default_parser,
                        mrn_default_tokenizer,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "default fulltext parser "
                        "(Deprecated. Use mroonga_default_tokenizer instead.)",
                        NULL,
                        mrn_default_tokenizer_update,
                        MRN_DEFAULT_TOKENIZER);

static MYSQL_SYSVAR_STR(default_tokenizer,
                        mrn_default_tokenizer,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "default tokenizer for fulltext index",
                        NULL,
                        mrn_default_tokenizer_update,
                        MRN_DEFAULT_TOKENIZER);

static MYSQL_THDVAR_BOOL(
  dry_write,                                                /* name */
  PLUGIN_VAR_OPCMDARG,                                      /* options */
  "If dry_write is true, any write operations are ignored", /* comment */
  NULL,                                                     /* check */
  NULL,                                                     /* update */
  false                                                     /* default */
);

static MYSQL_THDVAR_BOOL(enable_optimization, /* name */
                         PLUGIN_VAR_OPCMDARG, /* options */
                         "If enable_optimization is true, some optimizations "
                         "will be applied", /* comment */
                         NULL,              /* check */
                         NULL,              /* update */
                         true               /* default */
);

static MYSQL_THDVAR_LONGLONG(
  match_escalation_threshold,
  PLUGIN_VAR_RQCMDARG,
  "The threshold to determin whether search method is escalated",
  NULL,
  NULL,
  grn_get_default_match_escalation_threshold(),
  -1,
  INT_MAX64,
  0);

static void mrn_vector_column_delimiter_update(THD* thd,
                                               mrn_sys_var* var,
                                               void* var_ptr,
                                               const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const char* new_value = *((const char**)save);
  char** old_value_ptr = (char**)var_ptr;

#ifdef MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
  my_free(*old_value_ptr);
  *old_value_ptr = mrn_my_strdup(new_value, MYF(MY_WME));
#else
  *old_value_ptr = (char*)new_value;
#endif

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_STR(vector_column_delimiter,
                        mrn_vector_column_delimiter,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "The vector column delimiter",
                        NULL,
                        &mrn_vector_column_delimiter_update,
                        " ");

static void mrn_database_path_prefix_update(THD* thd,
                                            mrn_sys_var* var,
                                            void* var_ptr,
                                            const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const char* new_value = *((const char**)save);
  char** old_value_ptr = (char**)var_ptr;
#ifdef MRN_NEED_FREE_STRING_MEMALLOC_PLUGIN_VAR
  if (*old_value_ptr)
    my_free(*old_value_ptr);
  if (new_value)
    *old_value_ptr = mrn_my_strdup(new_value, MYF(MY_WME));
  else
    *old_value_ptr = NULL;
#else
  *old_value_ptr = (char*)new_value;
#endif
  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_STR(database_path_prefix,
                        mrn::PathMapper::default_path_prefix,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "The database path prefix",
                        NULL,
                        &mrn_database_path_prefix_update,
                        NULL);

static MYSQL_SYSVAR_STR(default_wrapper_engine,
                        mrn_default_wrapper_engine,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "The default engine for wrapper mode",
                        NULL,
                        NULL,
                        NULL);

static const char* mrn_action_on_error_names[] = {
  "ERROR",
  "ERROR_AND_LOG",
  "IGNORE",
  "IGNORE_AND_LOG",
  NullS,
};

static TYPELIB mrn_action_on_error_typelib = {
  array_elements(mrn_action_on_error_names) - 1,
  "mrn_action_on_error_typelib",
  mrn_action_on_error_names,
  NULL};

static MYSQL_THDVAR_ENUM(action_on_fulltext_query_error,
                         PLUGIN_VAR_RQCMDARG,
                         "action on fulltext query error",
                         NULL,
                         NULL,
                         mrn_action_on_fulltext_query_error_default,
                         &mrn_action_on_error_typelib);

static void mrn_lock_timeout_update(THD* thd,
                                    mrn_sys_var* var,
                                    void* var_ptr,
                                    const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const int new_value = *static_cast<const int*>(save);
  int* old_value_ptr = static_cast<int*>(var_ptr);

  *old_value_ptr = new_value;
  grn_set_lock_timeout(new_value);

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_INT(lock_timeout,
                        mrn_lock_timeout,
                        PLUGIN_VAR_RQCMDARG,
                        "lock timeout used in Groonga",
                        NULL,
                        mrn_lock_timeout_update,
                        grn_get_lock_timeout(),
                        -1,
                        INT_MAX,
                        1);

static void mrn_n_workers_update(THD* thd,
                                 mrn_sys_var* var,
                                 void* var_ptr,
                                 const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const int new_value = *static_cast<const int*>(save);
  int* old_value_ptr = static_cast<int*>(var_ptr);

  *old_value_ptr = new_value;
  grn_set_default_n_workers(new_value);
  mrn_context_pool->set_n_workers(new_value);

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_INT(n_workers,
                        mrn_n_workers,
                        PLUGIN_VAR_RQCMDARG,
                        "Number of workers in Groonga. "
                        "0: The default. It disables parallel workers. "
                        "-1: Use the all CPUs in the environment. "
                        "1 or larger: Use the specified number of CPUs",
                        nullptr,
                        mrn_n_workers_update,
                        grn_get_default_n_workers(),
                        -1,
                        INT_MAX,
                        0);

static MYSQL_SYSVAR_STR(libgroonga_version,
                        mrn_libgroonga_version,
                        PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_READONLY,
                        "The version of libgroonga",
                        NULL,
                        NULL,
                        grn_get_version());

static MYSQL_SYSVAR_STR(version,
                        mrn_version,
                        PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_READONLY,
                        "The version of Mroonga",
                        NULL,
                        NULL,
                        MRN_VERSION_FULL);

static MYSQL_SYSVAR_BOOL(libgroonga_support_zlib,
                         mrn_libgroonga_support_zlib,
                         PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
                         "The status of libgroonga supports zlib",
                         NULL,
                         NULL,
                         mrn_libgroonga_support_zlib);

static MYSQL_SYSVAR_BOOL(libgroonga_support_lz4,
                         mrn_libgroonga_support_lz4,
                         PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
                         "The status of libgroonga supports LZ4",
                         NULL,
                         NULL,
                         mrn_libgroonga_support_lz4);

static MYSQL_SYSVAR_BOOL(libgroonga_support_zstd,
                         mrn_libgroonga_support_zstd,
                         PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
                         "The status of libgroonga supports Zstandard",
                         NULL,
                         NULL,
                         mrn_libgroonga_support_zstd);

static MYSQL_SYSVAR_BOOL(libgroonga_support_mecab,
                         mrn_libgroonga_support_mecab,
                         PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
                         "The status of libgroonga supports MeCab",
                         NULL,
                         NULL,
                         mrn_libgroonga_support_mecab);

static void mrn_enable_operations_recording_update(THD* thd,
                                                   mrn_sys_var* var,
                                                   void* var_ptr,
                                                   const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const bool new_value = *static_cast<const bool*>(save);
  bool* old_value_ptr = static_cast<bool*>(var_ptr);

  *old_value_ptr = new_value;

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_BOOL(
  enable_operations_recording,
  mrn_enable_operations_recording,
  PLUGIN_VAR_RQCMDARG,
  "Whether recording operations for recovery is enabled or not",
  NULL,
  mrn_enable_operations_recording_update,
  true);

static MYSQL_THDVAR_SET(boolean_mode_syntax_flags,
                        PLUGIN_VAR_RQCMDARG,
                        "The flags to custom syntax in BOOLEAN MODE. "
                        "Available flags: "
                        "DEFAULT(=SYNTAX_QUERY,ALLOW_LEADING_NOT), "
                        "SYNTAX_QUERY, SYNTAX_SCRIPT, "
                        "ALLOW_COLUMN, ALLOW_UPDATE, "
                        "ALLOW_LEADING_NOT and QUERY_NO_SYNTAX_ERROR",
                        NULL,
                        NULL,
                        mrn::variables::BOOLEAN_MODE_SYNTAX_FLAG_DEFAULT,
                        &mrn_boolean_mode_syntax_flags_typelib);

static const int MRN_MAX_N_RECORDS_FOR_ESTIMATE_DEFAULT = 1000;

static MYSQL_THDVAR_INT(max_n_records_for_estimate,
                        PLUGIN_VAR_RQCMDARG,
                        "The max number of records to "
                        "estimate the number of matched records",
                        NULL,
                        NULL,
                        MRN_MAX_N_RECORDS_FOR_ESTIMATE_DEFAULT,
                        -1,
                        INT_MAX,
                        0);

static MYSQL_SYSVAR_BOOL(libgroonga_embedded,
                         mrn_libgroonga_embedded,
                         PLUGIN_VAR_NOCMDARG | PLUGIN_VAR_READONLY,
                         "Whether libgroonga is embedded or not",
                         NULL,
                         NULL,
                         mrn_libgroonga_embedded);

namespace mrn {
  namespace condition_push_down {
    enum type {
      NONE,
      ALL,
      ONE_FULL_TEXT_SEARCH
    };
  } // namespace condition_push_down
} // namespace mrn
static const char* mrn_condition_push_down_type_names[] = {
  "NONE", "ALL", "ONE_FULL_TEXT_SEARCH", NullS};
static TYPELIB mrn_condition_push_down_type_typelib = {
  array_elements(mrn_condition_push_down_type_names) - 1,
  "mrn_condition_push_down_type_typelib",
  mrn_condition_push_down_type_names,
  NULL};

static MYSQL_THDVAR_ENUM(condition_push_down_type,
                         PLUGIN_VAR_RQCMDARG,
                         "How to use condition push down",
                         NULL,
                         NULL,
                         static_cast<ulong>(mrn::condition_push_down::ALL),
                         &mrn_condition_push_down_type_typelib);

static void mrn_enable_back_trace_update(THD* thd,
                                         mrn_sys_var* var,
                                         void* var_ptr,
                                         const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  auto new_value = *static_cast<const mrn_bool*>(save);
  auto old_value_ptr = static_cast<mrn_bool*>(var_ptr);

  *old_value_ptr = new_value;

#if GRN_VERSION_OR_LATER(12, 0, 1)
  grn_set_back_trace_enable(new_value);
#endif

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_BOOL(enable_back_trace,
                         mrn_enable_back_trace,
                         PLUGIN_VAR_RQCMDARG,
                         "Whether back trace is logged or not",
                         NULL,
                         mrn_enable_back_trace_update,
                         true);

static void mrn_enable_reference_count_update(THD* thd,
                                              mrn_sys_var* var,
                                              void* var_ptr,
                                              const void* save)
{
  MRN_DBUG_ENTER_FUNCTION();
  const mrn_bool new_value = *static_cast<const mrn_bool*>(save);
  mrn_bool* old_value_ptr = static_cast<mrn_bool*>(var_ptr);

  *old_value_ptr = new_value;

#if GRN_VERSION_OR_LATER(12, 1, 0)
  grn_set_reference_count_enable(new_value);
#endif

  DBUG_VOID_RETURN;
}

static MYSQL_SYSVAR_BOOL(enable_reference_count,
                         mrn_enable_reference_count,
                         PLUGIN_VAR_RQCMDARG,
                         "Whether reference count feature is enabled or not",
                         NULL,
                         mrn_enable_reference_count_update,
                         false);

static mrn_sys_var* mrn_system_variables[] = {
  MYSQL_SYSVAR(log_level),
  MYSQL_SYSVAR(log_file),
  MYSQL_SYSVAR(default_parser),
  MYSQL_SYSVAR(default_tokenizer),
  MYSQL_SYSVAR(dry_write),
  MYSQL_SYSVAR(enable_optimization),
  MYSQL_SYSVAR(match_escalation_threshold),
  MYSQL_SYSVAR(database_path_prefix),
  MYSQL_SYSVAR(default_wrapper_engine),
  MYSQL_SYSVAR(action_on_fulltext_query_error),
  MYSQL_SYSVAR(lock_timeout),
  MYSQL_SYSVAR(n_workers),
  MYSQL_SYSVAR(libgroonga_version),
  MYSQL_SYSVAR(version),
  MYSQL_SYSVAR(vector_column_delimiter),
  MYSQL_SYSVAR(libgroonga_support_zlib),
  MYSQL_SYSVAR(libgroonga_support_lz4),
  MYSQL_SYSVAR(libgroonga_support_zstd),
  MYSQL_SYSVAR(libgroonga_support_mecab),
  MYSQL_SYSVAR(boolean_mode_syntax_flags),
  MYSQL_SYSVAR(max_n_records_for_estimate),
  MYSQL_SYSVAR(libgroonga_embedded),
  MYSQL_SYSVAR(query_log_file),
  MYSQL_SYSVAR(enable_operations_recording),
  MYSQL_SYSVAR(condition_push_down_type),
  MYSQL_SYSVAR(enable_back_trace),
  MYSQL_SYSVAR(enable_reference_count),
  NULL};

/* mroonga information schema */
static struct st_mysql_information_schema i_s_info = {
  MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};

#ifndef SKIP_OPEN_TABLE
#  define SKIP_OPEN_TABLE 0
#endif

static ST_FIELD_INFO i_s_mrn_stats_fields_info[] = {
  MRN_COLUMN("VERSION", 40, MYSQL_TYPE_STRING, Show::Varchar(40)),
  MRN_COLUMN("rows_written",
             MY_INT32_NUM_DECIMAL_DIGITS,
             MYSQL_TYPE_LONG,
             Show::SLong(MY_INT32_NUM_DECIMAL_DIGITS)),
  MRN_COLUMN("rows_read",
             MY_INT32_NUM_DECIMAL_DIGITS,
             MYSQL_TYPE_LONG,
             Show::SLong(MY_INT32_NUM_DECIMAL_DIGITS)),
  MRN_COLUMN_END()};

static int i_s_mrn_stats_deinit(void* p)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_RETURN(0);
}

static int i_s_mrn_stats_fill(THD* thd, TABLE_LIST* tables, Item* cond)
{
  TABLE* table = (TABLE*)tables->table;
  int status = 0;
  MRN_DBUG_ENTER_FUNCTION();
  table->field[0]->store(grn_get_version(),
                         strlen(grn_get_version()),
                         system_charset_info);
  table->field[0]->set_notnull();
  table->field[1]->store(1); /* TODO */
  table->field[2]->store(2); /* TODO */
  if (schema_table_store_record(thd, table)) {
    status = 1;
  }
  DBUG_RETURN(status);
}

static int i_s_mrn_stats_init(void* p)
{
  MRN_DBUG_ENTER_FUNCTION();
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = i_s_mrn_stats_fields_info;
  schema->fill_table = i_s_mrn_stats_fill;
  DBUG_RETURN(0);
}

struct st_mysql_plugin i_s_mrn_stats = {
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &i_s_info,
  MRN_STATUS_VARIABLE_NAME_PREFIX_STRING "_stats",
  MRN_PLUGIN_AUTHOR,
  "Statistics for " MRN_PLUGIN_NAME_STRING,
  PLUGIN_LICENSE_GPL,
  i_s_mrn_stats_init,
#ifdef MRN_ST_MYSQL_PLUGIN_HAVE_CHECK_UNINSTALL
  NULL,
#endif
  i_s_mrn_stats_deinit,
  MRN_VERSION_IN_HEX,
  NULL,
  NULL,
  MRN_PLUGIN_LAST_VALUES};
/* End of mroonga information schema implementations */

static handler* mrn_hton_handler_create(handlerton* hton,
                                        TABLE_SHARE* share,
#ifdef MRN_HANDLERTON_CREATE_HAVE_PARTITIONED
                                        bool partitioned,
#endif
                                        MEM_ROOT* root)
{
  MRN_DBUG_ENTER_FUNCTION();
  handler* new_handler = new (root) ha_mroonga(hton, share);
  DBUG_RETURN(new_handler);
}

static void mrn_hton_drop_database(handlerton* hton, char* path)
{
  MRN_DBUG_ENTER_FUNCTION();
  mrn_db_manager->drop(path);
  DBUG_VOID_RETURN;
}

#if !(defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 100504)
#  define MRN_HANDLERTON_CLOSE_CONNECTION_NEED_THREAD_DATA_RESET
#endif

#if defined(MRN_MARIADB_P) && (MYSQL_VERSION_ID >= 110800)
#  define MRN_TRANSACTION_PARTICIPANT_HAVE_CLOSE_CONNECTION
#endif

static int mrn_hton_close_connection(
#ifndef MRN_TRANSACTION_PARTICIPANT_HAVE_CLOSE_CONNECTION
  handlerton* hton,
#endif
  THD* thd)
{
  MRN_DBUG_ENTER_FUNCTION();
  mrn::SlotData* slot_data = mrn_get_slot_data(thd, false);
  if (slot_data) {
    delete slot_data;
#ifdef MRN_HANDLERTON_CLOSE_CONNECTION_NEED_THREAD_DATA_RESET
    mrn_thd_set_ha_data(thd, mrn_hton_ptr, NULL);
#endif
    {
      mrn::Lock lock(&mrn_allocated_thds_mutex);
      grn_hash_delete(&mrn_ctx, mrn_allocated_thds, &thd, sizeof(thd), NULL);
    }
  }
  DBUG_RETURN(0);
}

#ifdef MRN_HANDLERTON_HAVE_KILL_QUERY
static void
mrn_hton_kill_query(handlerton* hton, THD* thd, enum thd_kill_levels level)
{
  MRN_DBUG_ENTER_FUNCTION();
  auto slot_data = mrn_get_slot_data(thd, false);
  if (slot_data) {
    // TODO: Reduce log level when we fix a crash problem.
    GRN_LOG(&mrn_ctx, GRN_LOG_INFO, "mroonga: kill: slot-data: %p", slot_data);
    for (auto ctx : slot_data->associated_grn_ctxs) {
      GRN_LOG(&mrn_ctx,
              GRN_LOG_INFO,
              "mroonga: kill: associated-context: %p",
              ctx);
      if (ctx->rc == GRN_SUCCESS) {
        ctx->rc = GRN_CANCEL;
      }
    }
  }
  DBUG_VOID_RETURN;
}
#endif

#ifdef MRN_HANDLERTON_HAVE_KILL_CONNECTION
static void mrn_hton_kill_connection(handlerton* hton, THD* thd)
{
  MRN_DBUG_ENTER_FUNCTION();
  auto slot_data = mrn_get_slot_data(thd, false);
  if (slot_data) {
    GRN_LOG(&mrn_ctx, GRN_LOG_INFO, "mroonga: kill: slot-data: %p", slot_data);
    for (auto ctx : slot_data->associated_grn_ctxs) {
      GRN_LOG(&mrn_ctx,
              GRN_LOG_INFO,
              "mroonga: kill: associated-context: %p",
              ctx);
      if (ctx->rc == GRN_SUCCESS) {
        ctx->rc = GRN_CANCEL;
      }
    }
  }
  DBUG_VOID_RETURN;
}
#endif

#ifdef MRN_FLUSH_LOGS_HAVE_BINLOG_GROUP_FLUSH
static bool mrn_flush_logs(handlerton* hton, bool binlog_group_flush)
#else
static bool mrn_flush_logs(handlerton* hton)
#endif
{
  MRN_DBUG_ENTER_FUNCTION();
  bool result = 0;
  grn_logger_reopen(&mrn_ctx);
  grn_query_logger_reopen(&mrn_ctx);
  DBUG_RETURN(result);
}

static grn_builtin_type
mrn_grn_type_from_field(grn_ctx* ctx, Field* field, bool for_index_key)
{
  grn_builtin_type type = GRN_DB_VOID;
  enum_field_types mysql_field_type = field->real_type();
  switch (mysql_field_type) {
  case MYSQL_TYPE_DECIMAL:    // DECIMAL; <= 65bytes
    type = GRN_DB_SHORT_TEXT; // 4Kbytes
    break;
  case MYSQL_TYPE_TINY: // TINYINT; 1byte
    if (MRN_FIELD_IS_UNSIGNED(static_cast<Field_num*>(field))) {
      type = GRN_DB_UINT8; // 1byte
    } else {
      type = GRN_DB_INT8; // 1byte
    }
    break;
  case MYSQL_TYPE_SHORT: // SMALLINT; 2bytes
    if (MRN_FIELD_IS_UNSIGNED(static_cast<Field_num*>(field))) {
      type = GRN_DB_UINT16; // 2bytes
    } else {
      type = GRN_DB_INT16; // 2bytes
    }
    break;
  case MYSQL_TYPE_LONG: // INT; 4bytes
    if (MRN_FIELD_IS_UNSIGNED(static_cast<Field_num*>(field))) {
      type = GRN_DB_UINT32; // 4bytes
    } else {
      type = GRN_DB_INT32; // 4bytes
    }
    break;
  case MYSQL_TYPE_FLOAT:  // FLOAT; 4 or 8bytes
  case MYSQL_TYPE_DOUBLE: // DOUBLE; 8bytes
    type = GRN_DB_FLOAT;  // 8bytes
    break;
  case MYSQL_TYPE_NULL: // NULL; 1byte
    type = GRN_DB_INT8; // 1byte
    break;
  case MYSQL_TYPE_TIMESTAMP: // TIMESTAMP; 4bytes
    type = GRN_DB_TIME;      // 8bytes
    break;
  case MYSQL_TYPE_LONGLONG: // BIGINT; 8bytes
    if (MRN_FIELD_IS_UNSIGNED(static_cast<Field_num*>(field))) {
      type = GRN_DB_UINT64; // 8bytes
    } else {
      type = GRN_DB_INT64; // 8bytes
    }
    break;
  case MYSQL_TYPE_INT24: // MEDIUMINT; 3bytes
    if (MRN_FIELD_IS_UNSIGNED(static_cast<Field_num*>(field))) {
      type = GRN_DB_UINT32; // 4bytes
    } else {
      type = GRN_DB_INT32; // 4bytes
    }
    break;
  case MYSQL_TYPE_DATE:     // DATE; 4bytes
  case MYSQL_TYPE_TIME:     // TIME; 3bytes
  case MYSQL_TYPE_DATETIME: // DATETIME; 8bytes
  case MYSQL_TYPE_YEAR:     // YEAR; 1byte
  case MYSQL_TYPE_NEWDATE:  // DATE; 3bytes
    type = GRN_DB_TIME;     // 8bytes
    break;
  case MYSQL_TYPE_VARCHAR: // VARCHAR; <= 64KB * 4 + 2bytes
#ifdef MRN_HAVE_MYSQL_TYPE_VARCHAR_COMPRESSED
  case MYSQL_TYPE_VARCHAR_COMPRESSED:
#endif
    if (for_index_key) {
      type = GRN_DB_SHORT_TEXT; // 4Kbytes
    } else {
      if (field->field_length <= MRN_SHORT_TEXT_SIZE) {
        type = GRN_DB_SHORT_TEXT; //  4Kbytes
      } else if (field->field_length <= MRN_TEXT_SIZE) {
        type = GRN_DB_TEXT; // 64Kbytes
      } else {
        type = GRN_DB_LONG_TEXT; //  2Gbytes
      }
    }
    break;
  case MYSQL_TYPE_BIT: { // BIT; <= 8bytes
    const auto key_length = field->key_length();
    if (key_length <= 1) {
      type = GRN_DB_UINT8;
    } else if (key_length <= 2) {
      type = GRN_DB_UINT16;
    } else if (key_length <= 4) {
      type = GRN_DB_UINT32;
    } else if (key_length <= 8) {
      type = GRN_DB_UINT64;
    }
    break;
  }
  case MYSQL_TYPE_TIMESTAMP2: // TIMESTAMP; 4bytes
    type = GRN_DB_TIME;       // 8bytes
    break;
  case MYSQL_TYPE_DATETIME2: // DATETIME; 8bytes
    type = GRN_DB_TIME;      // 8bytes
    break;
  case MYSQL_TYPE_TIME2: // TIME(FSP); 3 + (FSP + 1) / 2 bytes
                         // 0 <= FSP <= 6; 3-6bytes
    type = GRN_DB_TIME;  // 8bytes
    break;
  case MYSQL_TYPE_NEWDECIMAL: // DECIMAL; <= 9bytes
    type = GRN_DB_SHORT_TEXT; // 4Kbytes
    break;
  case MYSQL_TYPE_ENUM: // ENUM; <= 2bytes
    if (field->pack_length() == 1) {
      type = GRN_DB_UINT8; // 1bytes
    } else {
      type = GRN_DB_UINT16; // 2bytes
    }
    break;
  case MYSQL_TYPE_SET: // SET; <= 8bytes
    switch (field->pack_length()) {
    case 1:
      type = GRN_DB_UINT8; // 1byte
      break;
    case 2:
      type = GRN_DB_UINT16; // 2bytes
      break;
    case 3:
    case 4:
      type = GRN_DB_UINT32; // 3bytes
      break;
    case 8:
    default:
      type = GRN_DB_UINT64; // 8bytes
      break;
    }
    break;
  case MYSQL_TYPE_TINY_BLOB:  // TINYBLOB; <= 256bytes + 1byte
    type = GRN_DB_SHORT_TEXT; // 4Kbytes
    break;
  case MYSQL_TYPE_MEDIUM_BLOB: // MEDIUMBLOB; <= 16Mbytes + 3bytes
    if (for_index_key) {
      type = GRN_DB_SHORT_TEXT; // 4Kbytes
    } else {
      type = GRN_DB_LONG_TEXT; // 2Gbytes
    }
    break;
  case MYSQL_TYPE_LONG_BLOB: // LONGBLOB; <= 4Gbytes + 4bytes
    if (for_index_key) {
      type = GRN_DB_SHORT_TEXT; // 4Kbytes
    } else {
      type = GRN_DB_LONG_TEXT; // 2Gbytes
    }
    break;
  case MYSQL_TYPE_BLOB: // BLOB; <= 64Kbytes + 2bytes
#ifdef MRN_HAVE_MYSQL_TYPE_BLOB_COMPRESSED
  case MYSQL_TYPE_BLOB_COMPRESSED:
#endif
    if (for_index_key) {
      type = GRN_DB_SHORT_TEXT; // 4Kbytes
    } else {
      type = GRN_DB_LONG_TEXT; // 2Gbytes
    }
    break;
  case MYSQL_TYPE_VAR_STRING: // VARCHAR; <= 255byte * 4 + 1bytes
    if (for_index_key) {
      type = GRN_DB_SHORT_TEXT; // 4Kbytes
    } else {
      if (field->field_length <= MRN_SHORT_TEXT_SIZE) {
        type = GRN_DB_SHORT_TEXT; //  4Kbytes
      } else if (field->field_length <= MRN_TEXT_SIZE) {
        type = GRN_DB_TEXT; // 64Kbytes
      } else {
        type = GRN_DB_LONG_TEXT; //  2Gbytes
      }
    }
    break;
  case MYSQL_TYPE_STRING: // CHAR; < 1Kbytes =~ (255 * 4)bytes
                          //              4 is the maximum size of a character
    type = GRN_DB_SHORT_TEXT; // 4Kbytes
    break;
  case MYSQL_TYPE_GEOMETRY:        // case-by-case
    type = GRN_DB_WGS84_GEO_POINT; // 8bytes
    break;
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
  case MYSQL_TYPE_JSON:
    type = GRN_DB_TEXT;
    break;
#endif
  }
  return type;
}

static bool mrn_parse_grn_table_create_flags(THD* thd,
                                             grn_ctx* ctx,
                                             const char* flag_names,
                                             uint flag_names_length,
                                             grn_table_flags* flags)
{
  const char* flag_names_end = flag_names + flag_names_length;
  bool found = false;

  while (flag_names < flag_names_end) {
    uint rest_length = flag_names_end - flag_names;

    if (*flag_names == '|' || *flag_names == ' ') {
      flag_names += 1;
      continue;
    }
    if (rest_length >= 14 && !memcmp(flag_names, "TABLE_HASH_KEY", 14)) {
      *flags |= GRN_OBJ_TABLE_HASH_KEY;
      flag_names += 14;
      found = true;
    } else if (rest_length >= 13 && !memcmp(flag_names, "TABLE_PAT_KEY", 13)) {
      *flags |= GRN_OBJ_TABLE_PAT_KEY;
      flag_names += 13;
      found = true;
    } else if (rest_length >= 13 && !memcmp(flag_names, "TABLE_DAT_KEY", 13)) {
      *flags |= GRN_OBJ_TABLE_DAT_KEY;
      flag_names += 13;
      found = true;
    } else if (rest_length >= 9 && !memcmp(flag_names, "KEY_LARGE", 9)) {
      *flags |= GRN_OBJ_KEY_LARGE;
      flag_names += 9;
      found = true;
    } else {
      char invalid_flag_name[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(invalid_flag_name,
               MRN_MESSAGE_BUFFER_SIZE,
               "%.*s",
               static_cast<int>(rest_length),
               flag_names);
      push_warning_printf(thd,
                          MRN_SEVERITY_WARNING,
                          ER_MRN_INVALID_TABLE_FLAG_NUM,
                          ER_MRN_INVALID_TABLE_FLAG_STR,
                          invalid_flag_name);
      break;
    }
  }
  return found;
}

static bool mrn_parse_grn_column_create_flags(THD* thd,
                                              grn_ctx* ctx,
                                              const char* flag_names,
                                              uint flag_names_length,
                                              grn_column_flags* column_flags)
{
  const char* flag_names_end = flag_names + flag_names_length;
  bool found = false;

  while (flag_names < flag_names_end) {
    uint rest_length = flag_names_end - flag_names;

    if (*flag_names == '|' || *flag_names == ' ') {
      flag_names += 1;
      continue;
    }
#define NAME_SIZE(name) (sizeof(name) - 1)
#define EQUAL(name)                                                            \
  (rest_length >= NAME_SIZE(name) &&                                           \
   memcmp(flag_names, name, NAME_SIZE(name)) == 0)
    if (EQUAL("COLUMN_SCALAR")) {
      *column_flags |= GRN_OBJ_COLUMN_SCALAR;
      flag_names += NAME_SIZE("COLUMN_SCALAR");
      found = true;
    } else if (EQUAL("COLUMN_VECTOR")) {
      *column_flags |= GRN_OBJ_COLUMN_VECTOR;
      flag_names += NAME_SIZE("COLUMN_VECTOR");
      found = true;
    } else if (EQUAL("COMPRESS_ZLIB")) {
      if (mrn_libgroonga_support_zlib) {
        *column_flags |= GRN_OBJ_COMPRESS_ZLIB;
        found = true;
      } else {
        push_warning_printf(thd,
                            MRN_SEVERITY_WARNING,
                            ER_MRN_UNSUPPORTED_COLUMN_FLAG_NUM,
                            ER_MRN_UNSUPPORTED_COLUMN_FLAG_STR,
                            "COMPRESS_ZLIB");
      }
      flag_names += NAME_SIZE("COMPRESS_ZLIB");
    } else if (EQUAL("COMPRESS_LZ4")) {
      if (mrn_libgroonga_support_lz4) {
        *column_flags |= GRN_OBJ_COMPRESS_LZ4;
        found = true;
      } else {
        push_warning_printf(thd,
                            MRN_SEVERITY_WARNING,
                            ER_MRN_UNSUPPORTED_COLUMN_FLAG_NUM,
                            ER_MRN_UNSUPPORTED_COLUMN_FLAG_STR,
                            "COMPRESS_LZ4");
      }
      flag_names += NAME_SIZE("COMPRESS_LZ4");
    } else if (EQUAL("COMPRESS_ZSTD")) {
      if (mrn_libgroonga_support_zstd) {
        *column_flags |= GRN_OBJ_COMPRESS_ZSTD;
        found = true;
      } else {
        push_warning_printf(thd,
                            MRN_SEVERITY_WARNING,
                            ER_MRN_UNSUPPORTED_COLUMN_FLAG_NUM,
                            ER_MRN_UNSUPPORTED_COLUMN_FLAG_STR,
                            "COMPRESS_ZSTD");
      }
      flag_names += NAME_SIZE("COMPRESS_ZSTD");
    } else if (EQUAL("WITH_WEIGHT")) {
      *column_flags |= GRN_OBJ_WITH_WEIGHT;
      flag_names += NAME_SIZE("WITH_WEIGHT");
      found = true;
    } else if (EQUAL("WEIGHT_FLOAT32")) {
      *column_flags |= GRN_OBJ_WEIGHT_FLOAT32;
      flag_names += NAME_SIZE("WEIGHT_FLOAT32");
      found = true;
#if GRN_VERSION_OR_LATER(12, 0, 2)
    } else if (EQUAL("MISSING_ADD")) {
      *column_flags |= GRN_OBJ_MISSING_ADD;
      flag_names += NAME_SIZE("MISSING_ADD");
      found = true;
    } else if (EQUAL("MISSING_IGNORE")) {
      *column_flags |= GRN_OBJ_MISSING_IGNORE;
      flag_names += NAME_SIZE("MISSING_IGNORE");
      found = true;
    } else if (EQUAL("MISSING_NIL")) {
      *column_flags |= GRN_OBJ_MISSING_NIL;
      flag_names += NAME_SIZE("MISSING_NIL");
      found = true;
    } else if (EQUAL("INVALID_ERROR")) {
      *column_flags |= GRN_OBJ_INVALID_ERROR;
      flag_names += NAME_SIZE("INVALID_ERROR");
      found = true;
    } else if (EQUAL("INVALID_WARN")) {
      *column_flags |= GRN_OBJ_INVALID_WARN;
      flag_names += NAME_SIZE("INVALID_WARN");
      found = true;
    } else if (EQUAL("INVALID_IGNORE")) {
      *column_flags |= GRN_OBJ_INVALID_IGNORE;
      flag_names += NAME_SIZE("INVALID_IGNORE");
      found = true;
#endif
    } else {
      char invalid_flag_name[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(invalid_flag_name,
               MRN_MESSAGE_BUFFER_SIZE,
               "%.*s",
               static_cast<int>(rest_length),
               flag_names);
      push_warning_printf(thd,
                          MRN_SEVERITY_WARNING,
                          ER_MRN_INVALID_COLUMN_FLAG_NUM,
                          ER_MRN_INVALID_COLUMN_FLAG_STR,
                          invalid_flag_name);
      break;
    }
#undef EQUAL
#undef NAME_SIZE
  }
  return found;
}

static bool mrn_parse_grn_lexicon_flags(THD* thd,
                                        grn_ctx* ctx,
                                        const char* flag_names,
                                        uint flag_names_length,
                                        grn_table_flags* lexicon_flags)
{
  const char* flag_names_end = flag_names + flag_names_length;
  bool found = false;

  while (flag_names < flag_names_end) {
    uint rest_length = flag_names_end - flag_names;

    if (*flag_names == '|' || *flag_names == ' ') {
      flag_names += 1;
      continue;
    }
#define NAME_SIZE(name) (sizeof(name) - 1)
#define EQUAL(name)                                                            \
  (rest_length >= NAME_SIZE(name) &&                                           \
   memcmp(flag_names, name, NAME_SIZE(name)) == 0)
    if (EQUAL("NONE")) {
      flag_names += NAME_SIZE("NONE");
      found = true;
    } else if (EQUAL("KEY_LARGE")) {
      *lexicon_flags |= GRN_OBJ_KEY_LARGE;
      flag_names += NAME_SIZE("KEY_LARGE");
      found = true;
    } else {
      char invalid_flag_name[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(invalid_flag_name,
               MRN_MESSAGE_BUFFER_SIZE,
               "%.*s",
               static_cast<int>(rest_length),
               flag_names);
      push_warning_printf(thd,
                          MRN_SEVERITY_WARNING,
                          ER_MRN_INVALID_LEXICON_FLAG_NUM,
                          ER_MRN_INVALID_LEXICON_FLAG_STR,
                          invalid_flag_name);
      break;
    }
  }
#undef EQUAL
#undef NAME_SIZE
  return found;
}

static bool
mrn_parse_grn_index_column_flags(THD* thd,
                                 grn_ctx* ctx,
                                 const char* flag_names,
                                 uint flag_names_length,
                                 grn_column_flags* index_column_flags)
{
  const char* flag_names_end = flag_names + flag_names_length;
  bool found = false;

  while (flag_names < flag_names_end) {
    uint rest_length = flag_names_end - flag_names;

    if (*flag_names == '|' || *flag_names == ' ') {
      flag_names += 1;
      continue;
    }
#define NAME_SIZE(name) (sizeof(name) - 1)
#define EQUAL(name)                                                            \
  (rest_length >= NAME_SIZE(name) &&                                           \
   memcmp(flag_names, name, NAME_SIZE(name)) == 0)
    if (EQUAL("NONE")) {
      flag_names += NAME_SIZE("NONE");
      found = true;
    } else if (EQUAL("WITH_POSITION")) {
      *index_column_flags |= GRN_OBJ_WITH_POSITION;
      flag_names += NAME_SIZE("WITH_POSITION");
      found = true;
    } else if (EQUAL("WITH_SECTION")) {
      *index_column_flags |= GRN_OBJ_WITH_SECTION;
      flag_names += NAME_SIZE("WITH_SECTION");
      found = true;
    } else if (EQUAL("WITH_WEIGHT")) {
      *index_column_flags |= GRN_OBJ_WITH_WEIGHT;
      flag_names += NAME_SIZE("WITH_WEIGHT");
      found = true;
    } else if (EQUAL("INDEX_SMALL")) {
      *index_column_flags |= GRN_OBJ_INDEX_SMALL;
      flag_names += NAME_SIZE("INDEX_SMALL");
      found = true;
    } else if (EQUAL("INDEX_MEDIUM")) {
      *index_column_flags |= GRN_OBJ_INDEX_MEDIUM;
      flag_names += NAME_SIZE("INDEX_MEDIUM");
      found = true;
    } else if (EQUAL("INDEX_LARGE")) {
      *index_column_flags |= GRN_OBJ_INDEX_LARGE;
      flag_names += NAME_SIZE("INDEX_LARGE");
      found = true;
    } else {
      char invalid_flag_name[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(invalid_flag_name,
               MRN_MESSAGE_BUFFER_SIZE,
               "%.*s",
               static_cast<int>(rest_length),
               flag_names);
      push_warning_printf(thd,
                          MRN_SEVERITY_WARNING,
                          ER_MRN_INVALID_INDEX_FLAG_NUM,
                          ER_MRN_INVALID_INDEX_FLAG_STR,
                          invalid_flag_name);
      break;
    }
  }
#undef EQUAL
#undef NAME_SIZE
  return found;
}

#ifdef MRN_HAVE_SPATIAL
static int
mrn_set_geometry(grn_ctx* ctx, grn_obj* buf, const char* wkb, uint wkb_size)
{
  int error = 0;
  Geometry_buffer buffer;
  Geometry* geometry;

  geometry = Geometry::construct(&buffer, wkb, wkb_size);
  if (!geometry) {
    return ER_CANT_CREATE_GEOMETRY_OBJECT;
  }
  switch (geometry->get_class_info()->m_type_id) {
  case Geometry::wkb_point: {
    Gis_point* point = (Gis_point*)geometry;
    double latitude = 0.0, longitude = 0.0;
#  ifdef MRN_HAVE_POINT_XY
    point_xy xy(0.0, 0.0);
    point->get_xy(&xy);
    latitude = xy.x;
    longitude = xy.y;
#  else
    point->get_xy(&latitude, &longitude);
#  endif
    grn_obj_reinit(ctx, buf, GRN_DB_WGS84_GEO_POINT, 0);
    GRN_GEO_POINT_SET(ctx,
                      buf,
                      GRN_GEO_DEGREE2MSEC(latitude),
                      GRN_GEO_DEGREE2MSEC(longitude));
    break;
  }
  default:
    my_printf_error(ER_MRN_GEOMETRY_NOT_SUPPORT_NUM,
                    ER_MRN_GEOMETRY_NOT_SUPPORT_STR,
                    MYF(0));
    error = ER_MRN_GEOMETRY_NOT_SUPPORT_NUM;
    break;
  }
  MRN_GEOMETRY_FREE(geometry);

  return error;
}
#endif

#ifdef MRN_HANDLERTON_HAVE_ALTER_TABLE_FLAGS
static mrn_alter_table_flags
mrn_hton_alter_table_flags(mrn_alter_table_flags flags)
{
  mrn_alter_table_flags alter_flags = 0;
#  ifdef HA_INPLACE_ADD_INDEX_NO_READ_WRITE
  bool is_inplace_index_change;
  is_inplace_index_change = (((flags & MRN_ALTER_INFO_FLAG(ADD_INDEX)) &&
                              (flags & MRN_ALTER_INFO_FLAG(DROP_INDEX))) ||
                             (flags & MRN_ALTER_INFO_FLAG(CHANGE_COLUMN)));
  if (!is_inplace_index_change) {
    alter_flags |=
      HA_INPLACE_ADD_INDEX_NO_READ_WRITE | HA_INPLACE_DROP_INDEX_NO_READ_WRITE |
      HA_INPLACE_ADD_UNIQUE_INDEX_NO_READ_WRITE |
      HA_INPLACE_DROP_UNIQUE_INDEX_NO_READ_WRITE |
      HA_INPLACE_ADD_INDEX_NO_WRITE | HA_INPLACE_DROP_INDEX_NO_WRITE |
      HA_INPLACE_ADD_UNIQUE_INDEX_NO_WRITE |
      HA_INPLACE_DROP_UNIQUE_INDEX_NO_WRITE;
  }
#  endif
  return alter_flags;
}
#endif

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
struct ha_table_option_struct {
  const char* tokenizer;
  const char* normalizer;
  const char* token_filters;
  const char* flags;
};

static ha_create_table_option mrn_table_options[] = {
  HA_TOPTION_STRING("TOKENIZER", tokenizer),
  HA_TOPTION_STRING("NORMALIZER", normalizer),
  HA_TOPTION_STRING("TOKEN_FILTERS", token_filters),
  HA_TOPTION_STRING("FLAGS", flags),
  HA_TOPTION_END};

static ha_create_table_option mrn_field_options[] = {
  HA_FOPTION_STRING("GROONGA_TYPE", groonga_type),
  HA_FOPTION_STRING("FLAGS", flags),
  HA_FOPTION_END};

static ha_create_table_option mrn_index_options[] = {
  HA_IOPTION_STRING("TOKENIZER", tokenizer),
  HA_IOPTION_STRING("NORMALIZER", normalizer),
  HA_IOPTION_STRING("TOKEN_FILTERS", token_filters),
  HA_IOPTION_STRING("FLAGS", flags),
  HA_IOPTION_STRING("LEXICON", lexicon),
  HA_IOPTION_STRING("LEXICON_FLAGS", lexicon_flags),
  HA_IOPTION_END};
#endif

static int mrn_init(void* p)
{
  // init handlerton
  grn_ctx* ctx = NULL;
  handlerton* hton = static_cast<handlerton*>(p);
#ifdef MRN_HANDLERTON_HAVE_STATE
  hton->state = SHOW_OPTION_YES;
#endif
  hton->create = mrn_hton_handler_create;
  hton->flags = HTON_NO_FLAGS;
#ifndef MRN_SUPPORT_PARTITION
  hton->flags |= HTON_NO_PARTITION;
#endif
#ifdef HTON_SUPPORTS_ATOMIC_DDL
  // For supporting wrapper mode for InnoDB
  // Is it OK?
  hton->flags |= HTON_SUPPORTS_ATOMIC_DDL;
#endif
#ifdef HTON_SUPPORTS_FOREIGN_KEYS
  hton->flags |= HTON_SUPPORTS_FOREIGN_KEYS;
#endif
  hton->drop_database = mrn_hton_drop_database;
  hton->close_connection = mrn_hton_close_connection;
#ifdef MRN_HANDLERTON_HAVE_KILL_QUERY
  hton->kill_query = mrn_hton_kill_query;
#endif
#ifdef MRN_HANDLERTON_HAVE_KILL_CONNECTION
  hton->kill_connection = mrn_hton_kill_connection;
#endif
  hton->flush_logs = mrn_flush_logs;
#ifdef MRN_HANDLERTON_HAVE_ALTER_TABLE_FLAGS
  hton->alter_table_flags = mrn_hton_alter_table_flags;
#endif
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  hton->table_options = mrn_table_options;
  hton->field_options = mrn_field_options;
  hton->index_options = mrn_index_options;
#endif
  mrn_hton_ptr = hton;

#ifdef _WIN32
#  ifdef MRN_MARIADB_P
  const char* module_name = "server.dll";
#  else
  const char* module_name = NULL;
#  endif
  HMODULE current_module = GetModuleHandle(module_name);
  mrn_binlog_filter =
    *((Rpl_filter**)GetProcAddress(current_module, MRN_BINLOG_FILTER_PROC));
  mrn_my_tz_UTC =
    *((Time_zone**)GetProcAddress(current_module, MRN_MY_TZ_UTC_PROC));
#  ifdef MRN_HAVE_TABLE_DEF_CACHE
#    ifdef MRN_TABLE_DEF_CACHE_TYPE_IS_MAP
#      error must confirm mangled name of mrn_table_def_cache
#    else
  mrn_table_def_cache =
    (HASH*)GetProcAddress(current_module, "?table_def_cache@@3Ust_hash@@A");
#    endif
#  endif
#  ifndef MRN_HAVE_TDC_LOCK_TABLE_SHARE
  mrn_LOCK_open =
    (mysql_mutex_t*)GetProcAddress(current_module,
                                   "?LOCK_open@@3Ust_mysql_mutex@@A");
#  endif
#  ifdef HAVE_PSI_INTERFACE
#    ifdef MRN_TABLE_SHARE_HAVE_LOCK_SHARE
  mrn_table_share_lock_share =
    (PSI_mutex_key*)GetProcAddress(current_module,
                                   MRN_TABLE_SHARE_LOCK_SHARE_PROC);
#    endif
  mrn_table_share_lock_ha_data =
    (PSI_mutex_key*)GetProcAddress(current_module,
                                   MRN_TABLE_SHARE_LOCK_HA_DATA_PROC);
#  endif
#else
  mrn_binlog_filter = binlog_filter;
  mrn_my_tz_UTC = my_tz_UTC;
#  ifdef MRN_HAVE_TABLE_DEF_CACHE
  mrn_table_def_cache = &table_def_cache;
#  endif
#  ifndef MRN_HAVE_TDC_LOCK_TABLE_SHARE
  mrn_LOCK_open = &LOCK_open;
#  endif
#endif

  MRN_REGISTER_MUTEXES("mroonga", mrn_mutexes);

#if GRN_VERSION_OR_LATER(12, 0, 1)
  grn_set_back_trace_enable(mrn_enable_back_trace);
#endif
#if GRN_VERSION_OR_LATER(12, 1, 0)
  grn_set_reference_count_enable(mrn_enable_reference_count);
#endif

  grn_default_logger_set_path(mrn_log_file_path);
  grn_default_logger_set_max_level(static_cast<grn_log_level>(mrn_log_level));
  {
    int flags = grn_default_logger_get_flags() | GRN_LOG_PID;
#ifdef GRN_LOG_THREAD_ID
    flags |= GRN_LOG_THREAD_ID;
#endif
    grn_default_logger_set_flags(flags);
  }

  grn_default_query_logger_set_path(mrn_query_log_file_path);

  if (grn_init() != GRN_SUCCESS) {
    goto err_grn_init;
  }

  grn_set_lock_timeout(mrn_lock_timeout);
  grn_set_default_n_workers(mrn_n_workers);

  mrn_init_encoding_map();

  grn_ctx_init(&mrn_ctx, 0);
  ctx = &mrn_ctx;
  if (mrn_change_encoding(ctx, system_charset_info))
    goto err_mrn_change_encoding;

  GRN_LOG(ctx, GRN_LOG_NOTICE, "%s started.", MRN_PACKAGE_STRING);

#ifdef HAVE_PSI_MEMORY_INTERFACE
  {
    const char* category = "mroonga";
    int n_keys = array_elements(mrn_all_memory_keys);
    mysql_memory_register(category, mrn_all_memory_keys, n_keys);
  }
#endif

  // init meta-info database
  if (!(mrn_db = grn_db_create(ctx, NULL, NULL))) {
    GRN_LOG(ctx, GRN_LOG_ERROR, "cannot create system database, exiting");
    goto err_db_create;
  }
  grn_ctx_use(ctx, mrn_db);

  grn_ctx_init(&mrn_db_manager_ctx, 0);
  if (mysql_mutex_init(mrn_db_manager_mutex_key,
                       &mrn_db_manager_mutex,
                       MY_MUTEX_INIT_FAST) != 0) {
    GRN_LOG(&mrn_db_manager_ctx,
            GRN_LOG_ERROR,
            "failed to initialize mutex for database manager");
    goto err_db_manager_mutex_init;
  }
  mrn_db_manager =
    new mrn::DatabaseManager(&mrn_db_manager_ctx, &mrn_db_manager_mutex);
  if (!mrn_db_manager->init()) {
    goto err_db_manager_init;
  }

  if (mysql_mutex_init(mrn_context_pool_mutex_key,
                       &mrn_context_pool_mutex,
                       MY_MUTEX_INIT_FAST) != 0) {
    GRN_LOG(ctx, GRN_LOG_ERROR, "failed to initialize mutex for context pool");
    goto error_context_pool_mutex_init;
  }
  mrn_context_pool =
    new mrn::ContextPool(&mrn_context_pool_mutex, &mrn_n_pooling_contexts);

  if (mysql_mutex_init(mrn_operations_mutex_key,
                       &mrn_operations_mutex,
                       MY_MUTEX_INIT_FAST) != 0) {
    GRN_LOG(ctx, GRN_LOG_ERROR, "failed to initialize mutex for operations");
    goto error_operations_mutex_init;
  }

  if ((mysql_mutex_init(mrn_allocated_thds_mutex_key,
                        &mrn_allocated_thds_mutex,
                        MY_MUTEX_INIT_FAST) != 0)) {
    goto err_allocated_thds_mutex_init;
  }
  mrn_allocated_thds =
    grn_hash_create(ctx, NULL, sizeof(THD*), 0, GRN_OBJ_TABLE_HASH_KEY);
  if (!mrn_allocated_thds) {
    goto error_allocated_thds_hash_init;
  }
  if ((mysql_mutex_init(mrn_open_tables_mutex_key,
                        &mrn_open_tables_mutex,
                        MY_MUTEX_INIT_FAST) != 0)) {
    goto err_allocated_open_tables_mutex_init;
  }
  mrn_open_tables =
    grn_hash_create(ctx,
                    NULL,
                    GRN_TABLE_MAX_KEY_SIZE,
                    sizeof(MRN_SHARE*),
                    GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_KEY_VAR_SIZE);
  if (!mrn_open_tables) {
    goto error_allocated_open_tables_init;
  }
  if ((mysql_mutex_init(mrn_long_term_shares_mutex_key,
                        &mrn_long_term_shares_mutex,
                        MY_MUTEX_INIT_FAST) != 0)) {
    goto error_allocated_long_term_shares_mutex_init;
  }
  mrn_long_term_shares =
    grn_hash_create(ctx,
                    NULL,
                    GRN_TABLE_MAX_KEY_SIZE,
                    sizeof(MRN_LONG_TERM_SHARE*),
                    GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_KEY_VAR_SIZE);
  if (!mrn_long_term_shares) {
    goto error_allocated_long_term_shares_init;
  }

#ifdef MRN_USE_MYSQL_DATA_HOME
  mrn::PathMapper::default_mysql_data_home_path = mysql_data_home;
#endif

  {
    grn_obj grn_support_p;
    GRN_BOOL_INIT(&grn_support_p, 0);

    GRN_BULK_REWIND(&grn_support_p);
    grn_obj_get_info(&mrn_ctx, NULL, GRN_INFO_SUPPORT_ZLIB, &grn_support_p);
    mrn_libgroonga_support_zlib = (GRN_BOOL_VALUE(&grn_support_p));

    GRN_BULK_REWIND(&grn_support_p);
    grn_obj_get_info(&mrn_ctx, NULL, GRN_INFO_SUPPORT_LZ4, &grn_support_p);
    mrn_libgroonga_support_lz4 = (GRN_BOOL_VALUE(&grn_support_p));

    GRN_BULK_REWIND(&grn_support_p);
    grn_obj_get_info(&mrn_ctx, NULL, GRN_INFO_SUPPORT_ZSTD, &grn_support_p);
    mrn_libgroonga_support_zstd = (GRN_BOOL_VALUE(&grn_support_p));

    GRN_OBJ_FIN(&mrn_ctx, &grn_support_p);
  }
  if (grn_ctx_at(&mrn_ctx, GRN_DB_MECAB)) {
    mrn_libgroonga_support_mecab = true;
  }

  mrn_initialized = true;

  return 0;

error_allocated_long_term_shares_init:
  mysql_mutex_destroy(&mrn_long_term_shares_mutex);
error_allocated_long_term_shares_mutex_init:
  grn_hash_close(ctx, mrn_open_tables);
error_allocated_open_tables_init:
  mysql_mutex_destroy(&mrn_open_tables_mutex);
err_allocated_open_tables_mutex_init:
  grn_hash_close(&mrn_ctx, mrn_allocated_thds);
error_allocated_thds_hash_init:
  mysql_mutex_destroy(&mrn_allocated_thds_mutex);
err_allocated_thds_mutex_init:
  mysql_mutex_destroy(&mrn_operations_mutex);
error_operations_mutex_init:
  delete mrn_context_pool;
  mysql_mutex_destroy(&mrn_context_pool_mutex);
error_context_pool_mutex_init:
err_db_manager_init:
  delete mrn_db_manager;
  mysql_mutex_destroy(&mrn_db_manager_mutex);
err_db_manager_mutex_init:
  grn_ctx_fin(&mrn_db_manager_ctx);
  grn_obj_unlink(ctx, mrn_db);
err_db_create:
err_mrn_change_encoding:
  grn_ctx_fin(ctx);
  grn_fin();
err_grn_init:
  return -1;
}

static int mrn_deinit(void* p)
{
  THD* thd = current_thd;
  grn_ctx* ctx = &mrn_ctx;

  mrn_initialized = false;

  GRN_LOG(ctx, GRN_LOG_NOTICE, "%s deinit", MRN_PACKAGE_STRING);

  if (thd && thd_sql_command(thd) == SQLCOM_UNINSTALL_PLUGIN) {
    mrn::Lock lock(&mrn_allocated_thds_mutex);
    GRN_HASH_EACH_BEGIN(ctx, mrn_allocated_thds, cursor, id)
    {
      void* allocated_thd_address;
      grn_hash_cursor_get_key(ctx, cursor, &allocated_thd_address);
      THD* allocated_thd;
      grn_memcpy(&allocated_thd, allocated_thd_address, sizeof(allocated_thd));
      mrn::SlotData* slot_data = mrn_get_slot_data(allocated_thd, false);
      if (slot_data) {
        delete slot_data;
        mrn_thd_set_ha_data(allocated_thd, mrn_hton_ptr, NULL);
      }
    }
    GRN_HASH_EACH_END(ctx, cursor);
  }

  while (grn_hash_size(&mrn_ctx, mrn_long_term_shares) > 0) {
    GRN_HASH_EACH_BEGIN(ctx, mrn_long_term_shares, cursor, id)
    {
      void* long_term_share_address;
      grn_hash_cursor_get_value(ctx, cursor, &long_term_share_address);
      MRN_LONG_TERM_SHARE* long_term_share;
      grn_memcpy(&long_term_share,
                 long_term_share_address,
                 sizeof(long_term_share));
      mrn_free_long_term_share(long_term_share);
      break;
    }
    GRN_HASH_EACH_END(ctx, cursor);
  }

  grn_hash_close(ctx, mrn_long_term_shares);
  mysql_mutex_destroy(&mrn_long_term_shares_mutex);
  grn_hash_close(ctx, mrn_open_tables);
  mysql_mutex_destroy(&mrn_open_tables_mutex);
  grn_hash_close(ctx, mrn_allocated_thds);
  mysql_mutex_destroy(&mrn_allocated_thds_mutex);
  mysql_mutex_destroy(&mrn_operations_mutex);
  delete mrn_context_pool;
  mysql_mutex_destroy(&mrn_context_pool_mutex);
  delete mrn_db_manager;
  mysql_mutex_destroy(&mrn_db_manager_mutex);
  grn_ctx_fin(&mrn_db_manager_ctx);

  grn_obj_unlink(ctx, mrn_db);
  grn_ctx_fin(ctx);
  grn_fin();

  return 0;
}

mrn_declare_plugin(MRN_PLUGIN_NAME){MYSQL_STORAGE_ENGINE_PLUGIN,
                                    &storage_engine_structure,
                                    MRN_PLUGIN_NAME_STRING,
                                    MRN_PLUGIN_AUTHOR,
                                    "CJK-ready fulltext search, column store",
                                    PLUGIN_LICENSE_GPL,
                                    mrn_init,
#ifdef MRN_ST_MYSQL_PLUGIN_HAVE_CHECK_UNINSTALL
                                    NULL,
#endif
                                    mrn_deinit,
                                    MRN_VERSION_IN_HEX,
                                    mrn_status_variables,
                                    mrn_system_variables,
                                    MRN_PLUGIN_LAST_VALUES},
  i_s_mrn_stats mrn_declare_plugin_end;

static double mrn_get_score_value(grn_obj* score)
{
  MRN_DBUG_ENTER_FUNCTION();
  double score_value;
  if (score->header.domain == GRN_DB_FLOAT) {
    score_value = GRN_FLOAT_VALUE(score);
  } else {
    score_value = (double)GRN_INT32_VALUE(score);
  }
  DBUG_RETURN(score_value);
}

static void mrn_generic_ft_clear(st_mrn_ft_info* info)
{
  MRN_DBUG_ENTER_FUNCTION();
  if (!info->ctx) {
    DBUG_VOID_RETURN;
  }

  if (info->cursor) {
    grn_obj_unlink(info->ctx, info->cursor);
  }
  if (info->id_accessor) {
    grn_obj_unlink(info->ctx, info->id_accessor);
  }
  if (info->key_accessor) {
    grn_obj_unlink(info->ctx, info->key_accessor);
  }
  grn_obj_unlink(info->ctx, info->expression);
  grn_obj_unlink(info->ctx, info->match_columns);
  grn_obj_unlink(info->ctx, info->score_column);
  if (info->sorted_result) {
    grn_obj_unlink(info->ctx, info->sorted_result);
  }
  grn_obj_unlink(info->ctx, info->result);
  grn_obj_unlink(info->ctx, &(info->query));
  grn_obj_unlink(info->ctx, &(info->key));
  grn_obj_unlink(info->ctx, &(info->score));

  info->ctx = NULL;

  DBUG_VOID_RETURN;
}

static void mrn_generic_ft_close_search(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  st_mrn_ft_info* info = reinterpret_cast<st_mrn_ft_info*>(handler);
  mrn_generic_ft_clear(info);
  delete info;
  DBUG_VOID_RETURN;
}

static inline void
mrn_generic_ft_ensure_searched(st_mrn_ft_info* info,
                               bool need_fast_order_limit_check)
{
  MRN_DBUG_ENTER_FUNCTION();

  if (info->result) {
    DBUG_VOID_RETURN;
  }

  GRN_CTX_SET_ENCODING(info->ctx, info->encoding);

  info->result = grn_table_create(info->ctx,
                                  NULL,
                                  0,
                                  NULL,
                                  GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_WITH_SUBREC,
                                  info->table,
                                  0);
  if (!info->result) {
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "[mroonga][ft][ensure-searched] failed to create a table "
             "to store matched records for one search: <%s>",
             info->ctx->errbuf);
    GRN_LOG(info->ctx, GRN_LOG_ERROR, "%s", error_message);
    DBUG_VOID_RETURN;
  }

  info->score_column = grn_obj_column(info->ctx,
                                      info->result,
                                      MRN_COLUMN_NAME_SCORE,
                                      strlen(MRN_COLUMN_NAME_SCORE));

  if (GRN_TEXT_LEN(&(info->query)) == 0) {
    DBUG_VOID_RETURN;
  }

  {
    longlong escalation_threshold =
      THDVAR(info->mroonga->current_thread(), match_escalation_threshold);
    mrn::MatchEscalationThresholdScope scope(info->ctx, escalation_threshold);
    grn_table_select(info->ctx,
                     info->table,
                     info->expression,
                     info->result,
                     GRN_OP_OR);
  }

  if (info->ctx->rc == GRN_SUCCESS) {
    if (info->flags & FT_SORTED) {
      grn_table_sort_key score_sort_key;
      score_sort_key.key = info->score_column;
      score_sort_key.offset = 0;
      score_sort_key.flags = GRN_TABLE_SORT_DESC;
      info->sorted_result = grn_table_create(info->ctx,
                                             NULL,
                                             0,
                                             NULL,
                                             GRN_OBJ_TABLE_NO_KEY,
                                             NULL,
                                             info->result);
      grn_table_sort(info->ctx,
                     info->result,
                     0,
                     -1,
                     info->sorted_result,
                     &score_sort_key,
                     1);
    } else if (need_fast_order_limit_check) {
      grn_table_sort_key* sort_keys = NULL;
      int n_sort_keys = 0;
      longlong limit = -1;
      bool fast_order_limit =
        info->mroonga->check_fast_order_limit(info->result,
                                              &sort_keys,
                                              &n_sort_keys,
                                              &limit);
      if (fast_order_limit) {
        info->sorted_result = grn_table_create(info->ctx,
                                               NULL,
                                               0,
                                               NULL,
                                               GRN_OBJ_TABLE_NO_KEY,
                                               NULL,
                                               info->result);
        grn_table_sort(info->ctx,
                       info->result,
                       0,
                       static_cast<int>(limit),
                       info->sorted_result,
                       sort_keys,
                       n_sort_keys);
        for (int i = 0; i < n_sort_keys; i++) {
          grn_obj_unlink(info->ctx, sort_keys[i].key);
        }
        my_free(sort_keys);
      }
    }
  }

  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
static int mrn_wrapper_ft_read_next(FT_INFO* handler, char* record)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_RETURN(HA_ERR_END_OF_FILE);
}

static float
mrn_wrapper_ft_find_relevance(FT_INFO* handler, uchar* record, uint length)
{
  MRN_DBUG_ENTER_FUNCTION();
  st_mrn_ft_info* info = reinterpret_cast<st_mrn_ft_info*>(handler);

  mrn_generic_ft_ensure_searched(info, false);

  float score = 0.0;
  grn_id record_id;

  mrn_change_encoding(info->ctx, NULL);
  key_copy((uchar*)(GRN_TEXT_VALUE(&(info->key))),
           record,
           info->primary_key_info,
           info->primary_key_info->key_length);
  record_id = grn_table_get(info->ctx,
                            info->table,
                            GRN_TEXT_VALUE(&(info->key)),
                            GRN_TEXT_LEN(&(info->key)));

  if (record_id != GRN_ID_NIL) {
    grn_id result_record_id;
    result_record_id =
      grn_table_get(info->ctx, info->result, &record_id, sizeof(grn_id));
    if (result_record_id != GRN_ID_NIL) {
      GRN_BULK_REWIND(&(info->score));
      grn_obj_get_value(info->ctx,
                        info->score_column,
                        result_record_id,
                        &(info->score));
      score = mrn_get_score_value(&(info->score));
    }
  }

  DBUG_PRINT("info", ("mroonga: record_id=%d score=%g", record_id, score));

  DBUG_RETURN(score);
}

static void mrn_wrapper_ft_close_search(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  mrn_generic_ft_close_search(handler);
  DBUG_VOID_RETURN;
}

static float mrn_wrapper_ft_get_relevance(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  st_mrn_ft_info* info = (st_mrn_ft_info*)handler;
  float score = 0.0;
  grn_id record_id;
  ha_mroonga* mroonga = info->mroonga;
  mrn_change_encoding(info->ctx, NULL);
  record_id = grn_table_get(info->ctx,
                            info->table,
                            GRN_TEXT_VALUE(&(mroonga->key_buffer)),
                            GRN_TEXT_LEN(&(mroonga->key_buffer)));

  if (record_id != GRN_ID_NIL) {
    grn_id result_record_id;
    result_record_id =
      grn_table_get(info->ctx, info->result, &record_id, sizeof(grn_id));
    if (result_record_id != GRN_ID_NIL) {
      GRN_BULK_REWIND(&(info->score));
      grn_obj_get_value(info->ctx,
                        info->score_column,
                        result_record_id,
                        &(info->score));
      score = mrn_get_score_value(&(info->score));
    }
  }

  DBUG_PRINT("info", ("mroonga: record_id=%d score=%g", record_id, score));

  DBUG_RETURN(score);
}

static void mrn_wrapper_ft_reinit_search(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_VOID_RETURN;
}

static _ft_vft mrn_wrapper_ft_vft = {mrn_wrapper_ft_read_next,
                                     mrn_wrapper_ft_find_relevance,
                                     mrn_wrapper_ft_close_search,
                                     mrn_wrapper_ft_get_relevance,
                                     mrn_wrapper_ft_reinit_search};
#endif

static int mrn_storage_ft_read_next(FT_INFO* handler, char* record)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_RETURN(HA_ERR_END_OF_FILE);
}

static float
mrn_storage_ft_find_relevance(FT_INFO* handler, uchar* record, uint length)
{
  MRN_DBUG_ENTER_FUNCTION();
  st_mrn_ft_info* info = reinterpret_cast<st_mrn_ft_info*>(handler);

  mrn_generic_ft_ensure_searched(info, false);

  ha_mroonga* mroonga = info->mroonga;
  mrn_change_encoding(info->ctx, NULL);

  float score = 0.0;
  if (mroonga->record_id != GRN_ID_NIL) {
    grn_id result_record_id;
    result_record_id = grn_table_get(info->ctx,
                                     info->result,
                                     &(mroonga->record_id),
                                     sizeof(grn_id));
    if (result_record_id != GRN_ID_NIL) {
      GRN_BULK_REWIND(&(info->score));
      grn_obj_get_value(info->ctx,
                        info->score_column,
                        result_record_id,
                        &(info->score));
      score = mrn_get_score_value(&(info->score));
    }
  }
  DBUG_PRINT("info",
             ("mroonga: record_id=%d score=%g", mroonga->record_id, score));

  DBUG_RETURN(score);
}

static void mrn_storage_ft_close_search(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  mrn_generic_ft_close_search(handler);
  DBUG_VOID_RETURN;
}

static float mrn_storage_ft_get_relevance(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  st_mrn_ft_info* info = (st_mrn_ft_info*)handler;
  ha_mroonga* mroonga = info->mroonga;
  mrn_change_encoding(info->ctx, NULL);

  float score = 0.0;
  if (mroonga->record_id != GRN_ID_NIL) {
    grn_id result_record_id;
    result_record_id = grn_table_get(info->ctx,
                                     info->result,
                                     &(mroonga->record_id),
                                     sizeof(grn_id));
    if (result_record_id != GRN_ID_NIL) {
      GRN_BULK_REWIND(&(info->score));
      grn_obj_get_value(info->ctx,
                        info->score_column,
                        result_record_id,
                        &(info->score));
      score = mrn_get_score_value(&(info->score));
    }
  }
  DBUG_PRINT("info",
             ("mroonga: record_id=%d score=%g", mroonga->record_id, score));

  DBUG_RETURN(score);
}

static void mrn_storage_ft_reinit_search(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_VOID_RETURN;
}

static _ft_vft mrn_storage_ft_vft = {mrn_storage_ft_read_next,
                                     mrn_storage_ft_find_relevance,
                                     mrn_storage_ft_close_search,
                                     mrn_storage_ft_get_relevance,
                                     mrn_storage_ft_reinit_search};

static int mrn_no_such_key_ft_read_next(FT_INFO* handler, char* record)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_RETURN(HA_ERR_END_OF_FILE);
}

static float
mrn_no_such_key_ft_find_relevance(FT_INFO* handler, uchar* record, uint length)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_RETURN(0.0);
}

static void mrn_no_such_key_ft_close_search(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  st_mrn_ft_info* info = (st_mrn_ft_info*)handler;
  delete info;
  DBUG_VOID_RETURN;
}

static float mrn_no_such_key_ft_get_relevance(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_RETURN(0.0);
}

static void mrn_no_such_key_ft_reinit_search(FT_INFO* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  DBUG_VOID_RETURN;
}

static _ft_vft mrn_no_such_key_ft_vft = {mrn_no_such_key_ft_read_next,
                                         mrn_no_such_key_ft_find_relevance,
                                         mrn_no_such_key_ft_close_search,
                                         mrn_no_such_key_ft_get_relevance,
                                         mrn_no_such_key_ft_reinit_search};

static uint mrn_generic_ft_get_version()
{
  MRN_DBUG_ENTER_FUNCTION();
  // This value is not used in MySQL 5.6.7-rc. So it is
  // meaningless. It may be used in the future...
  uint version = 1;
  DBUG_RETURN(version);
}

static ulonglong mrn_generic_ft_ext_get_flags()
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong flags = FTS_ORDERED_RESULT;
  DBUG_RETURN(flags);
}

// Not used.
static ulonglong mrn_generic_ft_ext_get_docid(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong id = 0;
  DBUG_RETURN(id);
}

static ulonglong mrn_generic_ft_ext_count_matches(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  st_mrn_ft_info* info = reinterpret_cast<st_mrn_ft_info*>(handler);
  mrn_generic_ft_ensure_searched(info, false);
  ulonglong n_records = grn_table_size(info->ctx, info->result);
  DBUG_RETURN(n_records);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
static uint mrn_wrapper_ft_ext_get_version()
{
  MRN_DBUG_ENTER_FUNCTION();
  uint version = mrn_generic_ft_get_version();
  DBUG_RETURN(version);
}

static ulonglong mrn_wrapper_ft_ext_get_flags()
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong flags = mrn_generic_ft_ext_get_flags();
  DBUG_RETURN(flags);
}

static ulonglong mrn_wrapper_ft_ext_get_docid(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong id = mrn_generic_ft_ext_get_docid(handler);
  DBUG_RETURN(id);
}

static ulonglong mrn_wrapper_ft_ext_count_matches(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong n_records = mrn_generic_ft_ext_count_matches(handler);
  DBUG_RETURN(n_records);
}

static _ft_vft_ext mrn_wrapper_ft_vft_ext = {mrn_wrapper_ft_ext_get_version,
                                             mrn_wrapper_ft_ext_get_flags,
                                             mrn_wrapper_ft_ext_get_docid,
                                             mrn_wrapper_ft_ext_count_matches};
#endif

static uint mrn_storage_ft_ext_get_version()
{
  MRN_DBUG_ENTER_FUNCTION();
  uint version = mrn_generic_ft_get_version();
  DBUG_RETURN(version);
}

static ulonglong mrn_storage_ft_ext_get_flags()
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong flags = mrn_generic_ft_ext_get_flags();
  DBUG_RETURN(flags);
}

static ulonglong mrn_storage_ft_ext_get_docid(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong id = mrn_generic_ft_ext_get_docid(handler);
  DBUG_RETURN(id);
}

static ulonglong mrn_storage_ft_ext_count_matches(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong n_records = mrn_generic_ft_ext_count_matches(handler);
  DBUG_RETURN(n_records);
}

static _ft_vft_ext mrn_storage_ft_vft_ext = {mrn_storage_ft_ext_get_version,
                                             mrn_storage_ft_ext_get_flags,
                                             mrn_storage_ft_ext_get_docid,
                                             mrn_storage_ft_ext_count_matches};

static uint mrn_no_such_key_ft_ext_get_version()
{
  MRN_DBUG_ENTER_FUNCTION();
  uint version = mrn_generic_ft_get_version();
  DBUG_RETURN(version);
}

static ulonglong mrn_no_such_key_ft_ext_get_flags()
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong flags = mrn_generic_ft_ext_get_flags();
  DBUG_RETURN(flags);
}

static ulonglong mrn_no_such_key_ft_ext_get_docid(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong id = mrn_generic_ft_ext_get_docid(handler);
  DBUG_RETURN(id);
}

static ulonglong mrn_no_such_key_ft_ext_count_matches(FT_INFO_EXT* handler)
{
  MRN_DBUG_ENTER_FUNCTION();
  ulonglong n_records = 0;
  DBUG_RETURN(n_records);
}

static _ft_vft_ext mrn_no_such_key_ft_vft_ext = {
  mrn_no_such_key_ft_ext_get_version,
  mrn_no_such_key_ft_ext_get_flags,
  mrn_no_such_key_ft_ext_get_docid,
  mrn_no_such_key_ft_ext_count_matches};

/* handler implementation */
ha_mroonga::ha_mroonga(handlerton* hton, TABLE_SHARE* share_arg)
    : handler(hton, share_arg),
#ifdef MRN_ENABLE_WRAPPER_MODE
      wrap_handler(NULL),
      is_clone(false),
      parent_for_clone(NULL),
      mem_root_for_clone(NULL),
#endif

      record_id(GRN_ID_NIL),
      key_id(NULL),
      del_key_id(NULL),

#ifdef MRN_ENABLE_WRAPPER_MODE
      wrap_ft_init_count(0),
#endif
      share(NULL),
#ifdef MRN_ENABLE_WRAPPER_MODE
      wrap_key_info(NULL),
      base_key_info(NULL),

      analyzed_for_create(false),
      wrap_handler_for_create(NULL),
      alter_key_info_buffer(NULL),
#endif
      mrn_lock_type(F_UNLCK),

      ctx_entity_(),
      ctx(&ctx_entity_),
      grn_table(NULL),
      grn_columns(NULL),
      grn_column_caches(NULL),
      grn_column_ranges(NULL),
      grn_index_tables(NULL),
      grn_index_columns(NULL),

      grn_source_column_geo(NULL),
      cursor_geo(NULL),
      cursor(NULL),
      index_table_cursor(NULL),
      empty_value_records(NULL),
      empty_value_records_cursor(NULL),

      condition_push_down_result(NULL),
      sorted_condition_push_down_result_(NULL),
      condition_push_down_result_cursor(NULL),
      blob_buffers_(ctx),

      dup_key(0),

      count_skip(false),
      fast_order_limit(false),
      fast_order_limit_with_index(false),

      ignoring_duplicated_key(false),
      inserting_with_update(false),
      fulltext_searching(false),
      ignoring_no_key_columns(false),
      replacing_(false),
      written_by_row_based_binlog(0),
      current_ft_item(NULL),
      operations_(NULL)
{
  MRN_DBUG_ENTER_METHOD();
  grn_ctx_init(ctx, 0);
  mrn_change_encoding(ctx, system_charset_info);
  grn_ctx_use(ctx, mrn_db);
  mrn::SlotData* slot_data = mrn_get_slot_data(ha_thd(), true);
  if (slot_data) {
    GRN_LOG(&mrn_ctx,
            GRN_LOG_INFO,
            "mroonga: associated-context: add: %p:%p",
            slot_data,
            ctx);
    slot_data->add_associated_grn_ctx(ctx);
  }
  GRN_WGS84_GEO_POINT_INIT(&top_left_point, 0);
  GRN_WGS84_GEO_POINT_INIT(&bottom_right_point, 0);
  GRN_WGS84_GEO_POINT_INIT(&source_point, 0);
  GRN_TEXT_INIT(&key_buffer, 0);
  GRN_TEXT_INIT(&encoded_key_buffer, 0);
  GRN_VOID_INIT(&old_value_buffer);
  GRN_VOID_INIT(&new_value_buffer);
  DBUG_VOID_RETURN;
}

ha_mroonga::~ha_mroonga()
{
  MRN_DBUG_ENTER_METHOD();

  delete operations_;

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (analyzed_for_create) {
    if (wrap_handler_for_create) {
      mrn_destroy(wrap_handler_for_create);
    }
    if (share_for_create.wrapper_mode) {
      plugin_unlock(NULL, share_for_create.plugin);
    }
    if (share_for_create.table_name) {
      my_free(share_for_create.table_name);
    }
    mrn_free_share_alloc(&share_for_create);
    MRN_FREE_ROOT(&mem_root_for_create);
  }
#endif
  grn_obj_unlink(ctx, &top_left_point);
  grn_obj_unlink(ctx, &bottom_right_point);
  grn_obj_unlink(ctx, &source_point);
  grn_obj_unlink(ctx, &key_buffer);
  grn_obj_unlink(ctx, &encoded_key_buffer);
  grn_obj_unlink(ctx, &old_value_buffer);
  grn_obj_unlink(ctx, &new_value_buffer);
  mrn::SlotData* slot_data = mrn_get_slot_data(ha_thd(), false);
  if (slot_data) {
    GRN_LOG(&mrn_ctx,
            GRN_LOG_INFO,
            "mroonga: associated-context: remove: %p:%p",
            slot_data,
            ctx);
    slot_data->remove_associated_grn_ctx(ctx);
  }
  grn_ctx_fin(ctx);
  DBUG_VOID_RETURN;
}

THD* ha_mroonga::current_thread()
{
  MRN_DBUG_ENTER_METHOD();
  THD* thread = ha_thd();
  DBUG_RETURN(thread);
}

#ifdef MRN_HANDLER_HAVE_TABLE_TYPE
const char* ha_mroonga::table_type() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(MRN_PLUGIN_NAME_STRING);
}
#endif

#ifdef MRN_HANDLER_HAVE_INDEX_TYPE
const char* ha_mroonga::index_type(uint key_nr)
{
  MRN_DBUG_ENTER_METHOD();
  KEY* key_info = &(table->s->key_info[key_nr]);
  if (key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
    DBUG_RETURN("FULLTEXT");
  } else if (key_info->algorithm == HA_KEY_ALG_HASH) {
    DBUG_RETURN("HASH");
  } else {
    DBUG_RETURN("BTREE");
  }
}
#endif

static const char* ha_mroonga_exts[] = {NullS};
const char** ha_mroonga::bas_ext() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(ha_mroonga_exts);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
uint ha_mroonga::wrapper_max_supported_record_length() const
{
  uint res;
  MRN_DBUG_ENTER_METHOD();
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    res = wrap_handler_for_create->max_supported_record_length();
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    res = wrap_handler->max_supported_record_length();
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(res);
}
#endif

uint ha_mroonga::storage_max_supported_record_length() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(HA_MAX_REC_LENGTH);
}

uint ha_mroonga::max_supported_record_length() const
{
  MRN_DBUG_ENTER_METHOD();

  uint res;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!share && !analyzed_for_create &&
      (thd_sql_command(ha_thd()) == SQLCOM_CREATE_TABLE ||
       thd_sql_command(ha_thd()) == SQLCOM_CREATE_INDEX ||
       thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE)) {
    create_share_for_create();
  }
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    res = wrapper_max_supported_record_length();
  } else if (wrap_handler && share && share->wrapper_mode) {
    res = wrapper_max_supported_record_length();
  } else {
#endif
    res = storage_max_supported_record_length();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  DBUG_RETURN(res);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
uint ha_mroonga::wrapper_max_supported_keys() const
{
  uint res;
  MRN_DBUG_ENTER_METHOD();
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    res = wrap_handler_for_create->max_supported_keys();
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    res = wrap_handler->max_supported_keys();
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(res);
}
#endif

uint ha_mroonga::storage_max_supported_keys() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(HA_MAX_REC_LENGTH);
}

uint ha_mroonga::max_supported_keys() const
{
  MRN_DBUG_ENTER_METHOD();

  uint res;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!share && !analyzed_for_create &&
      (thd_sql_command(ha_thd()) == SQLCOM_CREATE_TABLE ||
       thd_sql_command(ha_thd()) == SQLCOM_CREATE_INDEX ||
       thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE)) {
    create_share_for_create();
  }
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    res = wrapper_max_supported_keys();
  } else if (wrap_handler && share && share->wrapper_mode) {
    res = wrapper_max_supported_keys();
  } else {
#endif
    res = storage_max_supported_keys();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  DBUG_RETURN(res);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
uint ha_mroonga::wrapper_max_supported_key_length() const
{
  uint res;
  MRN_DBUG_ENTER_METHOD();
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    res = wrap_handler_for_create->max_supported_key_length();
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    res = wrap_handler->max_supported_key_length();
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(res);
}
#endif

uint ha_mroonga::storage_max_supported_key_length() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(GRN_TABLE_MAX_KEY_SIZE);
}

uint ha_mroonga::max_supported_key_length() const
{
  MRN_DBUG_ENTER_METHOD();

  uint res;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!share && !analyzed_for_create &&
      (thd_sql_command(ha_thd()) == SQLCOM_CREATE_TABLE ||
       thd_sql_command(ha_thd()) == SQLCOM_CREATE_INDEX ||
       thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE)) {
    create_share_for_create();
  }
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    res = wrapper_max_supported_key_length();
  } else if (wrap_handler && share && share->wrapper_mode) {
    res = wrapper_max_supported_key_length();
  } else {
#endif
    res = storage_max_supported_key_length();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  DBUG_RETURN(res);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
uint ha_mroonga::wrapper_max_supported_key_part_length(
#  ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
  HA_CREATE_INFO* create_info
#  endif
) const
{
  uint res;
  MRN_DBUG_ENTER_METHOD();
  if (analyzed_for_create && share_for_create.wrapper_mode) {
#  ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
    res = wrap_handler_for_create->max_supported_key_part_length(create_info);
#  else
    res = wrap_handler_for_create->max_supported_key_part_length();
#  endif
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
    res = wrap_handler->max_supported_key_part_length(create_info);
#  else
    res = wrap_handler->max_supported_key_part_length();
#  endif
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(res);
}
#endif

uint ha_mroonga::storage_max_supported_key_part_length(
#ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
  HA_CREATE_INFO* create_info
#endif
) const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(GRN_TABLE_MAX_KEY_SIZE);
}

uint ha_mroonga::max_supported_key_part_length(
#ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
  HA_CREATE_INFO* create_info
#endif
) const
{
  MRN_DBUG_ENTER_METHOD();

  uint res;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!share && !analyzed_for_create &&
      (thd_sql_command(ha_thd()) == SQLCOM_CREATE_TABLE ||
       thd_sql_command(ha_thd()) == SQLCOM_CREATE_INDEX ||
       thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE)) {
    create_share_for_create();
  }
  if ((analyzed_for_create && share_for_create.wrapper_mode) ||
      (wrap_handler && share && share->wrapper_mode)) {
#  ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
    res = wrapper_max_supported_key_part_length(create_info);
#  else
    res = wrapper_max_supported_key_part_length();
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_MAX_SUPPORTED_KEY_PART_LENGTH_HAVE_CREATE_INFO
    res = storage_max_supported_key_part_length(create_info);
#else
  res = storage_max_supported_key_part_length();
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  DBUG_RETURN(res);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
ulonglong ha_mroonga::wrapper_table_flags() const
{
  ulonglong table_flags;
  MRN_DBUG_ENTER_METHOD();
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    table_flags = wrap_handler_for_create->ha_table_flags();
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    table_flags = wrap_handler->ha_table_flags();
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  table_flags |= HA_CAN_FULLTEXT | HA_CAN_FULLTEXT_EXT |
                 HA_PRIMARY_KEY_REQUIRED_FOR_DELETE | HA_CAN_RTREEKEYS;
#  ifdef HA_CAN_REPAIR
  table_flags |= HA_CAN_REPAIR;
#  endif
#  ifdef HA_GENERATED_COLUMNS
  table_flags |= HA_GENERATED_COLUMNS;
#  endif
#  ifdef HA_CAN_VIRTUAL_COLUMNS
  table_flags |= HA_CAN_VIRTUAL_COLUMNS;
#  endif
#  ifdef HA_REC_NOT_IN_SEQ
  table_flags |= HA_REC_NOT_IN_SEQ;
#  endif
#  ifdef HA_CAN_HASH_KEYS
  table_flags |= HA_CAN_HASH_KEYS;
#  endif
#  ifdef HA_SUPPORTS_GEOGRAPHIC_GEOMETRY_COLUMN
  table_flags |= HA_SUPPORTS_GEOGRAPHIC_GEOMETRY_COLUMN;
#  endif
  DBUG_RETURN(table_flags);
}
#endif

ulonglong ha_mroonga::storage_table_flags() const
{
  MRN_DBUG_ENTER_METHOD();
  ulonglong flags = HA_NO_TRANSACTIONS | HA_PARTIAL_COLUMN_READ |
                    HA_NULL_IN_KEY | HA_CAN_INDEX_BLOBS |
                    HA_STATS_RECORDS_IS_EXACT | HA_CAN_FULLTEXT |
                    HA_CAN_FULLTEXT_EXT | HA_BINLOG_FLAGS | HA_CAN_BIT_FIELD |
                    HA_DUPLICATE_POS | HA_CAN_GEOMETRY | HA_CAN_RTREEKEYS;
  // HA_HAS_RECORDS;
#ifdef HA_MUST_USE_TABLE_CONDITION_PUSHDOWN
#  ifndef HA_CAN_TABLE_CONDITION_PUSHDOWN
#    define HA_CAN_TABLE_CONDITION_PUSHDOWN HA_MUST_USE_TABLE_CONDITION_PUSHDOWN
#  endif
#endif
#ifdef HA_CAN_TABLE_CONDITION_PUSHDOWN
  flags |= HA_CAN_TABLE_CONDITION_PUSHDOWN;
#endif
#ifdef HA_CAN_REPAIR
  flags |= HA_CAN_REPAIR;
#endif
#ifdef HA_GENERATED_COLUMNS
  flags |= HA_GENERATED_COLUMNS;
#endif
#ifdef HA_CAN_VIRTUAL_COLUMNS
  flags |= HA_CAN_VIRTUAL_COLUMNS;
#endif
#ifdef HA_REC_NOT_IN_SEQ
  flags |= HA_REC_NOT_IN_SEQ;
#endif
#ifdef HA_CAN_HASH_KEYS
  flags |= HA_CAN_HASH_KEYS;
#endif
#ifdef HA_SUPPORTS_GEOGRAPHIC_GEOMETRY_COLUMN
  flags |= HA_SUPPORTS_GEOGRAPHIC_GEOMETRY_COLUMN;
#endif
  DBUG_RETURN(flags);
}

ulonglong ha_mroonga::table_flags() const
{
  MRN_DBUG_ENTER_METHOD();

  ulonglong flags;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!share && !analyzed_for_create &&
      (thd_sql_command(ha_thd()) == SQLCOM_CREATE_TABLE ||
       thd_sql_command(ha_thd()) == SQLCOM_CREATE_INDEX ||
       thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE)) {
    create_share_for_create();
  }
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    flags = wrapper_table_flags();
  } else if (wrap_handler && share && share->wrapper_mode) {
    flags = wrapper_table_flags();
  } else {
#endif
    flags = storage_table_flags();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  DBUG_RETURN(flags);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
ulong ha_mroonga::wrapper_index_flags(uint idx, uint part, bool all_parts) const
{
  ulong index_flags;
  MRN_DBUG_ENTER_METHOD();
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    index_flags = wrap_handler_for_create->index_flags(idx, part, all_parts);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    index_flags = wrap_handler->index_flags(idx, part, all_parts);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(index_flags);
}
#endif

ulong ha_mroonga::storage_index_flags(uint idx, uint part, bool all_parts) const
{
  MRN_DBUG_ENTER_METHOD();
  ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE;
  KEY* key = &(table_share->key_info[idx]);
  bool need_normalize_p = false;
  // TODO: MariaDB 10.1 passes key->user_defined_key_parts as part
  // for ORDER BY DESC. We just it fallback to part = 0. We may use
  // it for optimization in the future.
  //
  // See also: test_if_order_by_key() in sql/sql_select.cc.
  if (KEY_N_KEY_PARTS(key) == part) {
    part = 0;
  }
  Field* field = &(key->key_part[part].field[0]);
  if (field &&
      (have_custom_normalizer(key) ||
       should_normalize(field, key->algorithm == HA_KEY_ALG_FULLTEXT))) {
    need_normalize_p = true;
  }
  if (!need_normalize_p) {
    flags |= HA_KEYREAD_ONLY;
  }
  if (KEY_N_KEY_PARTS(key) > 1 || !need_normalize_p) {
    flags |= HA_READ_ORDER;
  }
  if (key->algorithm == HA_KEY_ALG_RTREE) {
    flags |= HA_KEY_SCAN_NOT_ROR;
  }
  DBUG_RETURN(flags);
}

ulong ha_mroonga::index_flags(uint idx, uint part, bool all_parts) const
{
  MRN_DBUG_ENTER_METHOD();

  KEY* key = &(table_share->key_info[idx]);
  if (key->algorithm == HA_KEY_ALG_FULLTEXT) {
    DBUG_RETURN(HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR);
  }
  if (mrn_is_geo_key(key)) {
    DBUG_RETURN(HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR | HA_READ_RANGE);
  }

  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  // Not only CREATE TABLE, CREATE INDEX and ALTER TABLE but also
  // INSERT against not opened table call this without wrap_handler
  // (before wrapper_open()). So we can't use
  // thd_sql_command(ha_thd()) == SQLCOM_XXX here.
  if (!share && !analyzed_for_create) {
    create_share_for_create();
  }
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    error = wrapper_index_flags(idx, part, all_parts);
  } else if (wrap_handler && share && share->wrapper_mode) {
    error = wrapper_index_flags(idx, part, all_parts);
  } else {
#endif
    error = storage_index_flags(idx, part, all_parts);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::create_share_for_create() const
{
  int error;
  THD* thd = ha_thd();
  LEX* lex = thd->lex;
  HA_CREATE_INFO* create_info = MRN_LEX_GET_CREATE_INFO(lex);
  MRN_DBUG_ENTER_METHOD();
  wrap_handler_for_create = NULL;
  MRN_TABLE_RESET(&table_for_create);
  memset(&share_for_create, 0, sizeof(MRN_SHARE));
  MRN_TABLE_SHARE_RESET(&table_share_for_create);
  if (table_share) {
    table_share_for_create.comment = table_share->comment;
    table_share_for_create.connect_string = table_share->connect_string;
  } else {
    if (thd_sql_command(ha_thd()) != SQLCOM_CREATE_INDEX) {
      table_share_for_create.comment = create_info->comment;
      table_share_for_create.connect_string = create_info->connect_string;
    }
    if (thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE ||
        thd_sql_command(ha_thd()) == SQLCOM_CREATE_INDEX) {
      mrn::SlotData* slot_data = mrn_get_slot_data(thd, false);
      if (slot_data && slot_data->alter_create_info) {
        create_info = slot_data->alter_create_info;
        if (slot_data->alter_connect_string) {
          table_share_for_create.connect_string.str =
            slot_data->alter_connect_string;
          table_share_for_create.connect_string.length =
            strlen(slot_data->alter_connect_string);
        } else {
          table_share_for_create.connect_string.str = NULL;
          table_share_for_create.connect_string.length = 0;
        }
        if (slot_data->alter_comment) {
          table_share_for_create.comment.str = slot_data->alter_comment;
          table_share_for_create.comment.length =
            strlen(slot_data->alter_comment);
        } else {
          table_share_for_create.comment.str = NULL;
          table_share_for_create.comment.length = 0;
        }
      }
    }
  }
  mrn_init_alloc_root(&mem_root_for_create, "mroonga::create", 1024, 0, MYF(0));
  analyzed_for_create = true;
  if (lex->query_tables && MRN_GET_TABLE_NAME(lex->query_tables)) {
    share_for_create.table_name =
      mrn_my_strdup(MRN_GET_TABLE_NAME(lex->query_tables), MYF(MY_WME));
    share_for_create.table_name_length =
      MRN_GET_TABLE_NAME_LENGTH(lex->query_tables);
  }
  share_for_create.table_share = &table_share_for_create;
  table_for_create.s = &table_share_for_create;
#  ifdef WITH_PARTITION_STORAGE_ENGINE
  table_for_create.part_info = NULL;
#  endif
  if ((error = mrn_parse_table_param(&share_for_create, &table_for_create)))
    goto error;

  if (share_for_create.wrapper_mode) {
    wrap_handler_for_create = MRN_HANDLERTON_CREATE(share_for_create.hton,
                                                    table_share,
                                                    false,
                                                    &mem_root_for_create);
    if (!wrap_handler_for_create) {
      error = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    wrap_handler_for_create->init();
  }
  DBUG_RETURN(0);

error:
  if (share_for_create.wrapper_mode) {
    plugin_unlock(NULL, share_for_create.plugin);
  }
  mrn_free_share_alloc(&share_for_create);
  MRN_FREE_ROOT(&mem_root_for_create);
  analyzed_for_create = false;
  thd->clear_error();
  DBUG_RETURN(error);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_create(const char* name,
                               TABLE* table,
                               HA_CREATE_INFO* info,
#  ifdef MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
                               dd::Table* table_def,
#  endif
                               MRN_SHARE* tmp_share)
{
  int error = 0;
  handler* hnd;
  MRN_DBUG_ENTER_METHOD();

  if (table_share->primary_key == MAX_KEY) {
    my_message(ER_REQUIRES_PRIMARY_KEY,
               MRN_GET_ERR_MSG(ER_REQUIRES_PRIMARY_KEY),
               MYF(0));
    DBUG_RETURN(ER_REQUIRES_PRIMARY_KEY);
  }

  error = ensure_database_open(name);
  if (error)
    DBUG_RETURN(error);

  error = wrapper_create_index(name, table, info, tmp_share);
  if (error)
    DBUG_RETURN(error);

  wrap_key_info = mrn_create_key_info_for_table(tmp_share, table, &error);
  if (error)
    DBUG_RETURN(error);
  base_key_info = table->key_info;

  share = tmp_share;
  MRN_SET_WRAP_SHARE_KEY(tmp_share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (parse_engine_table_options(ha_thd(), tmp_share->hton, table->s)) {
    MRN_SET_BASE_SHARE_KEY(tmp_share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
    share = NULL;
    if (wrap_key_info) {
      my_free(wrap_key_info);
      wrap_key_info = NULL;
    }
    base_key_info = NULL;
    error = MRN_GET_ERROR_NUMBER;
    DBUG_RETURN(error);
  }
#  endif
  hnd = mrn_get_new_handler(table->s,
                            table->s->m_part_info != NULL,
                            current_thd->mem_root,
                            tmp_share->hton);
  if (!hnd) {
    MRN_SET_BASE_SHARE_KEY(tmp_share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
    share = NULL;
    if (wrap_key_info) {
      my_free(wrap_key_info);
      wrap_key_info = NULL;
    }
    base_key_info = NULL;
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
#  ifdef MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
  error = hnd->ha_create(name, table, info, table_def);
#  else
  error = hnd->ha_create(name, table, info);
#  endif
  MRN_SET_BASE_SHARE_KEY(tmp_share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  share = NULL;
  mrn_destroy(hnd);

  if (error) {
    mrn::PathMapper mapper(name);
#  ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
    generic_delete_table(name, table_def, mapper.table_name());
#  else
    generic_delete_table(name, mapper.table_name());
#  endif
  }

  if (wrap_key_info) {
    my_free(wrap_key_info);
    wrap_key_info = NULL;
  }
  base_key_info = NULL;
  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_create_index_fulltext_validate(KEY* key_info)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
  uint i;
  for (i = 0; i < KEY_N_KEY_PARTS(key_info); i++) {
    Field* field = key_info->key_part[i].field;

    grn_builtin_type gtype = mrn_grn_type_from_field(ctx, field, true);
    if (gtype != GRN_DB_SHORT_TEXT) {
      error = ER_CANT_CREATE_TABLE;
      GRN_LOG(ctx,
              GRN_LOG_ERROR,
              "key type must be text: <%d> "
              "(TODO: We should show type name not type ID.)",
              field->type());
      my_message(ER_CANT_CREATE_TABLE,
                 "key type must be text. (TODO: We should show type name.)",
                 MYF(0));
      DBUG_RETURN(error);
    }
  }

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_create_index_fulltext(const char* grn_table_name,
                                              int i,
                                              KEY* key_info,
                                              grn_obj** index_tables,
                                              grn_obj** index_columns)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  error = wrapper_create_index_fulltext_validate(key_info);
  if (error) {
    DBUG_RETURN(error);
  }

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  grn_obj_flags index_table_flags = GRN_OBJ_TABLE_PAT_KEY | GRN_OBJ_PERSISTENT;
  grn_obj* index_table;

  grn_column_flags index_column_flags =
    GRN_OBJ_COLUMN_INDEX | GRN_OBJ_PERSISTENT;

  if (!find_index_column_flags(key_info, &index_column_flags)) {
    index_column_flags |= GRN_OBJ_WITH_POSITION;
    if (KEY_N_KEY_PARTS(key_info) > 1) {
      index_column_flags |= GRN_OBJ_WITH_SECTION;
    }
  }

  mrn::SmartGrnObj lexicon_key_type(ctx, GRN_DB_SHORT_TEXT);
  error = mrn_change_encoding(ctx, key_info->key_part->field->charset());
  if (error) {
    DBUG_RETURN(error);
  }
  mrn::IndexTableName index_table_name(grn_table_name, KEY_NAME(key_info));
  index_table = grn_table_create(ctx,
                                 index_table_name.c_str(),
                                 index_table_name.length(),
                                 NULL,
                                 index_table_flags,
                                 lexicon_key_type.get(),
                                 0);
  if (ctx->rc) {
    error = ER_CANT_CREATE_TABLE;
    my_message(ER_CANT_CREATE_TABLE, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  mrn_change_encoding(ctx, system_charset_info);
  index_tables[i] = index_table;

  set_tokenizer(index_table, key_info);
  set_token_filters(index_table, key_info);

  if (have_custom_normalizer(key_info) ||
      should_normalize(&key_info->key_part->field[0], true)) {
    set_normalizer(index_table, key_info);
  }

  grn_obj* index_column = grn_column_create(ctx,
                                            index_table,
                                            INDEX_COLUMN_NAME,
                                            strlen(INDEX_COLUMN_NAME),
                                            NULL,
                                            index_column_flags,
                                            grn_table);
  if (ctx->rc) {
    error = ER_CANT_CREATE_TABLE;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  if (index_columns) {
    index_columns[i] = index_column;
  } else {
    grn_obj_unlink(ctx, index_column);
  }

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_create_index_geo(const char* grn_table_name,
                                         int i,
                                         KEY* key_info,
                                         grn_obj** index_tables,
                                         grn_obj** index_columns)
{
  MRN_DBUG_ENTER_METHOD();
  int error;

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  mrn::IndexTableName index_table_name(grn_table_name, KEY_NAME(key_info));

  grn_obj_flags index_table_flags = GRN_OBJ_TABLE_PAT_KEY | GRN_OBJ_PERSISTENT;
  grn_obj* index_table;

  grn_obj_flags index_column_flags = GRN_OBJ_COLUMN_INDEX | GRN_OBJ_PERSISTENT;

  grn_obj* lexicon_key_type = grn_ctx_at(ctx, GRN_DB_WGS84_GEO_POINT);
  index_table = grn_table_create(ctx,
                                 index_table_name.c_str(),
                                 index_table_name.length(),
                                 NULL,
                                 index_table_flags,
                                 lexicon_key_type,
                                 0);
  if (ctx->rc) {
    error = ER_CANT_CREATE_TABLE;
    my_message(ER_CANT_CREATE_TABLE, ctx->errbuf, MYF(0));
    grn_obj_unlink(ctx, lexicon_key_type);
    DBUG_RETURN(error);
  }
  grn_obj_unlink(ctx, lexicon_key_type);
  index_tables[i] = index_table;

  grn_obj* index_column = grn_column_create(ctx,
                                            index_table,
                                            INDEX_COLUMN_NAME,
                                            strlen(INDEX_COLUMN_NAME),
                                            NULL,
                                            index_column_flags,
                                            grn_table);
  if (ctx->rc) {
    error = ER_CANT_CREATE_TABLE;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  if (index_columns) {
    index_columns[i] = index_column;
  } else {
    grn_obj_unlink(ctx, index_column);
  }

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_create_index(const char* name,
                                     TABLE* table,
                                     HA_CREATE_INFO* info,
                                     MRN_SHARE* tmp_share)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  mrn::PathMapper mapper(name);
  const char* grn_table_name = mapper.table_name();

  {
    char* path = NULL; // we don't specify path
    grn_obj* pkey_type = grn_ctx_at(ctx, GRN_DB_SHORT_TEXT);
    grn_obj* pkey_value_type = NULL; // we don't use this
    grn_table_flags flags = GRN_OBJ_PERSISTENT;
    if (!find_table_flags(info, tmp_share, &flags)) {
      flags |= GRN_OBJ_TABLE_HASH_KEY;
    }
    grn_obj* table = grn_table_create(ctx,
                                      grn_table_name,
                                      strlen(grn_table_name),
                                      path,
                                      flags,
                                      pkey_type,
                                      pkey_value_type);
    if (ctx->rc) {
      error = ER_CANT_CREATE_TABLE;
      my_message(error, ctx->errbuf, MYF(0));
      DBUG_RETURN(error);
    }
    if (grn_table) {
      grn_obj_unlink(ctx, grn_table);
    }
    grn_table = table;
  }

  uint i;
  uint n_keys = table->s->keys;
  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*, index_tables, n_keys);
  if (!tmp_share->disable_keys) {
    for (i = 0; i < n_keys; i++) {
      index_tables[i] = NULL;

      KEY* key_info = &(table->s->key_info[i]);
      if (key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
        error = wrapper_create_index_fulltext(grn_table_name,
                                              i,
                                              key_info,
                                              index_tables,
                                              NULL);
      } else if (mrn_is_geo_key(key_info)) {
        error = wrapper_create_index_geo(grn_table_name,
                                         i,
                                         key_info,
                                         index_tables,
                                         NULL);
      }
    }
  }

  if (error) {
    for (uint j = 0; j < i; j++) {
      if (index_tables[j]) {
        grn_obj_remove(ctx, index_tables[j]);
      }
    }
    grn_obj_remove(ctx, grn_table);
    grn_table = NULL;
  }
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_create(const char* name,
                               TABLE* table,
                               HA_CREATE_INFO* info,
#ifdef MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
                               dd::Table* table_def,
#endif
                               MRN_SHARE* tmp_share)
{
  int error;
  MRN_LONG_TERM_SHARE* long_term_share = tmp_share->long_term_share;
  MRN_DBUG_ENTER_METHOD();

  if (info->auto_increment_value) {
    mrn::Lock lock(&long_term_share->auto_inc_mutex);
    long_term_share->auto_inc_value = info->auto_increment_value;
    DBUG_PRINT(
      "info",
      ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
    long_term_share->auto_inc_inited = true;
  }

  error = storage_create_validate_pseudo_column(table);
  if (error)
    DBUG_RETURN(error);

  error = storage_create_validate_index(table);
  if (error)
    DBUG_RETURN(error);

  error = ensure_database_open(name);
  if (error)
    DBUG_RETURN(error);

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  grn_table_flags table_flags = GRN_OBJ_PERSISTENT;

  /* primary key must be handled before creating table */
  grn_obj* pkey_type;
  uint pkey_nr = table->s->primary_key;
  if (pkey_nr != MAX_INDEXES) {
    KEY* key_info = &(table->s->key_info[pkey_nr]);
    bool is_id;

    int key_parts = KEY_N_KEY_PARTS(key_info);
    if (key_parts == 1) {
      Field* pkey_field = key_info->key_part[0].field;
      is_id = FIELD_NAME_EQUAL(pkey_field, MRN_COLUMN_NAME_ID);

      grn_builtin_type gtype = mrn_grn_type_from_field(ctx, pkey_field, false);
      pkey_type = grn_ctx_at(ctx, gtype);
    } else {
      is_id = false;
      pkey_type = grn_ctx_at(ctx, GRN_DB_SHORT_TEXT);
    }

    if (is_id) {
      table_flags |= GRN_OBJ_TABLE_NO_KEY;
      pkey_type = NULL;
    } else {
      if (!find_table_flags(info, tmp_share, &table_flags)) {
        if (key_info->algorithm == HA_KEY_ALG_HASH) {
          table_flags |= GRN_OBJ_TABLE_HASH_KEY;
        } else {
          table_flags |= GRN_OBJ_TABLE_PAT_KEY;
        }
      }
    }
  } else {
    // primary key doesn't exists
    table_flags |= GRN_OBJ_TABLE_NO_KEY;
    pkey_type = NULL;
  }

  /* create table */
  grn_obj* table_obj;
  mrn::PathMapper mapper(name);

  char* table_path = NULL;         // we don't specify path
  grn_obj* pkey_value_type = NULL; // we don't use this

  table_obj = grn_table_create(ctx,
                               mapper.table_name(),
                               strlen(mapper.table_name()),
                               table_path,
                               table_flags,
                               pkey_type,
                               pkey_value_type);
  if (ctx->rc) {
    error = ER_CANT_CREATE_TABLE;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }

  if ((table_flags & GRN_OBJ_TABLE_TYPE_MASK) != GRN_OBJ_TABLE_NO_KEY) {
    KEY* key_info = &(table->s->key_info[pkey_nr]);
    int key_parts = KEY_N_KEY_PARTS(key_info);
    if (key_parts == 1) {
      {
        const char* normalizer = NULL;
        size_t normalizer_length = 0;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
        if (info->option_struct) {
          normalizer = info->option_struct->normalizer;
          if (normalizer) {
            normalizer_length = strlen(normalizer);
          }
        }
#endif
        if (!normalizer && tmp_share->normalizer) {
          normalizer = tmp_share->normalizer;
          normalizer_length = tmp_share->normalizer_length;
        }
        if (normalizer) {
          set_normalizer(table_obj, NULL, normalizer, normalizer_length);
        } else {
          Field* field = &(key_info->key_part->field[0]);
          if (should_normalize(field, false)) {
            set_normalizer(table_obj, key_info);
          }
        }
      }
      {
        const char* tokenizer = NULL;
        size_t tokenizer_length = 0;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
        if (info->option_struct) {
          tokenizer = info->option_struct->tokenizer;
          if (tokenizer) {
            tokenizer_length = strlen(tokenizer);
          }
        }
#endif
        if (!tokenizer && tmp_share->tokenizer) {
          tokenizer = tmp_share->tokenizer;
          tokenizer_length = tmp_share->tokenizer_length;
        }
        /* Deprecated */
        if (!tokenizer && tmp_share->default_tokenizer) {
          tokenizer = tmp_share->default_tokenizer;
          tokenizer_length = tmp_share->default_tokenizer_length;
        }
        if (tokenizer) {
          set_tokenizer(table_obj, tokenizer, tokenizer_length);
        }
      }
      {
        const char* token_filters = NULL;
        size_t token_filters_length = 0;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
        if (info->option_struct) {
          token_filters = info->option_struct->token_filters;
          if (token_filters) {
            token_filters_length = strlen(token_filters);
          }
        }
#endif
        if (!token_filters && tmp_share->token_filters) {
          token_filters = tmp_share->token_filters;
          token_filters_length = tmp_share->token_filters_length;
        }
        if (token_filters) {
          set_token_filters(table_obj, token_filters, token_filters_length);
        }
      }
    }
  }

  /* create columns */
  uint n_columns = table->s->fields;
  for (uint i = 0; i < n_columns; i++) {
    Field* field = table->s->field[i];
    mrn::ColumnName column_name(FIELD_NAME(field));

    if (strcmp(MRN_COLUMN_NAME_ID, column_name.mysql_name()) == 0) {
      continue;
    }

#ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
    bool is_created = storage_create_foreign_key(table,
                                                 mapper.table_name(),
                                                 field,
                                                 table_obj,
                                                 table_def,
                                                 error);
#else
    bool is_created = storage_create_foreign_key(table,
                                                 mapper.table_name(),
                                                 field,
                                                 table_obj,
                                                 error);
#endif
    if (is_created) {
      continue;
    }
    if (error) {
      grn_obj_remove(ctx, table_obj);
      DBUG_RETURN(error);
    }

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif

    grn_column_flags col_flags = GRN_OBJ_PERSISTENT;
    if (!find_column_flags(field, tmp_share, i, &col_flags)) {
      col_flags |= GRN_OBJ_COLUMN_SCALAR;
    }

    grn_obj* col_type;
    {
      int column_type_error_code = ER_CANT_CREATE_TABLE;
      col_type = find_column_type(field, tmp_share, i, column_type_error_code);
      if (!col_type) {
        grn_obj_remove(ctx, table_obj);
        DBUG_RETURN(column_type_error_code);
      }
    }
    char* col_path = NULL; // we don't specify path

    grn_column_create(ctx,
                      table_obj,
                      column_name.c_str(),
                      column_name.length(),
                      col_path,
                      col_flags,
                      col_type);
    if (ctx->rc) {
      error = ER_CANT_CREATE_TABLE;
      my_message(error, ctx->errbuf, MYF(0));
      grn_obj_remove(ctx, table_obj);
      DBUG_RETURN(error);
    }
  }

  error =
    storage_create_indexes(table, mapper.table_name(), table_obj, tmp_share);
  if (error) {
    grn_obj_remove(ctx, table_obj);
    table_obj = NULL;
  }

  if (table_obj) {
    grn_obj_unlink(ctx, table_obj);
  }

  DBUG_RETURN(error);
}

int ha_mroonga::storage_create_validate_pseudo_column(TABLE* table)
{
  int error = 0;
  uint i, n_columns;

  MRN_DBUG_ENTER_METHOD();
  n_columns = table->s->fields;
  for (i = 0; i < n_columns; i++) {
    Field* field = table->s->field[i];
    if (FIELD_NAME_EQUAL(field, MRN_COLUMN_NAME_ID)) {
      switch (field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
        break;
      default:
        GRN_LOG(ctx, GRN_LOG_ERROR, "_id must be numeric data type");
        error = ER_CANT_CREATE_TABLE;
        my_message(error, "_id must be numeric data type", MYF(0));
        DBUG_RETURN(error);
      }
    }
  }

  DBUG_RETURN(error);
}

namespace {
  std::string mrn_name_normalize(const mrn_foreign_key_name& name)
  {
#if defined(MRN_MARIADB_P) && MYSQL_VERSION_ID >= 110800
    IdentBuffer<NAME_LEN> buffer;
    buffer.copy_casedn(name);
    return std::string(buffer.ptr(), buffer.length());
#else
    char buffer[NAME_LEN + 1];
    strmake(buffer, name.str, (std::min)(name.length, sizeof(buffer) - 1));
    my_casedn_str(files_charset_info, buffer);
    return std::string(buffer, strlen(buffer));
#endif
  }
}; // namespace

bool ha_mroonga::storage_create_foreign_key(TABLE* table,
                                            const char* grn_table_name,
                                            Field* field,
                                            grn_obj* table_obj,
#ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
                                            const dd::Table* table_def,
#endif
                                            int& error)
{
  MRN_DBUG_ENTER_METHOD();
  LEX* lex = ha_thd()->lex;
  Alter_info* alter_info = MRN_LEX_GET_ALTER_INFO(lex);
  MRN_KEY_SPEC_LIST_EACH_BEGIN(alter_info->key_list, key_spec)
  {
    if (key_spec->type != MRN_KEYTYPE_FOREIGN) {
      continue;
    }
    if (MRN_KEY_PART_SPEC_LIST_N_ELEMENTS(key_spec->columns) > 1) {
      error = ER_CANT_CREATE_TABLE;
      my_message(error,
                 "mroonga can't use FOREIGN_KEY with multiple columns",
                 MYF(0));
      DBUG_RETURN(false);
    }
    {
      bool is_same_field_name = false;
      MRN_KEY_PART_SPEC_LIST_EACH_BEGIN(key_spec->columns, key_part_spec)
      {
        const mrn_key_part_spec_field_name* field_name =
          MRN_KEY_PART_SPEC_FIELD_NAME(key_part_spec);
        DBUG_PRINT("info",
                   ("mroonga: field_name=" MRN_KEY_PART_SPEC_FIELD_NAME_FORMAT,
                    MRN_KEY_PART_SPEC_FIELD_NAME_VALUE(field_name)));
        DBUG_PRINT("info",
                   ("mroonga: field->field_name=" FIELD_NAME_FORMAT,
                    FIELD_NAME_FORMAT_VALUE(field)));
        is_same_field_name =
          MRN_FIELD_NAME_EQUAL_KEY_PART_SPEC_FIELD_NAME(field, field_name);
      }
      MRN_KEY_PART_SPEC_LIST_EACH_END();
      if (!is_same_field_name) {
        continue;
      }
    }
    const mrn_foreign_key_spec* fk =
      static_cast<const mrn_foreign_key_spec*>(key_spec);
    MRN_KEY_PART_SPEC_LIST_EACH_BEGIN(fk->ref_columns, key_part_ref_spec)
    {
      const mrn_key_part_spec_field_name* ref_field_name =
        MRN_KEY_PART_SPEC_FIELD_NAME(key_part_ref_spec);
      DBUG_PRINT(
        "info",
        ("mroonga: ref_field_name=" MRN_KEY_PART_SPEC_FIELD_NAME_FORMAT,
         MRN_KEY_PART_SPEC_FIELD_NAME_VALUE(ref_field_name)));
      const mrn_foreign_key_name& ref_db_name = fk->ref_db;
      if (ref_db_name.length > 0) {
        DBUG_PRINT("info",
                   ("mroonga: ref_db_name=%.*s",
                    static_cast<int>(ref_db_name.length),
                    ref_db_name.str));
        std::string normalized_ref_db_name;
        if (lower_case_table_names) {
          normalized_ref_db_name = mrn_name_normalize(ref_db_name);
          DBUG_PRINT(
            "info",
            ("mroonga: casedn ref_db_name=%s", normalized_ref_db_name.c_str()));
        } else {
          normalized_ref_db_name =
            std::string(ref_db_name.str, ref_db_name.length);
        }
        if (!(table->s->db.length == normalized_ref_db_name.size() &&
              std::memcmp(table->s->db.str,
                          normalized_ref_db_name.data(),
                          table->s->db.length) == 0)) {
          error = ER_CANT_CREATE_TABLE;
          my_message(error,
                     "mroonga: "
                     "can't use FOREIGN_KEY during different database tables",
                     MYF(0));
          DBUG_RETURN(false);
        }
      }

      const mrn_foreign_key_name& ref_table_name = fk->ref_table;
      DBUG_PRINT("info",
                 ("mroonga: ref_table_name=%.*s",
                  static_cast<int>(ref_table_name.length),
                  ref_table_name.str));
      std::string normalized_ref_table_name;
      if (lower_case_table_names) {
        normalized_ref_table_name = mrn_name_normalize(ref_table_name);
      } else {
        normalized_ref_table_name =
          std::string(ref_table_name.str, ref_table_name.length);
      }
      if (lower_case_table_names) {
        DBUG_PRINT("info",
                   ("mroonga: casedn ref_table_name=%s",
                    normalized_ref_table_name.c_str()));
      }

      grn_obj *column, *column_ref = NULL, *grn_table_ref = NULL;
      char ref_path[FN_REFLEN + 1];
      build_table_filename(ref_path,
                           sizeof(ref_path) - 1,
                           table->s->db.str,
                           normalized_ref_table_name.c_str(),
                           "",
                           0);
      DBUG_PRINT("info", ("mroonga: ref_path=%s", ref_path));

      error = mrn_change_encoding(ctx, system_charset_info);
      if (error)
        DBUG_RETURN(false);

      mrn::PathMapper mapper(ref_path);
      grn_table_ref =
        grn_ctx_get(ctx, mapper.table_name(), strlen(mapper.table_name()));
      if (!grn_table_ref) {
        error = ER_CANT_CREATE_TABLE;
        char err_msg[MRN_BUFFER_SIZE];
        sprintf(err_msg,
                "reference table [%s.%s] is not mroonga table",
                table->s->db.str,
                normalized_ref_table_name.c_str());
        my_message(error, err_msg, MYF(0));
        DBUG_RETURN(false);
      }

      MRN_DECLARE_TABLE_LIST(table_list,
                             mapper.db_name(),
                             strlen(mapper.db_name()),
                             mapper.mysql_table_name(),
                             strlen(mapper.mysql_table_name()),
                             mapper.mysql_table_name(),
                             TL_WRITE);
      mrn_open_mutex_lock(table->s);
      TABLE_SHARE* tmp_ref_table_share;
#ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
      {
        auto dd_client = dd::get_dd_client(ha_thd());
        dd::cache::Dictionary_client::Auto_releaser dd_releaser(dd_client);
        const dd::Table* tmp_ref_table_def;
        if (dd_client->acquire(mapper.db_name(),
                               mapper.mysql_table_name(),
                               &tmp_ref_table_def)) {
          mrn_open_mutex_unlock(table->s);
          grn_obj_unlink(ctx, grn_table_ref);
          error = ER_CANT_CREATE_TABLE;
          char err_msg[MRN_BUFFER_SIZE];
          sprintf(err_msg,
                  "reference table [%s.%s] is not found",
                  table->s->db.str,
                  normalized_ref_table_name.c_str());
          my_message(error, err_msg, MYF(0));
          DBUG_RETURN(false);
        }
        tmp_ref_table_share = mrn_create_tmp_table_share(&table_list,
                                                         ref_path,
                                                         tmp_ref_table_def,
                                                         &error);
      }
#else
      tmp_ref_table_share =
        mrn_create_tmp_table_share(&table_list, ref_path, &error);
#endif
      mrn_open_mutex_unlock(table->s);
      if (!tmp_ref_table_share) {
        grn_obj_unlink(ctx, grn_table_ref);
        error = ER_CANT_CREATE_TABLE;
        char err_msg[MRN_BUFFER_SIZE];
        sprintf(err_msg,
                "reference table [%s.%s] is not found",
                table->s->db.str,
                normalized_ref_table_name.c_str());
        my_message(error, err_msg, MYF(0));
        DBUG_RETURN(false);
      }
      uint ref_pkey_nr = tmp_ref_table_share->primary_key;
      if (ref_pkey_nr == MAX_KEY) {
        mrn_open_mutex_lock(table->s);
        mrn_free_tmp_table_share(tmp_ref_table_share);
        mrn_open_mutex_unlock(table->s);
        grn_obj_unlink(ctx, grn_table_ref);
        error = ER_CANT_CREATE_TABLE;
        char err_msg[MRN_BUFFER_SIZE];
        sprintf(err_msg,
                "reference table [%s.%s] has no primary key",
                table->s->db.str,
                normalized_ref_table_name.c_str());
        my_message(error, err_msg, MYF(0));
        DBUG_RETURN(false);
      }
      KEY* ref_key_info = &tmp_ref_table_share->key_info[ref_pkey_nr];
      uint ref_key_parts = KEY_N_KEY_PARTS(ref_key_info);
      if (ref_key_parts > 1) {
        mrn_open_mutex_lock(table->s);
        mrn_free_tmp_table_share(tmp_ref_table_share);
        mrn_open_mutex_unlock(table->s);
        grn_obj_unlink(ctx, grn_table_ref);
        error = ER_CANT_CREATE_TABLE;
        char err_msg[MRN_BUFFER_SIZE];
        sprintf(err_msg,
                "reference table [%s.%s] primary key is multiple column",
                table->s->db.str,
                normalized_ref_table_name.c_str());
        my_message(error, err_msg, MYF(0));
        DBUG_RETURN(false);
      }
      Field* ref_field = &ref_key_info->key_part->field[0];
      if (!MRN_FIELD_NAME_EQUAL_KEY_PART_SPEC_FIELD_NAME(ref_field,
                                                         ref_field_name)) {
        mrn_open_mutex_lock(table->s);
        mrn_free_tmp_table_share(tmp_ref_table_share);
        mrn_open_mutex_unlock(table->s);
        grn_obj_unlink(ctx, grn_table_ref);
        error = ER_CANT_CREATE_TABLE;
        char err_msg[MRN_BUFFER_SIZE];
        sprintf(err_msg,
                "reference column [%s.%s." MRN_KEY_PART_SPEC_FIELD_NAME_FORMAT
                "] is not used for primary key",
                table->s->db.str,
                normalized_ref_table_name.c_str(),
                MRN_KEY_PART_SPEC_FIELD_NAME_VALUE(ref_field_name));
        my_message(error, err_msg, MYF(0));
        DBUG_RETURN(false);
      }
      mrn_open_mutex_lock(table->s);
      mrn_free_tmp_table_share(tmp_ref_table_share);
      mrn_open_mutex_unlock(table->s);
      grn_column_flags col_flags = GRN_OBJ_COLUMN_SCALAR | GRN_OBJ_PERSISTENT;
      column = grn_column_create(ctx,
                                 table_obj,
                                 FIELD_NAME(field),
                                 NULL,
                                 col_flags,
                                 grn_table_ref);
      if (ctx->rc) {
        grn_obj_unlink(ctx, grn_table_ref);
        error = ER_CANT_CREATE_TABLE;
        my_message(error, ctx->errbuf, MYF(0));
        DBUG_RETURN(false);
      }

      mrn::IndexColumnName index_column_name(grn_table_name, FIELD_NAME(field));
      grn_obj_flags ref_col_flags = GRN_OBJ_COLUMN_INDEX | GRN_OBJ_PERSISTENT;
      column_ref = grn_column_create(ctx,
                                     grn_table_ref,
                                     index_column_name.c_str(),
                                     index_column_name.length(),
                                     NULL,
                                     ref_col_flags,
                                     table_obj);
      if (ctx->rc) {
        grn_obj_unlink(ctx, column);
        grn_obj_unlink(ctx, grn_table_ref);
        error = ER_CANT_CREATE_TABLE;
        my_message(error, ctx->errbuf, MYF(0));
        DBUG_RETURN(false);
      }

      grn_obj source_ids;
      grn_id source_id = grn_obj_id(ctx, column);
      GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
      GRN_UINT32_PUT(ctx, &source_ids, source_id);
      if (error) {
        grn_obj_unlink(ctx, &source_ids);
        grn_obj_unlink(ctx, column_ref);
        grn_obj_unlink(ctx, column);
        grn_obj_unlink(ctx, grn_table_ref);
        DBUG_RETURN(false);
      }
      grn_obj_set_info(ctx, column_ref, GRN_INFO_SOURCE, &source_ids);
      grn_obj_unlink(ctx, &source_ids);
      grn_obj_unlink(ctx, column_ref);
      grn_obj_unlink(ctx, column);
      grn_obj_unlink(ctx, grn_table_ref);
      error = 0;
      DBUG_RETURN(true);
    }
    MRN_KEY_PART_SPEC_LIST_EACH_END();
  }
  MRN_KEY_SPEC_LIST_EACH_END();
  error = 0;
  DBUG_RETURN(false);
}

int ha_mroonga::storage_create_validate_index(TABLE* table)
{
  int error = 0;

  MRN_DBUG_ENTER_METHOD();
  const auto n_keys = table->s->keys;
  for (uint i = 0; i < n_keys; i++) {
    auto key = &(table->s->key_info[i]);
    error = storage_validate_key(key);
    if (error != 0) {
      break;
    }
  }

  DBUG_RETURN(error);
}

int ha_mroonga::storage_validate_key(KEY* key)
{
  int error = 0;

  MRN_DBUG_ENTER_METHOD();
  const auto n_key_parts = KEY_N_KEY_PARTS(key);
  for (uint j = 0; j < n_key_parts; ++j) {
    auto key_part = &(key->key_part[j]);
    auto field = key_part->field;

    if (key_part->key_part_flag & HA_REVERSE_SORT) {
      error = ER_ILLEGAL_HA_CREATE_OPTION;
      my_message(error, "descending index isn't supported yet", MYF(0));
      DBUG_RETURN(error);
    }

    // checking if index is used for virtual columns
    if (FIELD_NAME_EQUAL(field, MRN_COLUMN_NAME_ID)) {
      // must be single column key
      if (key->algorithm == HA_KEY_ALG_HASH && n_key_parts == 1) {
        continue; // hash index is ok
      }
      GRN_LOG(ctx, GRN_LOG_ERROR, "only hash index can be defined for _id");
      error = ER_CANT_CREATE_TABLE;
      my_message(error, "only hash index can be defined for _id", MYF(0));
      DBUG_RETURN(error);
    }
  }

  DBUG_RETURN(error);
}

bool ha_mroonga::find_lexicon_flags(KEY* key, grn_table_flags* lexicon_flags)
{
  MRN_DBUG_ENTER_METHOD();
  bool found = false;

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  {
    const char* flags = key->option_struct->lexicon_flags;
    if (flags) {
      found = mrn_parse_grn_lexicon_flags(ha_thd(),
                                          ctx,
                                          flags,
                                          strlen(flags),
                                          lexicon_flags);
      DBUG_RETURN(found);
    }
  }
#endif

  if (key->comment.length > 0) {
    mrn::ParametersParser parser(key->comment.str, key->comment.length);
    const char* flags = parser["lexicon_flags"];
    if (flags) {
      found = mrn_parse_grn_lexicon_flags(ha_thd(),
                                          ctx,
                                          flags,
                                          strlen(flags),
                                          lexicon_flags);
    }
  }

  DBUG_RETURN(found);
}

int ha_mroonga::storage_create_index_table(TABLE* table,
                                           const char* grn_table_name,
                                           grn_obj* grn_table,
                                           KEY* key_info,
                                           grn_obj** index_tables,
                                           uint i)
{
  MRN_DBUG_ENTER_METHOD();

  const char* lexicon_name = NULL;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (key_info->option_struct) {
    lexicon_name = key_info->option_struct->lexicon;
  }
#endif
  mrn::ParametersParser parser(key_info->comment.str, key_info->comment.length);
  if (!lexicon_name) {
    lexicon_name = parser.lexicon();
  }
  if (lexicon_name) {
    grn_obj* index_table = grn_ctx_get(ctx, lexicon_name, -1);
    if (!index_table) {
      int error = ER_CANT_CREATE_TABLE;
      char error_message[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(error_message,
               MRN_MESSAGE_BUFFER_SIZE,
               "mroonga: storage: specified table doesn't exist: <%s>",
               lexicon_name);
      my_message(error, error_message, MYF(0));
      DBUG_RETURN(error);
    }
    index_tables[i] = index_table;
    DBUG_RETURN(0);
  }

  grn_obj* index_type;
  grn_obj* index_table;
  grn_table_flags index_table_flags = GRN_OBJ_PERSISTENT;
  bool is_multiple_column_index = KEY_N_KEY_PARTS(key_info) > 1;
  if (is_multiple_column_index) {
    index_type = grn_ctx_at(ctx, GRN_DB_SHORT_TEXT);
  } else {
    Field* field = key_info->key_part[0].field;
    grn_builtin_type groonga_type = mrn_grn_type_from_field(ctx, field, true);
    index_type = grn_ctx_at(ctx, groonga_type);
  }
  // TODO: Add NULL check for index_type

  int key_alg = key_info->algorithm;
  if (key_alg == HA_KEY_ALG_FULLTEXT) {
    index_table_flags |= GRN_OBJ_TABLE_PAT_KEY;
    int error = mrn_change_encoding(ctx, key_info->key_part->field->charset());
    if (error) {
      grn_obj_remove(ctx, grn_table);
      DBUG_RETURN(error);
    }
  } else if (key_alg == HA_KEY_ALG_HASH) {
    index_table_flags |= GRN_OBJ_TABLE_HASH_KEY;
  } else {
    index_table_flags |= GRN_OBJ_TABLE_PAT_KEY;
  }
  find_lexicon_flags(key_info, &index_table_flags);

  {
    mrn::IndexTableName index_table_name(grn_table_name, KEY_NAME(key_info));
    index_table = grn_table_create(ctx,
                                   index_table_name.c_str(),
                                   index_table_name.length(),
                                   NULL,
                                   index_table_flags,
                                   index_type,
                                   NULL);
  }
  if (ctx->rc) {
    grn_obj_unlink(ctx, index_type);
    grn_obj_remove(ctx, grn_table);
    int error = ER_CANT_CREATE_TABLE;
    my_message(ER_CANT_CREATE_TABLE, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }

  if (key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
    set_tokenizer(index_table, key_info);
    set_token_filters(index_table, key_info);
  }

  {
    Field* field = &(key_info->key_part->field[0]);
    if (key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
      if (have_custom_normalizer(key_info) || should_normalize(field, true)) {
        set_normalizer(index_table, key_info);
      }
    } else if (key_alg != HA_KEY_ALG_HASH) {
      if (!is_multiple_column_index && (have_custom_normalizer(key_info) ||
                                        should_normalize(field, false))) {
        set_normalizer(index_table, key_info);
      }
    }
  }

  index_tables[i] = index_table;

  DBUG_RETURN(0);
}

int ha_mroonga::storage_create_index(TABLE* table,
                                     const char* grn_table_name,
                                     grn_obj* grn_table,
                                     KEY* key_info,
                                     grn_obj** index_tables,
                                     grn_obj** index_columns,
                                     uint i)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  grn_obj* index_column;

  bool is_multiple_column_index = KEY_N_KEY_PARTS(key_info) > 1;
  if (!is_multiple_column_index) {
    Field* field = key_info->key_part[0].field;
    if (FIELD_NAME_EQUAL(field, MRN_COLUMN_NAME_ID)) {
      // skipping _id virtual column
      DBUG_RETURN(0);
    }

    if (is_foreign_key_field(table->s->table_name.str, field->field_name)) {
      DBUG_RETURN(0);
    }

#ifdef HA_CAN_VIRTUAL_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      char error_message[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(error_message,
               MRN_MESSAGE_BUFFER_SIZE,
               "mroonga: storage: failed to create "
               "index: " ER_MRN_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN_STR
               ": " FIELD_NAME_FORMAT,
               FIELD_NAME_FORMAT_VALUE(field));
      error = ER_MRN_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN_NUM;
      my_message(error, error_message, MYF(0));
      DBUG_RETURN(error);
    }
  } else {
    int j, n_key_parts = KEY_N_KEY_PARTS(key_info);
    for (j = 0; j < n_key_parts; j++) {
      Field* field = key_info->key_part[j].field;
      if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
        char error_message[MRN_MESSAGE_BUFFER_SIZE];
        snprintf(error_message,
                 MRN_MESSAGE_BUFFER_SIZE,
                 "mroonga: storage: failed to create "
                 "index: " ER_MRN_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN_STR
                 ": " FIELD_NAME_FORMAT,
                 FIELD_NAME_FORMAT_VALUE(field));
        error = ER_MRN_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN_NUM;
        my_message(error, error_message, MYF(0));
        DBUG_RETURN(error);
      }
    }
#endif
  }

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  error = storage_create_index_table(table,
                                     grn_table_name,
                                     grn_table,
                                     key_info,
                                     index_tables,
                                     i);
  if (error)
    DBUG_RETURN(error);

  grn_obj* index_table = index_tables[i];

  grn_column_flags index_column_flags =
    GRN_OBJ_COLUMN_INDEX | GRN_OBJ_PERSISTENT;

  if (!find_index_column_flags(key_info, &index_column_flags)) {
    grn_obj* tokenizer =
      grn_obj_get_info(ctx, index_table, GRN_INFO_DEFAULT_TOKENIZER, NULL);
    if (tokenizer) {
      index_column_flags |= GRN_OBJ_WITH_POSITION;
    }
    if (is_multiple_column_index &&
        key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
      index_column_flags |= GRN_OBJ_WITH_SECTION;
    }
  }

  const char* index_column_name;
  size_t index_column_name_length;
  const char* lexicon_name = NULL;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (key_info->option_struct) {
    lexicon_name = key_info->option_struct->lexicon;
  }
#endif
  mrn::ParametersParser parser(key_info->comment.str, key_info->comment.length);
  if (!lexicon_name) {
    lexicon_name = parser.lexicon();
  }
  if (lexicon_name) {
    index_column_name = KEY_NAME_PTR(key_info);
    index_column_name_length = KEY_NAME_LENGTH(key_info);
  } else {
    index_column_name = INDEX_COLUMN_NAME;
    index_column_name_length = strlen(index_column_name);
  }
  index_column = grn_column_create(ctx,
                                   index_table,
                                   index_column_name,
                                   index_column_name_length,
                                   NULL,
                                   index_column_flags,
                                   grn_table);

  if (ctx->rc) {
    error = ER_CANT_CREATE_TABLE;
    my_message(error, ctx->errbuf, MYF(0));
    grn_obj_remove(ctx, index_table);
    DBUG_RETURN(error);
  }

  mrn_change_encoding(ctx, system_charset_info);
  if (is_multiple_column_index) {
    if (key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
      grn_obj source_ids;
      GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);

      int j, n_key_parts = KEY_N_KEY_PARTS(key_info);
      for (j = 0; j < n_key_parts; j++) {
        Field* field = key_info->key_part[j].field;
        mrn::ColumnName column_name(FIELD_NAME(field));
        grn_obj* source_column = grn_obj_column(ctx,
                                                grn_table,
                                                column_name.c_str(),
                                                column_name.length());
        grn_id source_id = grn_obj_id(ctx, source_column);
        GRN_UINT32_PUT(ctx, &source_ids, source_id);
        grn_obj_unlink(ctx, source_column);
      }
      mrn_change_encoding(ctx, key_info->key_part->field->charset());
      grn_obj_set_info(ctx, index_column, GRN_INFO_SOURCE, &source_ids);
      grn_obj_unlink(ctx, &source_ids);
    }
  } else {
    Field* field = key_info->key_part[0].field;
    mrn::ColumnName column_name(FIELD_NAME(field));
    grn_obj* column;
    column =
      grn_obj_column(ctx, grn_table, column_name.c_str(), column_name.length());
    if (column) {
      grn_obj source_ids;
      grn_id source_id = grn_obj_id(ctx, column);
      GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
      GRN_UINT32_PUT(ctx, &source_ids, source_id);
      mrn_change_encoding(ctx, key_info->key_part->field->charset());
      grn_obj_set_info(ctx, index_column, GRN_INFO_SOURCE, &source_ids);
      grn_obj_unlink(ctx, &source_ids);
      grn_obj_unlink(ctx, column);
    }
  }
  mrn_change_encoding(ctx, system_charset_info);

  if (index_columns) {
    index_columns[i] = index_column;
  }

  DBUG_RETURN(error);
}

int ha_mroonga::storage_create_indexes(TABLE* table,
                                       const char* grn_table_name,
                                       grn_obj* grn_table,
                                       MRN_SHARE* tmp_share)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  uint n_keys = table->s->keys;
  uint i;
  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*, index_tables, n_keys);
  for (i = 0; i < n_keys; i++) {
    index_tables[i] = NULL;
    if (i == table->s->primary_key) {
      continue; // pkey is already handled
    }
    KEY* key_info = &table->s->key_info[i];
    if (tmp_share->disable_keys && !(key_info->flags & HA_NOSAME)) {
      continue; // key is disabled
    }
    if ((error = storage_create_index(table,
                                      grn_table_name,
                                      grn_table,
                                      key_info,
                                      index_tables,
                                      NULL,
                                      i))) {
      break;
    }
  }
  if (error) {
    for (uint j = 0; j <= i; ++j) {
      if (!index_tables[j]) {
        continue;
      }

      KEY* key_info = &table->s->key_info[j];
      const char* lexicon_name = NULL;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
      if (key_info->option_struct) {
        lexicon_name = key_info->option_struct->lexicon;
      }
#endif
      mrn::ParametersParser parser(key_info->comment.str,
                                   key_info->comment.length);
      if (!lexicon_name) {
        lexicon_name = parser.lexicon();
      }
      if (!lexicon_name) {
        grn_obj_remove(ctx, index_tables[j]);
      }
    }
  }
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
  DBUG_RETURN(error);
}

int ha_mroonga::ensure_database_open(const char* name, mrn::Database** db)
{
  int error;

  MRN_DBUG_ENTER_METHOD();

  if (db)
    *db = NULL;

  mrn::Database* local_db;
  error = mrn_db_manager->open(name, &local_db);
  if (error)
    DBUG_RETURN(error);

  if (db)
    *db = local_db;
  grn_ctx_use(ctx, local_db->get());

  delete operations_;
  operations_ = new mrn::Operations(ctx);
  if (mrn_enable_operations_recording) {
    operations_->enable_recording();
  } else {
    operations_->disable_recording();
  }

  DBUG_RETURN(error);
}

int ha_mroonga::ensure_database_remove(const char* name)
{
  int error;

  MRN_DBUG_ENTER_METHOD();

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  delete operations_;
  operations_ = NULL;

  mrn_db_manager->close(name);

  mrn::PathMapper mapper(name);
  remove_related_files(mapper.db_path());

  DBUG_RETURN(error);
}

int ha_mroonga::create(const char* name,
                       TABLE* table,
                       HA_CREATE_INFO* info
#ifdef MRN_HANDLER_CREATE_HAVE_TABLE_DEFINITION
                       ,
                       dd::Table* table_def
#endif
)
{
  int error = 0;
  MRN_SHARE* tmp_share;
  MRN_DBUG_ENTER_METHOD();
  /* checking data type of virtual columns */

  if (!(tmp_share = mrn_get_share(name, table, &error)))
    DBUG_RETURN(error);

  mrn::SlotData* slot_data = mrn_get_slot_data(ha_thd(), false);
  if (slot_data && slot_data->disable_keys_create_info == info) {
    tmp_share->disable_keys = true;
  }

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (tmp_share->wrapper_mode) {
#  ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
    error = wrapper_create(name, table, info, table_def, tmp_share);
#  else
    error = wrapper_create(name, table, info, tmp_share);
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
    error = storage_create(name, table, info, table_def, tmp_share);
#else
  error = storage_create(name, table, info, tmp_share);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  if (error) {
    mrn_free_long_term_share(tmp_share->long_term_share);
    tmp_share->long_term_share = NULL;
#ifdef MRN_ENABLE_WRAPPER_MODE
  } else {
    error = add_wrap_hton(tmp_share->table_name, tmp_share->hton);
#endif
  }
  mrn_free_share(tmp_share);
  DBUG_RETURN(error);
}

#ifdef MRN_HANDLER_HAVE_GET_SE_PRIVATE_DATA
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_get_se_private_data(dd::Table* dd_table, bool reset)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  bool errored = wrap_handler->get_se_private_data(dd_table, reset);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(errored);
}
#  endif

bool ha_mroonga::storage_get_se_private_data(dd::Table* dd_table, bool reset)
{
  MRN_DBUG_ENTER_METHOD();
  bool errored = handler::get_se_private_data(dd_table, reset);
  DBUG_RETURN(errored);
}

bool ha_mroonga::get_se_private_data(dd::Table* dd_table, bool reset)
{
  MRN_DBUG_ENTER_METHOD();

  bool errored;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    errored = wrapper_get_se_private_data(dd_table, reset);
  } else {
#  endif
    errored = storage_get_se_private_data(dd_table, reset);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif

  DBUG_RETURN(errored);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_open(const char* name,
                             int mode,
                             uint open_options
#  ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
                             ,
                             const dd::Table* table_def
#  endif
)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  mrn::Database* db = NULL;
  error = ensure_database_open(name, &db);
  if (error)
    goto exit;

  error = open_table(name);
  if (error && !(open_options & HA_OPEN_FOR_REPAIR))
    goto exit;

  error = wrapper_open_indexes(name);
  if (error && !(open_options & HA_OPEN_FOR_REPAIR))
    goto exit;

  error = 0;

#  ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (parse_engine_table_options(ha_thd(),
                                 share->hton,
                                 share->wrap_table_share)) {
    error = my_errno;
    goto exit;
  }
#  endif

  mrn_init_alloc_root(&mem_root, "mroonga::wrapper", 1024, 0, MYF(0));
  wrap_key_info = mrn_create_key_info_for_table(share, table, &error);
  if (error)
    DBUG_RETURN(error);
  base_key_info = table->key_info;

  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (!is_clone) {
    wrap_handler = mrn_get_new_handler(table->s,
                                       table->s->m_part_info != NULL,
                                       &mem_root,
                                       share->hton);
    if (!wrap_handler) {
      MRN_SET_BASE_SHARE_KEY(share, table->s);
      MRN_SET_BASE_TABLE_KEY(this, table);
      if (wrap_key_info) {
        my_free(wrap_key_info);
        wrap_key_info = NULL;
      }
      base_key_info = NULL;
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    wrap_handler->set_ha_share_ref(&table->s->ha_share);
#  ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
    error = wrap_handler->ha_open(table, name, mode, open_options, table_def);
#  else
    error = wrap_handler->ha_open(table, name, mode, open_options);
#  endif
  } else {
    if (!(wrap_handler =
            parent_for_clone->wrap_handler->clone(name, mem_root_for_clone))) {
      MRN_SET_BASE_SHARE_KEY(share, table->s);
      MRN_SET_BASE_TABLE_KEY(this, table);
      if (wrap_key_info) {
        my_free(wrap_key_info);
        wrap_key_info = NULL;
      }
      base_key_info = NULL;
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }
  ref_length = wrap_handler->ref_length;
  key_used_on_scan = wrap_handler->key_used_on_scan;
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  init();
  wrapper_overwrite_index_bits();
  wrapper_set_keys_in_use();

  pk_keypart_map = make_prev_keypart_map(
    KEY_N_KEY_PARTS(&(table->key_info[table_share->primary_key])));

  if (!error) {
    if (open_options & HA_OPEN_FOR_REPAIR) {
      // TODO: How to check whether is DISABLE KEYS used or not?
      error = wrapper_recreate_indexes(ha_thd());
    } else if (db) {
      mrn::Lock lock(&mrn_operations_mutex);
      mrn::PathMapper mapper(name);
      const char* table_name = mapper.table_name();
      size_t table_name_size = strlen(table_name);
      if (db->is_broken_table(table_name, table_name_size)) {
        GRN_LOG(ctx, GRN_LOG_NOTICE, "Auto repair is started: <%s>", name);
        error = operations_->clear(table_name, table_name_size);
        if (!error) {
          db->mark_table_repaired(table_name, table_name_size);
          if (!share->disable_keys) {
            // TODO: implemented by "reindex" instead of "remove and recreate".
            // Because "remove and recreate" invalidates opened indexes by
            // other threads.
            error = wrapper_disable_indexes_mroonga(
              MRN_HANDLER_DISABLE_INDEXES_ALL_ARGS);
            if (!error) {
              error = wrapper_enable_indexes_mroonga(
                MRN_HANDLER_ENABLE_INDEXES_ALL_ARGS);
            }
          }
        }
        GRN_LOG(ctx,
                GRN_LOG_NOTICE,
                "Auto repair is done: <%s>: %s",
                name,
                error == 0 ? "success" : "failure");
      }
    }
  }

exit:
  if (error) {
    if (grn_table) {
      grn_obj_unlink(ctx, grn_table);
      grn_table = NULL;
    }

    if (grn_index_columns) {
      for (uint i = 0; i < table->s->keys; ++i) {
        if (grn_index_columns[i]) {
          grn_obj_unlink(ctx, grn_index_columns[i]);
        }
      }
      grn_index_columns = NULL;
    }

    if (grn_index_tables) {
      for (uint i = 0; i < table->s->keys; ++i) {
        if (grn_index_tables[i]) {
          grn_obj_unlink(ctx, grn_index_tables[i]);
        }
      }
    }

    if (wrap_handler) {
      mrn_destroy(wrap_handler);
      wrap_handler = NULL;
    }
    if (wrap_key_info) {
      my_free(wrap_key_info);
      wrap_key_info = NULL;
    }
    base_key_info = NULL;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_open_indexes(const char* name)
{
  int error;

  MRN_DBUG_ENTER_METHOD();

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  uint n_keys = table->s->keys;
  uint n_primary_keys = table->s->primary_key;
  if (n_keys > 0) {
    // TODO: reduce allocate memories. We only need just
    // for HA_KEY_ALG_FULLTEXT keys.
    grn_index_tables = (grn_obj**)malloc(sizeof(grn_obj*) * n_keys);
    grn_index_columns = (grn_obj**)malloc(sizeof(grn_obj*) * n_keys);
  } else {
    grn_index_tables = grn_index_columns = NULL;
  }

  mrn::PathMapper mapper(name);
  uint i = 0;
  for (i = 0; i < n_keys; i++) {
    KEY* key_info = &(table->s->key_info[i]);

    grn_index_tables[i] = NULL;
    grn_index_columns[i] = NULL;

    if (!(wrapper_is_target_index(key_info))) {
      continue;
    }

    if (i == n_primary_keys) {
      continue;
    }

    mrn::IndexTableName index_table_name(mapper.table_name(),
                                         KEY_NAME(key_info));
    grn_index_tables[i] =
      grn_ctx_get(ctx, index_table_name.c_str(), index_table_name.length());
    if (ctx->rc == GRN_SUCCESS && !grn_index_tables[i]) {
      grn_index_tables[i] = grn_ctx_get(ctx,
                                        index_table_name.old_c_str(),
                                        index_table_name.old_length());
    }
    if (ctx->rc) {
      DBUG_PRINT("info",
                 ("mroonga: sql_command=%u", thd_sql_command(ha_thd())));
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      goto error;
    }

    grn_index_columns[i] = grn_obj_column(ctx,
                                          grn_index_tables[i],
                                          INDEX_COLUMN_NAME,
                                          strlen(INDEX_COLUMN_NAME));
    if (!grn_index_columns[i]) {
      /* just for backward compatibility before 1.0. */
      Field* field = key_info->key_part[0].field;
      grn_index_columns[i] =
        grn_obj_column(ctx, grn_index_tables[i], FIELD_NAME(field));
    }

    if (ctx->rc) {
      DBUG_PRINT("info",
                 ("mroonga: sql_command=%u", thd_sql_command(ha_thd())));
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      grn_obj_unlink(ctx, grn_index_tables[i]);
      goto error;
    }
  }

  grn_bulk_space(ctx, &key_buffer, table->key_info->key_length);

error:
  if (error) {
    while (i-- > 0) {
      grn_obj* index_column = grn_index_columns[i];
      if (index_column) {
        grn_obj_unlink(ctx, index_column);
      }
      grn_obj* index_table = grn_index_tables[i];
      if (index_table) {
        grn_obj_unlink(ctx, index_table);
      }
    }
    free(grn_index_columns);
    free(grn_index_tables);
    grn_index_columns = NULL;
    grn_index_tables = NULL;
  }

  DBUG_RETURN(error);
}

void ha_mroonga::wrapper_overwrite_index_bits()
{
  uint i, j;
  longlong table_option = table_flags();
  MRN_DBUG_ENTER_METHOD();
  table_share->keys_for_keyread.clear_all();
  for (i = 0; i < table_share->fields; i++) {
    Field* field = table_share->field[i];
    field->part_of_key.clear_all();
    field->part_of_sortkey.clear_all();
    /*
      TODO: We may need to update field->part_of_key_not_extended for
      MySQL >= 5.7.18. If users report "raw InnoDB can use index for
      this case but Mroonga wrapper mode for InnoDB can't use index
      for the same case", we'll reconsider it again.
    */
  }
  for (i = 0; i < table_share->keys; i++) {
    KEY* key_info = &table->s->key_info[i];
    KEY_PART_INFO* key_part = key_info->key_part;
    for (j = 0; j < KEY_N_KEY_PARTS(key_info); key_part++, j++) {
      Field* field = key_part->field;
      if (field->key_length() == key_part->length &&
          !(MRN_FIELD_ALL_FLAGS(field) & BLOB_FLAG)) {
        if (index_flags(i, j, 0) & HA_KEYREAD_ONLY) {
          table_share->keys_for_keyread.set_bit(i);
          field->part_of_key.set_bit(i);
        }
        if (index_flags(i, j, 1) & HA_READ_ORDER)
          field->part_of_sortkey.set_bit(i);
      }
      if (i == table_share->primary_key &&
          (table_option & HA_PRIMARY_KEY_IN_READ_INDEX)) {
        if (field->key_length() == key_part->length &&
            !(MRN_FIELD_ALL_FLAGS(field) & BLOB_FLAG))
          field->part_of_key = table_share->keys_in_use;
        if (field->part_of_sortkey.is_set(i))
          field->part_of_sortkey = table_share->keys_in_use;
      }
    }
  }
  DBUG_VOID_RETURN;
}
#endif

int ha_mroonga::storage_reindex()
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  uint n_keys = table_share->keys;
  KEY* key_info = table->key_info;

  bool have_multiple_column_index = false;
  bitmap_clear_all(table->read_set);
  for (uint i = 0; i < n_keys; ++i) {
    if (!grn_index_columns[i])
      continue;

    grn_hash* columns = grn_hash_create(ctx,
                                        NULL,
                                        sizeof(grn_id),
                                        0,
                                        GRN_OBJ_TABLE_HASH_KEY | GRN_HASH_TINY);
    grn_table_columns(ctx,
                      grn_index_tables[i],
                      NULL,
                      0,
                      reinterpret_cast<grn_obj*>(columns));
    unsigned int n_columns =
      grn_table_size(ctx, reinterpret_cast<grn_obj*>(columns));
    grn_hash_close(ctx, columns);

    bool is_multiple_column_index =
      (KEY_N_KEY_PARTS(&(key_info[i])) != 1 &&
       key_info[i].algorithm != HA_KEY_ALG_FULLTEXT);

    if (n_columns == 1 || is_multiple_column_index) {
      grn_table_truncate(ctx, grn_index_tables[i]);
      if (ctx->rc != GRN_SUCCESS) {
        error = ER_ERROR_ON_WRITE;
        char error_message[MRN_MESSAGE_BUFFER_SIZE];
        char index_table_name[GRN_TABLE_MAX_KEY_SIZE];
        int index_table_name_size = grn_obj_name(ctx,
                                                 grn_index_tables[i],
                                                 index_table_name,
                                                 GRN_TABLE_MAX_KEY_SIZE);
        snprintf(error_message,
                 MRN_MESSAGE_BUFFER_SIZE,
                 "mroonga: reindex: failed to truncate index table: "
                 "<%.*s>: <%s>(%d)",
                 index_table_name_size,
                 index_table_name,
                 ctx->errbuf,
                 ctx->rc);
        my_message(error, error_message, MYF(0));
        break;
      }
    }

    if (is_multiple_column_index) {
      mrn_set_bitmap_by_key(table->read_set, &key_info[i]);
      have_multiple_column_index = true;
    } else {
      grn_obj_reindex(ctx, grn_index_columns[i]);
      if (ctx->rc != GRN_SUCCESS) {
        error = ER_ERROR_ON_WRITE;
        char error_message[MRN_MESSAGE_BUFFER_SIZE];
        char index_column_name[GRN_TABLE_MAX_KEY_SIZE];
        int index_column_name_size = grn_obj_name(ctx,
                                                  grn_index_columns[i],
                                                  index_column_name,
                                                  GRN_TABLE_MAX_KEY_SIZE);
        snprintf(error_message,
                 MRN_MESSAGE_BUFFER_SIZE,
                 "mroonga: reindex: failed to reindex: "
                 "<%.*s>: <%s>(%d)",
                 index_column_name_size,
                 index_column_name,
                 ctx->errbuf,
                 ctx->rc);
        my_message(error, error_message, MYF(0));
        break;
      }
    }
  }

  if (!error && have_multiple_column_index)
    error = storage_add_index_multiple_columns(table,
                                               key_info,
                                               n_keys,
                                               grn_index_tables,
                                               grn_index_columns,
                                               false);
  bitmap_set_all(table->read_set);

  DBUG_RETURN(error);
}

int ha_mroonga::storage_open(const char* name,
                             int mode,
                             uint open_options
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
                             ,
                             const dd::Table* table_def
#endif
)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  mrn::Database* db;
  error = ensure_database_open(name, &db);
  if (error)
    DBUG_RETURN(error);

  error = open_table(name);
  if (error)
    DBUG_RETURN(error);

  error = storage_open_columns();
  if (error) {
    grn_obj_unlink(ctx, grn_table);
    grn_table = NULL;
    DBUG_RETURN(error);
  }
  error = storage_open_indexes(name);
  if (error && !(open_options & HA_OPEN_FOR_REPAIR)) {
    storage_close_columns();
    grn_obj_unlink(ctx, grn_table);
    grn_table = NULL;
    DBUG_RETURN(error);
  }

  storage_set_keys_in_use();

  if (!(open_options & HA_OPEN_FOR_REPAIR)) {
    mrn::Lock lock(&mrn_operations_mutex);
    mrn::PathMapper mapper(name);
    const char* table_name = mapper.table_name();
    size_t table_name_size = strlen(table_name);
    if (db->is_broken_table(table_name, table_name_size)) {
      GRN_LOG(ctx, GRN_LOG_NOTICE, "Auto repair is started: <%s>", name);
      error = operations_->repair(table_name, table_name_size);
      if (!error)
        db->mark_table_repaired(table_name, table_name_size);
      if (!share->disable_keys) {
        if (!error)
          error = storage_reindex();
      }
      GRN_LOG(ctx,
              GRN_LOG_NOTICE,
              "Auto repair is done: <%s>: %s",
              name,
              error == 0 ? "success" : "failure");
    }
  }

  ref_length = sizeof(grn_id);
  DBUG_RETURN(0);
}

int ha_mroonga::open_table(const char* name)
{
  int error;
  MRN_DBUG_ENTER_METHOD();

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  mrn::PathMapper mapper(name);
  grn_table =
    grn_ctx_get(ctx, mapper.table_name(), strlen(mapper.table_name()));
  if (ctx->rc) {
    error = ER_CANT_OPEN_FILE;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  if (!grn_table) {
    error = ER_CANT_OPEN_FILE;
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "mroonga: failed to open table: <%s>",
             mapper.table_name());
    my_message(error, error_message, MYF(0));
    DBUG_RETURN(error);
  }

  DBUG_RETURN(0);
}

int ha_mroonga::storage_open_columns(void)
{
  int error;
  MRN_DBUG_ENTER_METHOD();

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  int n_columns = table->s->fields;
  grn_columns = (grn_obj**)malloc(sizeof(grn_obj*) * n_columns);
  grn_column_caches = static_cast<grn_column_cache**>(
    malloc(sizeof(grn_column_cache*) * n_columns));
  grn_column_ranges = (grn_obj**)malloc(sizeof(grn_obj*) * n_columns);
  for (int i = 0; i < n_columns; i++) {
    grn_columns[i] = NULL;
    grn_column_caches[i] = NULL;
    grn_column_ranges[i] = NULL;
  }

  if (table_share->blob_fields) {
    blob_buffers_.resize(n_columns);
  } else {
    blob_buffers_.resize(0);
  }

  for (int i = 0; i < n_columns; i++) {
    Field* field = table->field[i];
    mrn::ColumnName column_name(FIELD_NAME(field));
    if (strcmp(MRN_COLUMN_NAME_ID, column_name.mysql_name()) == 0) {
      continue;
    }
#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif

    grn_columns[i] =
      grn_obj_column(ctx, grn_table, column_name.c_str(), column_name.length());
    if (!grn_columns[i]) {
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      break;
    }

    grn_column_caches[i] = grn_column_cache_open(ctx, grn_columns[i]);

    grn_id range_id = grn_obj_get_range(ctx, grn_columns[i]);
    grn_column_ranges[i] = grn_ctx_at(ctx, range_id);
    if (!grn_column_ranges[i]) {
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      break;
    }
  }

  if (error != 0) {
    storage_close_columns();
  }

  DBUG_RETURN(error);
}

void ha_mroonga::storage_close_columns(void)
{
  int n_columns = table->s->fields;
  for (int i = 0; i < n_columns; i++) {
    grn_column_cache* column_cache = grn_column_caches[i];
    if (column_cache) {
      grn_column_cache_close(ctx, column_cache);
    }

    grn_obj* column = grn_columns[i];
    if (column) {
      grn_obj_unlink(ctx, column);
    }

    grn_obj* range = grn_column_ranges[i];
    if (range) {
      grn_obj_unlink(ctx, range);
    }
  }

  free(grn_columns);
  grn_columns = NULL;
  free(grn_column_caches);
  grn_column_caches = NULL;
  free(grn_column_ranges);
  grn_column_ranges = NULL;
}

int ha_mroonga::storage_open_indexes(const char* name)
{
  int error;

  MRN_DBUG_ENTER_METHOD();

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  uint n_keys = table->s->keys;
  uint pkey_nr = table->s->primary_key;
  if (n_keys > 0) {
    grn_index_tables = (grn_obj**)malloc(sizeof(grn_obj*) * n_keys);
    grn_index_columns = (grn_obj**)malloc(sizeof(grn_obj*) * n_keys);
    key_id = (grn_id*)malloc(sizeof(grn_id) * n_keys);
    del_key_id = (grn_id*)malloc(sizeof(grn_id) * n_keys);
  } else {
    grn_index_tables = grn_index_columns = NULL;
    key_id = NULL;
    del_key_id = NULL;
  }

  mrn::PathMapper mapper(name);
  uint i, j;
  for (i = 0; i < n_keys; i++) {
    if (i == pkey_nr) {
      grn_index_tables[i] = grn_index_columns[i] = NULL;
      continue;
    }

    KEY* key_info = &(table->s->key_info[i]);
    if (KEY_N_KEY_PARTS(key_info) > 1) {
      KEY_PART_INFO* key_part = key_info->key_part;
      for (j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
        bitmap_set_bit(&multiple_column_key_bitmap,
                       MRN_FIELD_FIELD_INDEX(key_part[j].field));
      }
    }

    const char* lexicon_name = NULL;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
    if (key_info->option_struct) {
      lexicon_name = key_info->option_struct->lexicon;
    }
#endif
    mrn::ParametersParser parser(key_info->comment.str,
                                 key_info->comment.length);
    if (!lexicon_name) {
      lexicon_name = parser.lexicon();
    }
    if (lexicon_name) {
      grn_index_tables[i] = grn_ctx_get(ctx, lexicon_name, -1);
      if (ctx->rc == GRN_SUCCESS) {
        grn_index_columns[i] =
          grn_obj_column(ctx, grn_index_tables[i], KEY_NAME(key_info));
      }
    } else {
      mrn::IndexTableName index_table_name(mapper.table_name(),
                                           KEY_NAME(key_info));
      grn_index_tables[i] =
        grn_ctx_get(ctx, index_table_name.c_str(), index_table_name.length());
      if (ctx->rc == GRN_SUCCESS && !grn_index_tables[i]) {
        grn_index_tables[i] = grn_ctx_get(ctx,
                                          index_table_name.old_c_str(),
                                          index_table_name.old_length());
      }
      if (ctx->rc == GRN_SUCCESS) {
        grn_index_columns[i] = grn_obj_column(ctx,
                                              grn_index_tables[i],
                                              INDEX_COLUMN_NAME,
                                              strlen(INDEX_COLUMN_NAME));
        if (!grn_index_columns[i] && ctx->rc == GRN_SUCCESS) {
          /* just for backward compatibility before 1.0. */
          Field* field = key_info->key_part[0].field;
          grn_index_columns[i] =
            grn_obj_column(ctx, grn_index_tables[i], FIELD_NAME(field));
        }
      }
    }
    if (ctx->rc) {
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      goto error;
    }

    if (ctx->rc) {
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      goto error;
    }
  }

error:
  if (error) {
    if (i) {
      while (true) {
        grn_obj* index_column = grn_index_columns[i];
        if (index_column) {
          grn_obj_unlink(ctx, index_column);
        }
        grn_obj* index_table = grn_index_tables[i];
        if (index_table) {
          grn_obj_unlink(ctx, index_table);
        }
        if (!i)
          break;
        i--;
      }
    }
    free(key_id);
    free(del_key_id);
    free(grn_index_columns);
    free(grn_index_tables);
    key_id = NULL;
    del_key_id = NULL;
    grn_index_columns = NULL;
    grn_index_tables = NULL;
  }

  DBUG_RETURN(error);
}

int ha_mroonga::open(const char* name,
                     int mode,
                     uint open_options
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
                     ,
                     const dd::Table* table_def
#endif
)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  if (!(share = mrn_get_share(name, table, &error)))
    DBUG_RETURN(error);
  thr_lock_data_init(&share->lock, &thr_lock_data, NULL);

  if (mrn_bitmap_init(&multiple_column_key_bitmap, NULL, table->s->fields)) {
    mrn_free_share(share);
    share = NULL;
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
#  ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
    error = wrapper_open(name, mode, open_options, table_def);
#  else
    error = wrapper_open(name, mode, open_options);
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
    error = storage_open(name, mode, open_options, table_def);
#else
  error = storage_open(name, mode, open_options);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  if (error) {
    mrn_bitmap_free(&multiple_column_key_bitmap);
    mrn_free_share(share);
    share = NULL;
  }
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_close()
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_close();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  mrn_destroy(wrap_handler);
  wrap_handler = NULL;
  if (wrap_key_info) {
    my_free(wrap_key_info);
    wrap_key_info = NULL;
  }
  base_key_info = NULL;
  MRN_FREE_ROOT(&mem_root);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_close()
{
  MRN_DBUG_ENTER_METHOD();
  storage_close_columns();
  grn_obj_unlink(ctx, grn_table);
  DBUG_RETURN(0);
}

int ha_mroonga::close()
{
  int error = 0;
  THD* thd = ha_thd();
  MRN_DBUG_ENTER_METHOD();

  clear_indexes();

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_close();
  } else {
#endif
    error = storage_close();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  if (error != 0) {
    DBUG_RETURN(error);
  }

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (thd) {
    error = add_wrap_hton(share->table_name, share->hton);
  }
#endif
  mrn_bitmap_free(&multiple_column_key_bitmap);
  mrn_free_share(share, true);
  share = NULL;
#ifdef MRN_ENABLE_WRAPPER_MODE
  is_clone = false;
#endif

  if (thd && thd_sql_command(thd) == SQLCOM_FLUSH) {
    /* flush tables */
    {
      mrn::Lock lock(&mrn_open_tables_mutex);
      if (grn_hash_size(&mrn_ctx, mrn_open_tables) == 0) {
        int tmp_error = mrn_db_manager->clear();
        if (tmp_error)
          error = tmp_error;
      }
    }
    {
      mrn_context_pool->clear();
    }
  }
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_delete_table(const char* name,
#  ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
                                     const dd::Table* table_def,
#  endif
                                     handlerton* wrap_handlerton,
                                     const char* table_name)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  handler* hnd =
    mrn_get_new_handler(NULL,
                        table_def->partition_type() != dd::Table::PT_NONE,
                        current_thd->mem_root,
                        wrap_handlerton);
  if (!hnd) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

#  ifdef MRN_HANDLER_DELETE_TABLE_HAVE_HTON_DROP_TABLE
  error = wrap_handlerton->drop_table(wrap_handlerton, name);
#  elif defined(MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION)
  error = hnd->ha_delete_table(name, table_def);
#  else
  error = hnd->ha_delete_table(name);
#  endif
  mrn_destroy(hnd);

  DBUG_RETURN(error);
}
#endif

int ha_mroonga::generic_delete_table(const char* name,
#ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
                                     const dd::Table* table_def,
#endif
                                     const char* table_name)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  if (!mrn_db_manager->exist(name))
    DBUG_RETURN(0);

  error = ensure_database_open(name);
  if (error)
    DBUG_RETURN(error);

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  error = drop_indexes(table_name);
  grn_obj* table_obj = grn_ctx_get(ctx, table_name, strlen(table_name));
  if (table_obj) {
    if (thd_sql_command(ha_thd()) == SQLCOM_DROP_DB) {
      grn_obj_remove_dependent(ctx, table_obj);
    } else {
      grn_obj_remove(ctx, table_obj);
    }
  }
  if (ctx->rc) {
    error = ER_CANT_OPEN_FILE;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  DBUG_RETURN(error);
}

int ha_mroonga::delete_table(const char* name
#ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
                             ,
                             const dd::Table* table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
  mrn::PathMapper mapper(name);
  handlerton* wrap_handlerton = NULL;
#ifdef MRN_ENABLE_WRAPPER_MODE
  mrn::SlotData* slot_data = mrn_get_slot_data(ha_thd(), false);
  if (slot_data && slot_data->first_wrap_hton) {
    st_mrn_wrap_hton *wrap_hton, *tmp_wrap_hton;
    tmp_wrap_hton = NULL;
    wrap_hton = slot_data->first_wrap_hton;
    while (wrap_hton) {
      if (!strcmp(wrap_hton->path, name)) {
        /* found */
        wrap_handlerton = wrap_hton->hton;
        if (tmp_wrap_hton)
          tmp_wrap_hton->next = wrap_hton->next;
        else
          slot_data->first_wrap_hton = wrap_hton->next;
        free(wrap_hton);
        break;
      }
      tmp_wrap_hton = wrap_hton;
      wrap_hton = wrap_hton->next;
    }
  }
#endif

  if (!wrap_handlerton) {
    bool open_table_to_get_wrap_handlerton = true;
    if (mapper.is_internal_table_name()) {
      open_table_to_get_wrap_handlerton = false;
    }
    if (open_table_to_get_wrap_handlerton) {
      MRN_DECLARE_TABLE_LIST(table_list,
                             mapper.db_name(),
                             strlen(mapper.db_name()),
                             mapper.mysql_table_name(),
                             strlen(mapper.mysql_table_name()),
                             mapper.mysql_table_name(),
                             TL_WRITE);
      mrn_open_mutex_lock(NULL);
#ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
      TABLE_SHARE* tmp_table_share =
        mrn_create_tmp_table_share(&table_list, name, table_def, &error);
#else
      TABLE_SHARE* tmp_table_share =
        mrn_create_tmp_table_share(&table_list, name, &error);
#endif
      error = 0;
      mrn_open_mutex_unlock(NULL);
      if (tmp_table_share) {
        TABLE tmp_table;
        tmp_table.s = tmp_table_share;
#ifdef WITH_PARTITION_STORAGE_ENGINE
        tmp_table.part_info = NULL;
#endif
        MRN_SHARE* tmp_share = mrn_get_share(name, &tmp_table, &error);
        if (tmp_share) {
#ifdef MRN_ENABLE_WRAPPER_MODE
          wrap_handlerton = tmp_share->hton;
#endif
          mrn_free_long_term_share(tmp_share->long_term_share);
          tmp_share->long_term_share = NULL;
          mrn_free_share(tmp_share);
        }
        mrn_open_mutex_lock(NULL);
        mrn_free_tmp_table_share(tmp_table_share);
        mrn_open_mutex_unlock(NULL);
        if (error) {
          DBUG_RETURN(error);
        }
      }
    }
  }

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (wrap_handlerton) {
#  ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
    error = wrapper_delete_table(name,
                                 table_def,
                                 wrap_handlerton,
                                 mapper.table_name());
#  else
    error = wrapper_delete_table(name, wrap_handlerton, mapper.table_name());
#  endif
  }
#endif

  if (!error) {
#ifdef MRN_HANDLER_DELETE_TABLE_HAVE_TABLE_DEFINITION
    error = generic_delete_table(name, table_def, mapper.table_name());
#else
    error = generic_delete_table(name, mapper.table_name());
#endif
  }

  if (!error && operations_) {
    error = operations_->clear(name, strlen(name));
  }

  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_set_keys_in_use()
{
  uint i, j;
  MRN_DBUG_ENTER_METHOD();
  mrn::AutoIncrementValueLock lock_(table_share);
  table_share->keys_in_use.set_prefix(table_share->keys);
  share->disable_keys = false;
  for (i = 0; i < table_share->keys; i++) {
    j = share->wrap_key_nr[i];
    if (j < MAX_KEY) {
      if (!share->wrap_table_share->keys_in_use.is_set(j)) {
        /* copy bitmap */
        table_share->keys_in_use.clear_bit(i);
        share->disable_keys = true;
      }
    } else {
      if (!grn_index_tables || !grn_index_tables[i]) {
        /* disabled */
        table_share->keys_in_use.clear_bit(i);
        share->disable_keys = true;
      }
    }
  }
  table_share->keys_for_keyread.set_prefix(table_share->keys);
  table_share->keys_for_keyread.intersect(table_share->keys_in_use);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_set_keys_in_use()
{
  uint i;
  MRN_DBUG_ENTER_METHOD();
  mrn::AutoIncrementValueLock lock_(table_share);
  table_share->keys_in_use.set_prefix(table_share->keys);
  share->disable_keys = false;
  for (i = 0; i < table_share->keys; i++) {
    if (i == table_share->primary_key) {
      continue;
    }
    if (!grn_index_tables || !grn_index_tables[i]) {
      /* disabled */
      table_share->keys_in_use.clear_bit(i);
      DBUG_PRINT("info", ("mroonga: key %u disabled", i));
      share->disable_keys = true;
    }
  }
  table_share->keys_for_keyread.set_prefix(table_share->keys);
  table_share->keys_for_keyread.intersect(table_share->keys_in_use);
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_info(uint flag)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->info(flag);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  if (flag & HA_STATUS_ERRKEY) {
    errkey = wrap_handler->errkey;
    memcpy(dup_ref, wrap_handler->dup_ref, wrap_handler->ref_length);
  }
  if (flag & HA_STATUS_TIME) {
    stats.update_time = wrap_handler->stats.update_time;
  }
  if (flag & HA_STATUS_CONST) {
    stats.max_data_file_length = wrap_handler->stats.max_data_file_length;
    stats.create_time = wrap_handler->stats.create_time;
    stats.block_size = wrap_handler->stats.block_size;
    wrapper_set_keys_in_use();
  }
  if (flag & HA_STATUS_VARIABLE) {
    stats.data_file_length = wrap_handler->stats.data_file_length;
    stats.index_file_length = wrap_handler->stats.index_file_length;
    stats.records = wrap_handler->stats.records;
    stats.mean_rec_length = wrap_handler->stats.mean_rec_length;
    stats.check_time = wrap_handler->stats.check_time;
  }
  if (flag & HA_STATUS_AUTO) {
    stats.auto_increment_value = wrap_handler->stats.auto_increment_value;
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_info(uint flag)
{
  MRN_DBUG_ENTER_METHOD();
  mrn_change_encoding(ctx, NULL);

  if (flag & (HA_STATUS_ERRKEY | HA_STATUS_NO_LOCK)) {
    errkey = dup_key;
  }

  if ((flag & HA_STATUS_AUTO) && table->found_next_number_field) {
    THD* thd = ha_thd();
    ulonglong nb_reserved_values;
    bool next_number_field_is_null = !table->next_number_field;
    mrn::ExternalLock mrn_external_lock(ha_thd(),
                                        this,
                                        mrn_lock_type == F_UNLCK ? F_RDLCK
                                                                 : F_UNLCK);
    if (mrn_external_lock.error()) {
      DBUG_RETURN(mrn_external_lock.error());
    }
    if (next_number_field_is_null) {
      table->next_number_field = table->found_next_number_field;
    }
    MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
    {
      mrn::Lock lock(&long_term_share->auto_inc_mutex);
      unsigned long auto_increment_offset, auto_increment_increment;
      MRN_THD_GET_AUTOINC(thd,
                          &auto_increment_offset,
                          &auto_increment_increment);
      storage_get_auto_increment(auto_increment_offset,
                                 auto_increment_increment,
                                 1,
                                 &stats.auto_increment_value,
                                 &nb_reserved_values);
    }
    if (next_number_field_is_null) {
      table->next_number_field = NULL;
    }
  }

  if (flag & HA_STATUS_CONST) {
    storage_set_keys_in_use();
  }

  if (flag & HA_STATUS_VARIABLE) {
    storage_info_variable();
  }

  DBUG_RETURN(0);
}

void ha_mroonga::storage_info_variable()
{
  MRN_DBUG_ENTER_METHOD();

  storage_info_variable_records();
  storage_info_variable_data_file_length();

  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_info_variable_records()
{
  MRN_DBUG_ENTER_METHOD();

  stats.records = grn_table_size(ctx, grn_table);

  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_info_variable_data_file_length()
{
  MRN_DBUG_ENTER_METHOD();

  stats.data_file_length = 0;
  stats.data_file_length += grn_obj_get_disk_usage(ctx, grn_table);
  int n_columns = table->s->fields;
  for (int i = 0; i < n_columns; ++i) {
    grn_obj* column = grn_columns[i];
    if (!column) {
      stats.data_file_length += grn_obj_get_disk_usage(ctx, column);
    }
  }
  int n_indexes = table->s->keys;
  for (int i = 0; i < n_indexes; ++i) {
    grn_obj* lexicon = grn_index_tables[i];
    if (lexicon) {
      stats.data_file_length += grn_obj_get_disk_usage(ctx, lexicon);
    }
    grn_obj* index = grn_index_columns[i];
    if (index) {
      stats.data_file_length += grn_obj_get_disk_usage(ctx, index);
    }
  }

  DBUG_VOID_RETURN;
}

int ha_mroonga::info(uint flag)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_info(flag);
  } else {
#endif
    error = storage_info(flag);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
uint ha_mroonga::wrapper_lock_count() const
{
  uint lock_count;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  lock_count = wrap_handler->lock_count();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(lock_count);
}
#endif

uint ha_mroonga::storage_lock_count() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(1);
}

uint ha_mroonga::lock_count() const
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_lock_count();
  } else {
#endif
    error = storage_lock_count();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
THR_LOCK_DATA** ha_mroonga::wrapper_store_lock(THD* thd,
                                               THR_LOCK_DATA** to,
                                               enum thr_lock_type lock_type)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  to = wrap_handler->store_lock(thd, to, lock_type);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(to);
}
#endif

THR_LOCK_DATA** ha_mroonga::storage_store_lock(THD* thd,
                                               THR_LOCK_DATA** to,
                                               enum thr_lock_type lock_type)
{
  MRN_DBUG_ENTER_METHOD();
  if (lock_type != TL_IGNORE && thr_lock_data.type == TL_UNLOCK) {
    if (!thd_in_lock_tables(thd)) {
      if (lock_type == TL_READ_NO_INSERT) {
        lock_type = TL_READ;
      } else if (lock_type >= TL_WRITE_CONCURRENT_INSERT &&
                 lock_type <= TL_WRITE && !thd_tablespace_op(thd)) {
        lock_type = TL_WRITE_ALLOW_WRITE;
      }
    }

    thr_lock_data.type = lock_type;
  }
  *to++ = &thr_lock_data;
  DBUG_RETURN(to);
}

THR_LOCK_DATA** ha_mroonga::store_lock(THD* thd,
                                       THR_LOCK_DATA** to,
                                       enum thr_lock_type lock_type)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    to = wrapper_store_lock(thd, to, lock_type);
  } else {
#endif
    to = storage_store_lock(thd, to, lock_type);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(to);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_external_lock(THD* thd, int lock_type)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_external_lock(thd, lock_type);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_external_lock(THD* thd, int lock_type)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(0);
}

int ha_mroonga::external_lock(THD* thd, int lock_type)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  mrn_lock_type = lock_type;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_external_lock(thd, lock_type);
  } else {
#endif
    error = storage_external_lock(thd, lock_type);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_rnd_init(bool scan)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_rnd_init(scan);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_rnd_init(bool scan)
{
  MRN_DBUG_ENTER_METHOD();

  mrn_change_encoding(ctx, NULL);

  int error = 0;
  // MySQL uses rnd_init()/rnd_next()/rnd_end() for condition pushed
  // down SELECT.
  if (pushed_cond) {
    {
      check_count_skip(0);
      grn_obj *expression, *expression_variable;
      GRN_EXPR_CREATE_FOR_QUERY(ctx,
                                grn_table,
                                expression,
                                expression_variable);
      mrn::ConditionConverter converter(ha_thd(),
                                        ctx,
                                        grn_table,
                                        grn_index_columns,
                                        table->key_info,
                                        true);
      std::list<mrn::SmartGrnObj> match_columns_list;
      grn_encoding encoding =
        converter.convert(pushed_cond, expression, match_columns_list);
      GRN_CTX_SET_ENCODING(ctx, encoding);
      longlong escalation_threshold =
        THDVAR(ha_thd(), match_escalation_threshold);
      mrn::MatchEscalationThresholdScope scope(ctx, escalation_threshold);
      condition_push_down_result =
        grn_table_select(ctx, grn_table, expression, NULL, GRN_OP_OR);
      if (ctx->rc != GRN_SUCCESS) {
        error = ER_ERROR_ON_READ;
        my_message(error, ctx->errbuf, MYF(0));
      }
      grn_obj_unlink(ctx, expression);

      if (error == 0) {
        grn_table_sort_key* sort_keys = NULL;
        int n_sort_keys = 0;
        longlong limit = -1;
        check_fast_order_limit(condition_push_down_result,
                               &sort_keys,
                               &n_sort_keys,
                               &limit);

        if (fast_order_limit) {
          sorted_condition_push_down_result_ =
            grn_table_create(ctx,
                             NULL,
                             0,
                             NULL,
                             GRN_OBJ_TABLE_NO_KEY,
                             NULL,
                             condition_push_down_result);
          grn_table_sort(ctx,
                         condition_push_down_result,
                         0,
                         static_cast<int>(limit),
                         sorted_condition_push_down_result_,
                         sort_keys,
                         n_sort_keys);
          for (int i = 0; i < n_sort_keys; i++) {
            grn_obj_unlink(ctx, sort_keys[i].key);
          }
          my_free(sort_keys);

          condition_push_down_result_cursor =
            grn_table_cursor_open(ctx,
                                  sorted_condition_push_down_result_,
                                  NULL,
                                  0,
                                  NULL,
                                  0,
                                  0,
                                  -1,
                                  0);
        } else {
          condition_push_down_result_cursor =
            grn_table_cursor_open(ctx,
                                  condition_push_down_result,
                                  NULL,
                                  0,
                                  NULL,
                                  0,
                                  0,
                                  -1,
                                  0);
        }
        if (ctx->rc != GRN_SUCCESS) {
          error = ER_ERROR_ON_READ;
          my_message(error, ctx->errbuf, MYF(0));
        }
      }
    }
  } else {
    cursor = grn_table_cursor_open(ctx, grn_table, NULL, 0, NULL, 0, 0, -1, 0);
    if (ctx->rc != GRN_SUCCESS) {
      error = ER_ERROR_ON_READ;
      my_message(error, ctx->errbuf, MYF(0));
    }
  }
  DBUG_RETURN(error);
}

int ha_mroonga::rnd_init(bool scan)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_rnd_init(scan);
  } else {
#endif
    error = storage_rnd_init(scan);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_rnd_end()
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_rnd_end();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_rnd_end()
{
  MRN_DBUG_ENTER_METHOD();
  clear_cursor();
  DBUG_RETURN(0);
}

int ha_mroonga::rnd_end()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_rnd_end();
  } else {
#endif
    error = storage_rnd_end();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_HANDLER_RECORDS_RETURN_ERROR
#  ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_records(ha_rows* num_rows)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_records(num_rows);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#  endif

int ha_mroonga::storage_records(ha_rows* num_rows)
{
  MRN_DBUG_ENTER_METHOD();
  int error = handler::records(num_rows);
  DBUG_RETURN(error);
}

int ha_mroonga::records(ha_rows* num_rows)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_records(num_rows);
  } else {
#  endif
    error = storage_records(num_rows);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(error);
}
#else
#  ifdef MRN_ENABLE_WRAPPER_MODE
ha_rows ha_mroonga::wrapper_records()
{
  ha_rows num_rows;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  num_rows = wrap_handler->records();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(num_rows);
}
#  endif

ha_rows ha_mroonga::storage_records()
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows num_rows = handler::records();
  DBUG_RETURN(num_rows);
}

ha_rows ha_mroonga::records()
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows num_rows;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    num_rows = wrapper_records();
  } else {
#  endif
    num_rows = storage_records();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(num_rows);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_rnd_next(uchar* buf)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
  error = wrap_handler->ha_rnd_next(buf);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_rnd_next(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = storage_get_next_record(buf);
  DBUG_RETURN(error);
}

int ha_mroonga::rnd_next(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_rnd_next(buf);
  } else {
#endif
    error = storage_rnd_next(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_rnd_pos(uchar* buf, uchar* pos)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_rnd_pos(buf, pos);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_rnd_pos(uchar* buf, uchar* pos)
{
  MRN_DBUG_ENTER_METHOD();
  record_id = *((grn_id*)pos);
  storage_store_fields(table, buf, record_id);
  DBUG_RETURN(0);
}

int ha_mroonga::rnd_pos(uchar* buf, uchar* pos)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_rnd_pos(buf, pos);
  } else {
#endif
    error = storage_rnd_pos(buf, pos);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_position(const uchar* record)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->ref = ref;
  wrap_handler->position(record);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_position(const uchar* record)
{
  MRN_DBUG_ENTER_METHOD();
  memcpy(ref, &record_id, sizeof(grn_id));
  DBUG_VOID_RETURN;
}

void ha_mroonga::position(const uchar* record)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_position(record);
  } else {
#endif
    storage_position(record);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

int ha_mroonga::generic_extra(enum ha_extra_function operation)
{
  MRN_DBUG_ENTER_METHOD();
  switch (operation) {
  case HA_EXTRA_IGNORE_DUP_KEY:
    ignoring_duplicated_key = true;
    break;
  case HA_EXTRA_NO_IGNORE_DUP_KEY:
    ignoring_duplicated_key = false;
    break;
  case HA_EXTRA_WRITE_CAN_REPLACE:
    replacing_ = true;
    break;
  case HA_EXTRA_WRITE_CANNOT_REPLACE:
    replacing_ = false;
    break;
  case HA_EXTRA_INSERT_WITH_UPDATE:
    inserting_with_update = true;
    break;
  case HA_EXTRA_KEYREAD:
    ignoring_no_key_columns = true;
    break;
  case HA_EXTRA_NO_KEYREAD:
    ignoring_no_key_columns = false;
    break;
  default:
    break;
  }
  DBUG_RETURN(0);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_extra(enum ha_extra_function operation)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_HAVE_HA_EXTRA
  error = wrap_handler->ha_extra(operation);
#  else
  error = wrap_handler->extra(operation);
#  endif
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_extra(enum ha_extra_function operation)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(0);
}

int ha_mroonga::extra(enum ha_extra_function operation)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  DBUG_PRINT("info",
             ("mroonga: this=%p; extra-operation=%s",
              this,
              mrn_inspect_extra_function(operation)));
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    if ((error = wrapper_extra(operation)))
      DBUG_RETURN(error);
  } else {
#endif
    if ((error = storage_extra(operation)))
      DBUG_RETURN(error);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  error = generic_extra(operation);
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_extra_opt(enum ha_extra_function operation,
                                  ulong cache_size)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->extra_opt(operation, cache_size);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_extra_opt(enum ha_extra_function operation,
                                  ulong cache_size)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(0);
}

int ha_mroonga::extra_opt(enum ha_extra_function operation, ulong cache_size)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    if ((error = wrapper_extra_opt(operation, cache_size)))
      DBUG_RETURN(error);
  } else {
#endif
    if ((error = storage_extra_opt(operation, cache_size)))
      DBUG_RETURN(error);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  error = generic_extra(operation);
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_is_target_index(KEY* key_info)
{
  MRN_DBUG_ENTER_METHOD();
  bool target_index =
    (key_info->algorithm == HA_KEY_ALG_FULLTEXT) || mrn_is_geo_key(key_info);
  DBUG_PRINT("info", ("mroonga: %s", target_index ? "true" : "false"));
  DBUG_RETURN(target_index);
}

bool ha_mroonga::wrapper_have_target_index()
{
  MRN_DBUG_ENTER_METHOD();

  bool have_target_index = false;

  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    KEY* key_info = &(table->key_info[i]);

    if (wrapper_is_target_index(key_info)) {
      have_target_index = true;
      break;
    }
  }

  DBUG_PRINT("info", ("mroonga: %s", have_target_index ? "true" : "false"));
  DBUG_RETURN(have_target_index);
}

int ha_mroonga::wrapper_write_row(mrn_write_row_buf_t buf)
{
  int error = 0;
  THD* thd = ha_thd();

  MRN_DBUG_ENTER_METHOD();

  mrn::Operation operation(operations_,
                           "write",
                           table->s->table_name.str,
                           table->s->table_name.length);

  operation.record_target(record_id);
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  MRN_DISABLE_BINLOG_BEGIN(thd)
  {
    error = wrap_handler->ha_write_row(buf);
    insert_id_for_cur_row = wrap_handler->insert_id_for_cur_row;
  }
  MRN_DISABLE_BINLOG_END(thd);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);

  if (!error && wrapper_have_target_index()) {
    error = wrapper_write_row_index(buf);
  }

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_write_row_index(mrn_write_row_buf_t buf)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  if (is_dry_write()) {
    DBUG_PRINT("info", ("mroonga: dry write: ha_mroonga::%s", __FUNCTION__));
    DBUG_RETURN(error);
  }

  mrn_change_encoding(ctx, NULL);
  GRN_BULK_REWIND(&key_buffer);
  grn_bulk_space(ctx, &key_buffer, table->key_info->key_length);
  key_copy((uchar*)(GRN_TEXT_VALUE(&key_buffer)),
           buf,
           &(table->key_info[table_share->primary_key]),
           table->key_info[table_share->primary_key].key_length);

  int added;
  grn_id record_id;
  record_id = grn_table_add(ctx,
                            grn_table,
                            GRN_TEXT_VALUE(&key_buffer),
                            GRN_TEXT_LEN(&key_buffer),
                            &added);
  if (record_id == GRN_ID_NIL) {
    DBUG_PRINT("info", ("mroonga: failed to add a new record into groonga"));
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "failed to add a new record into groonga: key=<%.*s>",
             (int)GRN_TEXT_LEN(&key_buffer),
             GRN_TEXT_VALUE(&key_buffer));
    error = ER_ERROR_ON_WRITE;
    push_warning(ha_thd(), MRN_SEVERITY_WARNING, error, error_message);
    DBUG_RETURN(0);
  }

  mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    KEY* key_info = &(table->key_info[i]);

    if (!(wrapper_is_target_index(key_info))) {
      continue;
    }

    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      continue;
    }

    uint j;
    for (j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
      Field* field = key_info->key_part[j].field;

      if (field->is_null())
        continue;

      error = mrn_change_encoding(ctx, field->charset());
      if (error)
        goto err;
      error = generic_store_bulk(field, &new_value_buffer);
      if (error) {
        my_message(error,
                   "mroonga: wrapper: "
                   "failed to get new value for updating index.",
                   MYF(0));
        goto err;
      }

      grn_rc rc;
      rc = grn_column_index_update(ctx,
                                   index_column,
                                   record_id,
                                   j + 1,
                                   NULL,
                                   &new_value_buffer);
      if (rc) {
        error = ER_ERROR_ON_WRITE;
        my_message(error, ctx->errbuf, MYF(0));
        goto err;
      }
    }
  }
err:

  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_write_row(mrn_write_row_buf_t buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool unique_indexes_are_processed = false;

  if (is_dry_write()) {
    DBUG_PRINT("info", ("mroonga: dry write: ha_mroonga::%s", __FUNCTION__));
    DBUG_RETURN(error);
  }

  mrn::Operation operation(operations_,
                           "write",
                           table->s->table_name.str,
                           table->s->table_name.length);

  THD* thd = ha_thd();
  int i;
  int n_columns = table->s->fields;

  if (table->next_number_field && buf == table->record[0]) {
    if ((error = update_auto_increment()))
      DBUG_RETURN(error);
  }

  mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
  for (i = 0; i < n_columns; i++) {
    Field* field = table->field[i];

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif

    if (field->is_null())
      continue;

    mrn::ColumnName column_name(FIELD_NAME(field));
    if (strcmp(MRN_COLUMN_NAME_ID, column_name.c_str()) == 0) {
      push_warning_printf(thd,
                          MRN_SEVERITY_WARNING,
                          WARN_DATA_TRUNCATED,
                          MRN_GET_ERR_MSG(WARN_DATA_TRUNCATED),
                          MRN_COLUMN_NAME_ID,
                          MRN_GET_CURRENT_ROW_FOR_WARNING(thd));
      if (thd->is_strict_mode()) {
        DBUG_RETURN(ER_DATA_TOO_LONG);
      }
    }
  }

  uint pkey_nr = table->s->primary_key;

  int added = 0;
  {
    mrn::Lock lock(&(share->record_mutex), have_unique_index());
    if ((error = storage_write_row_unique_indexes(buf))) {
      DBUG_RETURN(error);
    }
    unique_indexes_are_processed = true;

    char* pkey;
    int pkey_size;
    GRN_BULK_REWIND(&key_buffer);
    if (pkey_nr == MAX_INDEXES) {
      pkey = NULL;
      pkey_size = 0;
    } else {
      KEY* key_info = &(table->key_info[pkey_nr]);
      if (KEY_N_KEY_PARTS(key_info) == 1) {
        Field* pkey_field = key_info->key_part[0].field;
        error = mrn_change_encoding(ctx, pkey_field->charset());
        if (error) {
          DBUG_RETURN(error);
        }
        generic_store_bulk(pkey_field, &key_buffer);
        pkey = GRN_TEXT_VALUE(&key_buffer);
        pkey_size = GRN_TEXT_LEN(&key_buffer);
      } else {
        mrn_change_encoding(ctx, NULL);
        uchar key[MRN_MAX_KEY_SIZE];
        key_copy(key, buf, key_info, key_info->key_length);
        grn_bulk_reserve(ctx, &key_buffer, MRN_MAX_KEY_SIZE);
        pkey = GRN_TEXT_VALUE(&key_buffer);
        storage_encode_multiple_column_key(key_info,
                                           key,
                                           key_info->key_length,
                                           (uchar*)pkey,
                                           (uint*)&pkey_size);
      }
    }

    if (grn_table->header.type != GRN_TABLE_NO_KEY && pkey_size == 0) {
      my_message(ER_ERROR_ON_WRITE, "primary key is empty", MYF(0));
      DBUG_RETURN(ER_ERROR_ON_WRITE);
    }

    record_id = grn_table_add(ctx, grn_table, pkey, pkey_size, &added);
    if (ctx->rc) {
      my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
      DBUG_RETURN(ER_ERROR_ON_WRITE);
    }
    if (!added) {
      // duplicated error
      error = HA_ERR_FOUND_DUPP_KEY;
      memcpy(dup_ref, &record_id, sizeof(grn_id));
      dup_key = pkey_nr;
      if (!ignoring_duplicated_key) {
        GRN_LOG(ctx,
                GRN_LOG_ERROR,
                "duplicated id on insert: update primary key: <%.*s>",
                pkey_size,
                pkey);
      }
      uint j;
      for (j = 0; j < table->s->keys; j++) {
        if (j == pkey_nr) {
          continue;
        }
        KEY* key_info = &table->key_info[j];
        if (key_info->flags & HA_NOSAME) {
          grn_table_delete_by_id(ctx, grn_index_tables[j], key_id[j]);
        }
      }
      DBUG_RETURN(error);
    }
    operation.record_target(record_id);
  }

  grn_obj colbuf;
  GRN_VOID_INIT(&colbuf);
  for (i = 0; i < n_columns; i++) {
    Field* field = table->field[i];

    // TODO: Remove this when support for handling how to register NULLs
    //       in the index is implemented for all columns.
    if (field->is_null() && ((field->real_type() != MYSQL_TYPE_STRING) &&
                             (field->real_type() != MYSQL_TYPE_VARCHAR) &&
                             (field->real_type() != MYSQL_TYPE_TINY) &&
                             (field->real_type() != MYSQL_TYPE_SHORT) &&
                             (field->real_type() != MYSQL_TYPE_LONG) &&
                             (field->real_type() != MYSQL_TYPE_YEAR) &&
                             (field->real_type() != MYSQL_TYPE_NEWDATE) &&
                             (field->real_type() != MYSQL_TYPE_TIME) &&
                             (field->real_type() != MYSQL_TYPE_DATETIME) &&
                             (field->real_type() != MYSQL_TYPE_TIMESTAMP) &&
                             (field->real_type() != MYSQL_TYPE_TIME2) &&
                             (field->real_type() != MYSQL_TYPE_DATETIME2) &&
                             (field->real_type() != MYSQL_TYPE_TIMESTAMP2) &&
                             (field->real_type() != MYSQL_TYPE_FLOAT) &&
                             (field->real_type() != MYSQL_TYPE_DOUBLE) &&
                             (field->real_type() != MYSQL_TYPE_ENUM) &&
                             (field->real_type() != MYSQL_TYPE_SET) &&
                             (field->real_type() != MYSQL_TYPE_BIT) &&
                             (field->real_type() != MYSQL_TYPE_BLOB) &&
                             (field->real_type() != MYSQL_TYPE_MEDIUM_BLOB) &&
                             (field->real_type() != MYSQL_TYPE_LONG_BLOB)))
      continue;

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif

    mrn::ColumnName column_name(FIELD_NAME(field));

#ifdef MRN_HAVE_SPATIAL
    bool is_null_geometry_value =
      field->real_type() == MYSQL_TYPE_GEOMETRY &&
      static_cast<Field_geom*>(field)->get_length() == 0;
    if (is_null_geometry_value) {
      continue;
    }
#endif

    if (strcmp(MRN_COLUMN_NAME_ID, column_name.c_str()) == 0) {
      continue;
    }

    error = mrn_change_encoding(ctx, field->charset());
    if (error) {
      GRN_OBJ_FIN(ctx, &colbuf);
      goto err;
    }
    error = generic_store_bulk(field, &colbuf);
    if (error) {
      GRN_OBJ_FIN(ctx, &colbuf);
      goto err;
    }
    if (GRN_BULK_VSIZE(&colbuf) == 0) {
      continue;
    }

    grn_obj* column = grn_columns[i];
    if (is_foreign_key_field(table->s->table_name.str, field->field_name)) {
      grn_obj value;
      GRN_RECORD_INIT(&value, 0, grn_obj_get_range(ctx, column));
      grn_rc cast_rc = grn_obj_cast(ctx, &colbuf, &value, GRN_FALSE);
      if (cast_rc != GRN_SUCCESS) {
        grn_obj inspected;
        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, &colbuf);
        error = HA_ERR_NO_REFERENCED_ROW;
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "foreign record doesn't exist: "
                         "<" FIELD_NAME_FORMAT ">:<%.*s>",
                         FIELD_NAME_FORMAT_VALUE(field),
                         static_cast<int>(GRN_TEXT_LEN(&inspected)),
                         GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &value);
        GRN_OBJ_FIN(ctx, &colbuf);
        GRN_OBJ_FIN(ctx, &inspected);
        goto err;
      }
      grn_obj_set_value(ctx, column, record_id, &value, GRN_OBJ_SET);
    } else {
      grn_obj_set_value(ctx, column, record_id, &colbuf, GRN_OBJ_SET);
    }
    if (ctx->rc) {
      GRN_OBJ_FIN(ctx, &colbuf);
      my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
      error = ER_ERROR_ON_WRITE;
      goto err;
    }
  }
  GRN_OBJ_FIN(ctx, &colbuf);

  error = storage_write_row_multiple_column_indexes(buf, record_id);
  if (error) {
    goto err;
  }

  {
    // for UDF last_insert_grn_id()
    mrn::SlotData* slot_data = mrn_get_slot_data(thd, true);
    if (slot_data) {
      slot_data->last_insert_record_id = record_id;
    } else {
      error = HA_ERR_OUT_OF_MEM;
    }
  }
  if (error != 0) {
    goto err;
  }

  grn_db_touch(ctx, grn_ctx_db(ctx));

  if (table->found_next_number_field && !table->s->next_number_keypart) {
    Field_num* field = (Field_num*)table->found_next_number_field;
    if (MRN_FIELD_IS_UNSIGNED(field) || field->val_int() > 0) {
      MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
      ulonglong nr = (ulonglong)field->val_int();
      if (!long_term_share->auto_inc_inited) {
        storage_info(HA_STATUS_AUTO);
      }
      {
        mrn::Lock lock(&long_term_share->auto_inc_mutex);
        if (long_term_share->auto_inc_value <= nr) {
          long_term_share->auto_inc_value = nr + 1;
          DBUG_PRINT(
            "info",
            ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
        }
      }
    }
  }
  DBUG_RETURN(0);

err:
  if (unique_indexes_are_processed) {
    uint j;
    for (j = 0; j < table->s->keys; j++) {
      if (j == pkey_nr) {
        continue;
      }
      KEY* key_info = &table->key_info[j];
      if (key_info->flags & HA_NOSAME) {
        grn_table_delete_by_id(ctx, grn_index_tables[j], key_id[j]);
      }
    }
  }
  grn_table_delete_by_id(ctx, grn_table, record_id);
  DBUG_RETURN(error);
}

int ha_mroonga::storage_write_row_multiple_column_index(mrn_write_row_buf_t buf,
                                                        grn_id record_id,
                                                        KEY* key_info,
                                                        grn_obj* index_column)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  mrn_change_encoding(ctx, NULL);
  GRN_BULK_REWIND(&key_buffer);
  grn_bulk_space(ctx, &key_buffer, key_info->key_length);
  key_copy((uchar*)(GRN_TEXT_VALUE(&key_buffer)),
           buf,
           key_info,
           key_info->key_length);
  GRN_BULK_REWIND(&encoded_key_buffer);
  grn_bulk_reserve(ctx, &encoded_key_buffer, MRN_MAX_KEY_SIZE);
  uint encoded_key_length;
  storage_encode_multiple_column_key(
    key_info,
    (uchar*)(GRN_TEXT_VALUE(&key_buffer)),
    key_info->key_length,
    (uchar*)(GRN_TEXT_VALUE(&encoded_key_buffer)),
    &encoded_key_length);
  grn_bulk_space(ctx, &encoded_key_buffer, encoded_key_length);
  DBUG_PRINT("info", ("mroonga: key_length=%u", key_info->key_length));
  DBUG_PRINT("info", ("mroonga: encoded_key_length=%u", encoded_key_length));

  grn_rc rc;
  rc = grn_column_index_update(ctx,
                               index_column,
                               record_id,
                               1,
                               NULL,
                               &encoded_key_buffer);
  if (rc) {
    error = ER_ERROR_ON_WRITE;
    my_message(error, ctx->errbuf, MYF(0));
  }
  DBUG_RETURN(error);
}

int ha_mroonga::storage_write_row_multiple_column_indexes(
  mrn_write_row_buf_t buf, grn_id record_id)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &(table->key_info[i]);

    if (KEY_N_KEY_PARTS(key_info) == 1 ||
        key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
      continue;
    }

    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      continue;
    }

    if ((error = storage_write_row_multiple_column_index(buf,
                                                         record_id,
                                                         key_info,
                                                         index_column))) {
      goto err;
    }
  }

err:

  DBUG_RETURN(error);
}

int ha_mroonga::storage_write_row_unique_index(const uchar* buf,
                                               KEY* key_info,
                                               grn_obj* index_table,
                                               grn_obj* index_column,
                                               grn_id* key_id)
{
  char* ukey = NULL;
  int error, ukey_size = 0;
  MRN_DBUG_ENTER_METHOD();
  GRN_BULK_REWIND(&key_buffer);
  if (KEY_N_KEY_PARTS(key_info) == 1) {
    Field* ukey_field = key_info->key_part[0].field;
    error = mrn_change_encoding(ctx, ukey_field->charset());
    if (error) {
      DBUG_RETURN(error);
    }
    generic_store_bulk(ukey_field, &key_buffer);
    ukey = GRN_TEXT_VALUE(&key_buffer);
    ukey_size = GRN_TEXT_LEN(&key_buffer);
  } else {
    mrn_change_encoding(ctx, NULL);
    uchar key[MRN_MAX_KEY_SIZE];
#ifdef MRN_HANDLER_HA_UPDATE_ROW_NEW_DATA_CONST
    const uchar* key_copy_data = buf;
#else
    uchar* key_copy_data = const_cast<uchar*>(buf);
#endif
    key_copy(key, key_copy_data, key_info, key_info->key_length);
    grn_bulk_reserve(ctx, &key_buffer, MRN_MAX_KEY_SIZE);
    ukey = GRN_TEXT_VALUE(&key_buffer);
    storage_encode_multiple_column_key(key_info,
                                       key,
                                       key_info->key_length,
                                       (uchar*)(ukey),
                                       (uint*)&ukey_size);
  }

  int added;
  *key_id = grn_table_add(ctx, index_table, ukey, ukey_size, &added);
  if (ctx->rc) {
    my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  }
  if (!added) {
    // duplicated error
    error = HA_ERR_FOUND_DUPP_KEY;
    grn_id duplicated_record_id = GRN_ID_NIL;
    {
      grn_table_cursor* table_cursor;
      table_cursor = grn_table_cursor_open(ctx,
                                           index_table,
                                           ukey,
                                           ukey_size,
                                           ukey,
                                           ukey_size,
                                           0,
                                           -1,
                                           0);
      if (table_cursor) {
        grn_obj* index_cursor;
        index_cursor = grn_index_cursor_open(ctx,
                                             table_cursor,
                                             index_column,
                                             GRN_ID_NIL,
                                             GRN_ID_MAX,
                                             0);
        if (index_cursor) {
          grn_posting* posting;
          posting = grn_index_cursor_next(ctx, index_cursor, NULL);
          if (posting) {
            duplicated_record_id = posting->rid;
          }
        }
        grn_obj_unlink(ctx, index_cursor);
      }
      grn_table_cursor_close(ctx, table_cursor);
    }
    memcpy(dup_ref, &duplicated_record_id, sizeof(grn_id));
    if (!ignoring_duplicated_key) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size = grn_obj_name(ctx, index_table, name, sizeof(name));
      GRN_LOG(ctx,
              GRN_LOG_ERROR,
              "[mroonga][storage][index][unique][update][%.*s] "
              "duplicated ID on insert: <%.*s>",
              name_size,
              name,
              ukey_size,
              ukey);
    }
    DBUG_RETURN(error);
  }
  DBUG_RETURN(0);
}

int ha_mroonga::storage_write_row_unique_indexes(const uchar* buf)
{
  int error = 0;
  uint i;
  uint n_keys = table->s->keys;
  MRN_DBUG_ENTER_METHOD();

  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &table->key_info[i];

    if (!(key_info->flags & HA_NOSAME)) {
      continue;
    }

    grn_obj* index_table = grn_index_tables[i];
    if (!index_table) {
      continue;
    }
    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      continue;
    }

    if ((error = storage_write_row_unique_index(buf,
                                                key_info,
                                                index_table,
                                                index_column,
                                                &key_id[i]))) {
      if (error == HA_ERR_FOUND_DUPP_KEY) {
        dup_key = i;
      }
      goto err;
    }
  }
  DBUG_RETURN(0);

err:
  if (i) {
    mrn_change_encoding(ctx, NULL);
    do {
      i--;

      if (i == table->s->primary_key) {
        continue;
      }

      KEY* key_info = &table->key_info[i];
      if (!(key_info->flags & HA_NOSAME)) {
        continue;
      }

      if (key_info->flags & HA_NOSAME) {
        grn_table_delete_by_id(ctx, grn_index_tables[i], key_id[i]);
      }
    } while (i);
  }
  DBUG_RETURN(error);
}

int ha_mroonga::write_row(mrn_write_row_buf_t buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_write_row(buf);
  } else {
#endif
    error = storage_write_row(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_get_record_id(uchar* data,
                                      grn_id* record_id,
                                      const char* context)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  grn_obj key;
  GRN_TEXT_INIT(&key, 0);

  mrn_change_encoding(ctx, NULL);
  grn_bulk_space(ctx,
                 &key,
                 table->key_info[table_share->primary_key].key_length);
  key_copy((uchar*)(GRN_TEXT_VALUE(&key)),
           data,
           &(table->key_info[table_share->primary_key]),
           table->key_info[table_share->primary_key].key_length);

  *record_id =
    grn_table_get(ctx, grn_table, GRN_TEXT_VALUE(&key), GRN_TEXT_LEN(&key));
  if (*record_id == GRN_ID_NIL) {
    DBUG_PRINT("info", ("mroonga: %s", context));
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "%s: key=<%.*s>",
             context,
             (int)GRN_TEXT_LEN(&key),
             GRN_TEXT_VALUE(&key));
    error = ER_ERROR_ON_WRITE;
    push_warning(ha_thd(), MRN_SEVERITY_WARNING, error, error_message);
  }
  grn_obj_unlink(ctx, &key);

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_update_row(const uchar* old_data,
                                   mrn_update_row_new_data_t new_data)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
  THD* thd = ha_thd();

  mrn::Operation operation(operations_,
                           "update",
                           table->s->table_name.str,
                           table->s->table_name.length);

  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  MRN_DISABLE_BINLOG_BEGIN(thd)
  {
    error = wrap_handler->ha_update_row(old_data, new_data);
  }
  MRN_DISABLE_BINLOG_END(thd);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);

  if (!error && wrapper_have_target_index()) {
    error = wrapper_update_row_index(old_data, new_data);
  }

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_update_row_index(const uchar* old_data,
                                         mrn_update_row_new_data_t new_data)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  if (is_dry_write()) {
    DBUG_PRINT("info", ("mroonga: dry write: ha_mroonga::%s", __FUNCTION__));
    DBUG_RETURN(error);
  }

  mrn_change_encoding(ctx, NULL);
  KEY* key_info = &(table->key_info[table_share->primary_key]);
  GRN_BULK_REWIND(&key_buffer);
  key_copy((uchar*)(GRN_TEXT_VALUE(&key_buffer)),
           new_data,
           key_info,
           key_info->key_length);
  int added;
  grn_id new_record_id;
  new_record_id = grn_table_add(ctx,
                                grn_table,
                                GRN_TEXT_VALUE(&key_buffer),
                                table->key_info->key_length,
                                &added);
  if (new_record_id == GRN_ID_NIL) {
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(
      error_message,
      MRN_MESSAGE_BUFFER_SIZE,
      "failed to get new record ID for updating from groonga: key=<%.*s>",
      (int)GRN_TEXT_LEN(&key_buffer),
      GRN_TEXT_VALUE(&key_buffer));
    error = ER_ERROR_ON_WRITE;
    my_message(error, error_message, MYF(0));
    DBUG_RETURN(error);
  }

  grn_id old_record_id;
  my_ptrdiff_t ptr_diff_for_key =
    mrn_compute_ptr_diff_for_key(old_data, table->record[0]);
  for (uint j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
    Field* field = key_info->key_part[j].field;
    field->move_field_offset(ptr_diff_for_key);
  }
  error = wrapper_get_record_id((uchar*)old_data,
                                &old_record_id,
                                "failed to get old record ID "
                                "for updating from groonga");
  for (uint j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
    Field* field = key_info->key_part[j].field;
    field->move_field_offset(-ptr_diff_for_key);
  }
  if (error) {
    DBUG_RETURN(0);
  }

  mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    KEY* key_info = &(table->key_info[i]);

    if (!(wrapper_is_target_index(key_info))) {
      continue;
    }

    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      /* disable keys */
      continue;
    }

    uint j;
    for (j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
      Field* field = key_info->key_part[j].field;

      generic_store_bulk(field, &new_value_buffer);

      const my_ptrdiff_t ptr_diff = old_data - table->record[0];
      field->move_field_offset(ptr_diff);
      generic_store_bulk(field, &old_value_buffer);
      field->move_field_offset(-ptr_diff);

      grn_rc rc;
      if (old_record_id == new_record_id) {
        if (added) {
          rc = grn_column_index_update(ctx,
                                       index_column,
                                       old_record_id,
                                       j + 1,
                                       &old_value_buffer,
                                       NULL);
          if (!rc) {
            rc = grn_column_index_update(ctx,
                                         index_column,
                                         new_record_id,
                                         j + 1,
                                         NULL,
                                         &new_value_buffer);
          }
        } else {
          rc = grn_column_index_update(ctx,
                                       index_column,
                                       old_record_id,
                                       j + 1,
                                       &old_value_buffer,
                                       &new_value_buffer);
        }
      } else {
        rc = grn_column_index_update(ctx,
                                     index_column,
                                     old_record_id,
                                     j + 1,
                                     &old_value_buffer,
                                     NULL);
        if (!rc) {
          rc = grn_column_index_update(ctx,
                                       index_column,
                                       new_record_id,
                                       j + 1,
                                       NULL,
                                       &new_value_buffer);
        }
        if (!rc) {
          rc = grn_table_delete_by_id(ctx, grn_table, old_record_id);
        }
      }
      if (rc) {
        error = ER_ERROR_ON_WRITE;
        my_message(error, ctx->errbuf, MYF(0));
        goto err;
      }
    }
  }
err:

  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_update_row(const uchar* old_data,
                                   mrn_update_row_new_data_t new_data)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  if (is_dry_write()) {
    DBUG_PRINT("info", ("mroonga: dry write: ha_mroonga::%s", __FUNCTION__));
    DBUG_RETURN(error);
  }

  mrn::Operation operation(operations_,
                           "update",
                           table->s->table_name.str,
                           table->s->table_name.length);
  operation.record_target(record_id);

  grn_obj colbuf;
  int i;
  uint j;
  int n_columns = table->s->fields;
  THD* thd = ha_thd();

  for (i = 0; i < n_columns; i++) {
    Field* field = table->field[i];

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif

    if (!bitmap_is_set(table->write_set, MRN_FIELD_FIELD_INDEX(field)))
      continue;

    if (field->is_null())
      continue;

    {
      mrn::ColumnName column_name(FIELD_NAME(field));
      if (strcmp(MRN_COLUMN_NAME_ID, column_name.c_str()) == 0) {
        push_warning_printf(thd,
                            MRN_SEVERITY_WARNING,
                            WARN_DATA_TRUNCATED,
                            MRN_GET_ERR_MSG(WARN_DATA_TRUNCATED),
                            MRN_COLUMN_NAME_ID,
                            MRN_GET_CURRENT_ROW_FOR_WARNING(thd));
        if (thd->is_strict_mode()) {
          DBUG_RETURN(ER_DATA_TOO_LONG);
        }
      }
    }

    if (!is_foreign_key_field(table->s->table_name.str, field->field_name))
      continue;

    {
      grn_obj* column = grn_columns[i];
      grn_obj new_value;
      GRN_VOID_INIT(&new_value);
      {
        mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
        generic_store_bulk(field, &new_value);
      }
      grn_obj casted_value;
      GRN_RECORD_INIT(&casted_value, 0, grn_obj_get_range(ctx, column));
      grn_rc cast_rc = grn_obj_cast(ctx, &new_value, &casted_value, GRN_FALSE);
      GRN_OBJ_FIN(ctx, &casted_value);
      if (cast_rc != GRN_SUCCESS) {
        grn_obj inspected;
        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, &new_value);
        GRN_OBJ_FIN(ctx, &new_value);
        error = HA_ERR_NO_REFERENCED_ROW;
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "foreign record doesn't exist: "
                         "<" FIELD_NAME_FORMAT ">:<%.*s>",
                         FIELD_NAME_FORMAT_VALUE(field),
                         static_cast<int>(GRN_TEXT_LEN(&inspected)),
                         GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
        DBUG_RETURN(error);
      }
      GRN_OBJ_FIN(ctx, &new_value);
    }
  }

  storage_store_fields_for_prep_update(old_data, new_data, record_id);
  {
    mrn::Lock lock(&(share->record_mutex), have_unique_index());
    mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
    if ((error =
           storage_prepare_delete_row_unique_indexes(old_data, record_id))) {
      DBUG_RETURN(error);
    }
    if ((error = storage_update_row_unique_indexes(new_data))) {
      DBUG_RETURN(error);
    }
  }

  KEY* pkey_info = NULL;
  if (table->s->primary_key != MAX_INDEXES) {
    pkey_info = &(table->key_info[table->s->primary_key]);
  }
  GRN_VOID_INIT(&colbuf);
  if (pkey_info) {
    grn_obj old_value;
    GRN_VOID_INIT(&old_value);
    for (i = 0; i < n_columns; i++) {
      Field* field = table->field[i];
      if (!bitmap_is_set(table->write_set, MRN_FIELD_FIELD_INDEX(field))) {
        continue;
      }

      mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
      if (field->is_null()) {
        continue;
      }

      mrn::ColumnName column_name(FIELD_NAME(field));
      for (j = 0; j < KEY_N_KEY_PARTS(pkey_info); j++) {
        Field* pkey_field = pkey_info->key_part[j].field;
        if (FIELD_NAME_EQUAL(pkey_field, column_name.c_str())) {
          generic_store_bulk(field, &colbuf);
          GRN_BULK_REWIND(&old_value);
          grn_obj_get_value(ctx, grn_columns[i], record_id, &old_value);
          if (!(GRN_BULK_VSIZE(&colbuf) == GRN_BULK_VSIZE(&old_value) &&
                (memcmp(GRN_BULK_HEAD(&colbuf),
                        GRN_BULK_HEAD(&old_value),
                        GRN_BULK_VSIZE(&colbuf)) == 0))) {
            GRN_OBJ_FIN(ctx, &colbuf);
            GRN_OBJ_FIN(ctx, &old_value);
            char message[MRN_BUFFER_SIZE];
            snprintf(message,
                     MRN_BUFFER_SIZE,
                     "changing composite primary key isn't supported: <%s>",
                     column_name.c_str());
            error = ER_ERROR_ON_WRITE;
            my_message(error, message, MYF(0));
            goto err;
          }
          break;
        }
      }
    }
    GRN_OBJ_FIN(ctx, &old_value);
  }
  for (i = 0; i < n_columns; i++) {
    Field* field = table->field[i];

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif

    if (bitmap_is_set(table->write_set, MRN_FIELD_FIELD_INDEX(field))) {
      mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
      DBUG_PRINT(
        "info",
        ("mroonga: update column %d(%d)", i, MRN_FIELD_FIELD_INDEX(field)));

      if (field->is_null())
        continue;

      mrn::ColumnName column_name(FIELD_NAME(field));
      if (strcmp(MRN_COLUMN_NAME_ID, column_name.c_str()) == 0) {
        continue;
      }

      error = mrn_change_encoding(ctx, field->charset());
      if (error)
        goto err;

      bool is_pkey = false;
      bool on_duplicate_key_update =
        (inserting_with_update && ignoring_duplicated_key);
      if (pkey_info && !on_duplicate_key_update) {
        for (j = 0; j < KEY_N_KEY_PARTS(pkey_info); j++) {
          Field* pkey_field = pkey_info->key_part[j].field;
          if (FIELD_NAME_EQUAL(pkey_field, column_name.c_str())) {
            is_pkey = true;
            break;
          }
        }
      }

      if (is_pkey) {
        continue;
      }

      generic_store_bulk(field, &colbuf);
      grn_obj_set_value(ctx, grn_columns[i], record_id, &colbuf, GRN_OBJ_SET);
      if (ctx->rc) {
        grn_obj_unlink(ctx, &colbuf);
        error = ER_ERROR_ON_WRITE;
        my_message(error, ctx->errbuf, MYF(0));
        goto err;
      }
    }
  }
  grn_obj_unlink(ctx, &colbuf);

  if ((error = storage_update_row_index(old_data, new_data))) {
    goto err;
  }

  if ((error = storage_delete_row_unique_indexes())) {
    DBUG_RETURN(error);
  }

  grn_db_touch(ctx, grn_ctx_db(ctx));

  DBUG_RETURN(0);

err:
  for (j = 0; j < table->s->keys; j++) {
    if (j == table->s->primary_key) {
      continue;
    }
    KEY* key_info = &table->key_info[j];
    if ((key_info->flags & HA_NOSAME) && key_id[j] != GRN_ID_NIL) {
      grn_table_delete_by_id(ctx, grn_index_tables[j], key_id[j]);
    }
  }

  if (!error && thd_sql_command(ha_thd()) == SQLCOM_TRUNCATE) {
    MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
    mrn::Lock lock(&long_term_share->auto_inc_mutex);
    long_term_share->auto_inc_value = 0;
    DBUG_PRINT(
      "info",
      ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
    long_term_share->auto_inc_inited = false;
  }

  DBUG_RETURN(error);
}

int ha_mroonga::storage_update_row_index(const uchar* old_data,
                                         mrn_update_row_new_data_t new_data)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  grn_obj old_key, old_encoded_key, new_key, new_encoded_key;
  GRN_TEXT_INIT(&old_key, 0);
  GRN_TEXT_INIT(&old_encoded_key, 0);
  GRN_TEXT_INIT(&new_key, 0);
  GRN_TEXT_INIT(&new_encoded_key, 0);

  my_ptrdiff_t ptr_diff =
    mrn_compute_ptr_diff_for_key(old_data, table->record[0]);

  mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
  uint i;
  uint n_keys = table->s->keys;
  mrn_change_encoding(ctx, NULL);
  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &(table->key_info[i]);

    if (KEY_N_KEY_PARTS(key_info) == 1 ||
        key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
      continue;
    }

    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      /* disable keys */
      continue;
    }

    GRN_BULK_REWIND(&old_key);
    grn_bulk_space(ctx, &old_key, key_info->key_length);
    for (uint j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
      Field* field = key_info->key_part[j].field;
      field->move_field_offset(ptr_diff);
    }
    key_copy((uchar*)(GRN_TEXT_VALUE(&old_key)),
             const_cast<const uchar*>(old_data),
             key_info,
             key_info->key_length);
    for (uint j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
      Field* field = key_info->key_part[j].field;
      field->move_field_offset(-ptr_diff);
    }
    GRN_BULK_REWIND(&old_encoded_key);
    grn_bulk_reserve(ctx, &old_encoded_key, MRN_MAX_KEY_SIZE);
    uint old_encoded_key_length;
    storage_encode_multiple_column_key(
      key_info,
      (uchar*)(GRN_TEXT_VALUE(&old_key)),
      key_info->key_length,
      (uchar*)(GRN_TEXT_VALUE(&old_encoded_key)),
      &old_encoded_key_length);
    grn_bulk_space(ctx, &old_encoded_key, old_encoded_key_length);

    GRN_BULK_REWIND(&new_key);
    grn_bulk_space(ctx, &new_key, key_info->key_length);
    key_copy((uchar*)(GRN_TEXT_VALUE(&new_key)),
             new_data,
             key_info,
             key_info->key_length);
    GRN_BULK_REWIND(&new_encoded_key);
    grn_bulk_reserve(ctx, &new_encoded_key, MRN_MAX_KEY_SIZE);
    uint new_encoded_key_length;
    storage_encode_multiple_column_key(
      key_info,
      (uchar*)(GRN_TEXT_VALUE(&new_key)),
      key_info->key_length,
      (uchar*)(GRN_TEXT_VALUE(&new_encoded_key)),
      &new_encoded_key_length);
    grn_bulk_space(ctx, &new_encoded_key, new_encoded_key_length);

    grn_rc rc;
    rc = grn_column_index_update(ctx,
                                 index_column,
                                 record_id,
                                 1,
                                 &old_encoded_key,
                                 &new_encoded_key);
    if (rc) {
      error = ER_ERROR_ON_WRITE;
      my_message(error, ctx->errbuf, MYF(0));
      goto err;
    }
  }
err:
  grn_obj_unlink(ctx, &old_key);
  grn_obj_unlink(ctx, &old_encoded_key);
  grn_obj_unlink(ctx, &new_key);
  grn_obj_unlink(ctx, &new_encoded_key);

  DBUG_RETURN(error);
}

int ha_mroonga::storage_update_row_unique_indexes(
  mrn_update_row_new_data_t new_data)
{
  int error;
  uint i;
  uint n_keys = table->s->keys;
  MRN_DBUG_ENTER_METHOD();

  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &table->key_info[i];
    if (!(key_info->flags & HA_NOSAME)) {
      continue;
    }

    grn_obj* index_table = grn_index_tables[i];
    if (!index_table) {
      key_id[i] = GRN_ID_NIL;
      del_key_id[i] = GRN_ID_NIL;
      continue;
    }

    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      key_id[i] = GRN_ID_NIL;
      del_key_id[i] = GRN_ID_NIL;
      continue;
    }

    uint n_key_parts = KEY_N_KEY_PARTS(key_info);
    bool have_any_changed_field = false;
    for (uint j = 0; j < n_key_parts; ++j) {
      if (bitmap_is_set(table->write_set,
                        MRN_FIELD_FIELD_INDEX(key_info->key_part[j].field))) {
        have_any_changed_field = true;
        break;
      }
    }

    if (!have_any_changed_field) {
      key_id[i] = GRN_ID_NIL;
      del_key_id[i] = GRN_ID_NIL;
      continue;
    }

    if ((error = storage_write_row_unique_index(new_data,
                                                key_info,
                                                index_table,
                                                index_column,
                                                &key_id[i]))) {
      if (error == HA_ERR_FOUND_DUPP_KEY) {
        if (key_id[i] == del_key_id[i]) {
          /* no change */
          key_id[i] = GRN_ID_NIL;
          del_key_id[i] = GRN_ID_NIL;
          continue;
        }
        dup_key = i;
        DBUG_PRINT("info",
                   ("mroonga: different key ID: %d record ID: %d,%d",
                    i,
                    key_id[i],
                    del_key_id[i]));
      }
      goto err;
    }
  }
  DBUG_RETURN(0);

err:
  if (i) {
    mrn_change_encoding(ctx, NULL);
    do {
      i--;
      KEY* key_info = &table->key_info[i];
      if ((key_info->flags & HA_NOSAME) && key_id[i] != GRN_ID_NIL) {
        grn_table_delete_by_id(ctx, grn_index_tables[i], key_id[i]);
      }
    } while (i);
  }
  DBUG_RETURN(error);
}

int ha_mroonga::update_row(const uchar* old_data,
                           mrn_update_row_new_data_t new_data)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_update_row(old_data, new_data);
  } else {
#endif
    error = storage_update_row(old_data, new_data);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_delete_row(const uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
  THD* thd = ha_thd();

  mrn::Operation operation(operations_,
                           "delete",
                           table->s->table_name.str,
                           table->s->table_name.length);

  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  MRN_DISABLE_BINLOG_BEGIN(thd) { error = wrap_handler->ha_delete_row(buf); }
  MRN_DISABLE_BINLOG_END(thd);

  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);

  if (!error && wrapper_have_target_index()) {
    error = wrapper_delete_row_index(buf);
  }

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_delete_row_index(const uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  if (is_dry_write()) {
    DBUG_PRINT("info", ("mroonga: dry write: ha_mroonga::%s", __FUNCTION__));
    DBUG_RETURN(error);
  }

  mrn_change_encoding(ctx, NULL);
  grn_id record_id;
  error = wrapper_get_record_id((uchar*)buf,
                                &record_id,
                                "failed to get record ID "
                                "for deleting from groonga");
  if (error) {
    DBUG_RETURN(0);
  }

  mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    KEY* key_info = &(table->key_info[i]);

    if (!(wrapper_is_target_index(key_info))) {
      continue;
    }

    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      /* disable keys */
      continue;
    }

    uint j;
    for (j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
      Field* field = key_info->key_part[j].field;

      if (field->is_null())
        continue;

      generic_store_bulk(field, &old_value_buffer);
      grn_rc rc;
      rc = grn_column_index_update(ctx,
                                   index_column,
                                   record_id,
                                   j + 1,
                                   &old_value_buffer,
                                   NULL);
      if (rc) {
        error = ER_ERROR_ON_WRITE;
        my_message(error, ctx->errbuf, MYF(0));
        goto err;
      }
    }
  }
err:
  grn_table_delete_by_id(ctx, grn_table, record_id);
  if (ctx->rc) {
    error = ER_ERROR_ON_WRITE;
    my_message(error, ctx->errbuf, MYF(0));
  }

  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_delete_row(const uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error;

  if (is_dry_write()) {
    DBUG_PRINT("info", ("mroonga: dry write: ha_mroonga::%s", __FUNCTION__));
    DBUG_RETURN(0);
  }

  mrn::Operation operation(operations_,
                           "delete",
                           table->s->table_name.str,
                           table->s->table_name.length);
  operation.record_target(record_id);

  {
    grn_id referencing_child_table_id = GRN_ID_NIL;
    grn_hash* columns = grn_hash_create(ctx,
                                        NULL,
                                        sizeof(grn_id),
                                        0,
                                        GRN_OBJ_TABLE_HASH_KEY | GRN_HASH_TINY);
    grn_table_columns(ctx,
                      grn_table,
                      "",
                      0,
                      reinterpret_cast<grn_obj*>(columns));
    GRN_HASH_EACH_BEGIN(ctx, columns, cursor, id)
    {
      void* key;
      grn_hash_cursor_get_key(ctx, cursor, &key);
      grn_id column_id = *static_cast<grn_id*>(key);
      grn_obj* column = grn_ctx_at(ctx, column_id);
      if (!column)
        continue;

      if (column->header.type != GRN_COLUMN_INDEX)
        continue;

      grn_ii_cursor* ii_cursor =
        grn_ii_cursor_open(ctx,
                           reinterpret_cast<grn_ii*>(column),
                           record_id,
                           GRN_ID_NIL,
                           GRN_ID_MAX,
                           0,
                           0);
      if (!ii_cursor)
        continue;

      if (grn_ii_cursor_next(ctx, ii_cursor)) {
        referencing_child_table_id = grn_obj_get_range(ctx, column);
      }

      grn_ii_cursor_close(ctx, ii_cursor);

      if (referencing_child_table_id != GRN_ID_NIL)
        break;
    }
    GRN_HASH_EACH_END(ctx, cursor);
    grn_hash_close(ctx, columns);

    if (referencing_child_table_id != GRN_ID_NIL) {
      grn_obj* referencing_child_table =
        grn_ctx_at(ctx, referencing_child_table_id);
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      name_size = grn_obj_name(ctx,
                               referencing_child_table,
                               name,
                               GRN_TABLE_MAX_KEY_SIZE);
      error = HA_ERR_ROW_IS_REFERENCED;
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "one or more child rows exist in <%.*s>",
                       name_size,
                       name);
      DBUG_RETURN(error);
    }
  }

  storage_store_fields_for_prep_update(buf, NULL, record_id);
  {
    mrn::Lock lock(&(share->record_mutex), have_unique_index());
    if ((error = storage_prepare_delete_row_unique_indexes(buf, record_id))) {
      DBUG_RETURN(error);
    }
    mrn_change_encoding(ctx, NULL);
    grn_table_delete_by_id(ctx, grn_table, record_id);
    if (ctx->rc) {
      my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
      DBUG_RETURN(ER_ERROR_ON_WRITE);
    }
    if ((error = storage_delete_row_index(buf)) ||
        (error = storage_delete_row_unique_indexes())) {
      DBUG_RETURN(error);
    }
  }

  grn_db_touch(ctx, grn_ctx_db(ctx));

  DBUG_RETURN(0);
}

int ha_mroonga::storage_delete_row_index(const uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  grn_obj key, encoded_key;
  GRN_TEXT_INIT(&key, 0);
  GRN_TEXT_INIT(&encoded_key, 0);

  mrn::DebugColumnAccess debug_column_access(table, &(table->read_set));
  uint i;
  uint n_keys = table->s->keys;
  mrn_change_encoding(ctx, NULL);
  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &(table->key_info[i]);

    if (KEY_N_KEY_PARTS(key_info) == 1 ||
        key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
      continue;
    }

    grn_obj* index_column = grn_index_columns[i];
    if (!index_column) {
      /* disable keys */
      continue;
    }

    GRN_BULK_REWIND(&key);
    grn_bulk_space(ctx, &key, key_info->key_length);
#ifdef MRN_HANDLER_HA_UPDATE_ROW_NEW_DATA_CONST
    const uchar* key_copy_data = buf;
#else
    uchar* key_copy_data = const_cast<uchar*>(buf);
#endif
    key_copy((uchar*)(GRN_TEXT_VALUE(&key)),
             key_copy_data,
             key_info,
             key_info->key_length);
    GRN_BULK_REWIND(&encoded_key);
    grn_bulk_reserve(ctx, &encoded_key, MRN_MAX_KEY_SIZE);
    uint encoded_key_length;
    storage_encode_multiple_column_key(key_info,
                                       (uchar*)(GRN_TEXT_VALUE(&key)),
                                       key_info->key_length,
                                       (uchar*)(GRN_TEXT_VALUE(&encoded_key)),
                                       &encoded_key_length);
    grn_bulk_space(ctx, &encoded_key, encoded_key_length);

    grn_rc rc;
    rc = grn_column_index_update(ctx,
                                 index_column,
                                 record_id,
                                 1,
                                 &encoded_key,
                                 NULL);
    if (rc) {
      error = ER_ERROR_ON_WRITE;
      my_message(error, ctx->errbuf, MYF(0));
      goto err;
    }
  }
err:
  grn_obj_unlink(ctx, &encoded_key);
  grn_obj_unlink(ctx, &key);

  DBUG_RETURN(error);
}

int ha_mroonga::storage_delete_row_unique_index(grn_obj* index_table,
                                                grn_id del_key_id)
{
  MRN_DBUG_ENTER_METHOD();
  grn_rc rc = grn_table_delete_by_id(ctx, index_table, del_key_id);
  if (rc) {
    my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  }
  DBUG_RETURN(0);
}

int ha_mroonga::storage_delete_row_unique_indexes()
{
  int error = 0, tmp_error;
  uint i;
  uint n_keys = table->s->keys;
  MRN_DBUG_ENTER_METHOD();

  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &table->key_info[i];
    if ((!(key_info->flags & HA_NOSAME)) || del_key_id[i] == GRN_ID_NIL) {
      continue;
    }

    grn_obj* index_table = grn_index_tables[i];
    if ((tmp_error =
           storage_delete_row_unique_index(index_table, del_key_id[i]))) {
      error = tmp_error;
    }
  }
  DBUG_RETURN(error);
}

int ha_mroonga::storage_prepare_delete_row_unique_index(const uchar* buf,
                                                        grn_id record_id,
                                                        KEY* key_info,
                                                        grn_obj* index_table,
                                                        grn_obj* index_column,
                                                        grn_id* del_key_id)
{
  const void* ukey = NULL;
  uint32 ukey_size = 0;
  MRN_DBUG_ENTER_METHOD();
  if (KEY_N_KEY_PARTS(key_info) == 1) {
    GRN_BULK_REWIND(&key_buffer);
    grn_obj_get_value(ctx, index_column, record_id, &key_buffer);
    ukey = GRN_TEXT_VALUE(&key_buffer);
    ukey_size = GRN_TEXT_LEN(&key_buffer);
  } else {
    mrn_change_encoding(ctx, NULL);
    uchar key[MRN_MAX_KEY_SIZE];
#ifdef MRN_HANDLER_HA_UPDATE_ROW_NEW_DATA_CONST
    const uchar* key_copy_data = buf;
#else
    uchar* key_copy_data = const_cast<uchar*>(buf);
#endif
    my_ptrdiff_t ptr_diff = mrn_compute_ptr_diff_for_key(buf, table->record[0]);
    for (uint i = 0; i < KEY_N_KEY_PARTS(key_info); ++i) {
      Field* field = key_info->key_part[i].field;
      field->move_field_offset(ptr_diff);
    }
    key_copy(key, key_copy_data, key_info, key_info->key_length);
    for (uint i = 0; i < KEY_N_KEY_PARTS(key_info); ++i) {
      Field* field = key_info->key_part[i].field;
      field->move_field_offset(-ptr_diff);
    }
    grn_bulk_reserve(ctx, &key_buffer, MRN_MAX_KEY_SIZE);
    ukey = GRN_TEXT_VALUE(&key_buffer);
    storage_encode_multiple_column_key(key_info,
                                       key,
                                       key_info->key_length,
                                       (uchar*)ukey,
                                       (uint*)&ukey_size);
  }
  *del_key_id = grn_table_get(ctx, index_table, ukey, ukey_size);
  DBUG_RETURN(0);
}

int ha_mroonga::storage_prepare_delete_row_unique_indexes(const uchar* buf,
                                                          grn_id record_id)
{
  int error = 0, tmp_error;
  uint i;
  uint n_keys = table->s->keys;
  MRN_DBUG_ENTER_METHOD();

  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &table->key_info[i];
    if (!(key_info->flags & HA_NOSAME)) {
      continue;
    }

    grn_obj* index_table = grn_index_tables[i];
    if (!index_table) {
      del_key_id[i] = GRN_ID_NIL;
      continue;
    }

    grn_obj* index_column;
    if (KEY_N_KEY_PARTS(key_info) == 1) {
      Field* field = key_info->key_part[0].field;
      mrn_change_encoding(ctx, field->charset());
      index_column = grn_columns[MRN_FIELD_FIELD_INDEX(field)];
    } else {
      mrn_change_encoding(ctx, NULL);
      index_column = grn_index_columns[i];
    }
    if ((tmp_error = storage_prepare_delete_row_unique_index(buf,
                                                             record_id,
                                                             key_info,
                                                             index_table,
                                                             index_column,
                                                             &del_key_id[i]))) {
      error = tmp_error;
    }
  }
  DBUG_RETURN(error);
}

int ha_mroonga::delete_row(const uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_delete_row(buf);
  } else {
#endif
    error = storage_delete_row(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
uint ha_mroonga::wrapper_max_supported_key_parts() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(MAX_REF_PARTS);
}
#endif

uint ha_mroonga::storage_max_supported_key_parts() const
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(MAX_REF_PARTS);
}

uint ha_mroonga::max_supported_key_parts() const
{
  MRN_DBUG_ENTER_METHOD();

  uint parts;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!share && !analyzed_for_create &&
      (thd_sql_command(ha_thd()) == SQLCOM_CREATE_TABLE ||
       thd_sql_command(ha_thd()) == SQLCOM_CREATE_INDEX ||
       thd_sql_command(ha_thd()) == SQLCOM_ALTER_TABLE)) {
    create_share_for_create();
  }
  if (analyzed_for_create && share_for_create.wrapper_mode) {
    parts = wrapper_max_supported_key_parts();
  } else if (wrap_handler && share && share->wrapper_mode) {
    parts = wrapper_max_supported_key_parts();
  } else {
#endif
    parts = storage_max_supported_key_parts();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

  DBUG_RETURN(parts);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
#  ifdef MRN_HANDLER_RECORDS_IN_RANGE_HAVE_PAGE_RANGE
ha_rows ha_mroonga::wrapper_records_in_range(uint key_nr,
                                             const key_range* range_min,
                                             const key_range* range_max,
                                             page_range* pages)
{
  ha_rows row_count;
  MRN_DBUG_ENTER_METHOD();
  KEY* key_info = &(table->s->key_info[key_nr]);
  if (mrn_is_geo_key(key_info)) {
    row_count = generic_records_in_range_geo(key_nr, range_min, range_max);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    row_count =
      wrap_handler->records_in_range(key_nr, range_min, range_max, pages);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(row_count);
}
#  else
ha_rows ha_mroonga::wrapper_records_in_range(uint key_nr,
                                             key_range* range_min,
                                             key_range* range_max)
{
  ha_rows row_count;
  MRN_DBUG_ENTER_METHOD();
  KEY* key_info = &(table->s->key_info[key_nr]);
  if (mrn_is_geo_key(key_info)) {
    row_count = generic_records_in_range_geo(key_nr, range_min, range_max);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    row_count = wrap_handler->records_in_range(key_nr, range_min, range_max);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(row_count);
}
#  endif
#endif

ha_rows ha_mroonga::storage_records_in_range(uint key_nr,
                                             const key_range* range_min,
                                             const key_range* range_max)
{
  MRN_DBUG_ENTER_METHOD();
  int flags = 0;
  uint size_min = 0, size_max = 0;
  ha_rows row_count = 0;
  uchar *key_min = NULL, *key_max = NULL;
  uchar key_min_entity[MRN_MAX_KEY_SIZE];
  uchar key_max_entity[MRN_MAX_KEY_SIZE];
  KEY* key_info = &(table->s->key_info[key_nr]);
  bool is_multiple_column_index = KEY_N_KEY_PARTS(key_info) > 1;

  if (is_multiple_column_index) {
    mrn_change_encoding(ctx, NULL);
    if (range_min && range_max && range_min->length == range_max->length &&
        memcmp(range_min->key, range_max->key, range_min->length) == 0) {
      flags |= GRN_CURSOR_PREFIX;
      key_min = key_min_entity;
      storage_encode_multiple_column_key(key_info,
                                         range_min->key,
                                         range_min->length,
                                         key_min,
                                         &size_min);
    } else {
      key_min = key_min_entity;
      key_max = key_max_entity;
      storage_encode_multiple_column_key_range(key_info,
                                               range_min,
                                               range_max,
                                               key_min,
                                               &size_min,
                                               key_max,
                                               &size_max);
    }
  } else if (mrn_is_geo_key(key_info)) {
    mrn_change_encoding(ctx, key_info->key_part->field->charset());
    row_count = generic_records_in_range_geo(key_nr, range_min, range_max);
    DBUG_RETURN(row_count);
  } else {
    Field* field = key_info->key_part[0].field;
    mrn_change_encoding(ctx, field->charset());

    if (FIELD_NAME_EQUAL(field, MRN_COLUMN_NAME_ID)) {
      DBUG_RETURN((ha_rows)1);
    }

    if (range_min) {
      if (field->null_bit && !range_min->key[0]) {
        mrn_bool is_null = false;
        key_min = key_min_entity;
        storage_encode_key(field, range_min->key, key_min, &size_min, &is_null);
        if (is_null) {
          key_min = NULL;
        } else {
          if (size_min == 0) {
            DBUG_RETURN(HA_POS_ERROR);
          }
        }
      }
    }
    if (range_max) {
      mrn_bool is_null = false;
      key_max = key_max_entity;
      storage_encode_key(field, range_max->key, key_max, &size_max, &is_null);
      if (is_null) {
        key_max = NULL;
      } else {
        if (size_max == 0) {
          DBUG_RETURN(HA_POS_ERROR);
        }
      }
    }
  }

  if (range_min) {
    DBUG_PRINT("info", ("mroonga: range_min->flag=%u", range_min->flag));
    if (range_min->flag == HA_READ_AFTER_KEY) {
      flags |= GRN_CURSOR_GT;
    }
  }
  if (range_max) {
    DBUG_PRINT("info", ("mroonga: range_min->flag=%u", range_max->flag));
    if (range_max->flag == HA_READ_BEFORE_KEY) {
      flags |= GRN_CURSOR_LT;
    }
  }

  int cursor_limit = THDVAR(ha_thd(), max_n_records_for_estimate);
  uint pkey_nr = table->s->primary_key;
  if (key_nr == pkey_nr) {
    DBUG_PRINT("info", ("mroonga: use primary key"));
    grn_table_cursor* cursor;
    cursor = grn_table_cursor_open(ctx,
                                   grn_table,
                                   key_min,
                                   size_min,
                                   key_max,
                                   size_max,
                                   0,
                                   cursor_limit,
                                   flags);
    while (grn_table_cursor_next(ctx, cursor) != GRN_ID_NIL) {
      row_count++;
    }
    grn_table_cursor_close(ctx, cursor);
  } else {
    if (is_multiple_column_index) {
      DBUG_PRINT("info", ("mroonga: use multiple column key%u", key_nr));
    } else {
      DBUG_PRINT("info", ("mroonga: use key%u", key_nr));
    }

    grn_table_cursor* cursor;
    cursor = grn_table_cursor_open(ctx,
                                   grn_index_tables[key_nr],
                                   key_min,
                                   size_min,
                                   key_max,
                                   size_max,
                                   0,
                                   cursor_limit,
                                   flags);
    grn_obj* index_column = grn_index_columns[key_nr];
    grn_ii* ii = reinterpret_cast<grn_ii*>(index_column);
    row_count = grn_ii_estimate_size_for_lexicon_cursor(ctx, ii, cursor);
    grn_table_cursor_close(ctx, cursor);

    unsigned int max_n_lexicon_records =
      grn_table_size(ctx, grn_index_tables[key_nr]);
    if (cursor_limit >= 0 &&
        static_cast<unsigned int>(cursor_limit) < max_n_lexicon_records) {
      row_count++;
    }
  }
  DBUG_RETURN(row_count);
}

ha_rows ha_mroonga::generic_records_in_range_geo(uint key_nr,
                                                 const key_range* range_min,
                                                 const key_range* range_max)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows row_count;
  int error;

  if (!range_min) {
    DBUG_PRINT("info",
               ("mroonga: range min is missing for geometry range search"));
    DBUG_RETURN(HA_POS_ERROR);
  }
  if (range_max) {
    DBUG_PRINT("info",
               ("mroonga: range max is specified for geometry range search"));
    DBUG_RETURN(HA_POS_ERROR);
  }
  Field_geom* field =
    static_cast<Field_geom*>(table->key_info[key_nr].key_part->field);
  error = mrn_change_encoding(ctx, field->charset());
  if (error)
    DBUG_RETURN(error);
  if (!(range_min->flag & HA_READ_MBR_CONTAIN)) {
    push_warning_unsupported_spatial_index_search(range_min->flag);
    row_count = grn_table_size(ctx, grn_table);
    DBUG_RETURN(row_count);
  }

  geo_store_rectangle(range_min->key, geo_need_reverse(field));
  row_count = grn_geo_estimate_in_rectangle(ctx,
                                            grn_index_columns[key_nr],
                                            &top_left_point,
                                            &bottom_right_point);
  DBUG_RETURN(row_count);
}

#ifdef MRN_HANDLER_RECORDS_IN_RANGE_HAVE_PAGE_RANGE
ha_rows ha_mroonga::records_in_range(uint key_nr,
                                     const key_range* range_min,
                                     const key_range* range_max,
                                     page_range* pages)
{
  MRN_DBUG_ENTER_METHOD();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  ha_rows row_count = 0;
  if (share->wrapper_mode) {
    row_count = wrapper_records_in_range(key_nr, range_min, range_max, pages);
  } else {
#  endif
    row_count = storage_records_in_range(key_nr, range_min, range_max);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_PRINT("info", ("mroonga: row_count=%" MRN_HA_ROWS_FORMAT, row_count));
  DBUG_RETURN(row_count);
}
#else
ha_rows ha_mroonga::records_in_range(uint key_nr,
                                     key_range* range_min,
                                     key_range* range_max)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows row_count = 0;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    row_count = wrapper_records_in_range(key_nr, range_min, range_max);
  } else {
#  endif
    row_count = storage_records_in_range(key_nr, range_min, range_max);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_PRINT("info", ("mroonga: row_count=%" MRN_HA_ROWS_FORMAT, row_count));
  DBUG_RETURN(row_count);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_init(uint idx, bool sorted)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  KEY* key_info = &(table->s->key_info[idx]);
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (!mrn_is_geo_key(key_info) && key_info->algorithm != HA_KEY_ALG_FULLTEXT) {
    error = wrap_handler->ha_index_init(share->wrap_key_nr[idx], sorted);
  } else {
    error = wrap_handler->ha_index_init(share->wrap_primary_key, sorted);
  }
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_init(uint idx, bool sorted)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(0);
}

int ha_mroonga::index_init(uint idx, bool sorted)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_PRINT("info", ("mroonga: idx=%u", idx));
  active_index = idx;
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_init(idx, sorted);
  } else {
#endif
    error = storage_index_init(idx, sorted);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_end()
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_index_or_rnd_end();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_end()
{
  MRN_DBUG_ENTER_METHOD();
  clear_cursor();
  clear_cursor_geo();
  DBUG_RETURN(0);
}

int ha_mroonga::index_end()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_end();
  } else {
#endif
    error = storage_index_end();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_read_map(uchar* buf,
                                       const uchar* key,
                                       key_part_map keypart_map,
                                       enum ha_rkey_function find_flag)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  KEY* key_info = &(table->key_info[active_index]);
  if (mrn_is_geo_key(key_info)) {
    clear_cursor_geo();
    error = generic_geo_open_cursor(key, find_flag);
    if (!error) {
      error = wrapper_get_next_geo_record(buf);
    }
    DBUG_RETURN(error);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    if (fulltext_searching)
      set_pk_bitmap();
    error = wrap_handler->ha_index_read_map(buf, key, keypart_map, find_flag);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_read_map(uchar* buf,
                                       const uchar* key,
                                       key_part_map keypart_map,
                                       enum ha_rkey_function find_flag)
{
  MRN_DBUG_ENTER_METHOD();
  check_count_skip(keypart_map);

  int error = 0;

  uint key_nr = active_index;
  KEY* key_info = &(table->key_info[key_nr]);
  int flags = 0;
  uint size_min = 0, size_max = 0;
  uchar *key_min = NULL, *key_max = NULL;
  uchar key_min_entity[MRN_MAX_KEY_SIZE];
  uchar key_max_entity[MRN_MAX_KEY_SIZE];

  clear_cursor();
  clear_cursor_geo();
  clear_empty_value_records();

  switch (find_flag) {
  case HA_READ_BEFORE_KEY:
    flags |= GRN_CURSOR_LT | GRN_CURSOR_DESCENDING;
    break;
  case HA_READ_PREFIX_LAST:
    flags |= GRN_CURSOR_PREFIX | GRN_CURSOR_DESCENDING;
    break;
  case HA_READ_PREFIX_LAST_OR_PREV:
    flags |= GRN_CURSOR_LE | GRN_CURSOR_DESCENDING;
    break;
  case HA_READ_AFTER_KEY:
    flags |= GRN_CURSOR_GT | GRN_CURSOR_ASCENDING;
    break;
  case HA_READ_KEY_OR_NEXT:
    flags |= GRN_CURSOR_GE | GRN_CURSOR_ASCENDING;
    break;
  case HA_READ_KEY_EXACT:
    flags |= GRN_CURSOR_LE | GRN_CURSOR_GE;
    break;
  default:
    break;
  }

  bool is_multiple_column_index = KEY_N_KEY_PARTS(key_info) > 1;
  if (is_multiple_column_index) {
    mrn_change_encoding(ctx, NULL);
    uint key_length =
      mrn_calculate_key_len(table, active_index, key, keypart_map);
    DBUG_PRINT("info",
               ("mroonga: multiple column index: "
                "search key length=<%u>, "
                "multiple column index key length=<%u>",
                key_length,
                key_info->key_length));
    if (key_length == key_info->key_length) {
      switch (find_flag) {
      case HA_READ_BEFORE_KEY:
      case HA_READ_PREFIX_LAST_OR_PREV:
        key_max = key_max_entity;
        storage_encode_multiple_column_key(key_info,
                                           key,
                                           key_length,
                                           key_max,
                                           &size_max);
        break;
      case HA_READ_PREFIX_LAST:
        key_min = key_min_entity;
        storage_encode_multiple_column_key(key_info,
                                           key,
                                           key_length,
                                           key_min,
                                           &size_min);
        break;
      default:
        key_min = key_min_entity;
        storage_encode_multiple_column_key(key_info,
                                           key,
                                           key_length,
                                           key_min,
                                           &size_min);
        if (find_flag == HA_READ_KEY_EXACT) {
          key_max = key_min;
          size_max = size_min;
        }
        break;
      }
    } else {
      const uchar* prev_key = NULL;
      uint prev_key_length = 0;
      if ((keypart_map >> 1) > 0) {
        prev_key = key;
        prev_key_length =
          mrn_calculate_key_len(table, active_index, key, keypart_map >> 1);
      }
      switch (find_flag) {
      case HA_READ_BEFORE_KEY:
        if (prev_key) {
          flags |= GRN_CURSOR_GE;
          key_min = key_min_entity;
          storage_encode_multiple_column_key_range(key_info,
                                                   prev_key,
                                                   prev_key_length,
                                                   NULL,
                                                   0,
                                                   key_min,
                                                   &size_min,
                                                   NULL,
                                                   NULL);
        }
        key_max = key_max_entity;
        storage_encode_multiple_column_key_range(key_info,
                                                 key,
                                                 key_length,
                                                 NULL,
                                                 0,
                                                 key_max,
                                                 &size_max,
                                                 NULL,
                                                 NULL);
        break;
      case HA_READ_PREFIX_LAST:
        key_min = key_min_entity;
        storage_encode_multiple_column_key(key_info,
                                           key,
                                           key_length,
                                           key_min,
                                           &size_min);
        break;
      case HA_READ_PREFIX_LAST_OR_PREV:
        if (prev_key) {
          flags |= GRN_CURSOR_GE;
          key_min = key_min_entity;
          storage_encode_multiple_column_key_range(key_info,
                                                   prev_key,
                                                   prev_key_length,
                                                   NULL,
                                                   0,
                                                   key_min,
                                                   &size_min,
                                                   NULL,
                                                   NULL);
        }
        key_max = key_max_entity;
        storage_encode_multiple_column_key_range(key_info,
                                                 NULL,
                                                 0,
                                                 key,
                                                 key_length,
                                                 NULL,
                                                 NULL,
                                                 key_max,
                                                 &size_max);
        break;
      case HA_READ_AFTER_KEY:
        key_min = key_min_entity;
        storage_encode_multiple_column_key_range(key_info,
                                                 NULL,
                                                 0,
                                                 key,
                                                 key_length,
                                                 NULL,
                                                 NULL,
                                                 key_min,
                                                 &size_min);
        if (prev_key) {
          flags |= GRN_CURSOR_LE;
          key_max = key_max_entity;
          storage_encode_multiple_column_key_range(key_info,
                                                   NULL,
                                                   0,
                                                   prev_key,
                                                   prev_key_length,
                                                   NULL,
                                                   NULL,
                                                   key_max,
                                                   &size_max);
        }
        break;
      case HA_READ_KEY_OR_NEXT:
        key_min = key_min_entity;
        storage_encode_multiple_column_key_range(key_info,
                                                 key,
                                                 key_length,
                                                 NULL,
                                                 0,
                                                 key_min,
                                                 &size_min,
                                                 NULL,
                                                 NULL);
        if (prev_key) {
          flags |= GRN_CURSOR_LE;
          key_max = key_max_entity;
          storage_encode_multiple_column_key_range(key_info,
                                                   NULL,
                                                   0,
                                                   prev_key,
                                                   prev_key_length,
                                                   NULL,
                                                   NULL,
                                                   key_max,
                                                   &size_max);
        }
        break;
      case HA_READ_KEY_EXACT:
        key_min = key_min_entity;
        key_max = key_max_entity;
        storage_encode_multiple_column_key_range(key_info,
                                                 key,
                                                 key_length,
                                                 key,
                                                 key_length,
                                                 key_min,
                                                 &size_min,
                                                 key_max,
                                                 &size_max);
      default:
        break;
      }
    }
  } else if (mrn_is_geo_key(key_info)) {
    error = mrn_change_encoding(ctx, key_info->key_part->field->charset());
    if (error)
      DBUG_RETURN(error);
    error = generic_geo_open_cursor(key, find_flag);
    if (!error) {
      error = storage_get_next_record(buf);
    }
    DBUG_RETURN(error);
  } else {
    Field* field = key_info->key_part[0].field;
    error = mrn_change_encoding(ctx, field->charset());
    if (error)
      DBUG_RETURN(error);

    if (find_flag == HA_READ_KEY_EXACT) {
      mrn_bool is_null = false;
      key_min = key_min_entity;
      key_max = key_min_entity;
      storage_encode_key(field, key, key_min, &size_min, &is_null);
      if (is_null) {
        key_min = NULL;
        key_max = NULL;
      } else {
        size_max = size_min;
      }
      // for _id
      if (FIELD_NAME_EQUAL(field, MRN_COLUMN_NAME_ID)) {
        grn_id found_record_id = *((grn_id*)key_min);
        if (grn_table_at(ctx, grn_table, found_record_id) !=
            GRN_ID_NIL) { // found
          storage_store_fields(table, buf, found_record_id);
          MRN_TABLE_SET_FOUND_ROW(table);
          record_id = found_record_id;
          DBUG_RETURN(0);
        } else {
          MRN_TABLE_SET_NO_ROW(table);
          DBUG_RETURN(HA_ERR_END_OF_FILE);
        }
      }
    } else if (find_flag == HA_READ_BEFORE_KEY ||
               find_flag == HA_READ_PREFIX_LAST_OR_PREV) {
      mrn_bool is_null = false;
      key_max = key_max_entity;
      storage_encode_key(field, key, key_max_entity, &size_max, &is_null);
      if (is_null) {
        key_max = NULL;
      }
    } else {
      mrn_bool is_null = false;
      key_min = key_min_entity;
      storage_encode_key(field, key, key_min_entity, &size_min, &is_null);
      if (is_null) {
        key_min = NULL;
      }
    }
  }

  uint pkey_nr = table->s->primary_key;
  if (key_nr == pkey_nr) {
    DBUG_PRINT("info", ("mroonga: use primary key"));
    if (flags == 0 && size_min == 0 && size_max == 0) {
      // Groonga doesn't allow empty primary key. So exact empty
      // primary key search never match any records.
      empty_value_records =
        grn_table_create(ctx,
                         NULL,
                         0,
                         NULL,
                         GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_WITH_SUBREC,
                         grn_table,
                         0);
      empty_value_records_cursor = grn_table_cursor_open(ctx,
                                                         empty_value_records,
                                                         NULL,
                                                         0,
                                                         NULL,
                                                         0,
                                                         0,
                                                         -1,
                                                         flags);
    } else {
      cursor = grn_table_cursor_open(ctx,
                                     grn_table,
                                     key_min,
                                     size_min,
                                     key_max,
                                     size_max,
                                     0,
                                     -1,
                                     flags);
    }
  } else {
    bool is_empty_value_records_search = false;
    if (is_multiple_column_index) {
      DBUG_PRINT("info", ("mroonga: use multiple column key%u", key_nr));
    } else if (flags == 0 && size_min == 0 && size_max == 0) {
      is_empty_value_records_search = true;
      DBUG_PRINT("info",
                 ("mroonga: use table scan for searching empty value records"));
    } else {
      DBUG_PRINT("info", ("mroonga: use key%u", key_nr));
    }
    if (is_empty_value_records_search) {
      grn_obj *expression, *expression_variable;
      GRN_EXPR_CREATE_FOR_QUERY(ctx,
                                grn_table,
                                expression,
                                expression_variable);
      grn_obj* target_column =
        grn_columns[MRN_FIELD_FIELD_INDEX(key_info->key_part->field)];
      grn_expr_append_const(ctx,
                            expression,
                            target_column,
                            GRN_OP_GET_VALUE,
                            1);
      grn_obj empty_value;
      GRN_TEXT_INIT(&empty_value, 0);
      grn_expr_append_obj(ctx, expression, &empty_value, GRN_OP_PUSH, 1);
      grn_expr_append_op(ctx, expression, GRN_OP_EQUAL, 2);

      empty_value_records =
        grn_table_create(ctx,
                         NULL,
                         0,
                         NULL,
                         GRN_OBJ_TABLE_HASH_KEY | GRN_OBJ_WITH_SUBREC,
                         grn_table,
                         0);
      grn_table_select(ctx,
                       grn_table,
                       expression,
                       empty_value_records,
                       GRN_OP_OR);
      grn_obj_unlink(ctx, expression);
      grn_obj_unlink(ctx, &empty_value);

      empty_value_records_cursor = grn_table_cursor_open(ctx,
                                                         empty_value_records,
                                                         NULL,
                                                         0,
                                                         NULL,
                                                         0,
                                                         0,
                                                         -1,
                                                         flags);
    } else {
      index_table_cursor = grn_table_cursor_open(ctx,
                                                 grn_index_tables[key_nr],
                                                 key_min,
                                                 size_min,
                                                 key_max,
                                                 size_max,
                                                 0,
                                                 -1,
                                                 flags);
      cursor = grn_index_cursor_open(ctx,
                                     index_table_cursor,
                                     grn_index_columns[key_nr],
                                     0,
                                     GRN_ID_MAX,
                                     0);
    }
  }
  if (ctx->rc) {
    my_message(ER_ERROR_ON_READ, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_READ);
  }
  error = storage_get_next_record(buf);
  DBUG_RETURN(error);
}

int ha_mroonga::index_read_map(uchar* buf,
                               const uchar* key,
                               key_part_map keypart_map,
                               enum ha_rkey_function find_flag)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_read_map(buf, key, keypart_map, find_flag);
  } else {
#endif
    error = storage_index_read_map(buf, key, keypart_map, find_flag);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_PRINT("info", ("mroonga: error=%d", error));
  DBUG_RETURN(error);
}

#ifdef MRN_HANDLER_HAVE_INDEX_READ_LAST_MAP
#  ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_read_last_map(uchar* buf,
                                            const uchar* key,
                                            key_part_map keypart_map)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
  error = wrap_handler->ha_index_read_last_map(buf, key, keypart_map);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#  endif

int ha_mroonga::storage_index_read_last_map(uchar* buf,
                                            const uchar* key,
                                            key_part_map keypart_map)
{
  MRN_DBUG_ENTER_METHOD();
  uint key_nr = active_index;
  KEY* key_info = &(table->key_info[key_nr]);

  int flags = GRN_CURSOR_DESCENDING, error;
  uint size_min = 0, size_max = 0;
  uchar *key_min = NULL, *key_max = NULL;
  uchar key_min_entity[MRN_MAX_KEY_SIZE];

  clear_cursor();

  bool is_multiple_column_index = KEY_N_KEY_PARTS(key_info) > 1;
  if (is_multiple_column_index) {
    mrn_change_encoding(ctx, NULL);
    flags |= GRN_CURSOR_PREFIX;
    uint key_length =
      mrn_calculate_key_len(table, active_index, key, keypart_map);
    key_min = key_min_entity;
    storage_encode_multiple_column_key(key_info,
                                       key,
                                       key_length,
                                       key_min,
                                       &size_min);
  } else {
    Field* field = key_info->key_part[0].field;
    error = mrn_change_encoding(ctx, field->charset());
    if (error)
      DBUG_RETURN(error);

    mrn_bool is_null = false;
    key_min = key_min_entity;
    key_max = key_min_entity;
    storage_encode_key(field, key, key_min, &size_min, &is_null);
    if (is_null) {
      key_min = NULL;
      key_max = NULL;
    } else {
      size_max = size_min;
    }
  }

  uint pkey_nr = table->s->primary_key;
  if (key_nr == pkey_nr) {
    DBUG_PRINT("info", ("mroonga: use primary key"));
    cursor = grn_table_cursor_open(ctx,
                                   grn_table,
                                   key_min,
                                   size_min,
                                   key_max,
                                   size_max,
                                   0,
                                   -1,
                                   flags);
  } else {
    if (is_multiple_column_index) {
      DBUG_PRINT("info", ("mroonga: use multiple column key%u", key_nr));
    } else {
      DBUG_PRINT("info", ("mroonga: use key%u", key_nr));
    }
    index_table_cursor = grn_table_cursor_open(ctx,
                                               grn_index_tables[key_nr],
                                               key_min,
                                               size_min,
                                               key_max,
                                               size_max,
                                               0,
                                               -1,
                                               flags);
    cursor = grn_index_cursor_open(ctx,
                                   index_table_cursor,
                                   grn_index_columns[key_nr],
                                   0,
                                   GRN_ID_MAX,
                                   0);
  }
  if (ctx->rc) {
    my_message(ER_ERROR_ON_READ, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_READ);
  }
  error = storage_get_next_record(buf);
  DBUG_RETURN(error);
}

int ha_mroonga::index_read_last_map(uchar* buf,
                                    const uchar* key,
                                    key_part_map keypart_map)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_read_last_map(buf, key, keypart_map);
  } else {
#  endif
    error = storage_index_read_last_map(buf, key, keypart_map);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(error);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_next(uchar* buf)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  KEY* key_info = &(table->key_info[active_index]);
  if (mrn_is_geo_key(key_info)) {
    error = wrapper_get_next_geo_record(buf);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    if (fulltext_searching)
      set_pk_bitmap();
    error = wrap_handler->ha_index_next(buf);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_next(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = storage_get_next_record(buf);
  DBUG_RETURN(error);
}

int ha_mroonga::index_next(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_next(buf);
  } else {
#endif
    error = storage_index_next(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_prev(uchar* buf)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  KEY* key_info = &(table->key_info[active_index]);
  if (mrn_is_geo_key(key_info)) {
    error = wrapper_get_next_geo_record(buf);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    if (fulltext_searching)
      set_pk_bitmap();
    error = wrap_handler->ha_index_prev(buf);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_prev(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = storage_get_next_record(buf);
  DBUG_RETURN(error);
}

int ha_mroonga::index_prev(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_prev(buf);
  } else {
#endif
    error = storage_index_prev(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_first(uchar* buf)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
  error = wrap_handler->ha_index_first(buf);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_first(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  clear_cursor();
  int flags = GRN_CURSOR_ASCENDING;
  uint pkey_nr = table->s->primary_key;
  mrn_change_encoding(ctx, NULL);
  if (active_index == pkey_nr ||
      table->key_info[active_index].algorithm == HA_KEY_ALG_FULLTEXT) {
    DBUG_PRINT("info", ("mroonga: use primary key"));
    cursor =
      grn_table_cursor_open(ctx, grn_table, NULL, 0, NULL, 0, 0, -1, flags);
  } else {
    if (KEY_N_KEY_PARTS(&(table->key_info[active_index])) > 1) {
      DBUG_PRINT("info", ("mroonga: use multiple column key%u", active_index));
    } else {
      DBUG_PRINT("info", ("mroonga: use key%u", active_index));
    }
    index_table_cursor = grn_table_cursor_open(ctx,
                                               grn_index_tables[active_index],
                                               NULL,
                                               0,
                                               NULL,
                                               0,
                                               0,
                                               -1,
                                               flags);
    cursor = grn_index_cursor_open(ctx,
                                   index_table_cursor,
                                   grn_index_columns[active_index],
                                   0,
                                   GRN_ID_MAX,
                                   0);
  }
  if (ctx->rc) {
    my_message(ER_ERROR_ON_READ, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_READ);
  }
  int error = storage_get_next_record(buf);
  DBUG_RETURN(error);
}

int ha_mroonga::index_first(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_first(buf);
  } else {
#endif
    error = storage_index_first(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_last(uchar* buf)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
  error = wrap_handler->ha_index_last(buf);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_last(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  clear_cursor();
  int flags = GRN_CURSOR_DESCENDING;
  uint pkey_nr = table->s->primary_key;
  mrn_change_encoding(ctx, NULL);
  if (active_index == pkey_nr ||
      table->key_info[active_index].algorithm == HA_KEY_ALG_FULLTEXT) {
    DBUG_PRINT("info", ("mroonga: use primary key"));
    cursor =
      grn_table_cursor_open(ctx, grn_table, NULL, 0, NULL, 0, 0, -1, flags);
  } else {
    if (KEY_N_KEY_PARTS(&(table->key_info[active_index])) > 1) {
      DBUG_PRINT("info", ("mroonga: use multiple column key%u", active_index));
    } else {
      DBUG_PRINT("info", ("mroonga: use key%u", active_index));
    }
    index_table_cursor = grn_table_cursor_open(ctx,
                                               grn_index_tables[active_index],
                                               NULL,
                                               0,
                                               NULL,
                                               0,
                                               0,
                                               -1,
                                               flags);
    cursor = grn_index_cursor_open(ctx,
                                   index_table_cursor,
                                   grn_index_columns[active_index],
                                   0,
                                   GRN_ID_MAX,
                                   0);
  }
  if (ctx->rc) {
    my_message(ER_ERROR_ON_READ, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_READ);
  }
  int error = storage_get_next_record(buf);
  DBUG_RETURN(error);
}

int ha_mroonga::index_last(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_last(buf);
  } else {
#endif
    error = storage_index_last(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_index_next_same(uchar* buf,
                                        const uchar* key,
                                        uint keylen)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  KEY* key_info = &(table->s->key_info[active_index]);
  if (mrn_is_geo_key(key_info)) {
    error = wrapper_get_next_geo_record(buf);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    if (fulltext_searching)
      set_pk_bitmap();
    error = wrap_handler->ha_index_next_same(buf, key, keylen);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_index_next_same(uchar* buf,
                                        const uchar* key,
                                        uint keylen)
{
  MRN_DBUG_ENTER_METHOD();
  int error = storage_get_next_record(count_skip ? NULL : buf);
  DBUG_RETURN(error);
}

int ha_mroonga::index_next_same(uchar* buf, const uchar* key, uint keylen)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_index_next_same(buf, key, keylen);
  } else {
#endif
    error = storage_index_next_same(buf, key, keylen);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

int ha_mroonga::generic_ft_init()
{
  MRN_DBUG_ENTER_METHOD();

  check_count_skip(0);

  st_mrn_ft_info* mrn_ft_info = reinterpret_cast<st_mrn_ft_info*>(ft_handler);
  GRN_CTX_SET_ENCODING(ctx, mrn_ft_info->encoding);

  // MariaDB uses index_init()/ft_init()/ft_read()/index_end() for
  //   SELECT *
  //       FROM table
  //      WHERE MATCH(...) AGAINST(...)
  //   ORDER BY MATCH(...) AGAINST(...)
  //      LIMIT X
  // even when condition is pushed down.
  // FT_SORTED isn't used. All matched records are fetched.
  //
  // MySQL uses FT_SORTED. LIMIT records are only fetched.

  bool need_fast_order_limit_check;
#ifdef MRN_MARIADB_P
  if (share->wrapper_mode) {
    need_fast_order_limit_check = true;
  } else if (inited == INDEX) {
    need_fast_order_limit_check = true;
  } else {
    need_fast_order_limit_check = !pushed_cond;
  }
#else
  need_fast_order_limit_check = !pushed_cond;
#endif
  mrn_generic_ft_ensure_searched(mrn_ft_info, need_fast_order_limit_check);

  int error = 0;
  if (mrn_ft_info->sorted_result) {
    mrn_ft_info->cursor = grn_table_cursor_open(ctx,
                                                mrn_ft_info->sorted_result,
                                                NULL,
                                                0,
                                                NULL,
                                                0,
                                                0,
                                                -1,
                                                0);
  } else {
    mrn_ft_info->cursor = grn_table_cursor_open(ctx,
                                                mrn_ft_info->result,
                                                NULL,
                                                0,
                                                NULL,
                                                0,
                                                0,
                                                -1,
                                                0);
  }
  if (ctx->rc) {
    error = ER_ERROR_ON_READ;
    my_message(error, ctx->errbuf, MYF(0));
  } else {
    if (mrn_ft_info->sorted_result) {
      if (grn_table->header.type == GRN_TABLE_NO_KEY) {
        mrn_ft_info->id_accessor = grn_obj_column(ctx,
                                                  mrn_ft_info->sorted_result,
                                                  MRN_COLUMN_NAME_ID,
                                                  strlen(MRN_COLUMN_NAME_ID));
      } else {
        mrn_ft_info->key_accessor = grn_obj_column(ctx,
                                                   mrn_ft_info->sorted_result,
                                                   MRN_COLUMN_NAME_KEY,
                                                   strlen(MRN_COLUMN_NAME_KEY));
      }
    } else {
      mrn_ft_info->key_accessor = grn_obj_column(ctx,
                                                 mrn_ft_info->result,
                                                 MRN_COLUMN_NAME_KEY,
                                                 strlen(MRN_COLUMN_NAME_KEY));
    }
  }
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_ft_init()
{
  MRN_DBUG_ENTER_METHOD();
  int error = generic_ft_init();
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_ft_init()
{
  MRN_DBUG_ENTER_METHOD();
  int error = generic_ft_init();
  record_id = GRN_ID_NIL;
  DBUG_RETURN(error);
}

int ha_mroonga::ft_init()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_ft_init();
  } else {
#endif
    error = storage_ft_init();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

st_mrn_ft_info*
ha_mroonga::generic_ft_init_ext_select(uint flags, uint key_nr, String* key)
{
  MRN_DBUG_ENTER_METHOD();

  st_mrn_ft_info* info = new st_mrn_ft_info();
  info->mroonga = this;
  info->ctx = ctx;
  mrn_change_encoding(info->ctx,
                      table->key_info[key_nr].key_part->field->charset());
  info->encoding = GRN_CTX_GET_ENCODING(info->ctx);
  info->flags = flags;
  info->active_index = key_nr;
  GRN_TEXT_INIT(&(info->query), 0);
  GRN_TEXT_SET(ctx, &(info->query), key->ptr(), key->length());
  info->table = grn_table;
  info->index_column = grn_index_columns[info->active_index];
  info->expression = NULL;
  info->match_columns = NULL;
  info->result = NULL;
  info->sorted_result = NULL;
  info->score_column = NULL;
  GRN_TEXT_INIT(&(info->key), 0);
  grn_bulk_space(info->ctx, &(info->key), table->key_info->key_length);
  GRN_INT32_INIT(&(info->score), 0);
  info->key_info = &(table->key_info[key_nr]);
  info->primary_key_info = &(table->key_info[table_share->primary_key]);
  info->cursor = NULL;
  info->id_accessor = NULL;
  info->key_accessor = NULL;

  if (GRN_TEXT_LEN(&(info->query)) == 0) {
    DBUG_RETURN(info);
  }

  grn_obj* expression_variable;
  GRN_EXPR_CREATE_FOR_QUERY(info->ctx,
                            info->table,
                            info->expression,
                            expression_variable);
  // TODO: Unify this code with ConditionConverter
  if (info->flags & FT_BOOL) {
    grn_obj* match_columns_variable;
    GRN_EXPR_CREATE_FOR_QUERY(info->ctx,
                              info->table,
                              info->match_columns,
                              match_columns_variable);
    mrn::QueryParser query_parser(info->ctx,
                                  ha_thd(),
                                  info->expression,
                                  info->index_column,
                                  KEY_N_KEY_PARTS(info->key_info),
                                  info->match_columns);
    query_parser.parse(GRN_TEXT_VALUE(&(info->query)),
                       GRN_TEXT_LEN(&(info->query)));
  } else {
    grn_expr_append_obj(info->ctx,
                        info->expression,
                        info->index_column,
                        GRN_OP_PUSH,
                        1);
    grn_expr_append_const(info->ctx,
                          info->expression,
                          &(info->query),
                          GRN_OP_PUSH,
                          1);
    grn_expr_append_op(info->ctx, info->expression, GRN_OP_SIMILAR, 2);
  }

  DBUG_RETURN(info);
}

FT_INFO* ha_mroonga::generic_ft_init_ext(uint flags, uint key_nr, String* key)
{
  MRN_DBUG_ENTER_METHOD();

  mrn_change_encoding(ctx, system_charset_info);

  st_mrn_ft_info* info = generic_ft_init_ext_select(flags, key_nr, key);
  DBUG_RETURN(reinterpret_cast<FT_INFO*>(info));
}

#ifdef MRN_ENABLE_WRAPPER_MODE
FT_INFO* ha_mroonga::wrapper_ft_init_ext(uint flags, uint key_nr, String* key)
{
  MRN_DBUG_ENTER_METHOD();

  FT_INFO* info = generic_ft_init_ext(flags, key_nr, key);
  if (!info) {
    DBUG_RETURN(NULL);
  }

  st_mrn_ft_info* mrn_ft_info = reinterpret_cast<st_mrn_ft_info*>(info);
  mrn_ft_info->please = &mrn_wrapper_ft_vft;
  mrn_ft_info->could_you = &mrn_wrapper_ft_vft_ext;
  ++wrap_ft_init_count;

  DBUG_RETURN(info);
}
#endif

FT_INFO* ha_mroonga::storage_ft_init_ext(uint flags, uint key_nr, String* key)
{
  MRN_DBUG_ENTER_METHOD();

  FT_INFO* info = generic_ft_init_ext(flags, key_nr, key);
  if (!info) {
    DBUG_RETURN(NULL);
  }

  st_mrn_ft_info* mrn_ft_info = reinterpret_cast<st_mrn_ft_info*>(info);
  mrn_ft_info->please = &mrn_storage_ft_vft;
  mrn_ft_info->could_you = &mrn_storage_ft_vft_ext;
  DBUG_RETURN(info);
}

FT_INFO* ha_mroonga::ft_init_ext(uint flags, uint key_nr, String* key)
{
  MRN_DBUG_ENTER_METHOD();
  fulltext_searching = true;
  FT_INFO* info;
  if (key_nr == NO_SUCH_KEY) {
    st_mrn_ft_info* mrn_ft_info = new st_mrn_ft_info();
    mrn_ft_info->please = &mrn_no_such_key_ft_vft;
    mrn_ft_info->could_you = &mrn_no_such_key_ft_vft_ext;
    info = reinterpret_cast<FT_INFO*>(mrn_ft_info);
  } else {
#ifdef MRN_ENABLE_WRAPPER_MODE
    if (share->wrapper_mode) {
      info = wrapper_ft_init_ext(flags, key_nr, key);
    } else {
#endif
      info = storage_ft_init_ext(flags, key_nr, key);
#ifdef MRN_ENABLE_WRAPPER_MODE
    }
#endif
  }
  DBUG_RETURN(info);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_ft_read(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  if (wrap_ft_init_count)
    set_pk_bitmap();

  st_mrn_ft_info* mrn_ft_info = reinterpret_cast<st_mrn_ft_info*>(ft_handler);
  GRN_CTX_SET_ENCODING(ctx, mrn_ft_info->encoding);

  int error = 0;
  do {
    grn_id found_record_id;
    found_record_id = grn_table_cursor_next(ctx, mrn_ft_info->cursor);
    if (found_record_id == GRN_ID_NIL) {
      error = HA_ERR_END_OF_FILE;
      break;
    }

    GRN_BULK_REWIND(&key_buffer);
    if (mrn_ft_info->key_accessor) {
      grn_obj_get_value(ctx,
                        mrn_ft_info->key_accessor,
                        found_record_id,
                        &key_buffer);
    } else {
      void* key;
      int key_length;
      key_length = grn_table_cursor_get_key(ctx, mrn_ft_info->cursor, &key);
      GRN_TEXT_SET(ctx, &key_buffer, key, key_length);
    }
    error = wrapper_get_record(buf, (const uchar*)GRN_TEXT_VALUE(&key_buffer));
  } while (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_ft_read(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  struct st_mrn_ft_info* mrn_ft_info =
    reinterpret_cast<struct st_mrn_ft_info*>(ft_handler);
  GRN_CTX_SET_ENCODING(ctx, mrn_ft_info->encoding);

  grn_id found_record_id;
  found_record_id = grn_table_cursor_next(ctx, mrn_ft_info->cursor);
  if (ctx->rc) {
    my_message(ER_ERROR_ON_READ, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_READ);
  }

  if (found_record_id == GRN_ID_NIL) {
    MRN_TABLE_SET_NO_ROW(table);
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  MRN_TABLE_SET_FOUND_ROW(table);

  if (count_skip && record_id != GRN_ID_NIL) {
    DBUG_RETURN(0);
  }

  GRN_BULK_REWIND(&key_buffer);
  if (mrn_ft_info->id_accessor) {
    grn_obj id_buffer;
    GRN_RECORD_INIT(&id_buffer, 0, grn_obj_id(ctx, grn_table));
    grn_obj_get_value(ctx,
                      mrn_ft_info->id_accessor,
                      found_record_id,
                      &id_buffer);
    record_id = GRN_RECORD_VALUE(&id_buffer);
  } else if (mrn_ft_info->key_accessor) {
    grn_obj_get_value(ctx,
                      mrn_ft_info->key_accessor,
                      found_record_id,
                      &key_buffer);
    record_id = grn_table_get(ctx,
                              grn_table,
                              GRN_TEXT_VALUE(&key_buffer),
                              GRN_TEXT_LEN(&key_buffer));
  } else {
    void* key;
    grn_table_cursor_get_key(ctx, mrn_ft_info->cursor, &key);
    if (ctx->rc) {
      record_id = GRN_ID_NIL;
      my_message(ER_ERROR_ON_READ, ctx->errbuf, MYF(0));
      DBUG_RETURN(ER_ERROR_ON_READ);
    } else {
      record_id = *((grn_id*)key);
    }
  }
  storage_store_fields(table, buf, record_id);
  DBUG_RETURN(0);
}

int ha_mroonga::ft_read(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_ft_read(buf);
  } else {
#endif
    error = storage_ft_read(buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
const Item* ha_mroonga::wrapper_cond_push(const Item* cond
#  ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                          ,
                                          bool other_tables_ok
#  endif
)
{
  const Item* reminder_cond;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  reminder_cond = wrap_handler->cond_push(cond
#  ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                          ,
                                          other_tables_ok
#  endif
  );
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(reminder_cond);
}
#endif

const Item* ha_mroonga::storage_cond_push(const Item* cond
#ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                          ,
                                          bool other_tables_ok
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  const Item* reminder_cond = cond;
  ulong type_raw = THDVAR(ha_thd(), condition_push_down_type);
  mrn::condition_push_down::type type =
    static_cast<mrn::condition_push_down::type>(type_raw);
  switch (type) {
  case mrn::condition_push_down::NONE:
    break;
  case mrn::condition_push_down::ALL: {
    mrn::ConditionConverter converter(ha_thd(),
                                      ctx,
                                      grn_table,
                                      grn_index_columns,
                                      table->key_info,
                                      true);
    if (converter.is_convertable(cond)) {
      ++mrn_condition_push_down;
      reminder_cond = NULL;
    }
  } break;
  case mrn::condition_push_down::ONE_FULL_TEXT_SEARCH: {
    mrn::ConditionConverter converter(ha_thd(),
                                      ctx,
                                      grn_table,
                                      grn_index_columns,
                                      table->key_info,
                                      true);
    if (converter.count_match_against(cond) == 1 &&
        converter.is_convertable(cond)) {
      ++mrn_condition_push_down;
      reminder_cond = NULL;
    }
  } break;
  }
  DBUG_RETURN(reminder_cond);
}

const Item* ha_mroonga::cond_push(const Item* cond
#ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                  ,
                                  bool other_tables_ok
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  const Item* reminder_cond;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    reminder_cond = wrapper_cond_push(cond
#  ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                      ,
                                      other_tables_ok
#  endif
    );
  } else {
#endif
    reminder_cond = storage_cond_push(cond
#ifdef MRN_HANDLER_COND_PUSH_HAVE_OTHER_TABLES_OK
                                      ,
                                      other_tables_ok
#endif
    );
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(reminder_cond);
}

#ifdef MRN_HANDLER_HAVE_COND_POP
#  ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_cond_pop()
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->cond_pop();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#  endif

void ha_mroonga::storage_cond_pop()
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_VOID_RETURN;
}

void ha_mroonga::cond_pop()
{
  MRN_DBUG_ENTER_METHOD();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_cond_pop();
  } else {
#  endif
    storage_cond_pop();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_VOID_RETURN;
}
#endif

bool ha_mroonga::have_unique_index()
{
  MRN_DBUG_ENTER_METHOD();

  uint n_keys = table->s->keys;

  for (uint i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &(table->key_info[i]);
    if (key_info->flags & HA_NOSAME) {
      DBUG_RETURN(true);
    }
  }

  DBUG_RETURN(false);
}

bool ha_mroonga::is_foreign_key_field(const char* table_name,
                                      const char* field_name,
                                      size_t field_name_length)
{
  MRN_DBUG_ENTER_METHOD();

  grn_obj* table = grn_ctx_get(ctx, table_name, -1);
  if (!table) {
    DBUG_RETURN(false);
  }

  mrn::ColumnName column_name(field_name, field_name_length);
  grn_obj* column =
    grn_obj_column(ctx, table, column_name.c_str(), column_name.length());
  if (!column) {
    DBUG_RETURN(false);
  }

  grn_obj* range = grn_ctx_at(ctx, grn_obj_get_range(ctx, column));
  if (!range) {
    grn_obj_unref(ctx, column);
    DBUG_RETURN(false);
  }

  if (!mrn::grn::is_table(range)) {
    grn_obj_unref(ctx, column);
    DBUG_RETURN(false);
  }

  grn_obj* foreign_index_column;
  mrn::IndexColumnName index_column_name(table_name,
                                         field_name,
                                         field_name_length);
  foreign_index_column = grn_obj_column(ctx,
                                        range,
                                        index_column_name.c_str(),
                                        index_column_name.length());
  if (foreign_index_column) {
    grn_obj_unref(ctx, foreign_index_column);
    grn_obj_unref(ctx, column);
    DBUG_RETURN(true);
  }

  grn_obj_unref(ctx, column);

  DBUG_RETURN(false);
}

bool ha_mroonga::is_foreign_key_field(const char* table_name,
                                      const char* field_name)
{
  MRN_DBUG_ENTER_METHOD();

  bool is_foreign_key_field_ =
    is_foreign_key_field(table_name, field_name, strlen(field_name));
  DBUG_RETURN(is_foreign_key_field_);
}

bool ha_mroonga::is_foreign_key_field(const char* table_name,
                                      LEX_CSTRING& field_name)
{
  MRN_DBUG_ENTER_METHOD();

  bool is_foreign_key_field_ =
    is_foreign_key_field(table_name, field_name.str, field_name.length);
  DBUG_RETURN(is_foreign_key_field_);
}

void ha_mroonga::push_warning_unsupported_spatial_index_search(
  enum ha_rkey_function flag)
{
  char search_name[MRN_BUFFER_SIZE];
  if (flag == HA_READ_MBR_INTERSECT) {
    strcpy(search_name, "intersect");
  } else if (flag == HA_READ_MBR_WITHIN) {
    strcpy(search_name, "within");
  } else if (flag & HA_READ_MBR_DISJOINT) {
    strcpy(search_name, "disjoint");
  } else if (flag & HA_READ_MBR_EQUAL) {
    strcpy(search_name, "equal");
  } else {
    sprintf(search_name, "unknown: %d", flag);
  }
  push_warning_printf(ha_thd(),
                      MRN_SEVERITY_WARNING,
                      ER_UNSUPPORTED_EXTENSION,
                      "spatial index search "
                      "except MBRContains aren't supported: <%s>",
                      search_name);
}

void ha_mroonga::clear_cursor()
{
  MRN_DBUG_ENTER_METHOD();
  if (cursor) {
    grn_obj_unlink(ctx, cursor);
    cursor = NULL;
  }
  if (index_table_cursor) {
    grn_table_cursor_close(ctx, index_table_cursor);
    index_table_cursor = NULL;
  }
  if (condition_push_down_result_cursor) {
    grn_table_cursor_close(ctx, condition_push_down_result_cursor);
    condition_push_down_result_cursor = NULL;
  }
  if (sorted_condition_push_down_result_) {
    grn_obj_unlink(ctx, sorted_condition_push_down_result_);
    sorted_condition_push_down_result_ = NULL;
  }
  if (condition_push_down_result) {
    grn_obj_unlink(ctx, condition_push_down_result);
    condition_push_down_result = NULL;
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::clear_cursor_geo()
{
  MRN_DBUG_ENTER_METHOD();
  if (cursor_geo) {
    grn_obj_unlink(ctx, cursor_geo);
    cursor_geo = NULL;
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::clear_empty_value_records()
{
  MRN_DBUG_ENTER_METHOD();
  if (empty_value_records_cursor) {
    grn_table_cursor_close(ctx, empty_value_records_cursor);
    empty_value_records_cursor = NULL;
  }
  if (empty_value_records) {
    grn_obj_unlink(ctx, empty_value_records);
    empty_value_records = NULL;
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::clear_search_result()
{
  MRN_DBUG_ENTER_METHOD();
  clear_cursor();
  DBUG_VOID_RETURN;
}

void ha_mroonga::clear_search_result_geo()
{
  MRN_DBUG_ENTER_METHOD();
  clear_cursor_geo();
  if (grn_source_column_geo) {
    grn_obj_unlink(ctx, grn_source_column_geo);
    grn_source_column_geo = NULL;
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::clear_indexes()
{
  MRN_DBUG_ENTER_METHOD();
  uint n_keys = table->s->keys;
  uint pkey_nr = table->s->primary_key;

  for (uint i = 0; i < n_keys; i++) {
    if (i != pkey_nr) {
      if (grn_index_tables) {
        grn_obj_unlink(ctx, grn_index_tables[i]);
      }
      if (grn_index_columns) {
        grn_obj_unlink(ctx, grn_index_columns[i]);
      }
    }
  }

  if (grn_index_tables) {
    free(grn_index_tables);
    grn_index_tables = NULL;
  }

  if (grn_index_columns) {
    free(grn_index_columns);
    grn_index_columns = NULL;
  }

  if (key_id) {
    free(key_id);
    key_id = NULL;
  }

  if (del_key_id) {
    free(del_key_id);
    del_key_id = NULL;
  }

  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::add_wrap_hton(const char* path, handlerton* wrap_handlerton)
{
  MRN_DBUG_ENTER_METHOD();
  mrn::SlotData* slot_data = mrn_get_slot_data(ha_thd(), true);
  if (!slot_data)
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  st_mrn_wrap_hton* wrap_hton =
    (st_mrn_wrap_hton*)malloc(sizeof(st_mrn_wrap_hton));
  if (!wrap_hton)
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  wrap_hton->next = NULL;
  strcpy(wrap_hton->path, path);
  wrap_hton->hton = wrap_handlerton;
  if (slot_data->first_wrap_hton) {
    st_mrn_wrap_hton* tmp_wrap_hton = slot_data->first_wrap_hton;
    while (tmp_wrap_hton->next)
      tmp_wrap_hton = tmp_wrap_hton->next;
    tmp_wrap_hton->next = wrap_hton;
  } else {
    slot_data->first_wrap_hton = wrap_hton;
  }
  DBUG_RETURN(0);
}
#endif

void ha_mroonga::remove_related_files(const char* base_path)
{
  MRN_DBUG_ENTER_METHOD();

  const char* base_directory_name = ".";
  size_t base_path_length = strlen(base_path);
#ifdef WIN32
  WIN32_FIND_DATA data;
  HANDLE finder = FindFirstFile(base_directory_name, &data);
  if (finder != INVALID_HANDLE_VALUE) {
    do {
      if (!(data.dwFileAttributes & FILE_ATTRIBUTE_NORMAL)) {
        continue;
      }
      if (strncmp(data.cFileName, base_path, base_path_length) == 0) {
        DeleteFile(data.cFileName);
      }
    } while (FindNextFile(finder, &data) != 0);
    FindClose(finder);
  }
#else
  DIR* dir = opendir(base_directory_name);
  if (dir) {
    while (struct dirent* entry = readdir(dir)) {
      struct stat file_status;
      if (stat(entry->d_name, &file_status) != 0) {
        continue;
      }
      if (!((file_status.st_mode & S_IFMT) == S_IFREG)) {
        continue;
      }
      if (strncmp(entry->d_name, base_path, base_path_length) == 0) {
        unlink(entry->d_name);
      }
    }
    closedir(dir);
  }
#endif

  DBUG_VOID_RETURN;
}

void ha_mroonga::remove_grn_obj_force(const char* name)
{
  MRN_DBUG_ENTER_METHOD();

  grn_obj* obj = grn_ctx_get(ctx, name, strlen(name));
  if (obj) {
    grn_obj_remove(ctx, obj);
  } else {
    grn_obj* db = grn_ctx_db(ctx);
    grn_id id = grn_table_get(ctx, db, name, strlen(name));
    if (id) {
      grn_obj_remove_force(ctx, name, strlen(name));
    }
  }

  DBUG_VOID_RETURN;
}

int ha_mroonga::drop_index(MRN_SHARE* target_share, uint key_index)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  grn_rc rc = GRN_SUCCESS;
  char target_name[GRN_TABLE_MAX_KEY_SIZE];
  int target_name_length;

  KEY* key_info = &(target_share->table_share->key_info[key_index]);
  const char* lexicon_name = NULL;
  mrn::ParametersParser parser(key_info->comment.str, key_info->comment.length);
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!target_share->wrapper_mode) {
#endif
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
    if (key_info->option_struct) {
      lexicon_name = key_info->option_struct->lexicon;
    }
#endif
    if (!lexicon_name) {
      lexicon_name = parser.lexicon();
    }
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  if (lexicon_name) {
    snprintf(target_name,
             GRN_TABLE_MAX_KEY_SIZE,
             "%s." KEY_NAME_FORMAT,
             lexicon_name,
             KEY_NAME_FORMAT_VALUE(key_info));
    target_name_length = strlen(target_name);
    grn_obj* index_column = grn_ctx_get(ctx, target_name, target_name_length);
    if (index_column) {
      rc = grn_obj_remove(ctx, index_column);
    }
  } else {
    mrn::PathMapper mapper(target_share->table_name);
    mrn::IndexTableName index_table_name(mapper.table_name(),
                                         KEY_NAME(key_info));
    grn_obj* index_table =
      grn_ctx_get(ctx, index_table_name.c_str(), index_table_name.length());
    if (!index_table) {
      index_table = grn_ctx_get(ctx,
                                index_table_name.old_c_str(),
                                index_table_name.old_length());
    }
    if (index_table) {
      target_name_length =
        grn_obj_name(ctx, index_table, target_name, GRN_TABLE_MAX_KEY_SIZE);
      rc = grn_obj_remove(ctx, index_table);
    } else {
      target_name_length = 0;
    }
  }

  if (rc != GRN_SUCCESS) {
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "failed to drop index: <%.*s>: <%s>",
             target_name_length,
             target_name,
             ctx->errbuf);
    my_message(ER_ERROR_ON_WRITE, error_message, MYF(0));
    GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
  }

  DBUG_RETURN(error);
}

int ha_mroonga::drop_indexes_normal(const char* table_name, grn_obj* table)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  grn_hash* columns_raw =
    grn_hash_create(ctx,
                    NULL,
                    sizeof(grn_id),
                    0,
                    GRN_OBJ_TABLE_HASH_KEY | GRN_HASH_TINY);
  mrn::SmartGrnObj columns(ctx, reinterpret_cast<grn_obj*>(columns_raw));
  if (!columns.get()) {
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "failed to allocate columns buffer: <%s>: <%s>",
             table_name,
             ctx->errbuf);
    error = HA_ERR_OUT_OF_MEM;
    my_message(ER_ERROR_ON_WRITE, error_message, MYF(0));
    GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
    DBUG_RETURN(error);
  }

  grn_table_columns(ctx, table, "", 0, columns.get());
  grn_table_cursor* cursor =
    grn_table_cursor_open(ctx, columns.get(), NULL, 0, NULL, 0, 0, -1, 0);
  if (!cursor) {
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "failed to allocate columns cursor: <%s>: <%s>",
             table_name,
             ctx->errbuf);
    error = HA_ERR_OUT_OF_MEM;
    my_message(ER_ERROR_ON_WRITE, error_message, MYF(0));
    GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
    DBUG_RETURN(error);
  }

  while (grn_table_cursor_next(ctx, cursor) != GRN_ID_NIL) {
    void* key;
    grn_table_cursor_get_key(ctx, cursor, &key);
    grn_id* id = reinterpret_cast<grn_id*>(key);
    mrn::SmartGrnObj column(ctx, grn_ctx_at(ctx, *id));
    if (!column.get()) {
      continue;
    }

    grn_operator index_operators[] = {GRN_OP_EQUAL,
                                      GRN_OP_MATCH,
                                      GRN_OP_LESS,
                                      GRN_OP_REGEXP};
    size_t n_index_operators = sizeof(index_operators) / sizeof(grn_operator);
    for (size_t i = 0; i < n_index_operators; i++) {
      grn_index_datum index_datum;
      while (grn_column_find_index_data(ctx,
                                        column.get(),
                                        index_operators[i],
                                        &index_datum,
                                        1) > 0) {
        grn_id index_table_id = index_datum.index->header.domain;
        mrn::SmartGrnObj index_table(ctx, grn_ctx_at(ctx, index_table_id));
        char index_table_name[GRN_TABLE_MAX_KEY_SIZE];
        int index_table_name_length;
        index_table_name_length = grn_obj_name(ctx,
                                               index_table.get(),
                                               index_table_name,
                                               GRN_TABLE_MAX_KEY_SIZE);
        if (mrn::IndexTableName::is_custom_name(table_name,
                                                strlen(table_name),
                                                index_table_name,
                                                index_table_name_length)) {
          char index_column_name[GRN_TABLE_MAX_KEY_SIZE];
          int index_column_name_length;
          index_column_name_length = grn_obj_name(ctx,
                                                  index_datum.index,
                                                  index_column_name,
                                                  GRN_TABLE_MAX_KEY_SIZE);
          grn_rc rc = grn_obj_remove(ctx, index_datum.index);
          if (rc != GRN_SUCCESS) {
            char error_message[MRN_MESSAGE_BUFFER_SIZE];
            snprintf(error_message,
                     MRN_MESSAGE_BUFFER_SIZE,
                     "failed to drop index column: <%.*s>: <%s>",
                     index_column_name_length,
                     index_column_name,
                     ctx->errbuf);
            error = ER_ERROR_ON_WRITE;
            my_message(error, error_message, MYF(0));
            GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
          }
        } else {
          grn_rc rc = grn_obj_remove(ctx, index_table.get());
          if (rc == GRN_SUCCESS) {
            index_table.release();
          } else {
            char error_message[MRN_MESSAGE_BUFFER_SIZE];
            snprintf(error_message,
                     MRN_MESSAGE_BUFFER_SIZE,
                     "failed to drop index table: <%.*s>: <%s>",
                     index_table_name_length,
                     index_table_name,
                     ctx->errbuf);
            error = ER_ERROR_ON_WRITE;
            my_message(error, error_message, MYF(0));
            GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
          }
        }

        if (error != 0) {
          break;
        }
      }

      if (error != 0) {
        break;
      }
    }

    if (error != 0) {
      break;
    }
  }

  grn_table_cursor_close(ctx, cursor);

  DBUG_RETURN(error);
}

int ha_mroonga::drop_indexes_multiple(const char* table_name,
                                      grn_obj* table,
                                      const char* index_table_name_separator)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  char index_table_name_prefix[GRN_TABLE_MAX_KEY_SIZE];
  snprintf(index_table_name_prefix,
           GRN_TABLE_MAX_KEY_SIZE,
           "%s%s",
           table_name,
           index_table_name_separator);
  grn_table_cursor* cursor =
    grn_table_cursor_open(ctx,
                          grn_ctx_db(ctx),
                          index_table_name_prefix,
                          strlen(index_table_name_prefix),
                          NULL,
                          0,
                          0,
                          -1,
                          GRN_CURSOR_PREFIX);
  if (!cursor) {
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "failed to allocate index tables cursor: <%s>: <%s>",
             table_name,
             ctx->errbuf);
    error = HA_ERR_OUT_OF_MEM;
    my_message(ER_ERROR_ON_WRITE, error_message, MYF(0));
    GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
    DBUG_RETURN(error);
  }

  grn_id table_id = grn_obj_id(ctx, table);
  grn_id id;
  while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
    mrn::SmartGrnObj object(ctx, grn_ctx_at(ctx, id));
    if (!object.get()) {
      continue;
    }
    if (!grn_obj_is_table(ctx, object.get())) {
      continue;
    }

    char multiple_column_index_table_name[GRN_TABLE_MAX_KEY_SIZE];
    int multiple_column_index_table_name_length;
    multiple_column_index_table_name_length =
      grn_obj_name(ctx,
                   object.get(),
                   multiple_column_index_table_name,
                   GRN_TABLE_MAX_KEY_SIZE);

    char multiple_column_index_name[GRN_TABLE_MAX_KEY_SIZE];
    snprintf(multiple_column_index_name,
             GRN_TABLE_MAX_KEY_SIZE,
             "%.*s.%s",
             multiple_column_index_table_name_length,
             multiple_column_index_table_name,
             INDEX_COLUMN_NAME);
    mrn::SmartGrnObj index_column(ctx, multiple_column_index_name);
    if (!index_column.get()) {
      continue;
    }

    if (grn_obj_get_range(ctx, index_column.get()) != table_id) {
      continue;
    }

    grn_rc rc = grn_obj_remove(ctx, object.get());
    if (rc == GRN_SUCCESS) {
      object.release();
      index_column.release();
    } else {
      char error_message[MRN_MESSAGE_BUFFER_SIZE];
      snprintf(error_message,
               MRN_MESSAGE_BUFFER_SIZE,
               "failed to drop multiple column index table: <%.*s>: <%s>",
               multiple_column_index_table_name_length,
               multiple_column_index_table_name,
               ctx->errbuf);
      error = ER_ERROR_ON_WRITE;
      my_message(error, error_message, MYF(0));
      GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
      break;
    }
  }

  grn_table_cursor_close(ctx, cursor);

  DBUG_RETURN(error);
}

int ha_mroonga::drop_indexes(const char* table_name)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  mrn::SmartGrnObj table(ctx, table_name);
  if (!table.get()) {
    DBUG_RETURN(0);
  }

  error = drop_indexes_normal(table_name, table.get());
  if (error == 0) {
    error = drop_indexes_multiple(table_name,
                                  table.get(),
                                  mrn::IndexTableName::SEPARATOR);
  }
  if (error == 0) {
    error = drop_indexes_multiple(table_name,
                                  table.get(),
                                  mrn::IndexTableName::OLD_SEPARATOR);
  }

  DBUG_RETURN(error);
}

bool ha_mroonga::find_table_flags(HA_CREATE_INFO* info,
                                  MRN_SHARE* mrn_share,
                                  grn_table_flags* flags)
{
  MRN_DBUG_ENTER_METHOD();
  bool found = false;

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (info->option_struct) {
    const char* flag_names = info->option_struct->flags;
    if (flag_names) {
      found = mrn_parse_grn_table_create_flags(ha_thd(),
                                               ctx,
                                               flag_names,
                                               strlen(flag_names),
                                               flags);
      DBUG_RETURN(found);
    }
  }
#endif

  if (mrn_share->table_flags) {
    found = mrn_parse_grn_table_create_flags(ha_thd(),
                                             ctx,
                                             mrn_share->table_flags,
                                             mrn_share->table_flags_length,
                                             flags);
    DBUG_RETURN(found);
  }

  DBUG_RETURN(found);
}

bool ha_mroonga::find_column_flags(Field* field,
                                   MRN_SHARE* mrn_share,
                                   int i,
                                   grn_column_flags* column_flags)
{
  MRN_DBUG_ENTER_METHOD();
  bool found = false;

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  {
    const char* names = field->option_struct->flags;
    if (names) {
      found = mrn_parse_grn_column_create_flags(ha_thd(),
                                                ctx,
                                                names,
                                                strlen(names),
                                                column_flags);
      DBUG_RETURN(found);
    }
  }
#endif

  if (mrn_share->col_flags[i]) {
    found = mrn_parse_grn_column_create_flags(ha_thd(),
                                              ctx,
                                              mrn_share->col_flags[i],
                                              mrn_share->col_flags_length[i],
                                              column_flags);
    DBUG_RETURN(found);
  }

  DBUG_RETURN(found);
}

grn_obj* ha_mroonga::find_column_type(Field* field,
                                      MRN_SHARE* mrn_share,
                                      int i,
                                      int error_code)
{
  MRN_DBUG_ENTER_METHOD();

  const char* grn_type_name = NULL;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  grn_type_name = field->option_struct->groonga_type;
#endif
  if (!grn_type_name) {
    grn_type_name = mrn_share->col_type[i];
  }

  grn_obj* type = NULL;
  if (grn_type_name) {
    type = grn_ctx_get(ctx, grn_type_name, -1);
    if (!type) {
      char error_message[MRN_BUFFER_SIZE];
      snprintf(error_message,
               MRN_BUFFER_SIZE,
               "unknown custom Groonga type name for "
               "<" FIELD_NAME_FORMAT "> column: <%s>",
               FIELD_NAME_FORMAT_VALUE(field),
               grn_type_name);
      GRN_LOG(ctx, GRN_LOG_ERROR, "%s", error_message);
      my_message(error_code, error_message, MYF(0));

      DBUG_RETURN(NULL);
    }
  } else {
    grn_builtin_type grn_type_id = mrn_grn_type_from_field(ctx, field, false);
    type = grn_ctx_at(ctx, grn_type_id);
  }

  DBUG_RETURN(type);
}

void ha_mroonga::set_tokenizer(grn_obj* lexicon, KEY* key)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (key->option_struct->tokenizer) {
    set_tokenizer(lexicon,
                  key->option_struct->tokenizer,
                  strlen(key->option_struct->tokenizer));
    DBUG_VOID_RETURN;
  }
#endif
  if (key->comment.length > 0) {
    mrn::ParametersParser parser(key->comment.str, key->comment.length);
    const char* tokenizer = parser.tokenizer();
    if (tokenizer) {
      set_tokenizer(lexicon, tokenizer, strlen(tokenizer));
      DBUG_VOID_RETURN;
    }
  }

  set_tokenizer(lexicon, mrn_default_tokenizer, strlen(mrn_default_tokenizer));
  DBUG_VOID_RETURN;
}

void ha_mroonga::set_tokenizer(grn_obj* lexicon,
                               const char* tokenizer,
                               size_t tokenizer_length)
{
  MRN_DBUG_ENTER_METHOD();

  /* Deprecated */
  if (tokenizer && tokenizer_length == strlen("off") &&
      (strncasecmp("off", tokenizer, tokenizer_length) == 0)) {
    DBUG_VOID_RETURN;
  }
  if (tokenizer && tokenizer_length == strlen("none") &&
      (strncasecmp("none", tokenizer, tokenizer_length) == 0)) {
    DBUG_VOID_RETURN;
  }

  mrn_change_encoding(ctx, system_charset_info);

  grn_obj tokenizer_spec;
  GRN_TEXT_INIT(&tokenizer_spec, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &tokenizer_spec, tokenizer, tokenizer_length);

  grn_info_type info_type = GRN_INFO_DEFAULT_TOKENIZER;
  grn_rc rc = grn_obj_set_info(ctx, lexicon, info_type, &tokenizer_spec);
  if (rc == GRN_SUCCESS) {
    GRN_OBJ_FIN(ctx, &tokenizer_spec);
    DBUG_VOID_RETURN;
  }

  char message[MRN_BUFFER_SIZE];
  sprintf(message,
          "specified tokenizer for fulltext index <%.*s> is invalid. "
          "The default tokenizer for fulltext index <%s> is used instead.",
          (int)tokenizer_length,
          tokenizer,
          MRN_DEFAULT_TOKENIZER);
  push_warning(ha_thd(),
               MRN_SEVERITY_WARNING,
               ER_UNSUPPORTED_EXTENSION,
               message);

  GRN_TEXT_SETS(ctx, &tokenizer_spec, MRN_DEFAULT_TOKENIZER);
  rc = grn_obj_set_info(ctx, lexicon, info_type, &tokenizer_spec);
  GRN_OBJ_FIN(ctx, &tokenizer_spec);
  if (rc == GRN_SUCCESS) {
    DBUG_VOID_RETURN;
  }

  sprintf(message,
          "the default tokenizer for fulltext index <%s> is invalid. "
          "Bigram tokenizer is used instead.",
          MRN_DEFAULT_TOKENIZER);
  push_warning(ha_thd(),
               MRN_SEVERITY_WARNING,
               ER_UNSUPPORTED_EXTENSION,
               message);
  rc =
    grn_obj_set_info(ctx, lexicon, info_type, grn_ctx_at(ctx, GRN_DB_BIGRAM));

  DBUG_VOID_RETURN;
}

bool ha_mroonga::have_custom_normalizer(KEY* key) const
{
  MRN_DBUG_ENTER_METHOD();

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (key->option_struct && key->option_struct->normalizer) {
    DBUG_RETURN(true);
  }
#endif

  if (key->comment.length > 0) {
    mrn::ParametersParser parser(key->comment.str, key->comment.length);
    DBUG_RETURN(parser["normalizer"] != NULL);
  }

  DBUG_RETURN(false);
}

void ha_mroonga::set_normalizer(grn_obj* lexicon, KEY* key)
{
  MRN_DBUG_ENTER_METHOD();

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (key->option_struct->normalizer) {
    set_normalizer(lexicon,
                   key,
                   key->option_struct->normalizer,
                   strlen(key->option_struct->normalizer));
    DBUG_VOID_RETURN;
  }
#endif

  if (key->comment.length > 0) {
    mrn::ParametersParser parser(key->comment.str, key->comment.length);
    const char* normalizer = parser["normalizer"];
    if (normalizer) {
      set_normalizer(lexicon, key, normalizer, strlen(normalizer));
      DBUG_VOID_RETURN;
    }
  }

  set_normalizer(lexicon, key, NULL, 0);
  DBUG_VOID_RETURN;
}

void ha_mroonga::set_normalizer(grn_obj* lexicon,
                                KEY* key,
                                const char* normalizer,
                                size_t normalizer_length)
{
  MRN_DBUG_ENTER_METHOD();

  bool use_normalizer = true;
  if (normalizer && normalizer_length == strlen("none") &&
      (strncmp(normalizer, "none", normalizer_length) == 0)) {
    use_normalizer = false;
  }
  if (use_normalizer) {
    grn_obj normalizer_spec;
    GRN_TEXT_INIT(&normalizer_spec, 0);
    if (normalizer) {
      GRN_TEXT_SET(ctx, &normalizer_spec, normalizer, normalizer_length);
    } else {
      Field* field = key->key_part[0].field;
      mrn::FieldNormalizer field_normalizer(ctx, ha_thd(), field);
      field_normalizer.find_grn_normalizer(&normalizer_spec);
    }
    grn_obj_set_info(ctx, lexicon, GRN_INFO_NORMALIZER, &normalizer_spec);
    GRN_OBJ_FIN(ctx, &normalizer_spec);
  }

  DBUG_VOID_RETURN;
}

bool ha_mroonga::find_index_column_flags(KEY* key,
                                         grn_column_flags* index_column_flags)
{
  MRN_DBUG_ENTER_METHOD();
  bool found = false;

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  {
    const char* flags = key->option_struct->flags;
    if (flags) {
      found = mrn_parse_grn_index_column_flags(ha_thd(),
                                               ctx,
                                               flags,
                                               strlen(flags),
                                               index_column_flags);
      DBUG_RETURN(found);
    }
  }
#endif

  if (key->comment.length > 0) {
    mrn::ParametersParser parser(key->comment.str, key->comment.length);
    const char* flags = parser["flags"];
    if (!flags) {
      // Deprecated. It's for backward compatibility.
      flags = parser["index_flags"];
    }
    if (flags) {
      found = mrn_parse_grn_index_column_flags(ha_thd(),
                                               ctx,
                                               flags,
                                               strlen(flags),
                                               index_column_flags);
    }
  }

  DBUG_RETURN(found);
}

void ha_mroonga::set_token_filters(grn_obj* lexicon, KEY* key)
{
  MRN_DBUG_ENTER_METHOD();

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (key->option_struct->token_filters) {
    set_token_filters(lexicon,
                      key->option_struct->token_filters,
                      strlen(key->option_struct->token_filters));
    DBUG_VOID_RETURN;
  }
#endif

  if (key->comment.length > 0) {
    mrn::ParametersParser parser(key->comment.str, key->comment.length);
    const char* token_filters = parser["token_filters"];
    if (token_filters) {
      set_token_filters(lexicon, token_filters, strlen(token_filters));
      DBUG_VOID_RETURN;
    }
  }

  DBUG_VOID_RETURN;
}

void ha_mroonga::set_token_filters(grn_obj* lexicon,
                                   const char* token_filters,
                                   size_t token_filters_length)
{
  MRN_DBUG_ENTER_METHOD();

  mrn_change_encoding(ctx, system_charset_info);

  grn_obj token_filters_spec;
  GRN_TEXT_INIT(&token_filters_spec, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &token_filters_spec, token_filters, token_filters_length);
  grn_obj_set_info(ctx, lexicon, GRN_INFO_TOKEN_FILTERS, &token_filters_spec);
  GRN_OBJ_FIN(ctx, &token_filters_spec);

  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_get_record(uchar* buf, const uchar* key)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (wrap_handler->inited == NONE) {
    error = wrap_handler->ha_index_read_idx_map(buf,
                                                share->wrap_primary_key,
                                                key,
                                                pk_keypart_map,
                                                HA_READ_KEY_EXACT);
  } else {
    error = wrap_handler->ha_index_read_map(buf,
                                            key,
                                            pk_keypart_map,
                                            HA_READ_KEY_EXACT);
  }
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_get_next_geo_record(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  mrn_change_encoding(ctx, NULL);
  do {
    GRN_BULK_REWIND(&key_buffer);
    grn_id found_record_id;
    grn_posting* posting;
    posting = grn_geo_cursor_next(ctx, cursor_geo);
    if (!posting) {
      error = HA_ERR_END_OF_FILE;
      clear_cursor_geo();
      break;
    }
    found_record_id = posting->rid;
    grn_table_get_key(ctx,
                      grn_table,
                      found_record_id,
                      GRN_TEXT_VALUE(&key_buffer),
                      table->key_info->key_length);
    error = wrapper_get_record(buf, (const uchar*)GRN_TEXT_VALUE(&key_buffer));
  } while (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_get_next_record(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();
  if (condition_push_down_result_cursor) {
    grn_id next_record_id =
      grn_table_cursor_next(ctx, condition_push_down_result_cursor);
    if (next_record_id == GRN_ID_NIL) {
      record_id = GRN_ID_NIL;
    } else {
      if (sorted_condition_push_down_result_) {
        void* value;
        grn_table_cursor_get_value(ctx,
                                   condition_push_down_result_cursor,
                                   &value);
        grn_id condition_push_down_result_record_id =
          *static_cast<grn_id*>(value);
        grn_table_get_key(ctx,
                          condition_push_down_result,
                          condition_push_down_result_record_id,
                          &record_id,
                          sizeof(grn_id));
      } else {
        void* key;
        grn_table_cursor_get_key(ctx, condition_push_down_result_cursor, &key);
        record_id = *static_cast<grn_id*>(key);
      }
    }
  } else if (cursor_geo) {
    grn_posting* posting;
    posting = grn_geo_cursor_next(ctx, cursor_geo);
    if (posting) {
      record_id = posting->rid;
    } else {
      record_id = GRN_ID_NIL;
    }
  } else if (cursor) {
    record_id = grn_table_cursor_next(ctx, cursor);
  } else if (empty_value_records_cursor) {
    grn_id empty_value_record_id;
    empty_value_record_id =
      grn_table_cursor_next(ctx, empty_value_records_cursor);
    if (empty_value_record_id == GRN_ID_NIL) {
      record_id = GRN_ID_NIL;
    } else {
      grn_table_get_key(ctx,
                        empty_value_records,
                        empty_value_record_id,
                        &record_id,
                        sizeof(grn_id));
    }
  } else {
    record_id = GRN_ID_NIL;
  }
  if (ctx->rc) {
    int error = ER_ERROR_ON_READ;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  if (record_id == GRN_ID_NIL) {
    DBUG_PRINT("info", ("mroonga: storage_get_next_record: end-of-file"));
    MRN_TABLE_SET_NO_ROW(table);
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  if (buf) {
    if (ignoring_no_key_columns) {
      storage_store_fields_by_index(buf);
    } else {
      storage_store_fields(table, buf, record_id);
    }
    if (cursor_geo && grn_source_column_geo) {
      int latitude, longitude;
      GRN_GEO_POINT_VALUE(&source_point, latitude, longitude);
      double latitude_in_degree = GRN_GEO_MSEC2DEGREE(latitude);
      double longitude_in_degree = GRN_GEO_MSEC2DEGREE(longitude);
      if (!((bottom_right_latitude_in_degree <= latitude_in_degree &&
             latitude_in_degree <= top_left_latitude_in_degree) &&
            (top_left_longitude_in_degree <= longitude_in_degree &&
             longitude_in_degree <= bottom_right_longitude_in_degree))) {
        DBUG_PRINT("info",
                   ("mroonga: remove not contained geo point: "
                    "<%g,%g>(<%d,%d>); key: <%g,%g>(<%d,%d>), <%g,%g>(<%d,%d>)",
                    latitude_in_degree,
                    longitude_in_degree,
                    latitude,
                    longitude,
                    top_left_latitude_in_degree,
                    top_left_longitude_in_degree,
                    GRN_GEO_DEGREE2MSEC(top_left_latitude_in_degree),
                    GRN_GEO_DEGREE2MSEC(top_left_longitude_in_degree),
                    bottom_right_latitude_in_degree,
                    bottom_right_longitude_in_degree,
                    GRN_GEO_DEGREE2MSEC(bottom_right_latitude_in_degree),
                    GRN_GEO_DEGREE2MSEC(bottom_right_longitude_in_degree)));
        int error = storage_get_next_record(buf);
        DBUG_RETURN(error);
      }
    }
  }
  MRN_TABLE_SET_FOUND_ROW(table);
  DBUG_RETURN(0);
}

void ha_mroonga::geo_store_rectangle(const uchar* rectangle, bool reverse)
{
  MRN_DBUG_ENTER_METHOD();

  double locations[4];
  for (int i = 0; i < 4; i++) {
    uchar reversed_value[8];
    for (int j = 0; j < 8; j++) {
      reversed_value[j] = (rectangle + (8 * i))[7 - j];
    }
    MRN_MI_FLOAT8GET(locations[i], reversed_value);
  }
  if (reverse) {
    top_left_longitude_in_degree = locations[0];
    bottom_right_longitude_in_degree = locations[1];
    bottom_right_latitude_in_degree = locations[2];
    top_left_latitude_in_degree = locations[3];
  } else {
    bottom_right_latitude_in_degree = locations[0];
    top_left_latitude_in_degree = locations[1];
    top_left_longitude_in_degree = locations[2];
    bottom_right_longitude_in_degree = locations[3];
  }
  int top_left_latitude = GRN_GEO_DEGREE2MSEC(top_left_latitude_in_degree);
  int top_left_longitude = GRN_GEO_DEGREE2MSEC(top_left_longitude_in_degree);
  int bottom_right_latitude =
    GRN_GEO_DEGREE2MSEC(bottom_right_latitude_in_degree);
  int bottom_right_longitude =
    GRN_GEO_DEGREE2MSEC(bottom_right_longitude_in_degree);
  GRN_GEO_POINT_SET(ctx,
                    &top_left_point,
                    top_left_latitude,
                    top_left_longitude);
  GRN_GEO_POINT_SET(ctx,
                    &bottom_right_point,
                    bottom_right_latitude,
                    bottom_right_longitude);

  DBUG_VOID_RETURN;
}

int ha_mroonga::generic_geo_open_cursor(const uchar* key,
                                        enum ha_rkey_function find_flag)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  int flags = 0;
  if (find_flag & HA_READ_MBR_CONTAIN) {
    grn_obj* index = grn_index_columns[active_index];
    Field_geom* field =
      static_cast<Field_geom*>(table->key_info[active_index].key_part->field);
    geo_store_rectangle(key, geo_need_reverse(field));
    cursor_geo = grn_geo_cursor_open_in_rectangle(ctx,
                                                  index,
                                                  &top_left_point,
                                                  &bottom_right_point,
                                                  0,
                                                  -1);
    if (cursor_geo) {
      if (grn_source_column_geo) {
        grn_obj_unlink(ctx, grn_source_column_geo);
      }
      grn_obj sources;
      GRN_OBJ_INIT(&sources, GRN_BULK, 0, GRN_ID_NIL);
      grn_obj_get_info(ctx, index, GRN_INFO_SOURCE, &sources);
      grn_source_column_geo = grn_ctx_at(ctx, GRN_RECORD_VALUE(&sources));
      grn_obj_unlink(ctx, &sources);
    }
  } else {
    push_warning_unsupported_spatial_index_search(find_flag);
    cursor =
      grn_table_cursor_open(ctx, grn_table, NULL, 0, NULL, 0, 0, -1, flags);
  }
  if (ctx->rc) {
    error = ER_ERROR_ON_READ;
    my_message(error, ctx->errbuf, MYF(0));
  }
  DBUG_RETURN(error);
}

bool ha_mroonga::is_dry_write()
{
  MRN_DBUG_ENTER_METHOD();
  bool dry_write_p = THDVAR(ha_thd(), dry_write);
  DBUG_RETURN(dry_write_p);
}

bool ha_mroonga::is_enable_optimization()
{
  MRN_DBUG_ENTER_METHOD();
  bool enable_optimization_p = THDVAR(ha_thd(), enable_optimization);
  DBUG_RETURN(enable_optimization_p);
}

bool ha_mroonga::should_normalize(Field* field, bool is_fulltext_index) const
{
  MRN_DBUG_ENTER_METHOD();
  bool need_normalize_p;
  if (is_fulltext_index) {
    need_normalize_p = !(field->charset()->state & MY_CS_BINSORT);
  } else {
    mrn::FieldNormalizer field_normalizer(ctx, ha_thd(), field);
    need_normalize_p = field_normalizer.should_normalize();
  }
  DBUG_RETURN(need_normalize_p);
}

void ha_mroonga::check_count_skip(key_part_map target_key_part_map)
{
  MRN_DBUG_ENTER_METHOD();

  if (!is_enable_optimization()) {
    GRN_LOG(ctx,
            GRN_LOG_DEBUG,
            "[mroonga][count-skip][false] optimization is disabled");
    count_skip = false;
    DBUG_VOID_RETURN;
  }

  if (thd_sql_command(ha_thd()) != SQLCOM_SELECT) {
    GRN_LOG(ctx, GRN_LOG_DEBUG, "[mroonga][count-skip][false] not SELECT");
    count_skip = false;
    DBUG_VOID_RETURN;
  }

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode &&
      !(wrap_handler->ha_table_flags() & HA_NO_TRANSACTIONS)) {
    GRN_LOG(ctx,
            GRN_LOG_DEBUG,
            "[mroonga][count-skip][false] wrapped engine is transactional");
    count_skip = false;
    DBUG_VOID_RETURN;
  }
#endif

  mrn_query_block* query_block =
    MRN_TABLE_LIST_QUERY_BLOCK(table->pos_in_table_list);
  KEY* key_info = NULL;
  if (active_index != MAX_KEY) {
    key_info = &(table->key_info[active_index]);
  }
#ifdef MRN_ENABLE_WRAPPER_MODE
  const bool is_storage_mode = !share->wrapper_mode;
#else
  const bool is_storage_mode = true;
#endif
  mrn::CountSkipChecker checker(ctx,
                                table,
                                query_block,
                                key_info,
                                target_key_part_map,
                                is_storage_mode);
  if (checker.check()) {
    count_skip = true;
    mrn_count_skip++;
    DBUG_VOID_RETURN;
  } else {
    count_skip = false;
    DBUG_VOID_RETURN;
  }
}

bool ha_mroonga::is_grn_zero_column_value(grn_obj* column, grn_obj* value)
{
  MRN_DBUG_ENTER_METHOD();

  if (column->header.type != GRN_COLUMN_FIX_SIZE) {
    DBUG_RETURN(false);
  }

  char* bytes = GRN_BULK_HEAD(value);
  unsigned int size = GRN_BULK_VSIZE(value);
  for (unsigned int i = 0; i < size; ++i) {
    if (bytes[i] != '\0') {
      DBUG_RETURN(false);
    }
  }

  DBUG_RETURN(true);
}

bool ha_mroonga::is_primary_key_field(Field* field) const
{
  MRN_DBUG_ENTER_METHOD();

  if (table->s->primary_key == MAX_INDEXES) {
    DBUG_RETURN(false);
  }

  KEY* key_info = &(table->s->key_info[table->s->primary_key]);
  if (KEY_N_KEY_PARTS(key_info) != 1) {
    DBUG_RETURN(false);
  }

  if (FIELD_NAME_EQUAL_FIELD(field, key_info->key_part[0].field)) {
    DBUG_RETURN(true);
  } else {
    DBUG_RETURN(false);
  }
}

bool ha_mroonga::check_fast_order_limit(grn_obj* result_set,
                                        grn_table_sort_key** sort_keys,
                                        int* n_sort_keys,
                                        longlong* limit)
{
  MRN_DBUG_ENTER_METHOD();

  const char* tag = "[mroonga][fast-order-limit][check]";

  if (!is_enable_optimization()) {
    GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[false] optimization is disabled", tag);
    fast_order_limit = false;
    DBUG_RETURN(fast_order_limit);
  }

  TABLE_LIST* table_list = table->pos_in_table_list;
  mrn_query_block* query_block = MRN_TABLE_LIST_QUERY_BLOCK(table_list);
  mrn_query_expression* query_expression =
    MRN_TABLE_LIST_DERIVED_QUERY_EXPRESSION(table_list);
  mrn_query_block* first_query_block;
  if (query_expression) {
    first_query_block =
      MRN_QUERY_EXPRESSION_FIRST_QUERY_BLOCK(query_expression);
  } else {
    first_query_block = query_block;
  }
  GRN_LOG(ctx,
          GRN_LOG_DEBUG,
          "%s first_query_block->options=%llu",
          tag,
          first_query_block
            ? MRN_QUERY_BLOCK_GET_ACTIVE_OPTIONS(first_query_block)
            : 0);

  // TODO: Extract as a class like mrn::ConditionConverter
  if (thd_sql_command(ha_thd()) == SQLCOM_SELECT &&
      !query_block->with_sum_func && !query_block->group_list.elements &&
      !MRN_QUERY_BLOCK_GET_HAVING_COND(query_block) &&
      MRN_QUERY_BLOCK_GET_TABLE_LIST(query_block).elements == 1 &&
      strcmp(MRN_GET_DB_NAME(MRN_QUERY_BLOCK_GET_TABLE_LIST(query_block).first),
             MRN_GET_DB_NAME(table_list)) == 0 &&
      strcmp(
        MRN_GET_TABLE_NAME(MRN_QUERY_BLOCK_GET_TABLE_LIST(query_block).first),
        MRN_GET_TABLE_NAME(table_list)) == 0 &&
      query_block->order_list.elements &&
      MRN_QUERY_BLOCK_HAS_LIMIT(query_block) &&
      MRN_QUERY_BLOCK_SELECT_LIMIT(query_block) &&
      MRN_QUERY_BLOCK_SELECT_LIMIT(query_block)->val_int() > 0) {
    if (MRN_QUERY_BLOCK_OFFSET_LIMIT(query_block)) {
      *limit = MRN_QUERY_BLOCK_OFFSET_LIMIT(query_block)->val_int();
    } else {
      *limit = 0;
    }
    *limit += MRN_QUERY_BLOCK_SELECT_LIMIT(query_block)->val_int();
    if (*limit > (longlong)INT_MAX) {
      GRN_LOG(ctx,
              GRN_LOG_DEBUG,
              "%s[false] too long limit: %lld <= %d is required",
              tag,
              *limit,
              INT_MAX);
      fast_order_limit = false;
      DBUG_RETURN(fast_order_limit);
    }
    if (first_query_block &&
        (MRN_QUERY_BLOCK_GET_ACTIVE_OPTIONS(first_query_block) &
         OPTION_FOUND_ROWS)) {
      GRN_LOG(ctx,
              GRN_LOG_DEBUG,
              "%s[false] SQL_CALC_FOUND_ROWS is specified",
              tag);
      fast_order_limit = false;
      DBUG_RETURN(fast_order_limit);
    }
#ifdef MRN_ENABLE_WRAPPER_MODE
    const bool is_storage_mode = !(share->wrapper_mode);
#else
    const bool is_storage_mode = true;
#endif
    Item* where = MRN_QUERY_BLOCK_GET_WHERE_COND(query_block);
    if (where) {
      if (is_storage_mode) {
        if (!pushed_cond) {
          GRN_LOG(ctx,
                  GRN_LOG_DEBUG,
                  "%s[storage][false] not Groonga layer condition search",
                  tag);
          fast_order_limit = false;
          DBUG_RETURN(fast_order_limit);
        }
        mrn::ConditionConverter converter(ha_thd(),
                                          ctx,
                                          grn_table,
                                          grn_index_columns,
                                          table->key_info,
                                          is_storage_mode);
        if (!converter.is_convertable(where)) {
          GRN_LOG(ctx,
                  GRN_LOG_DEBUG,
                  "%s[storage][false] any not Groonga layer condition exists",
                  tag);
          fast_order_limit = false;
          DBUG_RETURN(fast_order_limit);
        }
#ifdef MRN_ENABLE_WRAPPER_MODE
      } else {
        mrn::ConditionConverter converter(ha_thd(),
                                          ctx,
                                          grn_table,
                                          grn_index_columns,
                                          table->key_info,
                                          is_storage_mode);
        if (!converter.is_convertable(where)) {
          GRN_LOG(ctx,
                  GRN_LOG_DEBUG,
                  "%s[wrapper][false] any not Groonga layer condition exists",
                  tag);
          fast_order_limit = false;
          DBUG_RETURN(fast_order_limit);
        }
        unsigned int n_match_againsts = converter.count_match_against(where);
        if (n_match_againsts == 0) {
          GRN_LOG(ctx,
                  GRN_LOG_DEBUG,
                  "%s[wrapper][false] "
                  "Groonga layer condition but not fulltext search",
                  tag);
          fast_order_limit = false;
          DBUG_RETURN(fast_order_limit);
        }
        if (n_match_againsts > 1) {
          GRN_LOG(ctx,
                  GRN_LOG_DEBUG,
                  "%s[wrapper][false] multiple MATCH AGAINSTs exist",
                  tag);
          fast_order_limit = false;
          DBUG_RETURN(fast_order_limit);
        }
#endif
      }
    }
    int n_max_sort_keys = query_block->order_list.elements;
    *n_sort_keys = 0;
    size_t sort_keys_size = sizeof(grn_table_sort_key) * n_max_sort_keys;
    *sort_keys =
      (grn_table_sort_key*)mrn_my_malloc(sort_keys_size, MYF(MY_WME));
    memset(*sort_keys, 0, sort_keys_size);
    ORDER* order;
    int i;
    mrn_change_encoding(ctx, system_charset_info);
    fast_order_limit = true;
    for (order = (ORDER*)query_block->order_list.first, i = 0; order;
         order = order->next, i++) {
      Item* item = *order->item;
      if (item->type() == Item::FIELD_ITEM) {
        Field* field = static_cast<Item_field*>(item)->field;
        mrn::ColumnName column_name(FIELD_NAME(field));

        if (should_normalize(field, false)) {
          GRN_LOG(ctx,
                  GRN_LOG_DEBUG,
                  "%s[false] sort by collated value isn't supported yet",
                  tag);
          fast_order_limit = false;
          break;
        }

        if (is_storage_mode) {
          (*sort_keys)[i].key = grn_obj_column(ctx,
                                               result_set,
                                               column_name.c_str(),
                                               column_name.length());
#ifdef MRN_ENABLE_WRAPPER_MODE
        } else {
          if (is_primary_key_field(field)) {
            (*sort_keys)[i].key = grn_obj_column(ctx,
                                                 result_set,
                                                 MRN_COLUMN_NAME_KEY,
                                                 strlen(MRN_COLUMN_NAME_KEY));
          } else {
            GRN_LOG(ctx,
                    GRN_LOG_DEBUG,
                    "%s[wrapper][false] "
                    "sort by not primary key value isn't supported",
                    tag);
            fast_order_limit = false;
            break;
          }
#endif
        }
      } else if (item->type() == Item::FUNC_ITEM) {
        Item_func* func_item = static_cast<Item_func*>(item);
        if (func_item->functype() == Item_func::FT_FUNC) {
          (*sort_keys)[i].key = grn_obj_column(ctx,
                                               result_set,
                                               MRN_COLUMN_NAME_SCORE,
                                               strlen(MRN_COLUMN_NAME_SCORE));
        } else {
          GRN_LOG(ctx,
                  GRN_LOG_DEBUG,
                  "%s[false] ORDER BY %s()",
                  tag,
                  func_item->func_name());
          fast_order_limit = false;
        }
      } else {
        GRN_LOG(ctx,
                GRN_LOG_DEBUG,
                "%s[false] not ORDER BY column nor ORDER BY MATCH AGAINST",
                tag);
        fast_order_limit = false;
        break;
      }
      (*sort_keys)[i].offset = 0;
      if (MRN_ORDER_IS_ASC(order)) {
        (*sort_keys)[i].flags = GRN_TABLE_SORT_ASC;
      } else {
        (*sort_keys)[i].flags = GRN_TABLE_SORT_DESC;
      }
      (*n_sort_keys)++;
    }
    if (fast_order_limit) {
      GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[true]", tag);
      mrn_fast_order_limit++;
    } else {
      for (int j = 0; j < i; ++j) {
        grn_obj_unlink(ctx, (*sort_keys)[j].key);
      }
      my_free(*sort_keys);
      *sort_keys = NULL;
      *n_sort_keys = 0;
    }
    DBUG_RETURN(fast_order_limit);
  }
  GRN_LOG(ctx, GRN_LOG_DEBUG, "%s[false]", tag);
  fast_order_limit = false;
  DBUG_RETURN(fast_order_limit);
}

int ha_mroonga::generic_store_bulk_fixed_size_string(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  grn_obj_reinit(ctx, buf, GRN_DB_SHORT_TEXT, 0);
  if (field->is_null()) {
    grn_bulk_space(ctx, buf, field->field_length);
    memset(GRN_TEXT_VALUE(buf), ' ', field->field_length);
  } else {
    GRN_TEXT_SET(ctx, buf, MRN_FIELD_FIELD_PTR(field), field->field_length);
  }
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_variable_size_string(Field* field,
                                                        grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  grn_obj_reinit(ctx, buf, GRN_DB_SHORT_TEXT, 0);
  if (!field->is_null()) {
    StringBuffer<MAX_FIELD_WIDTH> buffer(field->charset());
    auto value = field->val_str(&buffer, &buffer);
    DBUG_PRINT("info",
               ("mroonga: length=%" MRN_FORMAT_STRING_LENGTH, value->length()));
    DBUG_PRINT("info", ("mroonga: value=%s", value->c_ptr_safe()));
    GRN_TEXT_SET(ctx, buf, value->ptr(), value->length());
  }
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_integer(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  long long value = 0;
  if (!field->is_null()) {
    value = field->val_int();
  }
  DBUG_PRINT("info", ("mroonga: value=%lld", value));
  uint32 size = field->pack_length();
  DBUG_PRINT("info", ("mroonga: size=%u", size));
  Field_num* field_num = static_cast<Field_num*>(field);
  bool is_unsigned = MRN_FIELD_IS_UNSIGNED(field_num);
  DBUG_PRINT("info",
             ("mroonga: is_unsigned=%s", is_unsigned ? "true" : "false"));
  switch (size) {
  case 1:
    if (is_unsigned) {
      grn_obj_reinit(ctx, buf, GRN_DB_UINT8, 0);
      GRN_UINT8_SET(ctx, buf, value);
    } else {
      grn_obj_reinit(ctx, buf, GRN_DB_INT8, 0);
      GRN_INT8_SET(ctx, buf, value);
    }
    break;
  case 2:
    if (is_unsigned) {
      grn_obj_reinit(ctx, buf, GRN_DB_UINT16, 0);
      GRN_UINT16_SET(ctx, buf, value);
    } else {
      grn_obj_reinit(ctx, buf, GRN_DB_INT16, 0);
      GRN_INT16_SET(ctx, buf, value);
    }
    break;
  case 3:
  case 4:
    if (is_unsigned) {
      grn_obj_reinit(ctx, buf, GRN_DB_UINT32, 0);
      GRN_UINT32_SET(ctx, buf, value);
    } else {
      grn_obj_reinit(ctx, buf, GRN_DB_INT32, 0);
      GRN_INT32_SET(ctx, buf, value);
    }
    break;
  case 8:
    if (is_unsigned) {
      grn_obj_reinit(ctx, buf, GRN_DB_UINT64, 0);
      GRN_UINT64_SET(ctx, buf, value);
    } else {
      grn_obj_reinit(ctx, buf, GRN_DB_INT64, 0);
      GRN_INT64_SET(ctx, buf, value);
    }
    break;
  default:
    // Why!?
    error = HA_ERR_UNSUPPORTED;
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "unknown integer value size: <%u>: "
             "available sizes: [1, 2, 3, 4, 8]",
             size);
    push_warning(ha_thd(), MRN_SEVERITY_WARNING, error, error_message);
    break;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_unsigned_integer(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  long long signed_value = 0;
  if (!field->is_null()) {
    signed_value = field->val_int();
  }
  unsigned long long unsigned_value = *((unsigned long long*)(&signed_value));
  uint32 size = field->pack_length();
  switch (size) {
  case 1:
    grn_obj_reinit(ctx, buf, GRN_DB_UINT8, 0);
    GRN_UINT8_SET(ctx, buf, unsigned_value);
    break;
  case 2:
    grn_obj_reinit(ctx, buf, GRN_DB_UINT16, 0);
    GRN_UINT16_SET(ctx, buf, unsigned_value);
    break;
  case 3:
  case 4:
    grn_obj_reinit(ctx, buf, GRN_DB_UINT32, 0);
    GRN_UINT32_SET(ctx, buf, unsigned_value);
    break;
  case 5: // BIT(33-40)
  case 6: // BIT(41-48)
  case 7: // BIT(49-56)
  case 8:
    grn_obj_reinit(ctx, buf, GRN_DB_UINT64, 0);
    GRN_UINT64_SET(ctx, buf, unsigned_value);
    break;
  default:
    // Why!?
    error = HA_ERR_UNSUPPORTED;
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "unknown unsigned integer value size: <%u>: "
             "available sizes: [1, 2, 3, 4, 5, 6, 7, 8]",
             size);
    push_warning(ha_thd(), MRN_SEVERITY_WARNING, error, error_message);
    break;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_float(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  double value = 0.0;
  if (!field->is_null()) {
    value = field->val_real();
  }
  uint32 size = field->pack_length();
  switch (size) {
  case 4:
  case 8:
    grn_obj_reinit(ctx, buf, GRN_DB_FLOAT, 0);
    GRN_FLOAT_SET(ctx, buf, value);
    break;
  default:
    // Why!?
    error = HA_ERR_UNSUPPORTED;
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "unknown float value size: <%u>: "
             "available sizes: [4, 8]",
             size);
    push_warning(ha_thd(), MRN_SEVERITY_WARNING, error, error_message);
    break;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_timestamp(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  int64_t grn_time = 0;
  if (!field->is_null()) {
    Field_timestamp* timestamp_field = static_cast<Field_timestamp*>(field);
    mrn::TimestampFieldValueConverter<Field_timestamp> converter(
      timestamp_field);
    grn_time = converter.convert();
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, grn_time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_date(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  long long int date_value = field->val_int();
  struct tm date;
  memset(&date, 0, sizeof(struct tm));
  date.tm_year = date_value / 10000 % 10000 - mrn::TimeConverter::TM_YEAR_BASE;
  date.tm_mon = date_value / 100 % 100 - 1;
  date.tm_mday = date_value % 100;
  int usec = 0;
  mrn::TimeConverter time_converter;
  long long int time = time_converter.tm_to_grn_time(&date, usec, &truncated);
  if (truncated) {
    field->set_warning(MRN_SEVERITY_WARNING, WARN_DATA_TRUNCATED, 1);
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_time(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  Field_time* time_field = (Field_time*)field;
  MYSQL_TIME mysql_time;
  MRN_FIELD_GET_TIME(time_field, &mysql_time, current_thd);
  mrn::TimeConverter time_converter;
  long long int time =
    time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  if (truncated) {
    field->set_warning(MRN_SEVERITY_WARNING, WARN_DATA_TRUNCATED, 1);
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_datetime(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  Field_datetime* datetime_field = (Field_datetime*)field;
  MYSQL_TIME mysql_time;
  MRN_FIELD_GET_TIME(datetime_field, &mysql_time, current_thd);
  long long int time = 0;
  if (!field->is_null()) {
    mrn::TimeConverter time_converter;
    time = time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  }
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_year(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;

  int year = 1970;
  if (!field->is_null()) {
    if (field->field_length == 2) {
      const auto two_digits_year = field->val_int();
      // https://dev.mysql.com/doc/refman/9.0/en/year.html
      // As 1- or 2-digit numbers in the range 0 to 99. MySQL converts
      // values in the ranges 1 to 69 and 70 to 99 to YEAR values in the
      // ranges 2001 to 2069 and 1970 to 1999.
      if (two_digits_year < 70) {
        year = 2000 + two_digits_year;
      } else {
        year = 1900 + two_digits_year;
      }
    } else {
      year = static_cast<int>(field->val_int());
    }
  }

  DBUG_PRINT("info", ("mroonga: year=%d", year));
  struct tm date;
  memset(&date, 0, sizeof(struct tm));
  date.tm_year = year - mrn::TimeConverter::TM_YEAR_BASE;
  date.tm_mon = 0;
  date.tm_mday = 1;

  int usec = 0;
  mrn::TimeConverter time_converter;
  long long int time = time_converter.tm_to_grn_time(&date, usec, &truncated);
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_timestamp2(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  int64_t grn_time = 0;
  if (!field->is_null()) {
    Field_timestampf* timestamp_field = static_cast<Field_timestampf*>(field);
    mrn::TimestampFieldValueConverter<Field_timestampf> converter(
      timestamp_field);
    grn_time = converter.convert();
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, grn_time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_datetime2(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  Field_datetimef* datetimef_field = (Field_datetimef*)field;
  MYSQL_TIME mysql_time;
  MRN_FIELD_GET_TIME(datetimef_field, &mysql_time, current_thd);
  long long int time = 0;
  mrn::TimeConverter time_converter;
  if (!field->is_null()) {
    time = time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  }
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_time2(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  MYSQL_TIME mysql_time;
  MRN_FIELD_GET_TIME(field, &mysql_time, current_thd);
  mrn::TimeConverter time_converter;
  long long int time = 0;
  if (!field->is_null()) {
    time = time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  }
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_new_date(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  long long int time = 0;
  if (!field->is_null()) {
    auto newdate_field = static_cast<Field_newdate*>(field);
    MYSQL_TIME mysql_date;
    MRN_FIELD_GET_TIME(newdate_field, &mysql_date, current_thd);
    mrn::TimeConverter time_converter;
    time = time_converter.mysql_time_to_grn_time(&mysql_date, &truncated);
  }
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  grn_obj_reinit(ctx, buf, GRN_DB_TIME, 0);
  GRN_TIME_SET(ctx, buf, time);
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_new_decimal(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  String value;
  Field_new_decimal* new_decimal_field = (Field_new_decimal*)field;
  new_decimal_field->val_str(&value, NULL);
  grn_obj_reinit(ctx, buf, GRN_DB_SHORT_TEXT, 0);
  GRN_TEXT_SET(ctx, buf, value.ptr(), value.length());
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_blob(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  grn_obj_reinit(ctx, buf, GRN_DB_TEXT, 0);
  if (!field->is_null()) {
    StringBuffer<MAX_FIELD_WIDTH> buffer(field->charset());
    auto value = field->val_str(&buffer, &buffer);
    GRN_TEXT_SET(ctx, buf, value->ptr(), value->length());
  }
  DBUG_RETURN(error);
}

int ha_mroonga::generic_store_bulk_geometry(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_HAVE_SPATIAL
  Field_geom* geometry = static_cast<Field_geom*>(field);
  String buffer;
  String* value = geometry->val_str(0, &buffer);
  const char* wkb = value->ptr();
  int len = value->length();
  error = mrn_set_geometry(ctx, buf, wkb, len);
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_HAVE_MYSQL_TYPE_JSON
int ha_mroonga::generic_store_bulk_json(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  String buffer;
  Field_json* json = static_cast<Field_json*>(field);
  String* value = json->val_str(&buffer, NULL);
  grn_obj_reinit(ctx, buf, GRN_DB_TEXT, 0);
  GRN_TEXT_SET(ctx, buf, value->ptr(), value->length());
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::generic_store_bulk(Field* field, grn_obj* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error;
  error = mrn_change_encoding(ctx, field->charset());
  if (error)
    return error;
  switch (field->real_type()) {
  case MYSQL_TYPE_DECIMAL:
    error = generic_store_bulk_variable_size_string(field, buf);
    break;
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
    error = generic_store_bulk_integer(field, buf);
    break;
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    error = generic_store_bulk_float(field, buf);
    break;
  case MYSQL_TYPE_NULL:
    error = generic_store_bulk_unsigned_integer(field, buf);
    break;
  case MYSQL_TYPE_TIMESTAMP:
    error = generic_store_bulk_timestamp(field, buf);
    break;
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
    error = generic_store_bulk_integer(field, buf);
    break;
  case MYSQL_TYPE_DATE:
    error = generic_store_bulk_date(field, buf);
    break;
  case MYSQL_TYPE_TIME:
    error = generic_store_bulk_time(field, buf);
    break;
  case MYSQL_TYPE_DATETIME:
    error = generic_store_bulk_datetime(field, buf);
    break;
  case MYSQL_TYPE_YEAR:
    error = generic_store_bulk_year(field, buf);
    break;
  case MYSQL_TYPE_NEWDATE:
    error = generic_store_bulk_new_date(field, buf);
    break;
  case MYSQL_TYPE_VARCHAR:
    error = generic_store_bulk_variable_size_string(field, buf);
    break;
  case MYSQL_TYPE_BIT:
    error = generic_store_bulk_unsigned_integer(field, buf);
    break;
  case MYSQL_TYPE_TIMESTAMP2:
    error = generic_store_bulk_timestamp2(field, buf);
    break;
  case MYSQL_TYPE_DATETIME2:
    error = generic_store_bulk_datetime2(field, buf);
    break;
  case MYSQL_TYPE_TIME2:
    error = generic_store_bulk_time2(field, buf);
    break;
  case MYSQL_TYPE_NEWDECIMAL:
    error = generic_store_bulk_new_decimal(field, buf);
    break;
  case MYSQL_TYPE_ENUM:
    error = generic_store_bulk_unsigned_integer(field, buf);
    break;
  case MYSQL_TYPE_SET:
    error = generic_store_bulk_unsigned_integer(field, buf);
    break;
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
    error = generic_store_bulk_blob(field, buf);
    break;
  case MYSQL_TYPE_VAR_STRING:
    error = generic_store_bulk_variable_size_string(field, buf);
    break;
  case MYSQL_TYPE_STRING:
    error = generic_store_bulk_fixed_size_string(field, buf);
    break;
  case MYSQL_TYPE_GEOMETRY:
    error = generic_store_bulk_geometry(field, buf);
    break;
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
  case MYSQL_TYPE_JSON:
    error = generic_store_bulk_json(field, buf);
    break;
#endif
  default:
    error = HA_ERR_UNSUPPORTED;
    break;
  }
  DBUG_RETURN(error);
}

void ha_mroonga::storage_store_field_string(Field* field,
                                            const char* value,
                                            uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  field->store(value, value_length, field->charset());
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_integer(Field* field,
                                             const char* value,
                                             uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  Field_num* field_num = static_cast<Field_num*>(field);
  bool is_unsigned = MRN_FIELD_IS_UNSIGNED(field_num);
  switch (value_length) {
  case 1: {
    if (is_unsigned) {
      unsigned char field_value;
      field_value = *((unsigned char*)value);
      field->store(field_value, is_unsigned);
    } else {
      signed char field_value;
      field_value = *((signed char*)value);
      field->store(field_value, is_unsigned);
    }
    break;
  }
  case 2: {
    if (is_unsigned) {
      unsigned short field_value;
      field_value = *((unsigned short*)value);
      field->store(field_value, is_unsigned);
    } else {
      short field_value;
      field_value = *((short*)value);
      field->store(field_value, is_unsigned);
    }
    break;
  }
  case 4: {
    if (is_unsigned) {
      unsigned int field_value;
      field_value = *((unsigned int*)value);
      field->store(field_value, is_unsigned);
    } else {
      int field_value;
      field_value = *((int*)value);
      field->store(field_value, is_unsigned);
    }
    break;
  }
  case 8: {
    if (is_unsigned) {
      unsigned long long int field_value;
      field_value = *((unsigned long long int*)value);
      DBUG_PRINT("info", ("mroonga: field_value=%llu", field_value));
      field->store(field_value, is_unsigned);
    } else {
      long long int field_value;
      field_value = *((long long int*)value);
      field->store(field_value, is_unsigned);
    }
    break;
  }
  default: {
    // Why!?
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "unknown integer value size: <%d>: "
             "available sizes: [1, 2, 4, 8]",
             value_length);
    push_warning(ha_thd(),
                 MRN_SEVERITY_WARNING,
                 HA_ERR_UNSUPPORTED,
                 error_message);
    storage_store_field_string(field, value, value_length);
    break;
  }
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_unsigned_integer(Field* field,
                                                      const char* value,
                                                      uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  switch (value_length) {
  case 1: {
    unsigned char field_value;
    field_value = *((unsigned char*)value);
    field->store(field_value, true);
    break;
  }
  case 2: {
    unsigned short field_value;
    field_value = *((unsigned short*)value);
    field->store(field_value, true);
    break;
  }
  case 4: {
    unsigned int field_value;
    field_value = *((unsigned int*)value);
    field->store(field_value, true);
    break;
  }
  case 8: {
    unsigned long long int field_value;
    field_value = *((unsigned long long int*)value);
    DBUG_PRINT("info", ("mroonga: field_value=%llu", field_value));
    field->store(field_value, true);
    break;
  }
  default: {
    // Why!?
    char error_message[MRN_MESSAGE_BUFFER_SIZE];
    snprintf(error_message,
             MRN_MESSAGE_BUFFER_SIZE,
             "unknown integer value size: <%d>: "
             "available sizes: [1, 2, 4, 8]",
             value_length);
    push_warning(ha_thd(),
                 MRN_SEVERITY_WARNING,
                 HA_ERR_UNSUPPORTED,
                 error_message);
    storage_store_field_string(field, value, value_length);
    break;
  }
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_float(Field* field,
                                           const char* value,
                                           uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  double field_value;
  field_value = *((double*)value);
  field->store(field_value);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_timestamp(Field* field,
                                               const char* value,
                                               uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);
  Field_timestamp* timestamp_field = (Field_timestamp*)field;
#ifdef MRN_TIMESTAMP_USE_TIMEVAL
  struct timeval time_value;
  GRN_TIME_UNPACK(time, time_value.tv_sec, time_value.tv_usec);
  timestamp_field->store_timestamp(&time_value);
#elif defined(MRN_TIMESTAMP_USE_MY_TIMEVAL)
  struct my_timeval my_time_value;
  GRN_TIME_UNPACK(time, my_time_value.m_tv_sec, my_time_value.m_tv_usec);
  timestamp_field->store_timestamp(&my_time_value);
#elif defined(MRN_TIMESTAMP_USE_MY_TIME_T)
  long long int sec, usec;
  GRN_TIME_UNPACK(time, sec, usec);
  timestamp_field->store_TIME(static_cast<int32>(sec),
                              static_cast<int32>(usec));
#else
  int32 sec, usec __attribute__((unused));
  GRN_TIME_UNPACK(time, sec, usec);
  timestamp_field->store_timestamp(sec);
#endif
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_date(Field* field,
                                          const char* value,
                                          uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);
  long long int sec, usec __attribute__((unused));
  GRN_TIME_UNPACK(time, sec, usec);
  struct tm date;
  time_t sec_t = static_cast<int32>(sec);
  gmtime_r(&sec_t, &date);
  long long int date_in_mysql =
    (date.tm_year + mrn::TimeConverter::TM_YEAR_BASE) * 10000 +
    (date.tm_mon + 1) * 100 + date.tm_mday;
  field->store(date_in_mysql, false);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_time(Field* field,
                                          const char* value,
                                          uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);
  MYSQL_TIME mysql_time;
  memset(&mysql_time, 0, sizeof(MYSQL_TIME));
  mysql_time.time_type = MYSQL_TIMESTAMP_TIME;
  mrn::TimeConverter time_converter;
  time_converter.grn_time_to_mysql_time(time, &mysql_time);
  field->store_time(&mysql_time);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_datetime(Field* field,
                                              const char* value,
                                              uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);
  MYSQL_TIME mysql_datetime;
  memset(&mysql_datetime, 0, sizeof(MYSQL_TIME));
  mysql_datetime.time_type = MYSQL_TIMESTAMP_DATETIME;
  mrn::TimeConverter time_converter;
  time_converter.grn_time_to_mysql_time(time, &mysql_datetime);
  field->store_time(&mysql_datetime);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_year(Field* field,
                                          const char* value,
                                          uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);
  MYSQL_TIME mysql_time;
  memset(&mysql_time, 0, sizeof(MYSQL_TIME));
  mysql_time.time_type = MYSQL_TIMESTAMP_DATE;
  mrn::TimeConverter time_converter;
  time_converter.grn_time_to_mysql_time(time, &mysql_time);
  DBUG_PRINT("info", ("mroonga: stored %d", mysql_time.year));
  field->store(mysql_time.year, false);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_new_date(Field* field,
                                              const char* value,
                                              uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);
  MYSQL_TIME mysql_date;
  memset(&mysql_date, 0, sizeof(MYSQL_TIME));
  mysql_date.time_type = MYSQL_TIMESTAMP_DATE;
  mrn::TimeConverter time_converter;
  time_converter.grn_time_to_mysql_time(time, &mysql_date);
  field->store_time(&mysql_date);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_datetime2(Field* field,
                                               const char* value,
                                               uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);
  MYSQL_TIME mysql_datetime;
  memset(&mysql_datetime, 0, sizeof(MYSQL_TIME));
  mysql_datetime.time_type = MYSQL_TIMESTAMP_DATETIME;
  mrn::TimeConverter time_converter;
  time_converter.grn_time_to_mysql_time(time, &mysql_datetime);
  field->store_time(&mysql_datetime);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_time2(Field* field,
                                           const char* value,
                                           uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  long long int time = *((long long int*)value);

  MYSQL_TIME mysql_time;
  memset(&mysql_time, 0, sizeof(MYSQL_TIME));
  mysql_time.time_type = MYSQL_TIMESTAMP_TIME;
  mrn::TimeConverter time_converter;
  time_converter.grn_time_to_mysql_time(time, &mysql_time);
  field->store_time(&mysql_time);
  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_field_blob(Field* field,
                                          const char* value,
                                          uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  Field_blob* blob = (Field_blob*)field;
  grn_obj* blob_buffer = blob_buffers_[MRN_FIELD_FIELD_INDEX(field)];
  GRN_TEXT_SET(ctx, blob_buffer, value, value_length);
  blob->set_ptr(GRN_TEXT_LEN(blob_buffer),
                reinterpret_cast<uchar*>(GRN_TEXT_VALUE(blob_buffer)));
  DBUG_VOID_RETURN;
}

#ifdef MRN_HAVE_MYSQL_TYPE_BLOB_COMPRESSED
void ha_mroonga::storage_store_field_blob_compressed(Field* field,
                                                     const char* value,
                                                     uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  Field_blob* blob = static_cast<Field_blob*>(field);
  blob->store(value, value_length, field->charset());
  DBUG_VOID_RETURN;
}
#endif

bool ha_mroonga::geo_need_reverse(Field_geom* field)
{
  MRN_DBUG_ENTER_METHOD();
  bool reverse = false;
#ifdef MRN_HAVE_SRS
  auto srid = MRN_FIELD_GEOM_GET_SRID(field);
  if (srid != 0) {
    auto thd = ha_thd();
    std::unique_ptr<dd::cache::Dictionary_client::Auto_releaser> releaser(
      new dd::cache::Dictionary_client::Auto_releaser(thd->dd_client()));
    Srs_fetcher fetcher(thd);
    const dd::Spatial_reference_system* srs = nullptr;
    if (!fetcher.acquire(srid, &srs)) {
      if (srs && srs->is_geographic() && srs->is_lat_long()) {
        reverse = true;
      }
    }
  }
#endif
  DBUG_RETURN(reverse);
}
void ha_mroonga::storage_store_field_geometry(Field* field,
                                              const char* value,
                                              uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_HAVE_SPATIAL
  const grn_geo_point* field_value =
    reinterpret_cast<const grn_geo_point*>(value);
  int latitude = field_value->latitude;
  int longitude = field_value->longitude;
  if (grn_source_column_geo) {
    GRN_GEO_POINT_SET(ctx, &source_point, latitude, longitude);
  }
  uchar wkb[SRID_SIZE + WKB_HEADER_SIZE + POINT_DATA_SIZE];
  Field_geom* geometry = static_cast<Field_geom*>(field);
  mrn_srid srid = MRN_FIELD_GEOM_GET_SRID(geometry);
  int4store(wkb, srid);
  memset(wkb + SRID_SIZE, Geometry::wkb_ndr, 1); // wkb_ndr is meaningless.
  int4store(wkb + SRID_SIZE + 1, Geometry::wkb_point);
  double latitude_in_degree = GRN_GEO_MSEC2DEGREE(latitude);
  double longitude_in_degree = GRN_GEO_MSEC2DEGREE(longitude);
  bool reverse = geo_need_reverse(geometry);
  if (reverse) {
    float8store(wkb + SRID_SIZE + WKB_HEADER_SIZE, longitude_in_degree);
    float8store(wkb + SRID_SIZE + WKB_HEADER_SIZE + SIZEOF_STORED_DOUBLE,
                latitude_in_degree);
  } else {
    float8store(wkb + SRID_SIZE + WKB_HEADER_SIZE, latitude_in_degree);
    float8store(wkb + SRID_SIZE + WKB_HEADER_SIZE + SIZEOF_STORED_DOUBLE,
                longitude_in_degree);
  }
  grn_obj* geometry_buffer = blob_buffers_[MRN_FIELD_FIELD_INDEX(field)];
  uint wkb_length = sizeof(wkb) / sizeof(*wkb);
  GRN_TEXT_SET(ctx, geometry_buffer, wkb, wkb_length);
  geometry->set_ptr(GRN_TEXT_LEN(geometry_buffer),
                    reinterpret_cast<uchar*>(GRN_TEXT_VALUE(geometry_buffer)));
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_HAVE_MYSQL_TYPE_JSON
void ha_mroonga::storage_store_field_json(Field* field,
                                          const char* value,
                                          uint value_length)
{
  MRN_DBUG_ENTER_METHOD();
  Field_json* json = static_cast<Field_json*>(field);
  json->store(value, value_length, field->charset());
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_store_field(Field* field,
                                     const char* value,
                                     uint value_length)
{
  field->set_notnull();
  switch (field->real_type()) {
  case MYSQL_TYPE_DECIMAL:
    storage_store_field_string(field, value, value_length);
    break;
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
    storage_store_field_integer(field, value, value_length);
    break;
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    storage_store_field_float(field, value, value_length);
    break;
  case MYSQL_TYPE_NULL:
    storage_store_field_unsigned_integer(field, value, value_length);
    break;
  case MYSQL_TYPE_TIMESTAMP:
    storage_store_field_timestamp(field, value, value_length);
    break;
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
    storage_store_field_integer(field, value, value_length);
    break;
  case MYSQL_TYPE_DATE:
    storage_store_field_date(field, value, value_length);
    break;
  case MYSQL_TYPE_TIME:
    storage_store_field_time(field, value, value_length);
    break;
  case MYSQL_TYPE_DATETIME:
    storage_store_field_datetime(field, value, value_length);
    break;
  case MYSQL_TYPE_YEAR:
    storage_store_field_year(field, value, value_length);
    break;
  case MYSQL_TYPE_NEWDATE:
    storage_store_field_new_date(field, value, value_length);
    break;
  case MYSQL_TYPE_VARCHAR:
#ifdef MRN_HAVE_MYSQL_TYPE_VARCHAR_COMPRESSED
  case MYSQL_TYPE_VARCHAR_COMPRESSED:
#endif
    storage_store_field_string(field, value, value_length);
    break;
  case MYSQL_TYPE_BIT:
    storage_store_field_unsigned_integer(field, value, value_length);
    break;
  case MYSQL_TYPE_TIMESTAMP2:
    storage_store_field_timestamp(field, value, value_length);
    break;
  case MYSQL_TYPE_DATETIME2:
    storage_store_field_datetime2(field, value, value_length);
    break;
  case MYSQL_TYPE_TIME2:
    storage_store_field_time2(field, value, value_length);
    break;
  case MYSQL_TYPE_NEWDECIMAL:
    storage_store_field_string(field, value, value_length);
    break;
  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_SET:
    storage_store_field_unsigned_integer(field, value, value_length);
    break;
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
#ifdef MRN_HAVE_MYSQL_TYPE_BLOB_COMPRESSED
  case MYSQL_TYPE_BLOB_COMPRESSED:
    if (field->unireg_check == Field::TMYSQL_COMPRESSED)
      storage_store_field_blob_compressed(field, value, value_length);
    else
#endif
      storage_store_field_blob(field, value, value_length);
    break;
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_STRING:
    storage_store_field_string(field, value, value_length);
    break;
  case MYSQL_TYPE_GEOMETRY:
    storage_store_field_geometry(field, value, value_length);
    break;
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
  case MYSQL_TYPE_JSON:
    storage_store_field_json(field, value, value_length);
    break;
#endif
  }
}

void ha_mroonga::storage_get_column_value(int nth_column,
                                          grn_id record_id,
                                          grn_obj* value)
{
  grn_obj* column = grn_columns[nth_column];
  grn_column_cache* column_cache = grn_column_caches[nth_column];
  grn_id range_id = grn_obj_get_range(ctx, column);
  grn_obj_flags flags;

  if (mrn::grn::is_vector_column(column)) {
    flags = GRN_OBJ_VECTOR;
  } else {
    flags = 0;
  }

  grn_obj_reinit(ctx, value, range_id, flags);
  if (column_cache) {
    size_t raw_value_size = 0;
    void* raw_value =
      grn_column_cache_ref(ctx, column_cache, record_id, &raw_value_size);
    if (value) {
      grn_bulk_write(ctx,
                     value,
                     static_cast<const char*>(raw_value),
                     raw_value_size);
    }
  } else {
    grn_obj_get_value(ctx, column, record_id, value);
  }
}

void ha_mroonga::storage_store_field_column(Field* field,
                                            bool is_primary_key,
                                            int nth_column,
                                            grn_id record_id)
{
  MRN_DBUG_ENTER_METHOD();

  grn_obj* column = grn_columns[nth_column];
  if (!column) {
    DBUG_VOID_RETURN;
  }

  grn_obj* range = grn_column_ranges[nth_column];

  grn_obj* value = &new_value_buffer;
  if (mrn::grn::is_table(range)) {
    if (mrn::grn::is_vector_column(column)) {
      storage_get_column_value(nth_column, record_id, value);

      grn_obj unvectored_value;
      GRN_TEXT_INIT(&unvectored_value, 0);
      int n_ids = GRN_RECORD_VECTOR_SIZE(value);
      if (grn_obj_is_table_with_key(ctx, range)) {
        grn_obj* tokenizer =
          grn_obj_get_info(ctx, range, GRN_INFO_DEFAULT_TOKENIZER, NULL);
        if (!tokenizer) {
          GRN_TEXT_PUTS(ctx, &unvectored_value, "[");
        }
        grn_obj key_buffer;
        if (grn_type_id_is_text_family(ctx, range->header.domain)) {
          GRN_SHORT_TEXT_INIT(&key_buffer, GRN_OBJ_DO_SHALLOW_COPY);
        } else {
          GRN_VALUE_FIX_SIZE_INIT(&key_buffer, 0, range->header.domain);
        }
        for (int i = 0; i < n_ids; i++) {
          grn_id id = GRN_RECORD_VALUE_AT(value, i);
          if (i > 0) {
            if (tokenizer) {
              GRN_TEXT_PUTS(ctx,
                            &unvectored_value,
                            mrn_vector_column_delimiter);
            } else {
              GRN_TEXT_PUTS(ctx, &unvectored_value, ",");
            }
          }
          char key[GRN_TABLE_MAX_KEY_SIZE];
          int key_length;
          key_length =
            grn_table_get_key(ctx, range, id, &key, GRN_TABLE_MAX_KEY_SIZE);
          if (tokenizer) {
            GRN_TEXT_PUT(ctx, &unvectored_value, key, key_length);
          } else {
            GRN_TEXT_SET(ctx, &key_buffer, key, key_length);
            grn_text_otoj(ctx, &unvectored_value, &key_buffer, NULL);
          }
        }
        GRN_OBJ_FIN(ctx, &key_buffer);
        if (!tokenizer) {
          GRN_TEXT_PUTS(ctx, &unvectored_value, "]");
        }
        storage_store_field(field,
                            GRN_TEXT_VALUE(&unvectored_value),
                            GRN_TEXT_LEN(&unvectored_value));
      } else {
        GRN_TEXT_PUTS(ctx, &unvectored_value, "[");
        for (int i = 0; i < n_ids; i++) {
          grn_id id = GRN_RECORD_VALUE_AT(value, i);
          if (i > 0) {
            GRN_TEXT_PUTS(ctx, &unvectored_value, ",");
          }
          grn_text_printf(ctx, &unvectored_value, "%u", id);
        }
        GRN_TEXT_PUTS(ctx, &unvectored_value, "]");
        storage_store_field(field,
                            GRN_TEXT_VALUE(&unvectored_value),
                            GRN_TEXT_LEN(&unvectored_value));
      }
      GRN_OBJ_FIN(ctx, &unvectored_value);
    } else {
      storage_get_column_value(nth_column, record_id, value);

      grn_id id = GRN_RECORD_VALUE(value);
      char key[GRN_TABLE_MAX_KEY_SIZE];
      int key_length;
      key_length =
        grn_table_get_key(ctx, range, id, &key, GRN_TABLE_MAX_KEY_SIZE);
      storage_store_field(field, key, key_length);
    }
  } else {
    storage_get_column_value(nth_column, record_id, value);
    if (is_primary_key && GRN_BULK_VSIZE(value) == 0) {
      char key[GRN_TABLE_MAX_KEY_SIZE];
      int key_length;
      key_length = grn_table_get_key(ctx,
                                     grn_table,
                                     record_id,
                                     &key,
                                     GRN_TABLE_MAX_KEY_SIZE);
      storage_store_field(field, key, key_length);
    } else {
      if (mrn::grn::is_vector_column(column)) {
        grn_obj unvectored_value;
        GRN_TEXT_INIT(&unvectored_value, 0);
        grn_text_otoj(ctx, &unvectored_value, value, NULL);
        storage_store_field(field,
                            GRN_BULK_HEAD(&unvectored_value),
                            GRN_BULK_VSIZE(&unvectored_value));
        GRN_OBJ_FIN(ctx, &unvectored_value);
      } else {
        storage_store_field(field, GRN_BULK_HEAD(value), GRN_BULK_VSIZE(value));
      }
    }
  }

  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_fields(TABLE* target_table,
                                      uchar* buf,
                                      grn_id record_id)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_PRINT("info", ("mroonga: stored record ID: %d", record_id));

  my_ptrdiff_t ptr_diff = buf - target_table->record[0];

  Field* primary_key_field = NULL;
  if (target_table->s->primary_key != MAX_INDEXES) {
    KEY* key_info = &(target_table->s->key_info[target_table->s->primary_key]);
    if (KEY_N_KEY_PARTS(key_info) == 1) {
      primary_key_field = key_info->key_part[0].field;
    }
  }

  int i;
  int n_columns = target_table->s->fields;
  int n_existing_grn_columns = table->s->fields;
  for (i = 0; i < n_columns && i < n_existing_grn_columns; i++) {
    Field* field = target_table->field[i];

    if (bitmap_is_set(target_table->read_set, MRN_FIELD_FIELD_INDEX(field)) ||
        bitmap_is_set(target_table->write_set, MRN_FIELD_FIELD_INDEX(field))) {
      if (ignoring_no_key_columns) {
        KEY* key_info = &(target_table->s->key_info[active_index]);
        bool have_key = false;
        uint n_keys = KEY_N_KEY_PARTS(key_info);
        for (uint j = 0; j < n_keys; ++j) {
          if (FIELD_NAME_EQUAL_FIELD(field, key_info->key_part[j].field)) {
            have_key = true;
            break;
          }
        }
        if (!have_key) {
          continue;
        }
      }

      mrn::DebugColumnAccess debug_column_access(target_table,
                                                 &(target_table->write_set));
      DBUG_PRINT(
        "info",
        ("mroonga: store column %d(%d)", i, MRN_FIELD_FIELD_INDEX(field)));
      field->move_field_offset(ptr_diff);
      if (FIELD_NAME_EQUAL(field, MRN_COLUMN_NAME_ID)) {
        // for _id column
        field->set_notnull();
        field->store((int)record_id);
      } else if (primary_key_field &&
                 FIELD_NAME_EQUAL_FIELD(field, primary_key_field)) {
        // for primary key column
        storage_store_field_column(field, true, i, record_id);
      } else {
        storage_store_field_column(field, false, i, record_id);
      }
      field->move_field_offset(-ptr_diff);
    }
  }

  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_fields_for_prep_update(const uchar* old_data,
                                                      const uchar* new_data,
                                                      grn_id record_id)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_PRINT("info", ("mroonga: stored record ID: %d", record_id));
  my_ptrdiff_t ptr_diff_old = old_data - table->record[0];
  my_ptrdiff_t ptr_diff_new = 0;
  if (new_data) {
    ptr_diff_new = new_data - table->record[0];
  }
  int i;
  int n_columns = table->s->fields;
  for (i = 0; i < n_columns; i++) {
    Field* field = table->field[i];

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif
    if (!bitmap_is_set(table->read_set, MRN_FIELD_FIELD_INDEX(field)) &&
        !bitmap_is_set(table->write_set, MRN_FIELD_FIELD_INDEX(field)) &&
        bitmap_is_set(&multiple_column_key_bitmap,
                      MRN_FIELD_FIELD_INDEX(field))) {
      mrn::DebugColumnAccess debug_column_access(table, &(table->write_set));
      DBUG_PRINT(
        "info",
        ("mroonga: store column %d(%d)", i, MRN_FIELD_FIELD_INDEX(field)));
      grn_obj value;
      GRN_OBJ_INIT(&value, GRN_BULK, 0, grn_obj_get_range(ctx, grn_columns[i]));
      grn_obj_get_value(ctx, grn_columns[i], record_id, &value);
      // old column
      field->move_field_offset(ptr_diff_old);
      storage_store_field(field, GRN_BULK_HEAD(&value), GRN_BULK_VSIZE(&value));
      field->move_field_offset(-ptr_diff_old);
      if (new_data) {
        // new column
        field->move_field_offset(ptr_diff_new);
        storage_store_field(field,
                            GRN_BULK_HEAD(&value),
                            GRN_BULK_VSIZE(&value));
        field->move_field_offset(-ptr_diff_new);
      }
      GRN_OBJ_FIN(ctx, &value);
    }
  }

  DBUG_VOID_RETURN;
}

void ha_mroonga::storage_store_fields_by_index(uchar* buf)
{
  MRN_DBUG_ENTER_METHOD();

  KEY* key_info = &table->key_info[active_index];
  uint n_keys = KEY_N_KEY_PARTS(key_info);
  for (uint i = 0; i < n_keys; ++i) {
    switch (key_info->key_part[i].field->real_type()) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_BLOB:
      // Text values in index may be normalized.
      if (strcmp(MRN_CHARSET_CSNAME(key_info->key_part[i].field->charset()),
                 "binary") != 0) {
        storage_store_fields(table, buf, record_id);
        DBUG_VOID_RETURN;
      }
      break;
    default:
      break;
    }
  }

  uint key_length;
  void* key;
  if (table->s->primary_key == active_index)
    key_length = grn_table_cursor_get_key(ctx, cursor, &key);
  else
    key_length = grn_table_cursor_get_key(ctx, index_table_cursor, &key);

  if (KEY_N_KEY_PARTS(key_info) == 1) {
    my_ptrdiff_t ptr_diff = buf - table->record[0];
    Field* field = key_info->key_part->field;
    mrn::DebugColumnAccess debug_column_access(table, &(table->write_set));
    field->move_field_offset(ptr_diff);
    storage_store_field(field, (const char*)key, key_length);
    field->move_field_offset(-ptr_diff);
  } else {
    uchar enc_buf[MAX_KEY_LENGTH];
    uint enc_len;
    mrn::MultipleColumnKeyCodec codec(ctx, ha_thd(), key_info);
    codec.decode(static_cast<uchar*>(key), key_length, enc_buf, &enc_len);
    key_restore(buf, enc_buf, key_info, enc_len);
  }
  DBUG_VOID_RETURN;
}

int ha_mroonga::storage_encode_key_normalize_min_sort_chars(Field* field,
                                                            uchar* buf,
                                                            uint size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  if (size == 0) {
    DBUG_RETURN(0);
  }
  if (!field->has_charset()) {
    DBUG_RETURN(0);
  }

  uint16 raw_min_sort_char =
    static_cast<uint16>(field->sort_charset()->min_sort_char);
  if (raw_min_sort_char <= UINT_MAX8) {
    uchar min_sort_char = static_cast<uchar>(raw_min_sort_char);
    for (uint i = size - 1; i > 0; --i) {
      if (buf[i] != min_sort_char) {
        break;
      }
      buf[i] = '\0';
    }
  }

  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_fixed_size_string(Field* field,
                                                     const uchar* key,
                                                     uchar* buf,
                                                     uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  memcpy(buf, key, field->field_length);
  *size = field->field_length;
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_variable_size_string(Field* field,
                                                        const uchar* key,
                                                        uchar* buf,
                                                        uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  *size = uint2korr(key);
  memcpy(buf, key + HA_KEY_BLOB_LENGTH, *size);
  storage_encode_key_normalize_min_sort_chars(field, buf, *size);
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_timestamp(Field* field,
                                             const uchar* key,
                                             uchar* buf,
                                             uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  long long int time;
  MYSQL_TIME mysql_time;
#ifdef MRN_MARIADB_P
  if (field->decimals() == 0) {
    my_time_t my_time = sint4korr(key);
    mrn_my_tz_UTC->gmt_sec_to_TIME(&mysql_time, my_time);
    mysql_time.second_part = 0;
  } else {
    Field_timestamp_hires* timestamp_hires_field =
      (Field_timestamp_hires*)field;
    uchar* ptr_backup = MRN_FIELD_FIELD_PTR(field);
    uchar* null_ptr_backup = field->null_ptr;
    TABLE* table_backup = field->table;
    MRN_FIELD_SET_FIELD_PTR(field, (uchar*)key);
    field->null_ptr = (uchar*)(key - 1);
    field->table = table;
    MRN_FIELD_GET_DATE_NO_FUZZY(timestamp_hires_field,
                                &mysql_time,
                                current_thd);
    MRN_FIELD_SET_FIELD_PTR(field, ptr_backup);
    field->null_ptr = null_ptr_backup;
    field->table = table_backup;
  }
#else
  my_time_t my_time = uint4korr(key);
  mrn_my_tz_UTC->gmt_sec_to_TIME(&mysql_time, my_time);
#endif
  mrn::TimeConverter time_converter;
  time = time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  memcpy(buf, &time, 8);
  *size = 8;
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_time(Field* field,
                                        const uchar* key,
                                        uchar* buf,
                                        uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  long long int time;
#ifdef MRN_MARIADB_P
  MYSQL_TIME mysql_time;
  bool truncated = false;
  if (field->decimals() == 0) {
    long long int packed_time = sint3korr(key);
    mysql_time.neg = false;
    if (packed_time < 0) {
      mysql_time.neg = true;
      packed_time = -packed_time;
    }
    mysql_time.year = 0;
    mysql_time.month = 0;
    mysql_time.day = 0;
    mysql_time.hour = (int)(packed_time / 10000);
    long long int minute_part = packed_time - mysql_time.hour * 10000;
    mysql_time.minute = (int)(minute_part / 100);
    mysql_time.second = (int)(minute_part % 100);
    mysql_time.second_part = 0;
    mysql_time.time_type = MYSQL_TIMESTAMP_TIME;
  } else {
    Field_time_hires* time_hires_field = (Field_time_hires*)field;
    uchar* ptr_backup = MRN_FIELD_FIELD_PTR(field);
    uchar* null_ptr_backup = field->null_ptr;
    MRN_FIELD_SET_FIELD_PTR(field, (uchar*)key);
    field->null_ptr = (uchar*)(key - 1);
    MRN_FIELD_GET_DATE_NO_FUZZY(time_hires_field, &mysql_time, current_thd);
    MRN_FIELD_SET_FIELD_PTR(field, ptr_backup);
    field->null_ptr = null_ptr_backup;
  }
  mrn::TimeConverter time_converter;
  time = time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
#else
  int mysql_time = (int)sint3korr(key);
  int sec = mysql_time / 10000 * 60 * 60 + mysql_time / 100 % 100 * 60 +
            mysql_time % 60;
  int usec = 0;
  time = GRN_TIME_PACK(sec, usec);
#endif
  memcpy(buf, &time, 8);
  *size = 8;
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_year(Field* field,
                                        const uchar* key,
                                        uchar* buf,
                                        uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  int year = (int)key[0];

  struct tm datetime;
  memset(&datetime, 0, sizeof(struct tm));
  datetime.tm_year = year;
  datetime.tm_mon = 0;
  datetime.tm_mday = 1;
  int usec = 0;
  mrn::TimeConverter time_converter;
  long long int time =
    time_converter.tm_to_grn_time(&datetime, usec, &truncated);
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  memcpy(buf, &time, 8);
  *size = 8;
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_datetime(Field* field,
                                            const uchar* key,
                                            uchar* buf,
                                            uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  long long int time;
#ifdef MRN_MARIADB_P
  if (field->decimals() > 0) {
    Field_datetime_hires* datetime_hires_field = (Field_datetime_hires*)field;
    MYSQL_TIME mysql_time;
    uchar* ptr_backup = MRN_FIELD_FIELD_PTR(field);
    uchar* null_ptr_backup = field->null_ptr;
    MRN_FIELD_SET_FIELD_PTR(field, (uchar*)key);
    field->null_ptr = (uchar*)(key - 1);
    MRN_FIELD_GET_DATE_NO_FUZZY(datetime_hires_field, &mysql_time, current_thd);
    MRN_FIELD_SET_FIELD_PTR(field, ptr_backup);
    field->null_ptr = null_ptr_backup;
    mrn::TimeConverter time_converter;
    time = time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  } else
#endif
  {
    long long int encoded_datetime = sint8korr(key);
    uint32 part1 = (uint32)(encoded_datetime / 1000000LL);
    uint32 part2 =
      (uint32)(encoded_datetime - (unsigned long long int)part1 * 1000000LL);
    struct tm date;
    memset(&date, 0, sizeof(struct tm));
    date.tm_year = part1 / 10000 - mrn::TimeConverter::TM_YEAR_BASE;
    date.tm_mon = part1 / 100 % 100 - 1;
    date.tm_mday = part1 % 100;
    date.tm_hour = part2 / 10000;
    date.tm_min = part2 / 100 % 100;
    date.tm_sec = part2 % 100;
    int usec = 0;
    mrn::TimeConverter time_converter;
    time = time_converter.tm_to_grn_time(&date, usec, &truncated);
  }
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  memcpy(buf, &time, 8);
  *size = 8;
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_timestamp2(Field* field,
                                              const uchar* key,
                                              uchar* buf,
                                              uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;

  Field_timestampf* timestamp2_field = (Field_timestampf*)field;
  MYSQL_TIME mysql_time;
#ifdef MRN_TIMESTAMP_USE_MY_TIMEVAL
  struct my_timeval my_tm;
  my_timestamp_from_binary(&my_tm, key, timestamp2_field->decimals());
  mrn_my_tz_UTC->gmt_sec_to_TIME(&mysql_time, my_tm);
#else
  struct timeval tm;
  my_timestamp_from_binary(&tm, key, timestamp2_field->decimals());
  mrn_my_tz_UTC->gmt_sec_to_TIME(&mysql_time, (my_time_t)tm.tv_sec);
  mysql_time.second_part = tm.tv_usec;
#endif
  mrn::TimeConverter time_converter;
  long long int grn_time =
    time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  memcpy(buf, &grn_time, 8);
  *size = 8;

  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_datetime2(Field* field,
                                             const uchar* key,
                                             uchar* buf,
                                             uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;

  Field_datetimef* datetime2_field = (Field_datetimef*)field;
  longlong packed_time =
    my_datetime_packed_from_binary(key, datetime2_field->decimals());
  MYSQL_TIME mysql_time;
  TIME_from_longlong_datetime_packed(&mysql_time, packed_time);
  mrn::TimeConverter time_converter;
  long long int grn_time =
    time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  memcpy(buf, &grn_time, 8);
  *size = 8;

  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_time2(Field* field,
                                         const uchar* key,
                                         uchar* buf,
                                         uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;

  Field_timef* time2_field = (Field_timef*)field;
  longlong packed_time =
    my_time_packed_from_binary(key, time2_field->decimals());
  MYSQL_TIME mysql_time;
  TIME_from_longlong_time_packed(&mysql_time, packed_time);
  mrn::TimeConverter time_converter;
  long long int grn_time =
    time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
  if (truncated) {
    if (ha_thd()->is_strict_mode()) {
      error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
    }
    field->set_warning(MRN_SEVERITY_WARNING,
                       MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                       1);
  }
  memcpy(buf, &grn_time, 8);
  *size = 8;

  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_enum(Field* field,
                                        const uchar* key,
                                        uchar* buf,
                                        uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  if (field->pack_length() == 1) {
    uchar value;
    value = key[0];
    *size = 1;
    memcpy(buf, &value, *size);
  } else {
    uint16 value;
    mrn::value_decoder::decode(&value, key);
    *size = 2;
    memcpy(buf, &value, *size);
  }
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key_set(Field* field,
                                       const uchar* key,
                                       uchar* buf,
                                       uint* size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  Field_set unpacker((uchar*)key,
                     field->field_length,
                     (uchar*)(key - 1),
                     field->null_bit,
#ifdef MRN_FIELD_HAVE_AUTO_FLAGS
                     field->auto_flags,
#else
                     field->unireg_check,
#endif
#ifdef MRN_FIELD_SET_USE_LEX_STRING
                     &(field->field_name),
#else
                     field->field_name,
#endif
                     field->pack_length(),
                     MRN_FIELD_ENUM_GET_TYPELIB(static_cast<Field_set*>(field)),
                     static_cast<Field_set*>(field)->charset());
  unpacker.table = table;
  switch (field->pack_length()) {
  case 1: {
    int8 signed_value = (int8)(unpacker.val_int());
    uint8 unsigned_value = *((uint8*)&signed_value);
    *size = 1;
    memcpy(buf, &unsigned_value, *size);
  } break;
  case 2: {
    int16 signed_value = (int16)(unpacker.val_int());
    uint16 unsigned_value = *((uint16*)&signed_value);
    *size = 2;
    memcpy(buf, &unsigned_value, *size);
  } break;
  case 3:
  case 4: {
    int32 signed_value = (int32)(unpacker.val_int());
    uint32 unsigned_value = *((uint32*)&signed_value);
    *size = 4;
    memcpy(buf, &unsigned_value, *size);
  } break;
  case 8:
  default: {
    int64 signed_value = (int64)(unpacker.val_int());
    uint64 unsigned_value = *((uint64*)&signed_value);
    *size = 8;
    memcpy(buf, &unsigned_value, *size);
  } break;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_key(
  Field* field, const uchar* key, uchar* buf, uint* size, mrn_bool* is_null)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  bool truncated = false;
  const uchar* ptr = key;

  error = mrn_change_encoding(ctx, field->charset());
  if (error)
    DBUG_RETURN(error);

  if (field->null_bit) {
    *is_null = ptr[0];
    if (*is_null) {
      *size = 0;
      DBUG_RETURN(error);
    }
    ptr += 1;
  } else {
    *is_null = false;
  }

  switch (field->real_type()) {
  case MYSQL_TYPE_BIT: {
    switch (field->key_length()) {
    case 0:
      *(reinterpret_cast<uint8_t*>(buf)) = 0;
      *size = 1;
      break;
    case 1:
      *(reinterpret_cast<uint8_t*>(buf)) = ptr[0];
      *size = 1;
      break;
    case 2:
      *(reinterpret_cast<uint16_t*>(buf)) = mi_uint2korr(ptr);
      *size = 2;
      break;
    case 3:
      *(reinterpret_cast<uint32_t*>(buf)) = mi_uint3korr(ptr);
      *size = 4;
      break;
    case 4:
      *(reinterpret_cast<uint32_t*>(buf)) = mi_uint4korr(ptr);
      ;
      *size = 4;
      break;
    case 5:
      *(reinterpret_cast<uint64_t*>(buf)) = mi_uint5korr(ptr);
      ;
      *size = 8;
      break;
    case 6:
      *(reinterpret_cast<uint64_t*>(buf)) = mi_uint6korr(ptr);
      ;
      *size = 8;
      break;
    case 7:
      *(reinterpret_cast<uint64_t*>(buf)) = mi_uint7korr(ptr);
      ;
      *size = 8;
      break;
    default:
      *(reinterpret_cast<uint64_t*>(buf)) = mi_uint8korr(ptr);
      ;
      *size = 8;
      break;
    }
    break;
  }
  case MYSQL_TYPE_TINY: {
    memcpy(buf, ptr, 1);
    *size = 1;
    break;
  }
  case MYSQL_TYPE_SHORT: {
    memcpy(buf, ptr, 2);
    *size = 2;
    break;
  }
  case MYSQL_TYPE_INT24: {
    memcpy(buf, ptr, 3);
    buf[3] = 0;
    *size = 4;
    break;
  }
  case MYSQL_TYPE_LONG: {
    memcpy(buf, ptr, 4);
    *size = 4;
    break;
  }
  case MYSQL_TYPE_TIMESTAMP:
    error = storage_encode_key_timestamp(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_LONGLONG: {
    memcpy(buf, ptr, 8);
    *size = 8;
    break;
  }
  case MYSQL_TYPE_FLOAT: {
    float float_value;
    double double_value;
    mrn::value_decoder::decode(&float_value, ptr);
    double_value = float_value;
    memcpy(buf, &double_value, 8);
    *size = 8;
    break;
  }
  case MYSQL_TYPE_DOUBLE: {
    double val;
    mrn::value_decoder::decode(&val, ptr);
    memcpy(buf, &val, 8);
    *size = 8;
    break;
  }
  case MYSQL_TYPE_TIME:
    error = storage_encode_key_time(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_YEAR:
    error = storage_encode_key_year(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_DATETIME:
    error = storage_encode_key_datetime(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_NEWDATE: {
    uint32 encoded_date = uint3korr(ptr);
    struct tm date;
    memset(&date, 0, sizeof(struct tm));
    date.tm_year = encoded_date / (16 * 32) - mrn::TimeConverter::TM_YEAR_BASE;
    date.tm_mon = encoded_date / 32 % 16 - 1;
    date.tm_mday = encoded_date % 32;
    int usec = 0;
    mrn::TimeConverter time_converter;
    long long int time = time_converter.tm_to_grn_time(&date, usec, &truncated);
    if (truncated) {
      if (ha_thd()->is_strict_mode()) {
        error = MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd());
      }
      field->set_warning(MRN_SEVERITY_WARNING,
                         MRN_ERROR_CODE_DATA_TRUNCATE(ha_thd()),
                         1);
    }
    memcpy(buf, &time, 8);
    *size = 8;
    break;
  }
  case MYSQL_TYPE_TIMESTAMP2:
    error = storage_encode_key_timestamp2(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_DATETIME2:
    error = storage_encode_key_datetime2(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_TIME2:
    error = storage_encode_key_time2(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_STRING:
    error = storage_encode_key_fixed_size_string(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_BLOB:
    error = storage_encode_key_variable_size_string(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_ENUM:
    error = storage_encode_key_enum(field, ptr, buf, size);
    break;
  case MYSQL_TYPE_SET:
    error = storage_encode_key_set(field, ptr, buf, size);
    break;
  default:
    error = HA_ERR_UNSUPPORTED;
    break;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_multiple_column_key(KEY* key_info,
                                                   const uchar* key,
                                                   uint key_length,
                                                   uchar* buffer,
                                                   uint* encoded_length)
{
  MRN_DBUG_ENTER_METHOD();
  mrn::MultipleColumnKeyCodec codec(ctx, ha_thd(), key_info);
  int error = codec.encode(key, key_length, buffer, encoded_length);
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_multiple_column_key_range(KEY* key_info,
                                                         const uchar* start,
                                                         uint start_size,
                                                         const uchar* end,
                                                         uint end_size,
                                                         uchar* min_buffer,
                                                         uint* min_encoded_size,
                                                         uchar* max_buffer,
                                                         uint* max_encoded_size)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  mrn::MultipleColumnKeyCodec codec(ctx, ha_thd(), key_info);
  uint encoded_key_size = codec.size();
  if (start) {
    memset(min_buffer, 0, encoded_key_size);
    error = codec.encode(start, start_size, min_buffer, min_encoded_size);
    // TODO: handle error?
    *min_encoded_size = encoded_key_size;
  }
  if (end) {
    memset(max_buffer, 0xff, encoded_key_size);
    error = codec.encode(end, end_size, max_buffer, max_encoded_size);
    // TODO: handle error?
    *max_encoded_size = encoded_key_size;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::storage_encode_multiple_column_key_range(KEY* key_info,
                                                         const key_range* start,
                                                         const key_range* end,
                                                         uchar* min_buffer,
                                                         uint* min_encoded_size,
                                                         uchar* max_buffer,
                                                         uint* max_encoded_size)
{
  MRN_DBUG_ENTER_METHOD();

  const uchar* start_data = NULL;
  uint start_size = 0;
  const uchar* end_data = NULL;
  uint end_size = 0;
  if (start) {
    start_data = start->key;
    start_size = start->length;
  }
  if (end) {
    end_data = end->key;
    end_size = end->length;
  }

  int error = storage_encode_multiple_column_key_range(key_info,
                                                       start_data,
                                                       start_size,
                                                       end_data,
                                                       end_size,
                                                       min_buffer,
                                                       min_encoded_size,
                                                       max_buffer,
                                                       max_encoded_size);

  DBUG_RETURN(error);
}

int ha_mroonga::generic_reset()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  ctx->rc = GRN_SUCCESS;
  ctx->errbuf[0] = '\0';

  if (thd_sql_command(ha_thd()) != SQLCOM_SELECT) {
    DBUG_RETURN(error);
  }

  TABLE_LIST* table_list = table->pos_in_table_list;
  if (!table_list) {
    DBUG_RETURN(error);
  }

  mrn_query_block* query_block = MRN_TABLE_LIST_QUERY_BLOCK(table_list);
  if (!query_block) {
    DBUG_RETURN(error);
  }

  if (!query_block->ftfunc_list) {
    DBUG_RETURN(error);
  }
  if (!query_block->ftfunc_list->elements) {
    DBUG_RETURN(error);
  }
  List_iterator<Item_func_match> iterator(*(query_block->ftfunc_list));
  Item_func_match* item;
  while ((item = iterator++)) {
    if (item->ft_handler) {
      mrn_generic_ft_clear(reinterpret_cast<st_mrn_ft_info*>(item->ft_handler));
    }
  }

  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_reset()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_reset();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  if (alter_key_info_buffer) {
    my_free(alter_key_info_buffer);
    alter_key_info_buffer = NULL;
  }
  wrap_ft_init_count = 0;
  int generic_error = generic_reset();
  if (error == 0) {
    error = generic_error;
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_reset()
{
  MRN_DBUG_ENTER_METHOD();
  int error;
  error = generic_reset();
  DBUG_RETURN(error);
}

int ha_mroonga::reset()
{
  int error = 0;
  THD* thd = ha_thd();
  MRN_DBUG_ENTER_METHOD();
  DBUG_PRINT("info", ("mroonga: this=%p", this));
  clear_empty_value_records();
  clear_search_result();
  clear_search_result_geo();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_reset();
  } else {
#endif
    error = storage_reset();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  ignoring_no_key_columns = false;
  inserting_with_update = false;
  ignoring_duplicated_key = false;
  fulltext_searching = false;
  replacing_ = false;
  written_by_row_based_binlog = 0;
  mrn_lock_type = F_UNLCK;
  mrn::SlotData* slot_data = mrn_get_slot_data(thd, false);
  if (slot_data) {
    slot_data->clear();
  }
  current_ft_item = NULL;
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
handler* ha_mroonga::wrapper_clone(const char* name, MEM_ROOT* mem_root)
{
  handler* cloned_handler;
  MRN_DBUG_ENTER_METHOD();
  if (!(cloned_handler = mrn_get_new_handler(table->s,
                                             table->s->m_part_info != NULL,
                                             mem_root,
                                             table->s->db_type())))
    DBUG_RETURN(NULL);
  ((ha_mroonga*)cloned_handler)->is_clone = true;
  ((ha_mroonga*)cloned_handler)->parent_for_clone = this;
  ((ha_mroonga*)cloned_handler)->mem_root_for_clone = mem_root;
#  ifdef MRN_HANDLER_OPEN_HAVE_TABLE_DEFINITION
  int error = cloned_handler->ha_open(table,
                                      table->s->normalized_path.str,
                                      table->db_stat,
                                      HA_OPEN_IGNORE_IF_LOCKED,
                                      NULL);
#  else
  int error = cloned_handler->ha_open(table,
                                      table->s->normalized_path.str,
                                      table->db_stat,
                                      HA_OPEN_IGNORE_IF_LOCKED);
#  endif
  if (error != 0) {
    mrn_destroy(cloned_handler);
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(cloned_handler);
}
#endif

handler* ha_mroonga::storage_clone(const char* name, MEM_ROOT* mem_root)
{
  MRN_DBUG_ENTER_METHOD();
  handler* cloned_handler;
  cloned_handler = handler::clone(name, mem_root);
  DBUG_RETURN(cloned_handler);
}

handler* ha_mroonga::clone(const char* name, MEM_ROOT* mem_root)
{
  MRN_DBUG_ENTER_METHOD();
  handler* cloned_handler;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    cloned_handler = wrapper_clone(name, mem_root);
  } else {
#endif
    cloned_handler = storage_clone(name, mem_root);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(cloned_handler);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_print_error(int error, myf flag)
{
  MRN_DBUG_ENTER_METHOD();
  if (wrap_handler) {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    wrap_handler->print_error(error, flag);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  } else {
    wrap_handler_for_create->print_error(error, flag);
  }
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_print_error(int error, myf flag)
{
  MRN_DBUG_ENTER_METHOD();
  handler::print_error(error, flag);
  DBUG_VOID_RETURN;
}

void ha_mroonga::print_error(int error, myf flag)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (wrap_handler || wrap_handler_for_create) {
    wrapper_print_error(error, flag);
  } else {
#endif
    storage_print_error(error, flag);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_get_error_message(int error, String* buffer)
{
  bool errored;
  MRN_DBUG_ENTER_METHOD();
  if (wrap_handler) {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    errored = wrap_handler->get_error_message(error, buffer);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  } else {
    errored = wrap_handler_for_create->get_error_message(error, buffer);
  }
  DBUG_RETURN(errored);
}
#endif

bool ha_mroonga::storage_get_error_message(int error, String* buffer)
{
  MRN_DBUG_ENTER_METHOD();
  bool errored = false;
  // latest error message
  buffer->copy(ctx->errbuf, (uint)strlen(ctx->errbuf), system_charset_info);
  DBUG_RETURN(errored);
}

bool ha_mroonga::get_error_message(int error, String* buffer)
{
  MRN_DBUG_ENTER_METHOD();
  bool errored;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (wrap_handler || wrap_handler_for_create) {
    errored = wrapper_get_error_message(error, buffer);
  } else {
#endif
    errored = storage_get_error_message(error, buffer);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(errored);
}

#ifdef MRN_HANDLER_HAVE_GET_FOREIGN_DUP_KEY
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_get_foreign_dup_key(char* child_table_name,
                                             uint child_table_name_len,
                                             char* child_key_name,
                                             uint child_key_name_len)
{
  bool success;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  success = wrap_handler->get_foreign_dup_key(child_table_name,
                                              child_table_name_len,
                                              child_key_name,
                                              child_key_name_len);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(success);
}
#  endif

bool ha_mroonga::storage_get_foreign_dup_key(char* child_table_name,
                                             uint child_table_name_len,
                                             char* child_key_name,
                                             uint child_key_name_len)
{
  MRN_DBUG_ENTER_METHOD();
  // TODO: Should implement?
  bool success = handler::get_foreign_dup_key(child_table_name,
                                              child_table_name_len,
                                              child_key_name,
                                              child_key_name_len);
  DBUG_RETURN(success);
}

bool ha_mroonga::get_foreign_dup_key(char* child_table_name,
                                     uint child_table_name_len,
                                     char* child_key_name,
                                     uint child_key_name_len)
{
  MRN_DBUG_ENTER_METHOD();
  bool success;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share && share->wrapper_mode) {
    success = wrapper_get_foreign_dup_key(child_table_name,
                                          child_table_name_len,
                                          child_key_name,
                                          child_key_name_len);
  } else {
#  endif
    success = storage_get_foreign_dup_key(child_table_name,
                                          child_table_name_len,
                                          child_key_name,
                                          child_key_name_len);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(success);
}
#endif

#ifdef MRN_HANDLER_HAVE_GET_MEMORY_BUFFER_SIZE
#  ifdef MRN_ENABLE_WRAPPER_MODE
longlong ha_mroonga::wrapper_get_memory_buffer_size() const
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  longlong size = wrap_handler->get_memory_buffer_size();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(size);
}
#  endif

longlong ha_mroonga::storage_get_memory_buffer_size() const
{
  MRN_DBUG_ENTER_METHOD();
  longlong size = handler::get_memory_buffer_size();
  DBUG_RETURN(size);
}

longlong ha_mroonga::get_memory_buffer_size() const
{
  MRN_DBUG_ENTER_METHOD();
  longlong size;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share && share->wrapper_mode) {
    size = wrapper_get_memory_buffer_size();
  } else {
#  endif
    size = storage_get_memory_buffer_size();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(size);
}
#endif

#ifdef MRN_HANDLER_HAVE_TABLE_CACHE_TYPE
#  ifdef MRN_ENABLE_WRAPPER_MODE
uint8 ha_mroonga::wrapper_table_cache_type()
{
  uint8 res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->table_cache_type();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

uint8 ha_mroonga::storage_table_cache_type()
{
  MRN_DBUG_ENTER_METHOD();
  uint8 type = handler::table_cache_type();
  DBUG_RETURN(type);
}

uint8 ha_mroonga::table_cache_type()
{
  MRN_DBUG_ENTER_METHOD();
  uint8 type;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    type = wrapper_table_cache_type();
  } else {
#  endif
    type = storage_table_cache_type();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(type);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
ha_rows ha_mroonga::wrapper_multi_range_read_info_const(uint keyno,
                                                        RANGE_SEQ_IF* seq,
                                                        void* seq_init_param,
                                                        uint n_ranges,
                                                        uint* bufsz,
                                                        uint* flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                                        ha_rows limit,
#  endif
                                                        Cost_estimate* cost)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows;
  KEY* key_info = &(table->key_info[keyno]);
  if (mrn_is_geo_key(key_info)) {
    rows = handler::multi_range_read_info_const(keyno,
                                                seq,
                                                seq_init_param,
                                                n_ranges,
                                                bufsz,
                                                flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                                limit,
#  endif
                                                cost);
    DBUG_RETURN(rows);
  }
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
  rows = wrap_handler->multi_range_read_info_const(keyno,
                                                   seq,
                                                   seq_init_param,
                                                   n_ranges,
                                                   bufsz,
                                                   flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                                   limit,
#  endif
                                                   cost);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(rows);
}

ha_rows ha_mroonga::storage_multi_range_read_info_const(uint keyno,
                                                        RANGE_SEQ_IF* seq,
                                                        void* seq_init_param,
                                                        uint n_ranges,
                                                        uint* bufsz,
                                                        uint* flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                                        ha_rows limit,
#  endif
                                                        Cost_estimate* cost)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows = handler::multi_range_read_info_const(keyno,
                                                      seq,
                                                      seq_init_param,
                                                      n_ranges,
                                                      bufsz,
                                                      flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                                      limit,
#  endif
                                                      cost);
  DBUG_RETURN(rows);
}

ha_rows ha_mroonga::multi_range_read_info_const(uint keyno,
                                                RANGE_SEQ_IF* seq,
                                                void* seq_init_param,
                                                uint n_ranges,
                                                uint* bufsz,
                                                uint* flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                                ha_rows limit,
#  endif
                                                Cost_estimate* cost)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows;
  if (share->wrapper_mode) {
    rows = wrapper_multi_range_read_info_const(keyno,
                                               seq,
                                               seq_init_param,
                                               n_ranges,
                                               bufsz,
                                               flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                               limit,
#  endif
                                               cost);
  } else {
    rows = storage_multi_range_read_info_const(keyno,
                                               seq,
                                               seq_init_param,
                                               n_ranges,
                                               bufsz,
                                               flags,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_CONST_LIMIT
                                               limit,
#  endif
                                               cost);
  }
  DBUG_RETURN(rows);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
ha_rows ha_mroonga::wrapper_multi_range_read_info(uint keyno,
                                                  uint n_ranges,
                                                  uint keys,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                                  uint key_parts,
#  endif
                                                  uint* bufsz,
                                                  uint* flags,
                                                  Cost_estimate* cost)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows;
  KEY* key_info = &(table->key_info[keyno]);
  if (mrn_is_geo_key(key_info)) {
    rows = handler::multi_range_read_info(keyno,
                                          n_ranges,
                                          keys,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                          key_parts,
#  endif
                                          bufsz,
                                          flags,
                                          cost);
    DBUG_RETURN(rows);
  }
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
  rows = wrap_handler->multi_range_read_info(keyno,
                                             n_ranges,
                                             keys,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                             key_parts,
#  endif
                                             bufsz,
                                             flags,
                                             cost);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(rows);
}
#endif

ha_rows ha_mroonga::storage_multi_range_read_info(uint keyno,
                                                  uint n_ranges,
                                                  uint keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                                  uint key_parts,
#endif
                                                  uint* bufsz,
                                                  uint* flags,
                                                  Cost_estimate* cost)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows = handler::multi_range_read_info(keyno,
                                                n_ranges,
                                                keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                                key_parts,
#endif
                                                bufsz,
                                                flags,
                                                cost);
  DBUG_RETURN(rows);
}

ha_rows ha_mroonga::multi_range_read_info(uint keyno,
                                          uint n_ranges,
                                          uint keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                          uint key_parts,
#endif
                                          uint* bufsz,
                                          uint* flags,
                                          Cost_estimate* cost)
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    rows = wrapper_multi_range_read_info(keyno,
                                         n_ranges,
                                         keys,
#  ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                         key_parts,
#  endif
                                         bufsz,
                                         flags,
                                         cost);
  } else {
#endif
    rows = storage_multi_range_read_info(keyno,
                                         n_ranges,
                                         keys,
#ifdef MRN_HANDLER_HAVE_MULTI_RANGE_READ_INFO_KEY_PARTS
                                         key_parts,
#endif
                                         bufsz,
                                         flags,
                                         cost);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(rows);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_multi_range_read_init(RANGE_SEQ_IF* seq,
                                              void* seq_init_param,
                                              uint n_ranges,
                                              uint mode,
                                              HANDLER_BUFFER* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  KEY* key_info = &(table->key_info[active_index]);
  if (mrn_is_geo_key(key_info)) {
    error =
      handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode, buf);
    DBUG_RETURN(error);
  }
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
  error = wrap_handler->multi_range_read_init(seq,
                                              seq_init_param,
                                              n_ranges,
                                              mode,
                                              buf);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_multi_range_read_init(RANGE_SEQ_IF* seq,
                                              void* seq_init_param,
                                              uint n_ranges,
                                              uint mode,
                                              HANDLER_BUFFER* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error =
    handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode, buf);
  DBUG_RETURN(error);
}

int ha_mroonga::multi_range_read_init(RANGE_SEQ_IF* seq,
                                      void* seq_init_param,
                                      uint n_ranges,
                                      uint mode,
                                      HANDLER_BUFFER* buf)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error =
      wrapper_multi_range_read_init(seq, seq_init_param, n_ranges, mode, buf);
  } else {
#endif
    error =
      storage_multi_range_read_init(seq, seq_init_param, n_ranges, mode, buf);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_multi_range_read_next(range_id_t* range_info)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  KEY* key_info = &(table->key_info[active_index]);
  if (mrn_is_geo_key(key_info)) {
    error = handler::multi_range_read_next(range_info);
    DBUG_RETURN(error);
  }
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  if (fulltext_searching)
    set_pk_bitmap();
#  ifdef MRN_HANDLER_HAVE_HA_MULTI_RANGE_READ_NEXT
  error = wrap_handler->ha_multi_range_read_next(range_info);
#  else
  error = wrap_handler->multi_range_read_next(range_info);
#  endif
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_multi_range_read_next(range_id_t* range_info)
{
  MRN_DBUG_ENTER_METHOD();
  int error = handler::multi_range_read_next(range_info);
  DBUG_RETURN(error);
}

int ha_mroonga::multi_range_read_next(range_id_t* range_info)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_multi_range_read_next(range_info);
  } else {
#endif
    error = storage_multi_range_read_next(range_info);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
#  ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
void ha_mroonga::wrapper_start_bulk_insert(ha_rows rows, uint flags)
#  else
void ha_mroonga::wrapper_start_bulk_insert(ha_rows rows)
#  endif
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
  wrap_handler->ha_start_bulk_insert(rows, flags);
#  else
  wrap_handler->ha_start_bulk_insert(rows);
#  endif
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

#ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
void ha_mroonga::storage_start_bulk_insert(ha_rows rows, uint flags)
#else
void ha_mroonga::storage_start_bulk_insert(ha_rows rows)
#endif
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_VOID_RETURN;
}

#ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
void ha_mroonga::start_bulk_insert(ha_rows rows, uint flags)
#else
void ha_mroonga::start_bulk_insert(ha_rows rows)
#endif
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
#  ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
    wrapper_start_bulk_insert(rows, flags);
#  else
    wrapper_start_bulk_insert(rows);
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_START_BULK_INSERT_HAS_FLAGS
    storage_start_bulk_insert(rows, flags);
#else
  storage_start_bulk_insert(rows);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_end_bulk_insert()
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_end_bulk_insert();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_end_bulk_insert()
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(0);
}

int ha_mroonga::end_bulk_insert()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_end_bulk_insert();
  } else {
#endif
    error = storage_end_bulk_insert();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_HANDLER_HAVE_UPGRADE_TABLE
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_upgrade_table(THD* thd,
                                       const char* db_name,
                                       const char* table_name,
                                       dd::Table* dd_table)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  bool errored =
    wrap_handler->ha_upgrade_table(thd, db_name, table_name, dd_table, table);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(errored);
}
#  endif

bool ha_mroonga::storage_upgrade_table(THD* thd,
                                       const char* db_name,
                                       const char* table_name,
                                       dd::Table* dd_table)
{
  MRN_DBUG_ENTER_METHOD();
  bool errored = false;
  DBUG_RETURN(errored);
}

bool ha_mroonga::upgrade_table(THD* thd,
                               const char* db_name,
                               const char* table_name,
                               dd::Table* dd_table)
{
  MRN_DBUG_ENTER_METHOD();

  bool errored;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share && share->wrapper_mode) {
    errored = wrapper_upgrade_table(thd, db_name, table_name, dd_table);
  } else {
#  endif
    errored = storage_upgrade_table(thd, db_name, table_name, dd_table);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif

  DBUG_RETURN(errored);
}
#endif

int ha_mroonga::generic_delete_all_rows(grn_obj* target_grn_table,
                                        const char* function_name)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  if (is_dry_write()) {
    DBUG_PRINT("info",
               ("mroonga: dry write: %s::%s", MRN_CLASS_NAME, function_name));
    DBUG_RETURN(error);
  }

  grn_table_cursor* cursor;
  cursor =
    grn_table_cursor_open(ctx, target_grn_table, NULL, 0, NULL, 0, 0, -1, 0);
  if (cursor) {
    while (grn_table_cursor_next(ctx, cursor) != GRN_ID_NIL) {
      grn_table_cursor_delete(ctx, cursor);
    }
    grn_table_cursor_close(ctx, cursor);
  } else {
    error = ER_ERROR_ON_WRITE;
    my_message(error, ctx->errbuf, MYF(0));
  }
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_delete_all_rows()
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_delete_all_rows();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);

  if (error) {
    DBUG_RETURN(error);
  }

  if (!wrapper_have_target_index()) {
    DBUG_RETURN(error);
  }

  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    KEY* key_info = &(table->key_info[i]);

    if (!(wrapper_is_target_index(key_info))) {
      continue;
    }

    if (!grn_index_tables[i]) {
      /* disable keys */
      continue;
    }

    error = generic_delete_all_rows(grn_index_tables[i], __FUNCTION__);
    if (error) {
      break;
    }
  }

  int grn_table_error;
  grn_table_error = generic_delete_all_rows(grn_table, __FUNCTION__);
  if (!error) {
    error = grn_table_error;
  }

  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_delete_all_rows()
{
  MRN_DBUG_ENTER_METHOD();
  int error = generic_delete_all_rows(grn_table, __FUNCTION__);
  if (!error) {
    uint n_keys = table->s->keys;
    for (uint i = 0; i < n_keys; i++) {
      if (i == table->s->primary_key) {
        continue;
      }

      KEY* key_info = &(table->key_info[i]);
      if (!(key_info->flags & HA_NOSAME)) {
        continue;
      }

      grn_obj* index_table = grn_index_tables[i];
      if (!index_table) {
        continue;
      }

      error = generic_delete_all_rows(index_table, __FUNCTION__);
      if (error) {
        break;
      }
    }
  }
  DBUG_RETURN(error);
}

int ha_mroonga::delete_all_rows()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_delete_all_rows();
  } else {
#endif
    error = storage_delete_all_rows();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_truncate(
#  ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
  dd::Table* table_def
#  endif
)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
  error = wrap_handler->ha_truncate(table_def);
#  else
  error = wrap_handler->ha_truncate();
#  endif
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);

  if (!error && wrapper_have_target_index()) {
    error = wrapper_truncate_index();
  }

  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_truncate_index()
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  if (is_dry_write()) {
    DBUG_PRINT("info",
               ("mroonga: dry write: %s::%s", MRN_CLASS_NAME, __FUNCTION__));
    DBUG_RETURN(error);
  }

  grn_rc rc;
  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    KEY* key_info = &(table->key_info[i]);

    if (!(wrapper_is_target_index(key_info))) {
      continue;
    }

    if (!grn_index_tables[i]) {
      /* disable keys */
      continue;
    }

    rc = grn_table_truncate(ctx, grn_index_tables[i]);
    if (rc) {
      error = ER_ERROR_ON_WRITE;
      my_message(error, ctx->errbuf, MYF(0));
      goto err;
    }
  }
err:
  rc = grn_table_truncate(ctx, grn_table);
  if (rc) {
    error = ER_ERROR_ON_WRITE;
    my_message(error, ctx->errbuf, MYF(0));
  }

  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_truncate(
#ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
  dd::Table* table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  if (is_dry_write()) {
    DBUG_PRINT("info", ("mroonga: dry write: ha_mroonga::%s", __FUNCTION__));
    DBUG_RETURN(error);
  }

  grn_rc rc;
  rc = grn_table_truncate(ctx, grn_table);
  if (rc) {
    my_message(ER_ERROR_ON_WRITE, ctx->errbuf, MYF(0));
    DBUG_RETURN(ER_ERROR_ON_WRITE);
  }
  error = storage_truncate_index();

  if (!error && thd_sql_command(ha_thd()) == SQLCOM_TRUNCATE) {
    MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
    mrn::Lock lock(&long_term_share->auto_inc_mutex);
    long_term_share->auto_inc_value = 0;
    DBUG_PRINT(
      "info",
      ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
    long_term_share->auto_inc_inited = false;
  }

  DBUG_RETURN(error);
}

int ha_mroonga::storage_truncate_index()
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;

  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  grn_rc rc;
  uint i;
  uint n_keys = table->s->keys;
  for (i = 0; i < n_keys; i++) {
    if (i == table->s->primary_key) {
      continue;
    }

    KEY* key_info = &(table->key_info[i]);

    if (!(key_info->flags & HA_NOSAME) &&
        (KEY_N_KEY_PARTS(key_info) == 1 ||
         key_info->algorithm == HA_KEY_ALG_FULLTEXT)) {
      continue;
    }

    if (!grn_index_tables[i]) {
      /* disable keys */
      continue;
    }

    rc = grn_table_truncate(ctx, grn_index_tables[i]);
    if (rc) {
      error = ER_ERROR_ON_WRITE;
      my_message(error, ctx->errbuf, MYF(0));
      goto err;
    }
  }
err:
  DBUG_RETURN(error);
}

int ha_mroonga::truncate(
#ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
  dd::Table* table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
#  ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
    error = wrapper_truncate(table_def);
#  else
    error = wrapper_truncate();
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_TRUNCATE_HAVE_TABLE_DEFINITION
    error = storage_truncate(table_def);
#else
  error = storage_truncate();
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  if (!error) {
    operations_->clear(table->s->table_name.str, table->s->table_name.length);
  }
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
mrn_io_and_cpu_cost ha_mroonga::wrapper_scan_time()
{
  mrn_io_and_cpu_cost res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->scan_time();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

mrn_io_and_cpu_cost ha_mroonga::storage_scan_time()
{
  MRN_DBUG_ENTER_METHOD();
  mrn_io_and_cpu_cost time = handler::scan_time();
  DBUG_RETURN(time);
}

mrn_io_and_cpu_cost ha_mroonga::scan_time()
{
  MRN_DBUG_ENTER_METHOD();
  mrn_io_and_cpu_cost time;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    time = wrapper_scan_time();
  } else {
#endif
    time = storage_scan_time();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(time);
}

#if defined(MRN_HANDLER_HAVE_KEYREAD_TIME) && defined(MRN_ENABLE_WRAPPER_MODE)
IO_AND_CPU_COST ha_mroonga::wrapper_rnd_pos_time(ha_rows rows)
{
  MRN_DBUG_ENTER_METHOD();
  IO_AND_CPU_COST cost;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  cost = wrap_handler->rnd_pos_time(rows);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(cost);
}

IO_AND_CPU_COST ha_mroonga::storage_rnd_pos_time(ha_rows rows)
{
  MRN_DBUG_ENTER_METHOD();
  auto cost = handler::rnd_pos_time(rows);
  DBUG_RETURN(cost);
}

IO_AND_CPU_COST ha_mroonga::rnd_pos_time(ha_rows rows)
{
  MRN_DBUG_ENTER_METHOD();
  IO_AND_CPU_COST cost;
  if (share->wrapper_mode) {
    cost = wrapper_rnd_pos_time(rows);
  } else {
    cost = storage_rnd_pos_time(rows);
  }
  DBUG_RETURN(cost);
}

IO_AND_CPU_COST ha_mroonga::wrapper_keyread_time(uint index,
                                                 ulong ranges,
                                                 ha_rows rows,
                                                 ulonglong blocks)
{
  IO_AND_CPU_COST cost;
  MRN_DBUG_ENTER_METHOD();
  if (index < MAX_KEY) {
    KEY* key_info = &(table->key_info[index]);
    if (mrn_is_geo_key(key_info)) {
      cost = handler::keyread_time(index, ranges, rows, blocks);
      DBUG_RETURN(cost);
    }
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    cost = wrap_handler->keyread_time(share->wrap_key_nr[index],
                                      ranges,
                                      rows,
                                      blocks);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    cost = wrap_handler->keyread_time(index, ranges, rows, blocks);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(cost);
}

IO_AND_CPU_COST ha_mroonga::storage_keyread_time(uint index,
                                                 ulong ranges,
                                                 ha_rows rows,
                                                 ulonglong blocks)
{
  MRN_DBUG_ENTER_METHOD();
  auto cost = handler::keyread_time(index, ranges, rows, blocks);
  DBUG_RETURN(cost);
}

IO_AND_CPU_COST ha_mroonga::keyread_time(uint index,
                                         ulong ranges,
                                         ha_rows rows,
                                         ulonglong blocks)
{
  MRN_DBUG_ENTER_METHOD();
  IO_AND_CPU_COST cost;
  if (share->wrapper_mode) {
    cost = wrapper_keyread_time(index, ranges, rows, blocks);
  } else {
    cost = storage_keyread_time(index, ranges, rows, blocks);
  }
  DBUG_RETURN(cost);
}
#endif

#if defined(MRN_HANDLER_HAVE_READ_TIME) && defined(MRN_ENABLE_WRAPPER_MODE)
double ha_mroonga::wrapper_read_time(uint index, uint ranges, ha_rows rows)
{
  double cost;
  MRN_DBUG_ENTER_METHOD();
  if (index < MAX_KEY) {
    KEY* key_info = &(table->key_info[index]);
    if (mrn_is_geo_key(key_info)) {
      cost = handler::read_time(index, ranges, rows);
      DBUG_RETURN(cost);
    }
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    cost = wrap_handler->read_time(share->wrap_key_nr[index], ranges, rows);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  } else {
    MRN_SET_WRAP_SHARE_KEY(share, table->s);
    MRN_SET_WRAP_TABLE_KEY(this, table);
    cost = wrap_handler->read_time(index, ranges, rows);
    MRN_SET_BASE_SHARE_KEY(share, table->s);
    MRN_SET_BASE_TABLE_KEY(this, table);
  }
  DBUG_RETURN(cost);
}

double ha_mroonga::storage_read_time(uint index, uint ranges, ha_rows rows)
{
  MRN_DBUG_ENTER_METHOD();
  auto cost = handler::read_time(index, ranges, rows);
  DBUG_RETURN(cost);
}

double ha_mroonga::read_time(uint index, uint ranges, ha_rows rows)
{
  MRN_DBUG_ENTER_METHOD();
  double cost;
  if (share->wrapper_mode) {
    cost = wrapper_read_time(index, ranges, rows);
  } else {
    cost = storage_read_time(index, ranges, rows);
  }
  DBUG_RETURN(cost);
}
#endif

#ifdef MRN_HANDLER_HAVE_KEYS_TO_USE_FOR_SCANNING
#  ifdef MRN_ENABLE_WRAPPER_MODE
const key_map* ha_mroonga::wrapper_keys_to_use_for_scanning()
{
  const key_map* res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->keys_to_use_for_scanning();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

const key_map* ha_mroonga::storage_keys_to_use_for_scanning()
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(&key_map_full);
}

const key_map* ha_mroonga::keys_to_use_for_scanning()
{
  MRN_DBUG_ENTER_METHOD();
  const key_map* key_map;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    key_map = wrapper_keys_to_use_for_scanning();
  } else {
#  endif
    key_map = storage_keys_to_use_for_scanning();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(key_map);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
ha_rows ha_mroonga::wrapper_estimate_rows_upper_bound()
{
  ha_rows res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->estimate_rows_upper_bound();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

ha_rows ha_mroonga::storage_estimate_rows_upper_bound()
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows = handler::estimate_rows_upper_bound();
  DBUG_RETURN(rows);
}

ha_rows ha_mroonga::estimate_rows_upper_bound()
{
  MRN_DBUG_ENTER_METHOD();
  ha_rows rows;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    rows = wrapper_estimate_rows_upper_bound();
  } else {
#endif
    rows = storage_estimate_rows_upper_bound();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(rows);
}

#ifdef MRN_HANDLER_HAVE_GET_REAL_ROW_TYPE
#  ifdef MRN_ENABLE_WRAPPER_MODE
enum row_type
ha_mroonga::wrapper_get_real_row_type(const HA_CREATE_INFO* create_info) const
{
  MRN_DBUG_ENTER_METHOD();
  enum row_type type = wrap_handler_for_create->get_real_row_type(create_info);
  DBUG_RETURN(type);
}
#  endif

enum row_type
ha_mroonga::storage_get_real_row_type(const HA_CREATE_INFO* create_info) const
{
  MRN_DBUG_ENTER_METHOD();
  enum row_type type = handler::get_real_row_type(create_info);
  DBUG_RETURN(type);
}

enum row_type
ha_mroonga::get_real_row_type(const HA_CREATE_INFO* create_info) const
{
  MRN_DBUG_ENTER_METHOD();
  enum row_type type;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (wrap_handler_for_create) {
    type = wrapper_get_real_row_type(create_info);
  } else {
#  endif
    type = storage_get_real_row_type(create_info);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(type);
}
#endif

#ifdef MRN_HANDLER_HAVE_GET_DEFAULT_INDEX_ALGORITHM
#  ifdef MRN_ENABLE_WRAPPER_MODE
enum ha_key_alg ha_mroonga::wrapper_get_default_index_algorithm() const
{
  MRN_DBUG_ENTER_METHOD();
  enum ha_key_alg algorithm =
    wrap_handler_for_create->get_default_index_algorithm();
  DBUG_RETURN(algorithm);
}
#  endif

enum ha_key_alg ha_mroonga::storage_get_default_index_algorithm() const
{
  MRN_DBUG_ENTER_METHOD();
  enum ha_key_alg algorithm = handler::get_default_index_algorithm();
  DBUG_RETURN(algorithm);
}

enum ha_key_alg ha_mroonga::get_default_index_algorithm() const
{
  MRN_DBUG_ENTER_METHOD();
  enum ha_key_alg algorithm;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (wrap_handler_for_create) {
    algorithm = wrapper_get_default_index_algorithm();
  } else {
#  endif
    algorithm = storage_get_default_index_algorithm();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(algorithm);
}
#endif

#ifdef MRN_HANDLER_HAVE_IS_INDEX_ALGORITHM_SUPPORTED
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_is_index_algorithm_supported(
  enum ha_key_alg algorithm) const
{
  MRN_DBUG_ENTER_METHOD();
  bool supported;
  if (algorithm == HA_KEY_ALG_FULLTEXT) {
    supported = true;
  } else {
    supported =
      wrap_handler_for_create->is_index_algorithm_supported(algorithm);
  }
  DBUG_RETURN(supported);
}
#  endif

bool ha_mroonga::storage_is_index_algorithm_supported(
  enum ha_key_alg algorithm) const
{
  MRN_DBUG_ENTER_METHOD();
  bool supported = (algorithm != HA_KEY_ALG_RTREE);
  DBUG_RETURN(supported);
}

bool ha_mroonga::is_index_algorithm_supported(enum ha_key_alg algorithm) const
{
  MRN_DBUG_ENTER_METHOD();
  bool supported;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (wrap_handler_for_create) {
    supported = wrapper_is_index_algorithm_supported(algorithm);
  } else {
#  endif
    supported = storage_is_index_algorithm_supported(algorithm);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(supported);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_update_create_info(HA_CREATE_INFO* create_info)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->update_create_info(create_info);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_update_create_info(HA_CREATE_INFO* create_info)
{
  MRN_DBUG_ENTER_METHOD();
  handler::update_create_info(create_info);
  if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) {
    MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
    if (!long_term_share->auto_inc_inited) {
      storage_info(HA_STATUS_AUTO);
    }
    create_info->auto_increment_value = long_term_share->auto_inc_value;
    DBUG_PRINT(
      "info",
      ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::update_create_info(HA_CREATE_INFO* create_info)
{
  MRN_DBUG_ENTER_METHOD();
  if (!create_info->connect_string.str) {
    create_info->connect_string.str = table->s->connect_string.str;
    create_info->connect_string.length = table->s->connect_string.length;
  }
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_update_create_info(create_info);
  } else {
#endif
    storage_update_create_info(create_info);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  mrn::SlotData* slot_data = mrn_get_slot_data(ha_thd(), true);
  if (slot_data) {
    slot_data->alter_create_info = create_info;
    if (slot_data->alter_connect_string) {
      my_free(slot_data->alter_connect_string);
      slot_data->alter_connect_string = NULL;
    }
    if (create_info->connect_string.str) {
      slot_data->alter_connect_string =
        mrn_my_strndup(create_info->connect_string.str,
                       create_info->connect_string.length,
                       MYF(MY_WME));
    }
    if (slot_data->alter_comment) {
      my_free(slot_data->alter_comment);
      slot_data->alter_comment = NULL;
    }
    if (create_info->comment.str) {
      slot_data->alter_comment = mrn_my_strndup(create_info->comment.str,
                                                create_info->comment.length,
                                                MYF(MY_WME));
    }
    if (share && share->disable_keys) {
      slot_data->disable_keys_create_info = create_info;
    }
  }
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_rename_table(const char* from,
                                     const char* to,
                                     MRN_SHARE* tmp_share,
                                     const char* from_table_name,
                                     const char* to_table_name
#  ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
                                     ,
                                     const dd::Table* from_table_def,
                                     dd::Table* to_table_def
#  endif
)
{
  int error = 0;
  handler* hnd;
  MRN_DBUG_ENTER_METHOD();

  hnd =
    mrn_get_new_handler(tmp_share->table_share,
                        from_table_def->partition_type() != dd::Table::PT_NONE,
                        current_thd->mem_root,
                        tmp_share->hton);
  if (!hnd) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

#  ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
  error = hnd->ha_rename_table(from, to, from_table_def, to_table_def);
#  else
  error = hnd->ha_rename_table(from, to);
#  endif

  if (error != 0) {
    mrn_destroy(hnd);
    DBUG_RETURN(error);
  }

  error =
    wrapper_rename_index(from, to, tmp_share, from_table_name, to_table_name);

  mrn_destroy(hnd);
  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_rename_index(const char* from,
                                     const char* to,
                                     MRN_SHARE* tmp_share,
                                     const char* from_table_name,
                                     const char* to_table_name)
{
  int error;
  grn_rc rc;
  MRN_DBUG_ENTER_METHOD();
  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  error = ensure_database_open(from);
  if (error)
    DBUG_RETURN(error);

  TABLE_SHARE* tmp_table_share = tmp_share->table_share;

  uint i;
  for (i = 0; i < tmp_table_share->keys; i++) {
    KEY* key = &(tmp_table_share->key_info[i]);
    mrn::IndexTableName from_index_table_name(from_table_name, KEY_NAME(key));
    mrn::IndexTableName to_index_table_name(to_table_name, KEY_NAME(key));
    grn_obj* index_table;
    index_table = grn_ctx_get(ctx,
                              from_index_table_name.c_str(),
                              from_index_table_name.length());
    if (!index_table) {
      index_table = grn_ctx_get(ctx,
                                from_index_table_name.old_c_str(),
                                from_index_table_name.old_length());
    }
    if (index_table) {
      rc = grn_table_rename(ctx,
                            index_table,
                            to_index_table_name.c_str(),
                            to_index_table_name.length());
      if (rc != GRN_SUCCESS) {
        error = ER_CANT_OPEN_FILE;
        my_message(error, ctx->errbuf, MYF(0));
        DBUG_RETURN(error);
      }
    }
  }

  grn_obj* table = grn_ctx_get(ctx, from_table_name, strlen(from_table_name));
  if (ctx->rc != GRN_SUCCESS) {
    error = ER_CANT_OPEN_FILE;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  rc = grn_table_rename(ctx, table, to_table_name, strlen(to_table_name));
  if (rc != GRN_SUCCESS) {
    error = ER_CANT_OPEN_FILE;
    my_message(error, ctx->errbuf, MYF(0));
    DBUG_RETURN(error);
  }
  DBUG_RETURN(0);
}
#endif

int ha_mroonga::storage_rename_table(const char* from,
                                     const char* to,
                                     MRN_SHARE* tmp_share,
                                     const char* from_table_name,
                                     const char* to_table_name
#ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
                                     ,
                                     const dd::Table* from_table_def,
                                     dd::Table* to_table_def
#endif
)
{
  int error;
  grn_rc rc;
  TABLE_SHARE* tmp_table_share = tmp_share->table_share;
  MRN_LONG_TERM_SHARE *from_long_term_share = tmp_share->long_term_share,
                      *to_long_term_share;
  MRN_DBUG_ENTER_METHOD();
  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(error);

  error = ensure_database_open(from);
  if (error)
    DBUG_RETURN(error);

  if (!(to_long_term_share = mrn_get_long_term_share(to, strlen(to), &error)))
    DBUG_RETURN(error);
  to_long_term_share->auto_inc_value = from_long_term_share->auto_inc_value;
  DBUG_PRINT(
    "info",
    ("mroonga: to_auto_inc_value=%llu", to_long_term_share->auto_inc_value));
  to_long_term_share->auto_inc_inited = from_long_term_share->auto_inc_inited;

  uint i;
  for (i = 0; i < tmp_table_share->keys; i++) {
    KEY* key = &(tmp_table_share->key_info[i]);
    mrn::IndexTableName from_index_table_name(from_table_name, KEY_NAME(key));
    mrn::IndexTableName to_index_table_name(to_table_name, KEY_NAME(key));
    grn_obj* index_table;
    index_table = grn_ctx_get(ctx,
                              from_index_table_name.c_str(),
                              from_index_table_name.length());
    if (!index_table) {
      index_table = grn_ctx_get(ctx,
                                from_index_table_name.old_c_str(),
                                from_index_table_name.old_length());
    }
    if (index_table) {
      rc = grn_table_rename(ctx,
                            index_table,
                            to_index_table_name.c_str(),
                            to_index_table_name.length());
      if (rc != GRN_SUCCESS) {
        error = ER_CANT_OPEN_FILE;
        my_message(error, ctx->errbuf, MYF(0));
        goto error_end;
      }
    }
  }
  error = storage_rename_foreign_key(tmp_share, from_table_name, to_table_name);
  if (error) {
    goto error_end;
  }
  {
    grn_obj* table_obj =
      grn_ctx_get(ctx, from_table_name, strlen(from_table_name));
    if (ctx->rc != GRN_SUCCESS) {
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      goto error_end;
    }
    rc = grn_table_rename(ctx, table_obj, to_table_name, strlen(to_table_name));
    if (rc != GRN_SUCCESS) {
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      goto error_end;
    }
  }
  DBUG_RETURN(0);

error_end:
  mrn_free_long_term_share(to_long_term_share);
  DBUG_RETURN(error);
}

int ha_mroonga::storage_rename_foreign_key(MRN_SHARE* tmp_share,
                                           const char* from_table_name,
                                           const char* to_table_name)
{
  int error;
  uint i;
  grn_obj *column, *ref_column;
  grn_rc rc;
  TABLE_SHARE* tmp_table_share = tmp_share->table_share;
  uint n_columns = tmp_table_share->fields;
  MRN_DBUG_ENTER_METHOD();
  for (i = 0; i < n_columns; ++i) {
    Field* field = tmp_table_share->field[i];

    if (!is_foreign_key_field(from_table_name, field->field_name)) {
      continue;
    }

    grn_obj* grn_from_table = grn_ctx_get(ctx, from_table_name, -1);
    mrn::ColumnName column_name(FIELD_NAME(field));
    column = grn_obj_column(ctx,
                            grn_from_table,
                            column_name.c_str(),
                            column_name.length());
    if (!column) {
      continue;
    }
    grn_id ref_table_id = grn_obj_get_range(ctx, column);
    grn_obj* ref_table = grn_ctx_at(ctx, ref_table_id);
    mrn::IndexColumnName from_index_column_name(from_table_name,
                                                column_name.c_str());
    ref_column = grn_obj_column(ctx,
                                ref_table,
                                from_index_column_name.c_str(),
                                from_index_column_name.length());
    if (!ref_column) {
      continue;
    }
    mrn::IndexColumnName to_index_column_name(to_table_name,
                                              column_name.c_str());
    rc = grn_column_rename(ctx,
                           ref_column,
                           to_index_column_name.c_str(),
                           to_index_column_name.length());
    if (rc != GRN_SUCCESS) {
      error = ER_CANT_OPEN_FILE;
      my_message(error, ctx->errbuf, MYF(0));
      DBUG_RETURN(error);
    }
  }
  DBUG_RETURN(0);
}

int ha_mroonga::rename_table(const char* from,
                             const char* to
#ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
                             ,
                             const dd::Table* from_table_def,
                             dd::Table* to_table_def
#endif
)
{
  int error = 0;
  TABLE tmp_table;
  MRN_SHARE* tmp_share;
  MRN_DBUG_ENTER_METHOD();
  mrn::PathMapper to_mapper(to);
  mrn::PathMapper from_mapper(from);
  if (strcmp(from_mapper.db_name(), to_mapper.db_name()))
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);

  MRN_DECLARE_TABLE_LIST(table_list,
                         from_mapper.db_name(),
                         strlen(from_mapper.db_name()),
                         from_mapper.mysql_table_name(),
                         strlen(from_mapper.mysql_table_name()),
                         from_mapper.mysql_table_name(),
                         TL_WRITE);
  mrn_open_mutex_lock(NULL);
#ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
  TABLE_SHARE* tmp_table_share =
    mrn_create_tmp_table_share(&table_list, from, from_table_def, &error);
#else
  TABLE_SHARE* tmp_table_share =
    mrn_create_tmp_table_share(&table_list, from, &error);
#endif
  mrn_open_mutex_unlock(NULL);
  if (!tmp_table_share) {
    DBUG_RETURN(error);
  }
  tmp_table.s = tmp_table_share;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  tmp_table.part_info = NULL;
#endif
  if (!(tmp_share = mrn_get_share(from, &tmp_table, &error))) {
    mrn_open_mutex_lock(NULL);
    mrn_free_tmp_table_share(tmp_table_share);
    mrn_open_mutex_unlock(NULL);
    DBUG_RETURN(error);
  }

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (tmp_share->wrapper_mode) {
#  ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
    error = wrapper_rename_table(from,
                                 to,
                                 tmp_share,
                                 from_mapper.table_name(),
                                 to_mapper.table_name(),
                                 from_table_def,
                                 to_table_def);
#  else
    error = wrapper_rename_table(from,
                                 to,
                                 tmp_share,
                                 from_mapper.table_name(),
                                 to_mapper.table_name());
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_RENAME_TABLE_HAVE_TABLE_DEFINITION
    error = storage_rename_table(from,
                                 to,
                                 tmp_share,
                                 from_mapper.table_name(),
                                 to_mapper.table_name(),
                                 from_table_def,
                                 to_table_def);
#else
  error = storage_rename_table(from,
                               to,
                               tmp_share,
                               from_mapper.table_name(),
                               to_mapper.table_name());
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
  if (!error && to_mapper.table_name()[0] == '#') {
    error = add_wrap_hton(to, tmp_share->hton);
  } else if (error && from_mapper.table_name()[0] == '#') {
    add_wrap_hton(from, tmp_share->hton);
  }
#endif
  if (!error) {
    mrn_free_long_term_share(tmp_share->long_term_share);
    tmp_share->long_term_share = NULL;
  }
  mrn_free_share(tmp_share);
  mrn_open_mutex_lock(NULL);
  mrn_free_tmp_table_share(tmp_table_share);
  mrn_open_mutex_unlock(NULL);

  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_is_crashed() const
{
  bool res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->is_crashed();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

bool ha_mroonga::storage_is_crashed() const
{
  MRN_DBUG_ENTER_METHOD();
  mrn::DatabaseRepairer repairer(ctx, ha_thd());
  mrn::PathMapper mapper(table_share->normalized_path.str);
  bool crashed = repairer.is_crashed(mapper.db_path());
  DBUG_RETURN(crashed);
}

bool ha_mroonga::is_crashed() const
{
  MRN_DBUG_ENTER_METHOD();
  bool crashed;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    crashed = wrapper_is_crashed();
  } else {
#endif
    crashed = storage_is_crashed();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(crashed);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_auto_repair(int error) const
{
  bool repaired;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_AUTO_REPAIR_HAVE_ERROR
  repaired = wrap_handler->auto_repair(error);
#  else
  repaired = wrap_handler->auto_repair();
#  endif
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(repaired);
}
#endif

bool ha_mroonga::storage_auto_repair(int error) const
{
  MRN_DBUG_ENTER_METHOD();
  bool repaired;
#ifdef MRN_HANDLER_AUTO_REPAIR_HAVE_ERROR
  repaired = handler::auto_repair(error);
#else
  repaired = handler::auto_repair();
#endif
  DBUG_RETURN(repaired);
}

bool ha_mroonga::auto_repair(int error) const
{
  MRN_DBUG_ENTER_METHOD();
  bool repaired;
#ifdef MRN_ENABLE_WRAPPER_MODE
  // TODO: We should consider about creating share for error =
  // ER_CANT_OPEN_FILE. The following code just ignores the error.
  if (share && share->wrapper_mode) {
    repaired = wrapper_auto_repair(error);
  } else {
#endif
    repaired = storage_auto_repair(error);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(repaired);
}

bool ha_mroonga::auto_repair() const
{
  MRN_DBUG_ENTER_METHOD();
  bool repaired = auto_repair(HA_ERR_CRASHED_ON_USAGE);
  DBUG_RETURN(repaired);
}

int ha_mroonga::generic_disable_index(int i, KEY* key_info)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;
  const char* lexicon_name = NULL;
#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  if (key_info->option_struct) {
    lexicon_name = key_info->option_struct->lexicon;
  }
#endif
  mrn::ParametersParser parser(key_info->comment.str, key_info->comment.length);
  if (!lexicon_name) {
    lexicon_name = parser.lexicon();
  }
  if (lexicon_name) {
    char index_column_name[GRN_TABLE_MAX_KEY_SIZE];
    snprintf(index_column_name,
             GRN_TABLE_MAX_KEY_SIZE - 1,
             "%s." KEY_NAME_FORMAT,
             lexicon_name,
             KEY_NAME_FORMAT_VALUE(key_info));
    grn_obj* index_column = grn_ctx_get(ctx, index_column_name, -1);
    if (index_column) {
      grn_obj_remove(ctx, index_column);
    }
  } else {
    mrn::PathMapper mapper(share->table_name);
    mrn::IndexTableName index_table_name(mapper.table_name(),
                                         KEY_NAME(key_info));
    grn_obj* index_table =
      grn_ctx_get(ctx, index_table_name.c_str(), index_table_name.length());
    if (!index_table) {
      index_table = grn_ctx_get(ctx,
                                index_table_name.old_c_str(),
                                index_table_name.old_length());
    }
    if (index_table) {
      grn_obj_remove(ctx, index_table);
    }
  }
  if (ctx->rc == GRN_SUCCESS) {
    grn_index_tables[i] = NULL;
    grn_index_columns[i] = NULL;
  } else {
    // TODO: Implement ctx->rc to error converter and use it.
    error = ER_ERROR_ON_WRITE;
    my_message(error, ctx->errbuf, MYF(0));
  }

  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_disable_indexes_mroonga(
  MRN_HANDLER_DISABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  if (MRN_HANDLER_DISABLE_INDEXES_NEED_TO_EXECUTE) {
    uint i;
    for (i = 0; i < table_share->keys; i++) {
      if (i == table->s->primary_key) {
        continue;
      }
      if (share->wrap_key_nr[i] < MAX_KEY) {
        continue;
      }
      if (!grn_index_tables[i]) {
        DBUG_PRINT("info", ("mroonga: keys are disabled already %u", i));
        DBUG_RETURN(0);
      }
    }
    for (i = 0; i < table_share->keys; i++) {
      KEY* key_info = &(table_share->key_info[i]);
      if (key_info->algorithm != HA_KEY_ALG_FULLTEXT &&
          !mrn_is_geo_key(key_info)) {
        continue;
      }

      int sub_error = generic_disable_index(i, key_info);
      if (error != 0 && sub_error != 0) {
        error = sub_error;
      }
    }
  } else {
    error = HA_ERR_WRONG_COMMAND;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_disable_indexes(MRN_HANDLER_DISABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error =
    wrap_handler->ha_disable_indexes(MRN_HANDLER_DISABLE_INDEXES_FORWARD_ARGS);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  if (error == HA_ERR_WRONG_COMMAND) {
    error = 0;
  }
  if (!error) {
    error =
      wrapper_disable_indexes_mroonga(MRN_HANDLER_DISABLE_INDEXES_FORWARD_ARGS);
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_disable_indexes(MRN_HANDLER_DISABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  if (MRN_HANDLER_DISABLE_INDEXES_NEED_TO_EXECUTE) {
    uint i;
    for (i = 0; i < table_share->keys; i++) {
      if (i == table->s->primary_key) {
        continue;
      }
      if (!grn_index_tables[i]) {
        DBUG_PRINT("info", ("mroonga: keys are disabled already %u", i));
        DBUG_RETURN(0);
      }
    }
    for (i = 0; i < table_share->keys; i++) {
      if (i == table->s->primary_key) {
        continue;
      }
      KEY* key_info = table_share->key_info;
      if (!MRN_HANDLER_DISABLE_INDEXES_IS_TARGET_KEY(key_info, i)) {
        continue;
      }

      int sub_error = generic_disable_index(i, &(key_info[i]));
      if (error != 0 && sub_error != 0) {
        error = sub_error;
      }
    }
  } else {
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  DBUG_RETURN(error);
}

int ha_mroonga::disable_indexes(MRN_HANDLER_DISABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_disable_indexes(MRN_HANDLER_DISABLE_INDEXES_FORWARD_ARGS);
  } else {
#endif
    error = storage_disable_indexes(MRN_HANDLER_DISABLE_INDEXES_FORWARD_ARGS);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_enable_indexes_mroonga(
  MRN_HANDLER_ENABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  if (MRN_HANDLER_ENABLE_INDEXES_NEED_TO_EXECUTE) {
    uint i, j;
    for (i = 0; i < table_share->keys; i++) {
      if (i == table->s->primary_key) {
        continue;
      }
      if (share->wrap_key_nr[i] < MAX_KEY) {
        continue;
      }
      if (!grn_index_columns[i]) {
        break;
      }
    }
    if (i == table_share->keys) {
      DBUG_PRINT("info", ("mroonga: keys are enabled already"));
      DBUG_RETURN(0);
    }
    KEY* p_key_info = &table->key_info[table_share->primary_key];
    KEY* key_info = table_share->key_info;
    uint n_keys = table_share->keys;
    MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*, index_tables, n_keys);
    MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*, index_columns, n_keys);
    bitmap_clear_all(table->read_set);
    mrn_set_bitmap_by_key(table->read_set, p_key_info);
    mrn::PathMapper mapper(share->table_name);
    for (i = 0, j = 0; i < n_keys; i++) {
      if (key_info[i].algorithm != HA_KEY_ALG_FULLTEXT &&
          !mrn_is_geo_key(&key_info[i])) {
        j++;
        continue;
      }

      index_tables[i] = NULL;
      index_columns[i] = NULL;
      if (!grn_index_columns[i]) {
        if (key_info[i].algorithm == HA_KEY_ALG_FULLTEXT &&
            (error = wrapper_create_index_fulltext(mapper.table_name(),
                                                   i,
                                                   &key_info[i],
                                                   index_tables,
                                                   index_columns))) {
          break;
        } else if (mrn_is_geo_key(&key_info[i]) &&
                   (error = wrapper_create_index_geo(mapper.table_name(),
                                                     i,
                                                     &key_info[i],
                                                     index_tables,
                                                     index_columns))) {
          break;
        }
        grn_index_columns[i] = index_columns[i];
      }
      mrn_set_bitmap_by_key(table->read_set, &key_info[i]);
    }
    if (!error && i > j) {
      error =
        wrapper_fill_indexes(ha_thd(), table->key_info, index_columns, n_keys);
    }
    bitmap_set_all(table->read_set);
    MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
    MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_columns);
  } else {
    error = HA_ERR_WRONG_COMMAND;
  }
  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_enable_indexes(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();

  int mroonga_error =
    wrapper_enable_indexes_mroonga(MRN_HANDLER_ENABLE_INDEXES_FORWARD_ARGS);

  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error =
    wrap_handler->ha_enable_indexes(MRN_HANDLER_ENABLE_INDEXES_FORWARD_ARGS);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  if (error == HA_ERR_WRONG_COMMAND) {
    error = mroonga_error;
  }
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_enable_indexes(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  uint n_keys = table_share->keys;
  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*, index_tables, n_keys);
  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*, index_columns, n_keys);
  bool have_multiple_column_index = false;
  bool skip_unique_key = MRN_HANDLER_ENABLE_INDEXES_SKIP_UNIQUE_KEY;
  MRN_DBUG_ENTER_METHOD();
  if (MRN_HANDLER_ENABLE_INDEXES_NEED_TO_EXECUTE) {
    uint i;
    for (i = 0; i < n_keys; i++) {
      if (i == table->s->primary_key) {
        continue;
      }
      if (!grn_index_columns[i]) {
        break;
      }
    }
    if (i == table_share->keys) {
      DBUG_PRINT("info", ("mroonga: keys are enabled already"));
      MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
      MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_columns);
      DBUG_RETURN(0);
    }
    KEY* key_info = table->key_info;
    bitmap_clear_all(table->read_set);
    mrn::PathMapper mapper(share->table_name);
    for (i = 0; i < n_keys; i++) {
      if (i == table->s->primary_key) {
        continue;
      }
      if (!MRN_HANDLER_ENABLE_INDEXES_IS_TARGET_KEY(skip_unique_key,
                                                    key_info,
                                                    i)) {
        continue;
      }

      index_tables[i] = NULL;
      if (!grn_index_columns[i]) {
        if ((error = storage_create_index(table,
                                          mapper.table_name(),
                                          grn_table,
                                          &key_info[i],
                                          index_tables,
                                          index_columns,
                                          i))) {
          break;
        }
        if (KEY_N_KEY_PARTS(&(key_info[i])) != 1 &&
            key_info[i].algorithm != HA_KEY_ALG_FULLTEXT) {
          mrn_set_bitmap_by_key(table->read_set, &key_info[i]);
          have_multiple_column_index = true;
        }
        grn_index_tables[i] = index_tables[i];
        grn_index_columns[i] = index_columns[i];
      } else {
        index_columns[i] = NULL;
      }
    }
    if (!error && have_multiple_column_index) {
      error = storage_add_index_multiple_columns(table,
                                                 key_info,
                                                 n_keys,
                                                 index_tables,
                                                 index_columns,
                                                 skip_unique_key);
    }
    bitmap_set_all(table->read_set);
  } else {
    MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
    MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_columns);
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_columns);
  DBUG_RETURN(error);
}

int ha_mroonga::enable_indexes(MRN_HANDLER_ENABLE_INDEXES_PARAMETERS)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  share->disable_keys = false;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_enable_indexes(MRN_HANDLER_ENABLE_INDEXES_FORWARD_ARGS);
  } else {
#endif
    error = storage_enable_indexes(MRN_HANDLER_ENABLE_INDEXES_FORWARD_ARGS);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_check(THD* thd, HA_CHECK_OPT* check_opt)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_check(thd, check_opt);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_check(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  mrn::DatabaseRepairer repairer(ctx, thd);
  mrn::PathMapper mapper(table_share->normalized_path.str);
  if (repairer.is_corrupt(mapper.db_path())) {
    DBUG_RETURN(HA_ADMIN_CORRUPT);
  } else {
    DBUG_RETURN(HA_ADMIN_OK);
  }
}

int ha_mroonga::check(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_check(thd, check_opt);
  } else {
#endif
    error = storage_check(thd, check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_fill_indexes(THD* thd,
                                     KEY* key_info,
                                     grn_obj** index_columns,
                                     uint n_keys)
{
  int error = 0;
  KEY* p_key_info = &table->key_info[table_share->primary_key];
  KEY* tmp_key_info;
  int wrapper_lock_type_backup = wrap_handler->get_lock_type();
  MRN_DBUG_ENTER_METHOD();
  DBUG_PRINT("info", ("mroonga: n_keys=%u", n_keys));

  grn_bool need_lock = true;
  if (mrn_lock_type != F_UNLCK) {
    need_lock = false;
  }
  if (wrapper_lock_type_backup != F_UNLCK) {
    need_lock = false;
  }
  if (need_lock) {
    error = wrapper_external_lock(thd, F_WRLCK);
  }
  if (!error) {
    if (!(error = wrapper_start_stmt(thd, thr_lock_data.type)) &&
        !(error = wrapper_rnd_init(true))) {
      grn_obj key;
      GRN_TEXT_INIT(&key, 0);
      grn_bulk_space(ctx, &key, p_key_info->key_length);
      while (!(error = wrapper_rnd_next(table->record[0]))) {
        key_copy((uchar*)(GRN_TEXT_VALUE(&key)),
                 table->record[0],
                 p_key_info,
                 p_key_info->key_length);
        int added;
        grn_id record_id;
        mrn_change_encoding(ctx, NULL);
        record_id = grn_table_add(ctx,
                                  grn_table,
                                  GRN_TEXT_VALUE(&key),
                                  p_key_info->key_length,
                                  &added);
        if (record_id == GRN_ID_NIL) {
          char error_message[MRN_MESSAGE_BUFFER_SIZE];
          snprintf(error_message,
                   MRN_MESSAGE_BUFFER_SIZE,
                   "failed to add a new record into groonga: key=<%.*s>",
                   (int)p_key_info->key_length,
                   GRN_TEXT_VALUE(&key));
          error = ER_ERROR_ON_WRITE;
          my_message(error, error_message, MYF(0));
        }
        if (error)
          break;

        uint k;
        for (k = 0; k < n_keys; k++) {
          tmp_key_info = &key_info[k];
          if (tmp_key_info->algorithm != HA_KEY_ALG_FULLTEXT &&
              !mrn_is_geo_key(tmp_key_info)) {
            continue;
          }
          if (!index_columns[k]) {
            continue;
          }
          DBUG_PRINT("info", ("mroonga: key_num=%u", k));

          uint l;
          for (l = 0; l < KEY_N_KEY_PARTS(tmp_key_info); l++) {
            Field* field = tmp_key_info->key_part[l].field;

            if (field->is_null())
              continue;
            error = mrn_change_encoding(ctx, field->charset());
            if (error)
              break;

            error = generic_store_bulk(field, &new_value_buffer);
            if (error) {
              my_message(error,
                         "mroonga: wrapper: "
                         "failed to get new value for updating index.",
                         MYF(0));
              break;
            }

            grn_obj* index_column = index_columns[k];
            grn_rc rc;
            rc = grn_column_index_update(ctx,
                                         index_column,
                                         record_id,
                                         l + 1,
                                         NULL,
                                         &new_value_buffer);
            if (rc) {
              error = ER_ERROR_ON_WRITE;
              my_message(error, ctx->errbuf, MYF(0));
              break;
            }
          }
          if (error)
            break;
        }
        if (error)
          break;
      }
      grn_obj_unlink(ctx, &key);
      if (error != HA_ERR_END_OF_FILE)
        wrapper_rnd_end();
      else
        error = wrapper_rnd_end();
    }
    if (need_lock) {
      wrapper_external_lock(thd, F_UNLCK);
    }
  }
  DBUG_RETURN(error);
}

int ha_mroonga::wrapper_recreate_indexes(THD* thd)
{
  int error;
  uint i, n_keys = table_share->keys;
  KEY* p_key_info = &table->key_info[table_share->primary_key];
  KEY* key_info = table->key_info;
  MRN_DBUG_ENTER_METHOD();
  mrn::PathMapper mapper(table_share->normalized_path.str);
  bitmap_clear_all(table->read_set);
  clear_indexes();
  remove_grn_obj_force(mapper.table_name());
  grn_table = NULL;
  mrn_set_bitmap_by_key(table->read_set, p_key_info);
  for (i = 0; i < n_keys; i++) {
    if (key_info[i].algorithm != HA_KEY_ALG_FULLTEXT &&
        !mrn_is_geo_key(&key_info[i])) {
      continue;
    }
    mrn::IndexTableName index_table_name(mapper.table_name(),
                                         KEY_NAME(&(table_share->key_info[i])));
    char index_column_full_name[MRN_MAX_PATH_SIZE];
    snprintf(index_column_full_name,
             MRN_MAX_PATH_SIZE,
             "%s.%s",
             index_table_name.c_str(),
             INDEX_COLUMN_NAME);
    remove_grn_obj_force(index_column_full_name);
    remove_grn_obj_force(index_table_name.c_str());

    char index_column_full_old_name[MRN_MAX_PATH_SIZE];
    snprintf(index_column_full_old_name,
             MRN_MAX_PATH_SIZE,
             "%s.%s",
             index_table_name.old_c_str(),
             INDEX_COLUMN_NAME);
    remove_grn_obj_force(index_column_full_old_name);
    remove_grn_obj_force(index_table_name.old_c_str());

    mrn_set_bitmap_by_key(table->read_set, &key_info[i]);
  }
  HA_CREATE_INFO info;
#  ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  info.option_struct = table_share->option_struct;
#  endif
  error =
    wrapper_create_index(table_share->normalized_path.str, table, &info, share);
  if (error)
    DBUG_RETURN(error);
  error = wrapper_open_indexes(table_share->normalized_path.str);
  if (error)
    DBUG_RETURN(error);
  error = wrapper_fill_indexes(thd, key_info, grn_index_columns, n_keys);
  bitmap_set_all(table->read_set);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_recreate_indexes(THD* thd)
{
  MRN_DBUG_ENTER_METHOD();

  if (share->disable_keys)
    DBUG_RETURN(HA_ADMIN_OK);

  clear_indexes();

  int n_columns = table->s->fields;
  for (int i = 0; i < n_columns; i++) {
    grn_obj* column = grn_columns[i];

    if (!column)
      continue;

    int n_hooks = grn_obj_get_nhooks(ctx, column, GRN_HOOK_SET);
    for (int j = 0; j < n_hooks; j++) {
      grn_obj_delete_hook(ctx, column, GRN_HOOK_SET, j);
    }
  }

  uint n_keys = table_share->keys;
  mrn::PathMapper mapper(table_share->normalized_path.str);
  for (uint i = 0; i < n_keys; i++) {
    KEY* key_info = &(table_share->key_info[i]);

#ifdef MRN_SUPPORT_CUSTOM_OPTIONS
    if (key_info->option_struct && key_info->option_struct->lexicon) {
      continue;
    }
#endif

    mrn::ParametersParser parser(key_info->comment.str,
                                 key_info->comment.length);
    if (parser.lexicon())
      continue;

    if (i == table_share->primary_key)
      continue;

    mrn::IndexTableName index_table_name(mapper.table_name(),
                                         KEY_NAME(&(table_share->key_info[i])));
    char index_column_full_name[MRN_MAX_PATH_SIZE];
    snprintf(index_column_full_name,
             MRN_MAX_PATH_SIZE,
             "%s.%s",
             index_table_name.c_str(),
             INDEX_COLUMN_NAME);
    remove_grn_obj_force(index_column_full_name);
    remove_grn_obj_force(index_table_name.c_str());

    char index_column_full_old_name[MRN_MAX_PATH_SIZE];
    snprintf(index_column_full_old_name,
             MRN_MAX_PATH_SIZE,
             "%s.%s",
             index_table_name.old_c_str(),
             INDEX_COLUMN_NAME);
    remove_grn_obj_force(index_column_full_old_name);
    remove_grn_obj_force(index_table_name.old_c_str());
  }

  int error;
  error = storage_create_indexes(table, mapper.table_name(), grn_table, share);
  if (error)
    DBUG_RETURN(HA_ADMIN_FAILED);

  error = storage_open_indexes(table_share->normalized_path.str);
  if (error)
    DBUG_RETURN(HA_ADMIN_FAILED);

  DBUG_RETURN(HA_ADMIN_OK);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_repair(THD* thd, HA_CHECK_OPT* check_opt)
{
  int error;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_repair(thd, check_opt);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  if (error && error != HA_ADMIN_NOT_IMPLEMENTED)
    DBUG_RETURN(error);
  error = wrapper_recreate_indexes(thd);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_repair(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  int error = storage_recreate_indexes(thd);
  DBUG_RETURN(error);
}

int ha_mroonga::repair(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
  share->disable_keys = false;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_repair(thd, check_opt);
  } else {
#endif
    error = storage_repair(thd, check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_check_and_repair(THD* thd)
{
  bool is_error_or_not_supported;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  is_error_or_not_supported = wrap_handler->ha_check_and_repair(thd);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(is_error_or_not_supported);
}
#endif

bool ha_mroonga::storage_check_and_repair(THD* thd)
{
  MRN_DBUG_ENTER_METHOD();
  bool is_error = false;
  mrn::DatabaseRepairer repairer(ctx, thd);
  mrn::PathMapper mapper(table_share->normalized_path.str);
  is_error = !repairer.repair(mapper.db_path());
  DBUG_RETURN(is_error);
}

bool ha_mroonga::check_and_repair(THD* thd)
{
  MRN_DBUG_ENTER_METHOD();
  bool is_error_or_not_supported;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    is_error_or_not_supported = wrapper_check_and_repair(thd);
  } else {
#endif
    is_error_or_not_supported = storage_check_and_repair(thd);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(is_error_or_not_supported);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_analyze(THD* thd, HA_CHECK_OPT* check_opt)
{
  int error = 0;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  error = wrap_handler->ha_analyze(thd, check_opt);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_analyze(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
}

int ha_mroonga::analyze(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_analyze(thd, check_opt);
  } else {
#endif
    error = storage_analyze(thd, check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(HA_ADMIN_TRY_ALTER);
}
#endif

int ha_mroonga::storage_optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
}

int ha_mroonga::optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  int error = 0;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_optimize(thd, check_opt);
  } else {
#endif
    error = storage_optimize(thd, check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_is_fatal_error(int error_num
#  ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                                        ,
                                        uint flags
#  endif
)
{
  bool res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
  res = wrap_handler->is_fatal_error(error_num, flags);
#  else
  res = wrap_handler->is_fatal_error(error_num);
#  endif
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

bool ha_mroonga::storage_is_fatal_error(int error_num
#ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                                        ,
                                        uint flags
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
  bool is_fatal_error = handler::is_fatal_error(error_num, flags);
#else
  bool is_fatal_error = handler::is_fatal_error(error_num);
#endif
  DBUG_RETURN(is_fatal_error);
}

bool ha_mroonga::is_fatal_error(int error_num
#ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                                ,
                                uint flags
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  bool is_fatal_error;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    is_fatal_error = wrapper_is_fatal_error(error_num
#  ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                                            ,
                                            flags
#  endif
    );
  } else {
#endif
    is_fatal_error = storage_is_fatal_error(error_num
#ifdef MRN_HANDLER_IS_FATAL_ERROR_HAVE_FLAGS
                                            ,
                                            flags
#endif
    );
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(is_fatal_error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_check_if_incompatible_data(HA_CREATE_INFO* create_info,
                                                    uint table_changes)
{
  bool res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->check_if_incompatible_data(create_info, table_changes);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

bool ha_mroonga::storage_check_if_incompatible_data(HA_CREATE_INFO* create_info,
                                                    uint table_changes)
{
  MRN_DBUG_ENTER_METHOD();
  uint n = table_share->fields;
  for (uint i = 0; i < n; i++) {
    Field* field = table->field[i];
    if (MRN_FIELD_ALL_FLAGS(field) & FIELD_IS_RENAMED) {
      DBUG_RETURN(COMPATIBLE_DATA_NO);
    }
  }
  DBUG_RETURN(COMPATIBLE_DATA_YES);
}

bool ha_mroonga::check_if_incompatible_data(HA_CREATE_INFO* create_info,
                                            uint table_changes)
{
  MRN_DBUG_ENTER_METHOD();
  bool res;
  if (create_info->comment.str != table_share->comment.str ||
      create_info->connect_string.str != table_share->connect_string.str) {
    DBUG_RETURN(COMPATIBLE_DATA_NO);
  }
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_check_if_incompatible_data(create_info, table_changes);
  } else {
#endif
    res = storage_check_if_incompatible_data(create_info, table_changes);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(res);
}

int ha_mroonga::storage_add_index_multiple_columns(TABLE* target_table,
                                                   KEY* key_info,
                                                   uint num_of_keys,
                                                   grn_obj** index_tables,
                                                   grn_obj** index_columns,
                                                   bool skip_unique_key)
{
  MRN_DBUG_ENTER_METHOD();

  int error = 0;

  GRN_TABLE_EACH_BEGIN(ctx, grn_table, cursor, id)
  {
    storage_store_fields(target_table, target_table->record[0], id);
    for (uint i = 0; i < num_of_keys; i++) {
      KEY* current_key_info = key_info + i;
      if (KEY_N_KEY_PARTS(current_key_info) == 1 ||
          current_key_info->algorithm == HA_KEY_ALG_FULLTEXT) {
        continue;
      }
      if (skip_unique_key && (key_info[i].flags & HA_NOSAME)) {
        continue;
      }
      if (!index_columns[i]) {
        continue;
      }

      /* fix key_info.key_length */
      for (uint j = 0; j < KEY_N_KEY_PARTS(current_key_info); j++) {
        if (!current_key_info->key_part[j].null_bit &&
            current_key_info->key_part[j].field->null_bit) {
          current_key_info->key_length++;
          current_key_info->key_part[j].null_bit =
            current_key_info->key_part[j].field->null_bit;
        }
      }
      if (key_info[i].flags & HA_NOSAME) {
        grn_id key_id;
        error = storage_write_row_unique_index(target_table->record[0],
                                               current_key_info,
                                               index_tables[i],
                                               index_columns[i],
                                               &key_id);
        if (error) {
          if (error == HA_ERR_FOUND_DUPP_KEY) {
            error = HA_ERR_FOUND_DUPP_UNIQUE;
          }
          break;
        }
      }
      error = storage_write_row_multiple_column_index(target_table->record[0],
                                                      id,
                                                      current_key_info,
                                                      index_columns[i]);
      if (error) {
        break;
      }
    }
  }
  GRN_TABLE_EACH_END(ctx, cursor);

  DBUG_RETURN(error);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_is_comment_changed(TABLE* table1, TABLE* table2)
{
  MRN_DBUG_ENTER_METHOD();

  if (table1->s->comment.length != table2->s->comment.length) {
    DBUG_RETURN(true);
  }

  if (strncmp(table1->s->comment.str,
              table2->s->comment.str,
              table1->s->comment.length) == 0) {
    DBUG_RETURN(false);
  } else {
    DBUG_RETURN(true);
  }
}

enum_alter_inplace_result ha_mroonga::wrapper_check_if_supported_inplace_alter(
  TABLE* altered_table, Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();
  uint n_keys;
  uint i;
  enum_alter_inplace_result result_mroonga = HA_ALTER_INPLACE_NO_LOCK;
  DBUG_PRINT("info",
             ("mroonga: handler_flags=%lu",
              static_cast<ulong>(ha_alter_info->handler_flags)));

  if (wrapper_is_comment_changed(table, altered_table)) {
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
  }
  if ((ha_alter_info->handler_flags &
       MRN_ALTER_INPLACE_INFO_ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX) &&
      (ha_alter_info->handler_flags &
       (MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_COLUMN,
                                    ADD_COLUMN) |
        MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_COLUMN,
                                    DROP_COLUMN) |
        MRN_ALTER_INPLACE_INFO_ALTER_FLAG(STORED_COLUMN_TYPE) |
        MRN_ALTER_INPLACE_INFO_ALTER_FLAG(STORED_COLUMN_ORDER) |
        MRN_ALTER_INPLACE_INFO_ALTER_FLAG(COLUMN_NULLABLE) |
        MRN_ALTER_INPLACE_INFO_ALTER_FLAG(COLUMN_NOT_NULLABLE) |
        MRN_ALTER_INPLACE_INFO_ALTER_FLAG(COLUMN_STORAGE_TYPE) |
        MRN_ALTER_INPLACE_INFO_ALTER_FLAG(COLUMN_COLUMN_FORMAT)))) {
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
  }
  if (ha_alter_info->handler_flags &
      MRN_ALTER_INPLACE_INFO_ALTER_FLAG(RENAME)) {
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
  }

  assert(ha_alter_info->key_count == altered_table->s->keys);
  alter_key_count = 0;
  alter_index_drop_count = 0;
  alter_index_add_count = 0;
  alter_handler_flags = ha_alter_info->handler_flags;
  if (!(alter_key_info_buffer = (KEY*)mrn_my_multi_malloc(
          MYF(MY_WME | MY_ZEROFILL),
          &alter_key_info_buffer,
          sizeof(KEY) * ha_alter_info->key_count,
          &alter_index_drop_buffer,
          sizeof(KEY) * ha_alter_info->index_drop_count,
          &alter_index_add_buffer,
          sizeof(uint) * ha_alter_info->index_add_count,
          &wrap_altered_table,
          sizeof(TABLE),
          &wrap_altered_table_key_info,
          sizeof(KEY) * altered_table->s->keys,
          &wrap_altered_table_share,
          sizeof(TABLE_SHARE),
          &wrap_altered_table_share_key_info,
          sizeof(KEY) * altered_table->s->keys,
          NullS))) {
    DBUG_RETURN(HA_ALTER_ERROR);
  }
  *wrap_altered_table = *altered_table;
  *wrap_altered_table_share = *(altered_table->s);
  mrn_init_sql_alloc(ha_thd(),
                     "mroonga::wrap-altered-table-share",
                     &(wrap_altered_table_share->mem_root));

  n_keys = ha_alter_info->index_drop_count;
  for (i = 0; i < n_keys; ++i) {
    const KEY* key = ha_alter_info->index_drop_buffer[i];
    if (key->algorithm == HA_KEY_ALG_FULLTEXT || mrn_is_geo_key(key)) {
      result_mroonga = HA_ALTER_INPLACE_EXCLUSIVE_LOCK;
    } else {
      memcpy(&alter_index_drop_buffer[alter_index_drop_count],
             ha_alter_info->index_drop_buffer[i],
             sizeof(KEY));
      ++alter_index_drop_count;
    }
  }
  if (!alter_index_drop_count) {
    alter_handler_flags &=
      ~MRN_ALTER_INPLACE_INFO_ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX;
  }
  n_keys = ha_alter_info->index_add_count;
  for (i = 0; i < n_keys; ++i) {
    const KEY* key =
      &altered_table->key_info[ha_alter_info->index_add_buffer[i]];
    if (key->algorithm == HA_KEY_ALG_FULLTEXT || mrn_is_geo_key(key)) {
      result_mroonga = HA_ALTER_INPLACE_EXCLUSIVE_LOCK;
    } else {
      alter_index_add_buffer[alter_index_add_count] =
        ha_alter_info->index_add_buffer[i];
      ++alter_index_add_count;
    }
  }
  if (!alter_index_add_count) {
    alter_handler_flags &=
      ~MRN_ALTER_INPLACE_INFO_ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX;
  }
  uint add_index_pos = 0;
  n_keys = ha_alter_info->key_count;
  for (i = 0; i < n_keys; ++i) {
    const KEY* key = &altered_table->key_info[i];
    if (!(key->algorithm == HA_KEY_ALG_FULLTEXT || mrn_is_geo_key(key))) {
      memcpy(&alter_key_info_buffer[alter_key_count],
             &ha_alter_info->key_info_buffer[i],
             sizeof(KEY));
      memcpy(&wrap_altered_table_key_info[alter_key_count],
             &altered_table->key_info[i],
             sizeof(KEY));
      memcpy(&wrap_altered_table_share_key_info[alter_key_count],
             &altered_table->s->key_info[i],
             sizeof(KEY));
      if (add_index_pos < alter_index_add_count &&
          alter_index_add_buffer[add_index_pos] == i) {
        alter_index_add_buffer[add_index_pos] = alter_key_count;
        ++add_index_pos;
      }
      ++alter_key_count;
    }
  }
  wrap_altered_table->key_info = wrap_altered_table_key_info;
  wrap_altered_table_share->key_info = wrap_altered_table_share_key_info;
  wrap_altered_table_share->keys = alter_key_count;
  wrap_altered_table->s = wrap_altered_table_share;

  if (!alter_handler_flags) {
    DBUG_RETURN(result_mroonga);
  }
  enum_alter_inplace_result result;
  MRN_SET_WRAP_ALTER_KEY(this, ha_alter_info);
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  result = wrap_handler->check_if_supported_inplace_alter(wrap_altered_table,
                                                          ha_alter_info);
  MRN_SET_BASE_ALTER_KEY(this, ha_alter_info);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  if (result_mroonga > result) {
    if (result == HA_ALTER_INPLACE_NOT_SUPPORTED) {
      my_free(alter_key_info_buffer);
      alter_key_info_buffer = NULL;
    }
    DBUG_RETURN(result);
  }
  DBUG_RETURN(result_mroonga);
}
#endif

enum_alter_inplace_result ha_mroonga::storage_check_if_supported_inplace_alter(
  TABLE* altered_table, Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();
  mrn_alter_flags explicitly_unsupported_flags =
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_FOREIGN_KEY,
                                ADD_FOREIGN_KEY) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_FOREIGN_KEY,
                                DROP_FOREIGN_KEY);
  mrn_alter_flags supported_flags =
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_INDEX, ADD_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_INDEX, DROP_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_UNIQUE_INDEX,
                                ADD_UNIQUE_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_UNIQUE_INDEX,
                                DROP_UNIQUE_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_VIRTUAL_COLUMN,
                                ADD_VIRTUAL_COLUMN) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_STORED_BASE_COLUMN,
                                ADD_STORED_BASE_COLUMN) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_STORED_GENERATED_COLUMN,
                                ADD_STORED_GENERATED_COLUMN) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_COLUMN, DROP_COLUMN) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ALTER_STORED_COLUMN_ORDER,
                                STORED_COLUMN_ORDER) |
    MRN_ALTER_INPLACE_INFO_ALTER_FLAG(COLUMN_NAME) |
    MRN_ALTER_INPLACE_INFO_ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX |
    MRN_ALTER_INPLACE_INFO_ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX |
    MRN_ALTER_INPLACE_INFO_ALTER_INDEX_ORDER;
  if (ha_alter_info->handler_flags & explicitly_unsupported_flags) {
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
  } else if ((ha_alter_info->handler_flags & supported_flags) ==
             ha_alter_info->handler_flags) {
    DBUG_RETURN(HA_ALTER_INPLACE_EXCLUSIVE_LOCK);
  } else {
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
  }
}

enum_alter_inplace_result
ha_mroonga::check_if_supported_inplace_alter(TABLE* altered_table,
                                             Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();
  enum_alter_inplace_result result;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    result =
      wrapper_check_if_supported_inplace_alter(altered_table, ha_alter_info);
  } else {
#endif
    result =
      storage_check_if_supported_inplace_alter(altered_table, ha_alter_info);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(result);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_prepare_inplace_alter_table(
  TABLE* altered_table,
  Alter_inplace_info* ha_alter_info
#  ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
  ,
  const dd::Table* old_table_def,
  dd::Table* new_table_def
#  endif
)
{
  bool result;
  MRN_DBUG_ENTER_METHOD();
  if (!alter_handler_flags) {
    DBUG_RETURN(false);
  }

#  ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  int error = 0;
  MRN_SHARE* tmp_share;
  tmp_share =
    mrn_get_share(altered_table->s->table_name.str, altered_table, &error);
  if (error != 0) {
    DBUG_RETURN(true);
  }

  if (parse_engine_table_options(ha_thd(),
                                 tmp_share->hton,
                                 wrap_altered_table->s)) {
    mrn_free_share(tmp_share);
    DBUG_RETURN(true);
  }
#  endif

  MRN_SET_WRAP_ALTER_KEY(this, ha_alter_info);
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
  result = wrap_handler->ha_prepare_inplace_alter_table(wrap_altered_table,
                                                        ha_alter_info,
                                                        old_table_def,
                                                        new_table_def);
#  else
  result = wrap_handler->ha_prepare_inplace_alter_table(wrap_altered_table,
                                                        ha_alter_info);
#  endif
  MRN_SET_BASE_ALTER_KEY(this, ha_alter_info);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);

#  ifdef MRN_SUPPORT_CUSTOM_OPTIONS
  mrn_free_share(tmp_share);
#  endif

  DBUG_RETURN(result);
}
#endif

bool ha_mroonga::storage_prepare_inplace_alter_table(
  TABLE* altered_table,
  Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
  ,
  const dd::Table* old_table_def,
  dd::Table* new_table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(false);
}

bool ha_mroonga::prepare_inplace_alter_table(TABLE* altered_table,
                                             Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                             ,
                                             const dd::Table* old_table_def,
                                             dd::Table* new_table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  bool result;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
#  ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
    result = wrapper_prepare_inplace_alter_table(altered_table,
                                                 ha_alter_info,
                                                 old_table_def,
                                                 new_table_def);
#  else
    result = wrapper_prepare_inplace_alter_table(altered_table, ha_alter_info);
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_PREPARE_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
    result = storage_prepare_inplace_alter_table(altered_table,
                                                 ha_alter_info,
                                                 old_table_def,
                                                 new_table_def);
#else
  result = storage_prepare_inplace_alter_table(altered_table, ha_alter_info);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(result);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_inplace_alter_table(TABLE* altered_table,
                                             Alter_inplace_info* ha_alter_info
#  ifdef MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                             ,
                                             const dd::Table* old_table_def,
                                             dd::Table* new_table_def
#  endif
)
{
  int error;
  bool result = false;
  uint n_keys;
  uint i, j = 0;
  KEY* key_info = table_share->key_info;
  MRN_DBUG_ENTER_METHOD();
  error = mrn_change_encoding(ctx, system_charset_info);
  if (error)
    DBUG_RETURN(true);

  DBUG_PRINT("info", ("mroonga: table_name=%s", share->table_name));
  mrn::PathMapper mapper(share->table_name);
  n_keys = ha_alter_info->index_drop_count;
  for (i = 0; i < n_keys; ++i) {
    const KEY* key = ha_alter_info->index_drop_buffer[i];
    if (!(key->algorithm == HA_KEY_ALG_FULLTEXT || mrn_is_geo_key(key))) {
      continue;
    }
    while (!KEY_NAME_EQUAL_KEY(key, &(key_info[j]))) {
      ++j;
    }
    DBUG_PRINT(
      "info",
      ("mroonga: key_name=" KEY_NAME_FORMAT, KEY_NAME_FORMAT_VALUE(key)));
    error = drop_index(share, j);
    if (error)
      DBUG_RETURN(true);
    grn_index_tables[j] = NULL;
    grn_index_columns[j] = NULL;
  }

  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*,
                                      index_tables,
                                      ha_alter_info->key_count);
  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*,
                                      index_columns,
                                      ha_alter_info->key_count);
  TABLE_SHARE tmp_table_share;
  KEY* p_key_info = &table->key_info[table_share->primary_key];
  bool need_fill_index = false;
  memset(index_tables, 0, sizeof(grn_obj*) * ha_alter_info->key_count);
  memset(index_columns, 0, sizeof(grn_obj*) * ha_alter_info->key_count);
  tmp_table_share.keys = ha_alter_info->key_count;
  tmp_table_share.fields = 0;
  bitmap_clear_all(table->read_set);
  mrn_set_bitmap_by_key(table->read_set, p_key_info);
  n_keys = ha_alter_info->index_add_count;
  for (i = 0; i < n_keys; ++i) {
    uint key_pos = ha_alter_info->index_add_buffer[i];
    KEY* key = &altered_table->key_info[key_pos];
    if (!(key->algorithm == HA_KEY_ALG_FULLTEXT || mrn_is_geo_key(key))) {
      continue;
    }
    if (share->disable_keys) {
      continue;
    }
    DBUG_PRINT("info", ("mroonga: add key pos=%u", key_pos));
    if (key->algorithm == HA_KEY_ALG_FULLTEXT &&
        (error = wrapper_create_index_fulltext(mapper.table_name(),
                                               key_pos,
                                               key,
                                               index_tables,
                                               NULL))) {
      break;
    } else if (mrn_is_geo_key(key) &&
               (error = wrapper_create_index_geo(mapper.table_name(),
                                                 key_pos,
                                                 key,
                                                 index_tables,
                                                 NULL))) {
      break;
    }
    mrn_set_bitmap_by_key(table->read_set, key);
    index_columns[key_pos] = grn_obj_column(ctx,
                                            index_tables[key_pos],
                                            INDEX_COLUMN_NAME,
                                            strlen(INDEX_COLUMN_NAME));
    need_fill_index = true;
  }
  if (!error && need_fill_index) {
    mrn::TableDataSwitcher switcher(altered_table, table);
    error = wrapper_fill_indexes(ha_thd(),
                                 altered_table->key_info,
                                 index_columns,
                                 ha_alter_info->key_count);
  }
  bitmap_set_all(table->read_set);

  if (!error && alter_handler_flags) {
#  ifdef MRN_SUPPORT_CUSTOM_OPTIONS
    {
      MRN_SHARE* alter_tmp_share;
      alter_tmp_share =
        mrn_get_share(altered_table->s->table_name.str, altered_table, &error);
      if (alter_tmp_share) {
        if (parse_engine_table_options(ha_thd(),
                                       alter_tmp_share->hton,
                                       wrap_altered_table->s)) {
          error = MRN_GET_ERROR_NUMBER;
        }
        mrn_free_share(alter_tmp_share);
      }
    }
#  endif
    if (!error) {
      MRN_SET_WRAP_ALTER_KEY(this, ha_alter_info);
      MRN_SET_WRAP_SHARE_KEY(share, table->s);
      MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
      result = wrap_handler->ha_inplace_alter_table(wrap_altered_table,
                                                    ha_alter_info,
                                                    old_table_def,
                                                    new_table_def);
#  else
      result =
        wrap_handler->ha_inplace_alter_table(wrap_altered_table, ha_alter_info);
#  endif
      MRN_SET_BASE_ALTER_KEY(this, ha_alter_info);
      MRN_SET_BASE_SHARE_KEY(share, table->s);
      MRN_SET_BASE_TABLE_KEY(this, table);
    }
  }

  if (result || error) {
    n_keys = ha_alter_info->index_add_count;
    for (i = 0; i < n_keys; ++i) {
      uint key_pos = ha_alter_info->index_add_buffer[i];
      KEY* key = &altered_table->key_info[key_pos];
      if (!(key->algorithm == HA_KEY_ALG_FULLTEXT || mrn_is_geo_key(key))) {
        continue;
      }
      if (share->disable_keys) {
        continue;
      }
      if (index_tables[key_pos]) {
        grn_obj_remove(ctx, index_tables[key_pos]);
      }
    }
    result = true;
  }
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_columns);
  DBUG_RETURN(result);
}
#endif

bool ha_mroonga::storage_inplace_alter_table_add_index(
  TABLE* altered_table, Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();

  {
    const auto n_keys = ha_alter_info->index_add_count;
    for (uint i = 0; i < n_keys; ++i) {
      auto key_pos = ha_alter_info->index_add_buffer[i];
      auto key = &altered_table->key_info[key_pos];
      auto error = storage_validate_key(key);
      if (error != 0) {
        DBUG_RETURN(true);
      }
    }
  }

  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*,
                                      index_tables,
                                      ha_alter_info->key_count);
  MRN_ALLOCATE_VARIABLE_LENGTH_ARRAYS(grn_obj*,
                                      index_columns,
                                      ha_alter_info->key_count);
  bool have_multiple_column_index = false;
  memset(index_tables, 0, sizeof(grn_obj*) * ha_alter_info->key_count);
  memset(index_columns, 0, sizeof(grn_obj*) * ha_alter_info->key_count);
  bitmap_clear_all(table->read_set);
  if (table_share->primary_key != MAX_KEY) {
    KEY* p_key_info = &table->key_info[table_share->primary_key];
    mrn_set_bitmap_by_key(table->read_set, p_key_info);
  }
  int error = 0;
  uint n_keys = ha_alter_info->index_add_count;
  for (uint i = 0; i < n_keys; ++i) {
    uint key_pos = ha_alter_info->index_add_buffer[i];
    KEY* key = &altered_table->key_info[key_pos];
    if (share->disable_keys && !(key->flags & HA_NOSAME)) {
      continue; // key is disabled
    }
    DBUG_PRINT("info", ("mroonga: add key pos=%u", key_pos));
    mrn::PathMapper mapper(share->table_name);
    if ((error = storage_create_index(table,
                                      mapper.table_name(),
                                      grn_table,
                                      key,
                                      index_tables,
                                      index_columns,
                                      key_pos))) {
      break;
    }
    if (KEY_N_KEY_PARTS(key) == 1 && (key->flags & HA_NOSAME) &&
        grn_table_size(ctx, grn_table) !=
          grn_table_size(ctx, index_tables[key_pos])) {
      error = HA_ERR_FOUND_DUPP_UNIQUE;
      my_printf_error(ER_DUP_UNIQUE,
                      MRN_GET_ERR_MSG(ER_DUP_UNIQUE),
                      MYF(0),
                      table_share->table_name.str);
      ++i;
      break;
    }
    if (KEY_N_KEY_PARTS(key) != 1 && key->algorithm != HA_KEY_ALG_FULLTEXT) {
      mrn_set_bitmap_by_key(table->read_set, key);
      have_multiple_column_index = true;
    }
  }
  if (!error && have_multiple_column_index) {
    error = storage_add_index_multiple_columns(altered_table,
                                               altered_table->key_info,
                                               ha_alter_info->key_count,
                                               index_tables,
                                               index_columns,
                                               false);
    if (error == HA_ERR_FOUND_DUPP_UNIQUE) {
      my_printf_error(ER_DUP_UNIQUE,
                      MRN_GET_ERR_MSG(ER_DUP_UNIQUE),
                      MYF(0),
                      table_share->table_name.str);
    } else if (error) {
      my_message(error, "failed to create multiple column index", MYF(0));
    }
  }
  bitmap_set_all(table->read_set);

  bool have_error = false;
  if (error) {
    n_keys = ha_alter_info->index_add_count;
    for (uint i = 0; i < n_keys; ++i) {
      uint key_pos = ha_alter_info->index_add_buffer[i];
      KEY* key = &altered_table->key_info[key_pos];
      if (share->disable_keys && !(key->flags & HA_NOSAME)) {
        continue;
      }
      if (index_tables[key_pos]) {
        grn_obj_remove(ctx, index_columns[key_pos]);
        grn_obj_remove(ctx, index_tables[key_pos]);
      }
    }
    have_error = true;
  }
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_tables);
  MRN_FREE_VARIABLE_LENGTH_ARRAYS(index_columns);

  DBUG_RETURN(have_error);
}

bool ha_mroonga::storage_inplace_alter_table_drop_index(
  TABLE* altered_table, Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();

  bool have_error = false;
  uint n_keys;
  uint i, j = 0;
  KEY* key_info = table_share->key_info;
  mrn::PathMapper mapper(share->table_name);
  n_keys = ha_alter_info->index_drop_count;
  for (i = 0; i < n_keys; ++i) {
    KEY* key = ha_alter_info->index_drop_buffer[i];
    while (!KEY_NAME_EQUAL_KEY(key, &(key_info[j]))) {
      ++j;
    }
    int error = drop_index(share, j);
    if (error != 0)
      DBUG_RETURN(true);
    grn_index_tables[j] = NULL;
    grn_index_columns[j] = NULL;
  }

  DBUG_RETURN(have_error);
}

bool ha_mroonga::storage_inplace_alter_table_add_column(
  TABLE* altered_table, Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();

  bool have_error = false;

  MRN_SHARE* tmp_share;
  TABLE_SHARE tmp_table_share;
  char **col_flags, **col_type;
  uint *col_flags_length, *col_type_length;
  tmp_table_share.keys = 0;
  tmp_table_share.fields = altered_table->s->fields;
  tmp_share =
    (MRN_SHARE*)mrn_my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
                                    &tmp_share,
                                    sizeof(*tmp_share),
                                    &col_flags,
                                    sizeof(char*) * tmp_table_share.fields,
                                    &col_flags_length,
                                    sizeof(uint) * tmp_table_share.fields,
                                    &col_type,
                                    sizeof(char*) * tmp_table_share.fields,
                                    &col_type_length,
                                    sizeof(uint) * tmp_table_share.fields,
                                    NullS);
  if (!tmp_share) {
    have_error = true;
    DBUG_RETURN(have_error);
  }
#ifdef MRN_ENABLE_WRAPPER_MODE
  tmp_share->engine = NULL;
#endif
  tmp_share->table_share = &tmp_table_share;
  tmp_share->col_flags = col_flags;
  tmp_share->col_flags_length = col_flags_length;
  tmp_share->col_type = col_type;
  tmp_share->col_type_length = col_type_length;

  mrn::PathMapper mapper(share->table_name);
  grn_obj* table_obj;
  table_obj =
    grn_ctx_get(ctx, mapper.table_name(), strlen(mapper.table_name()));

  Alter_info* alter_info = ha_alter_info->alter_info;
  List_iterator_fast<Create_field> create_fields(alter_info->create_list);
  for (uint i = 0; Create_field* create_field = create_fields++; i++) {
    if (create_field->field) {
      continue;
    }

    Field* field = altered_table->s->field[i];

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_VIRTUAL(field)) {
      continue;
    }
#endif

    mrn::ColumnName column_name(FIELD_NAME(field));
    int error = mrn_add_column_param(tmp_share, field, i);
    if (error) {
      have_error = true;
      break;
    }

    grn_column_flags col_flags = GRN_OBJ_PERSISTENT;
    if (!find_column_flags(field, tmp_share, i, &col_flags)) {
      col_flags |= GRN_OBJ_COLUMN_SCALAR;
    }

    grn_obj* col_type;
    {
      int column_type_error_code = ER_WRONG_FIELD_SPEC;
      col_type = find_column_type(field, tmp_share, i, column_type_error_code);
      if (!col_type) {
        error = column_type_error_code;
        have_error = true;
        break;
      }
    }
    char* col_path = NULL; // we don't specify path

    grn_obj* column_obj = grn_column_create(ctx,
                                            table_obj,
                                            column_name.c_str(),
                                            column_name.length(),
                                            col_path,
                                            col_flags,
                                            col_type);
    if (ctx->rc) {
      error = ER_WRONG_COLUMN_NAME;
      my_message(error, ctx->errbuf, MYF(0));
      have_error = true;
      break;
    }

#ifdef MRN_SUPPORT_GENERATED_COLUMNS
    if (MRN_GENERATED_COLUMNS_FIELD_IS_STORED(field)) {
#  ifndef MRN_MARIADB_P
      MY_BITMAP generated_column_bitmap;
      if (mrn_bitmap_init(&generated_column_bitmap,
                          NULL,
                          altered_table->s->fields)) {
        error = HA_ERR_OUT_OF_MEM;
        my_message(ER_OUTOFMEMORY,
                   "mroonga: storage: "
                   "failed to allocate memory for getting generated value",
                   MYF(0));
        have_error = true;
        grn_obj_remove(ctx, column_obj);
        break;
      }
      mrn::SmartBitmap smart_generated_column_bitmap(&generated_column_bitmap);
      bitmap_set_bit(&generated_column_bitmap, MRN_FIELD_FIELD_INDEX(field));
#  endif

      Field* altered_field = altered_table->field[i];
      grn_obj new_value;
      GRN_VOID_INIT(&new_value);
      mrn::SmartGrnObj smart_new_value(ctx, &new_value);
      GRN_TABLE_EACH_BEGIN(ctx, grn_table, cursor, id)
      {
        storage_store_fields(altered_table, altered_table->record[0], id);

#  ifdef MRN_MARIADB_P
        MRN_GENERATED_COLUMNS_UPDATE_VIRTUAL_FIELD(altered_table,
                                                   altered_field);
#  else
        if (update_generated_write_fields(&generated_column_bitmap,
                                          altered_table)) {
          error = ER_ERROR_ON_WRITE;
          my_message(error,
                     "mroonga: storage: "
                     "failed to update generated value for updating column",
                     MYF(0));
          have_error = true;
          grn_obj_remove(ctx, column_obj);
          break;
        }
#  endif

        error = mrn_change_encoding(ctx, altered_field->charset());
        if (error) {
          my_message(error,
                     "mroonga: storage: "
                     "failed to change encoding to store generated value",
                     MYF(0));
          have_error = true;
          grn_obj_remove(ctx, column_obj);
          break;
        }
        error = generic_store_bulk(altered_field, &new_value);
        if (error) {
          my_message(error,
                     "mroonga: storage: "
                     "failed to get generated value for updating column",
                     MYF(0));
          have_error = true;
          grn_obj_remove(ctx, column_obj);
          break;
        }

        grn_obj_set_value(ctx, column_obj, record_id, &new_value, GRN_OBJ_SET);
        if (ctx->rc) {
          error = ER_ERROR_ON_WRITE;
          my_message(error, ctx->errbuf, MYF(0));
          break;
        }
      }
      GRN_TABLE_EACH_END(ctx, cursor);
    }
#endif
  }

  grn_obj_unlink(ctx, table_obj);

  mrn_free_share_alloc(tmp_share);
  my_free(tmp_share);

  DBUG_RETURN(have_error);
}

bool ha_mroonga::storage_inplace_alter_table_drop_column(
  TABLE* altered_table, Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();

  bool have_error = false;

  Alter_info* alter_info = ha_alter_info->alter_info;

  uint n_fields = table->s->fields;
  for (uint i = 0; i < n_fields; i++) {
    Field* field = table->field[i];

    bool dropped = true;
    List_iterator_fast<Create_field> create_fields(alter_info->create_list);
    while (Create_field* create_field = create_fields++) {
      if (create_field->field == field) {
        dropped = false;
        break;
      }
    }
    if (!dropped) {
      continue;
    }

    grn_column_cache* column_cache = grn_column_caches[i];
    if (column_cache) {
      grn_column_cache_close(ctx, column_cache);
      grn_column_caches[i] = NULL;
    }

    grn_obj* range = grn_column_ranges[i];
    if (range) {
      grn_obj_unlink(ctx, range);
      grn_column_ranges[i] = NULL;
    }

    grn_obj* column;
    column = grn_columns[i];
    if (column) {
      grn_obj_remove(ctx, column);
      grn_columns[i] = NULL;
    }
    if (ctx->rc) {
      int error = ER_WRONG_COLUMN_NAME;
      my_message(error, ctx->errbuf, MYF(0));
      have_error = true;
      break;
    }
  }

  DBUG_RETURN(have_error);
}

bool ha_mroonga::storage_inplace_alter_table_rename_column(
  TABLE* altered_table, Alter_inplace_info* ha_alter_info)
{
  MRN_DBUG_ENTER_METHOD();

  bool have_error = false;

  mrn::PathMapper mapper(share->table_name);
  grn_obj* table_obj;
  table_obj =
    grn_ctx_get(ctx, mapper.table_name(), strlen(mapper.table_name()));

  Alter_info* alter_info = ha_alter_info->alter_info;
  uint n_fields = table->s->fields;
  for (uint i = 0; i < n_fields; i++) {
    Field* field = table->field[i];

    if (!(MRN_FIELD_ALL_FLAGS(field) & FIELD_IS_RENAMED)) {
      continue;
    }

    const char* new_field_name = NULL;
    size_t new_field_name_length = 0;
    List_iterator_fast<Create_field> create_fields(alter_info->create_list);
    while (Create_field* create_field = create_fields++) {
      if (create_field->field == field) {
#ifdef MRN_CREATE_FIELD_USE_LEX_STRING
        new_field_name = create_field->field_name.str;
        new_field_name_length = create_field->field_name.length;
#else
        new_field_name = create_field->field_name;
        new_field_name_length = strlen(new_field_name);
#endif
        break;
      }
    }

    if (!new_field_name) {
      continue;
    }

    Field* old_field = field;
    grn_obj* column_obj;
    column_obj = grn_obj_column(ctx, table_obj, FIELD_NAME(old_field));
    if (column_obj) {
      grn_column_rename(ctx, column_obj, new_field_name, new_field_name_length);
      if (ctx->rc) {
        int error = ER_WRONG_COLUMN_NAME;
        my_message(error, ctx->errbuf, MYF(0));
        have_error = true;
      }
      grn_obj_unlink(ctx, column_obj);
    }

    if (have_error) {
      break;
    }
  }
  grn_obj_unlink(ctx, table_obj);

  DBUG_RETURN(have_error);
}

bool ha_mroonga::storage_inplace_alter_table(TABLE* altered_table,
                                             Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                             ,
                                             const dd::Table* old_table_def,
                                             dd::Table* new_table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();

  bool have_error = false;

  int error = mrn_change_encoding(ctx, system_charset_info);
  if (error) {
    have_error = true;
  }

  mrn_alter_flags drop_index_related_flags =
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_INDEX, DROP_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_UNIQUE_INDEX,
                                DROP_UNIQUE_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_PK_INDEX,
                                DROP_PK_INDEX) |
    MRN_ALTER_INPLACE_INFO_ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX;
  if (!have_error &&
      (ha_alter_info->handler_flags & drop_index_related_flags)) {
    have_error =
      storage_inplace_alter_table_drop_index(altered_table, ha_alter_info);
  }

  mrn_alter_flags add_column_related_flags =
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_COLUMN, ADD_COLUMN);
  if (!have_error &&
      (ha_alter_info->handler_flags & add_column_related_flags)) {
    have_error =
      storage_inplace_alter_table_add_column(altered_table, ha_alter_info);
  }

  mrn_alter_flags drop_column_related_flags =
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::DROP_COLUMN, DROP_COLUMN);
  if (!have_error &&
      (ha_alter_info->handler_flags & drop_column_related_flags)) {
    have_error =
      storage_inplace_alter_table_drop_column(altered_table, ha_alter_info);
  }

  mrn_alter_flags rename_column_related_flags =
    MRN_ALTER_INPLACE_INFO_ALTER_FLAG(COLUMN_NAME);
  if (!have_error &&
      (ha_alter_info->handler_flags & rename_column_related_flags)) {
    have_error =
      storage_inplace_alter_table_rename_column(altered_table, ha_alter_info);
  }

  mrn_alter_flags add_index_related_flags =
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_INDEX, ADD_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_UNIQUE_INDEX,
                                ADD_UNIQUE_INDEX) |
    MRN_ALTER_INPLACE_INFO_FLAG(Alter_inplace_info::ADD_PK_INDEX,
                                ADD_PK_INDEX) |
    MRN_ALTER_INPLACE_INFO_ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX;
  if (!have_error && (ha_alter_info->handler_flags & add_index_related_flags)) {
    have_error =
      storage_inplace_alter_table_add_index(altered_table, ha_alter_info);
  }

  DBUG_RETURN(have_error);
}

bool ha_mroonga::inplace_alter_table(TABLE* altered_table,
                                     Alter_inplace_info* ha_alter_info
#ifdef MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                     ,
                                     const dd::Table* old_table_def,
                                     dd::Table* new_table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  bool result;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
#  ifdef MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
    result = wrapper_inplace_alter_table(altered_table,
                                         ha_alter_info,
                                         old_table_def,
                                         new_table_def);
#  else
    result = wrapper_inplace_alter_table(altered_table, ha_alter_info);
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
    result = storage_inplace_alter_table(altered_table,
                                         ha_alter_info,
                                         old_table_def,
                                         new_table_def);
#else
  result = storage_inplace_alter_table(altered_table, ha_alter_info);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(result);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_commit_inplace_alter_table(
  TABLE* altered_table,
  Alter_inplace_info* ha_alter_info,
  bool commit
#  ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
  ,
  const dd::Table* old_table_def,
  dd::Table* new_table_def
#  endif
)
{
  bool result;
  MRN_DBUG_ENTER_METHOD();
  if (!alter_handler_flags) {
    MRN_FREE_ROOT(&(wrap_altered_table_share->mem_root));
    my_free(alter_key_info_buffer);
    alter_key_info_buffer = NULL;
    DBUG_RETURN(false);
  }
  MRN_SET_WRAP_ALTER_KEY(this, ha_alter_info);
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#  ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
  result = wrap_handler->ha_commit_inplace_alter_table(wrap_altered_table,
                                                       ha_alter_info,
                                                       commit,
                                                       old_table_def,
                                                       new_table_def);
#  else
  result = wrap_handler->ha_commit_inplace_alter_table(wrap_altered_table,
                                                       ha_alter_info,
                                                       commit);
#  endif
  MRN_SET_BASE_ALTER_KEY(this, ha_alter_info);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  MRN_FREE_ROOT(&(wrap_altered_table_share->mem_root));
  my_free(alter_key_info_buffer);
  alter_key_info_buffer = NULL;
  DBUG_RETURN(result);
}
#endif

bool ha_mroonga::storage_commit_inplace_alter_table(
  TABLE* altered_table,
  Alter_inplace_info* ha_alter_info,
  bool commit
#ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
  ,
  const dd::Table* old_table_def,
  dd::Table* new_table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(false);
}

bool ha_mroonga::commit_inplace_alter_table(TABLE* altered_table,
                                            Alter_inplace_info* ha_alter_info,
                                            bool commit
#ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
                                            ,
                                            const dd::Table* old_table_def,
                                            dd::Table* new_table_def
#endif
)
{
  MRN_DBUG_ENTER_METHOD();
  bool result;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
#  ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
    result = wrapper_commit_inplace_alter_table(altered_table,
                                                ha_alter_info,
                                                commit,
                                                old_table_def,
                                                new_table_def);
#  else
    result =
      wrapper_commit_inplace_alter_table(altered_table, ha_alter_info, commit);
#  endif
  } else {
#endif
#ifdef MRN_HANDLER_COMMIT_INPLACE_ALTER_TABLE_HAVE_TABLE_DEFINITION
    result = storage_commit_inplace_alter_table(altered_table,
                                                ha_alter_info,
                                                commit,
                                                old_table_def,
                                                new_table_def);
#else
  result =
    storage_commit_inplace_alter_table(altered_table, ha_alter_info, commit);
#endif
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(result);
}

#ifdef MRN_HANDLER_HAVE_NOTIFY_TABLE_CHANGED
#  ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_notify_table_changed(
#    ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
  Alter_inplace_info* ha_alter_info
#    endif
)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
#    ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
  wrap_handler->ha_notify_table_changed(ha_alter_info);
#    else
  wrap_handler->ha_notify_table_changed();
#    endif
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#  endif

void ha_mroonga::storage_notify_table_changed(
#  ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
  Alter_inplace_info* ha_alter_info
#  endif
)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_VOID_RETURN;
}

void ha_mroonga::notify_table_changed(
#  ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
  Alter_inplace_info* ha_alter_info
#  endif
)
{
  MRN_DBUG_ENTER_METHOD();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
#    ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
    wrapper_notify_table_changed(ha_alter_info);
#    else
    wrapper_notify_table_changed();
#    endif
  } else {
#  endif
#  ifdef MRN_HANDLER_NOTIFY_TABLE_CHANGED_HAVE_ALTER_INPLACE_INFO
    storage_notify_table_changed(ha_alter_info);
#  else
  storage_notify_table_changed();
#  endif
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_VOID_RETURN;
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_update_auto_increment()
{
  int res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->update_auto_increment();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

int ha_mroonga::storage_update_auto_increment()
{
  MRN_DBUG_ENTER_METHOD();
  int res = handler::update_auto_increment();
  DBUG_PRINT(
    "info",
    ("mroonga: auto_inc_value=%llu", table->next_number_field->val_int()));
  DBUG_RETURN(res);
}

int ha_mroonga::update_auto_increment()
{
  MRN_DBUG_ENTER_METHOD();
  int res;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_update_auto_increment();
  } else {
#endif
    res = storage_update_auto_increment();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(res);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_set_next_insert_id(ulonglong id)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->set_next_insert_id(id);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_set_next_insert_id(ulonglong id)
{
  MRN_DBUG_ENTER_METHOD();
  handler::set_next_insert_id(id);
  DBUG_VOID_RETURN;
}

void ha_mroonga::set_next_insert_id(ulonglong id)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_set_next_insert_id(id);
  } else {
#endif
    storage_set_next_insert_id(id);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_get_auto_increment(ulonglong offset,
                                            ulonglong increment,
                                            ulonglong nb_desired_values,
                                            ulonglong* first_value,
                                            ulonglong* nb_reserved_values)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->get_auto_increment(offset,
                                   increment,
                                   nb_desired_values,
                                   first_value,
                                   nb_reserved_values);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_get_auto_increment(ulonglong offset,
                                            ulonglong increment,
                                            ulonglong nb_desired_values,
                                            ulonglong* first_value,
                                            ulonglong* nb_reserved_values)
{
  MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
  MRN_DBUG_ENTER_METHOD();
  if (table->found_next_number_field && !table->s->next_number_keypart) {
    if (long_term_share->auto_inc_inited) {
      *first_value = long_term_share->auto_inc_value;
      DBUG_PRINT("info",
                 ("mroonga: *first_value(auto_inc_value)=%llu", *first_value));
      *nb_reserved_values = UINT_MAX64;
    } else {
      handler::get_auto_increment(offset,
                                  increment,
                                  nb_desired_values,
                                  first_value,
                                  nb_reserved_values);
      long_term_share->auto_inc_value = *first_value;
      DBUG_PRINT(
        "info",
        ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
      long_term_share->auto_inc_inited = true;
    }
  } else {
    handler::get_auto_increment(offset,
                                increment,
                                nb_desired_values,
                                first_value,
                                nb_reserved_values);
  }
  DBUG_VOID_RETURN;
}

void ha_mroonga::get_auto_increment(ulonglong offset,
                                    ulonglong increment,
                                    ulonglong nb_desired_values,
                                    ulonglong* first_value,
                                    ulonglong* nb_reserved_values)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_get_auto_increment(offset,
                               increment,
                               nb_desired_values,
                               first_value,
                               nb_reserved_values);
  } else {
#endif
    MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
    mrn::Lock lock(&long_term_share->auto_inc_mutex);
    storage_get_auto_increment(offset,
                               increment,
                               nb_desired_values,
                               first_value,
                               nb_reserved_values);
    long_term_share->auto_inc_value += nb_desired_values * increment;
    DBUG_PRINT(
      "info",
      ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_restore_auto_increment(ulonglong prev_insert_id)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->restore_auto_increment(prev_insert_id);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_restore_auto_increment(ulonglong prev_insert_id)
{
  MRN_DBUG_ENTER_METHOD();
  handler::restore_auto_increment(prev_insert_id);
  DBUG_VOID_RETURN;
}

void ha_mroonga::restore_auto_increment(ulonglong prev_insert_id)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_restore_auto_increment(prev_insert_id);
  } else {
#endif
    storage_restore_auto_increment(prev_insert_id);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_release_auto_increment()
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->ha_release_auto_increment();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_release_auto_increment()
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_VOID_RETURN;
}

void ha_mroonga::release_auto_increment()
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_release_auto_increment();
  } else {
#endif
    storage_release_auto_increment();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_check_for_upgrade(HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  int error = wrap_handler->ha_check_for_upgrade(check_opt);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(error);
}
#endif

int ha_mroonga::storage_check_for_upgrade(HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  for (uint i = 0; i < table->s->fields; ++i) {
    grn_obj* column = grn_columns[i];
    if (!column) {
      continue;
    }
    Field* field = table->field[i];
    grn_id column_range = grn_obj_get_range(ctx, column);
    switch (field->real_type()) {
    case MYSQL_TYPE_ENUM:
      if (column_range != GRN_DB_UINT16) {
        DBUG_RETURN(HA_ADMIN_NEEDS_ALTER);
      }
      break;
    case MYSQL_TYPE_SET:
      if (column_range != GRN_DB_UINT64) {
        DBUG_RETURN(HA_ADMIN_NEEDS_ALTER);
      }
      break;
    default:
      break;
    }
  }
  DBUG_RETURN(HA_ADMIN_OK);
}

int ha_mroonga::check_for_upgrade(HA_CHECK_OPT* check_opt)
{
  MRN_DBUG_ENTER_METHOD();
  int error;
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    error = wrapper_check_for_upgrade(check_opt);
  } else {
#endif
    error = storage_check_for_upgrade(check_opt);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(error);
}

#ifdef MRN_HANDLER_HAVE_RESET_AUTO_INCREMENT
#  ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_reset_auto_increment(ulonglong value)
{
  int res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->ha_reset_auto_increment(value);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

int ha_mroonga::storage_reset_auto_increment(ulonglong value)
{
  MRN_LONG_TERM_SHARE* long_term_share = share->long_term_share;
  MRN_DBUG_ENTER_METHOD();
  mrn::Lock lock(&long_term_share->auto_inc_mutex);
  long_term_share->auto_inc_value = value;
  DBUG_PRINT("info",
             ("mroonga: auto_inc_value=%llu", long_term_share->auto_inc_value));
  long_term_share->auto_inc_inited = true;
  DBUG_RETURN(0);
}

int ha_mroonga::reset_auto_increment(ulonglong value)
{
  MRN_DBUG_ENTER_METHOD();
  int res;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_reset_auto_increment(value);
  } else {
#  endif
    res = storage_reset_auto_increment(value);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(res);
}
#endif

void ha_mroonga::set_pk_bitmap()
{
  MRN_DBUG_ENTER_METHOD();
  KEY* key_info = &(table->key_info[table_share->primary_key]);
  uint j;
  for (j = 0; j < KEY_N_KEY_PARTS(key_info); j++) {
    Field* field = key_info->key_part[j].field;
    bitmap_set_bit(table->read_set, MRN_FIELD_FIELD_INDEX(field));
  }
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_was_semi_consistent_read()
{
  bool res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->was_semi_consistent_read();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

bool ha_mroonga::storage_was_semi_consistent_read()
{
  bool res;
  MRN_DBUG_ENTER_METHOD();
  res = handler::was_semi_consistent_read();
  DBUG_RETURN(res);
}

bool ha_mroonga::was_semi_consistent_read()
{
  bool res;
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_was_semi_consistent_read();
  } else {
#endif
    res = storage_was_semi_consistent_read();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(res);
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_try_semi_consistent_read(bool yes)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->try_semi_consistent_read(yes);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_try_semi_consistent_read(bool yes)
{
  MRN_DBUG_ENTER_METHOD();
  handler::try_semi_consistent_read(yes);
  DBUG_VOID_RETURN;
}

void ha_mroonga::try_semi_consistent_read(bool yes)
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_try_semi_consistent_read(yes);
  } else {
#endif
    storage_try_semi_consistent_read(yes);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_unlock_row()
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->unlock_row();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_unlock_row()
{
  MRN_DBUG_ENTER_METHOD();
  handler::unlock_row();
  DBUG_VOID_RETURN;
}

void ha_mroonga::unlock_row()
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_unlock_row();
  } else {
#endif
    storage_unlock_row();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_start_stmt(THD* thd, thr_lock_type lock_type)
{
  int res;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->start_stmt(thd, lock_type);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#endif

int ha_mroonga::storage_start_stmt(THD* thd, thr_lock_type lock_type)
{
  int res;
  MRN_DBUG_ENTER_METHOD();
  res = handler::start_stmt(thd, lock_type);
  DBUG_RETURN(res);
}

int ha_mroonga::start_stmt(THD* thd, thr_lock_type lock_type)
{
  int res;
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_start_stmt(thd, lock_type);
  } else {
#endif
    res = storage_start_stmt(thd, lock_type);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_RETURN(res);
}

#ifdef MRN_HANDLER_HAVE_HAS_GAP_LOCKS
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_has_gap_locks() const
  MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT
{
  bool has;
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  has = wrap_handler->has_gap_locks();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(has);
}
#  endif

bool ha_mroonga::storage_has_gap_locks() const
  MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_RETURN(false);
}

bool ha_mroonga::has_gap_locks() const MRN_HANDLER_HAS_GAP_LOCKS_NOEXCEPT
{
  bool has;
  MRN_DBUG_ENTER_METHOD();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    has = wrapper_has_gap_locks();
  } else {
#  endif
    has = storage_has_gap_locks();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(has);
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_change_table_ptr(TABLE* table_arg,
                                          TABLE_SHARE* share_arg)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->change_table_ptr(table_arg, share->wrap_table_share);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_change_table_ptr(TABLE* table_arg,
                                          TABLE_SHARE* share_arg)
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_VOID_RETURN;
}

void ha_mroonga::change_table_ptr(TABLE* table_arg, TABLE_SHARE* share_arg)
{
  MRN_DBUG_ENTER_METHOD();
  handler::change_table_ptr(table_arg, share_arg);
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share && share->wrapper_mode) {
    wrapper_change_table_ptr(table_arg, share_arg);
  } else {
#endif
    storage_change_table_ptr(table_arg, share_arg);
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_HANDLER_HAVE_PRIMARY_KEY_IS_CLUSTERED
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_primary_key_is_clustered()
  MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST
{
  MRN_DBUG_ENTER_METHOD();
  bool is_clustered;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  is_clustered = wrap_handler->primary_key_is_clustered();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(is_clustered);
}
#  endif

bool ha_mroonga::storage_primary_key_is_clustered()
  MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST
{
  MRN_DBUG_ENTER_METHOD();
  bool is_clustered = handler::primary_key_is_clustered();
  DBUG_RETURN(is_clustered);
}

bool ha_mroonga::primary_key_is_clustered()
  MRN_HANDLER_PRIMARY_KEY_IS_CLUSTERED_CONST
{
  MRN_DBUG_ENTER_METHOD();
  bool is_clustered;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share && share->wrapper_mode) {
    is_clustered = wrapper_primary_key_is_clustered();
  } else {
#  endif
    is_clustered = storage_primary_key_is_clustered();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(is_clustered);
}
#endif

#ifdef MRN_HANDLER_HAVE_CAN_SWITCH_ENGINES
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_can_switch_engines()
{
  MRN_DBUG_ENTER_METHOD();
  bool res;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->can_switch_engines();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

bool ha_mroonga::storage_can_switch_engines()
{
  MRN_DBUG_ENTER_METHOD();
  bool res = handler::can_switch_engines();
  DBUG_RETURN(res);
}

bool ha_mroonga::can_switch_engines()
{
  MRN_DBUG_ENTER_METHOD();
  bool res;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_can_switch_engines();
  } else {
#  endif
    res = storage_can_switch_engines();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(res);
}
#endif

#ifdef MRN_HANDLER_HAVE_FOREIGN_KEY_INFO
#  ifdef MRN_ENABLE_WRAPPER_MODE
bool ha_mroonga::wrapper_is_fk_defined_on_table_or_index(uint index)
{
  MRN_DBUG_ENTER_METHOD();
  bool res;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->is_fk_defined_on_table_or_index(index);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

bool ha_mroonga::storage_is_fk_defined_on_table_or_index(uint index)
{
  MRN_DBUG_ENTER_METHOD();
  bool res = handler::is_fk_defined_on_table_or_index(index);
  DBUG_RETURN(res);
}

bool ha_mroonga::is_fk_defined_on_table_or_index(uint index)
{
  MRN_DBUG_ENTER_METHOD();
  bool res;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_is_fk_defined_on_table_or_index(index);
  } else {
#  endif
    res = storage_is_fk_defined_on_table_or_index(index);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(res);
}

#  ifdef MRN_ENABLE_WRAPPER_MODE
char* ha_mroonga::wrapper_get_foreign_key_create_info()
{
  MRN_DBUG_ENTER_METHOD();
  char* res;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->get_foreign_key_create_info();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

char* ha_mroonga::storage_get_foreign_key_create_info()
{
  int error;
  uint i;
  grn_obj* column;
  uint n_columns = table_share->fields;
  char create_info_buff[2048], *create_info;
  String create_info_str(create_info_buff,
                         sizeof(create_info_buff),
                         system_charset_info);
  MRN_DBUG_ENTER_METHOD();
  create_info_str.length(0);
  for (i = 0; i < n_columns; ++i) {
    Field* field = table_share->field[i];

    if (!is_foreign_key_field(table_share->table_name.str, field->field_name)) {
      continue;
    }

    mrn::ColumnName column_name(FIELD_NAME(field));
    column =
      grn_obj_column(ctx, grn_table, column_name.c_str(), column_name.length());
    if (!column) {
      continue;
    }
    grn_id ref_table_id = grn_obj_get_range(ctx, column);
    grn_obj* ref_table = grn_ctx_at(ctx, ref_table_id);
    char ref_table_buff[NAME_LEN + 1];
    int ref_table_name_length =
      grn_obj_name(ctx, ref_table, ref_table_buff, NAME_LEN);
    ref_table_buff[ref_table_name_length] = '\0';

    if (create_info_str.reserve(15)) {
      DBUG_RETURN(NULL);
    }
    create_info_str.MRN_STRING_APPEND(",\n  CONSTRAINT ", 15);
    append_identifier(ha_thd(),
                      &create_info_str,
                      column_name.c_str(),
                      column_name.length());
    if (create_info_str.reserve(14)) {
      DBUG_RETURN(NULL);
    }
    create_info_str.MRN_STRING_APPEND(" FOREIGN KEY (", 14);
    append_identifier(ha_thd(),
                      &create_info_str,
                      column_name.c_str(),
                      column_name.length());
    if (create_info_str.reserve(13)) {
      DBUG_RETURN(NULL);
    }
    create_info_str.MRN_STRING_APPEND(") REFERENCES ", 13);
    append_identifier(ha_thd(),
                      &create_info_str,
                      table_share->db.str,
                      table_share->db.length);
    if (create_info_str.reserve(1)) {
      DBUG_RETURN(NULL);
    }
    create_info_str.MRN_STRING_APPEND(".", 1);
    append_identifier(ha_thd(),
                      &create_info_str,
                      ref_table_buff,
                      ref_table_name_length);
    if (create_info_str.reserve(2)) {
      DBUG_RETURN(NULL);
    }
    create_info_str.MRN_STRING_APPEND(" (", 2);

    char ref_path[FN_REFLEN + 1];
    build_table_filename(ref_path,
                         sizeof(ref_path) - 1,
                         table_share->db.str,
                         ref_table_buff,
                         "",
                         0);
    DBUG_PRINT("info", ("mroonga: ref_path=%s", ref_path));
    MRN_DECLARE_TABLE_LIST(table_list,
                           table_share->db.str,
                           static_cast<size_t>(table_share->db.length),
                           ref_table_buff,
                           static_cast<size_t>(ref_table_name_length),
                           ref_table_buff,
                           TL_WRITE);
    mrn_open_mutex_lock(table_share);
#  ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
    // TODO
    TABLE_SHARE* tmp_ref_table_share =
      mrn_create_tmp_table_share(&table_list, ref_path, NULL, &error);
#  else
    TABLE_SHARE* tmp_ref_table_share =
      mrn_create_tmp_table_share(&table_list, ref_path, &error);
#  endif
    mrn_open_mutex_unlock(table_share);
    if (!tmp_ref_table_share) {
      DBUG_RETURN(NULL);
    }
    uint ref_pkey_nr = tmp_ref_table_share->primary_key;
    KEY* ref_key_info = &tmp_ref_table_share->key_info[ref_pkey_nr];
    Field* ref_field = &ref_key_info->key_part->field[0];
    append_identifier(ha_thd(), &create_info_str, FIELD_NAME(ref_field));
    mrn_open_mutex_lock(table_share);
    mrn_free_tmp_table_share(tmp_ref_table_share);
    mrn_open_mutex_unlock(table_share);
    if (create_info_str.reserve(39)) {
      DBUG_RETURN(NULL);
    }
    create_info_str.MRN_STRING_APPEND(")", 2);
  }
  if (!(create_info =
          (char*)mrn_my_malloc(create_info_str.length() + 1, MYF(MY_WME)))) {
    DBUG_RETURN(NULL);
  }
  memcpy(create_info, create_info_str.ptr(), create_info_str.length());
  create_info[create_info_str.length()] = '\0';
  DBUG_RETURN(create_info);
}

char* ha_mroonga::get_foreign_key_create_info()
{
  MRN_DBUG_ENTER_METHOD();
  char* res;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_get_foreign_key_create_info();
  } else {
#  endif
    res = storage_get_foreign_key_create_info();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(res);
}

#  ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_get_foreign_key_list(THD* thd,
                                             List<FOREIGN_KEY_INFO>* f_key_list)
{
  MRN_DBUG_ENTER_METHOD();
  int res;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->get_foreign_key_list(thd, f_key_list);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

int ha_mroonga::storage_get_foreign_key_list(THD* thd,
                                             List<FOREIGN_KEY_INFO>* f_key_list)
{
  int error;
  uint i;
  grn_obj* column;
  uint n_columns = table_share->fields;
  MRN_DBUG_ENTER_METHOD();
  for (i = 0; i < n_columns; ++i) {
    Field* field = table_share->field[i];

    if (!is_foreign_key_field(table_share->table_name.str, field->field_name)) {
      continue;
    }

    mrn::ColumnName column_name(FIELD_NAME(field));
    column =
      grn_obj_column(ctx, grn_table, column_name.c_str(), column_name.length());
    if (!column) {
      continue;
    }
    grn_id ref_table_id = grn_obj_get_range(ctx, column);
    grn_obj* ref_table = grn_ctx_at(ctx, ref_table_id);
    FOREIGN_KEY_INFO f_key_info;
    f_key_info.foreign_id = thd_make_lex_string(thd,
                                                NULL,
                                                column_name.c_str(),
                                                column_name.length(),
                                                true);
    f_key_info.foreign_db = thd_make_lex_string(thd,
                                                NULL,
                                                table_share->db.str,
                                                table_share->db.length,
                                                true);
    f_key_info.foreign_table =
      thd_make_lex_string(thd,
                          NULL,
                          table_share->table_name.str,
                          table_share->table_name.length,
                          true);
    f_key_info.referenced_db = f_key_info.foreign_db;

    char ref_table_buff[NAME_LEN + 1];
    int ref_table_name_length =
      grn_obj_name(ctx, ref_table, ref_table_buff, NAME_LEN);
    ref_table_buff[ref_table_name_length] = '\0';
    DBUG_PRINT("info", ("mroonga: ref_table_buff=%s", ref_table_buff));
    DBUG_PRINT("info",
               ("mroonga: ref_table_name_length=%d", ref_table_name_length));
    f_key_info.referenced_table = thd_make_lex_string(thd,
                                                      NULL,
                                                      ref_table_buff,
                                                      ref_table_name_length,
                                                      true);
#  ifdef MRN_FOREIGN_KEY_USE_METHOD_ENUM
    f_key_info.update_method = FK_OPTION_RESTRICT;
    f_key_info.delete_method = FK_OPTION_RESTRICT;
#  else
    f_key_info.update_method =
      thd_make_lex_string(thd, NULL, "RESTRICT", 8, true);
    f_key_info.delete_method =
      thd_make_lex_string(thd, NULL, "RESTRICT", 8, true);
#  endif
    f_key_info.referenced_key_name =
      thd_make_lex_string(thd, NULL, "PRIMARY", 7, true);
    mrn_thd_lex_string* field_name = thd_make_lex_string(thd,
                                                         NULL,
                                                         column_name.c_str(),
                                                         column_name.length(),
                                                         true);
    f_key_info.foreign_fields.push_back(field_name);

    char ref_path[FN_REFLEN + 1];
    build_table_filename(ref_path,
                         sizeof(ref_path) - 1,
                         table_share->db.str,
                         ref_table_buff,
                         "",
                         0);
    DBUG_PRINT("info", ("mroonga: ref_path=%s", ref_path));
    MRN_DECLARE_TABLE_LIST(table_list,
                           table_share->db.str,
                           static_cast<size_t>(table_share->db.length),
                           ref_table_buff,
                           static_cast<size_t>(ref_table_name_length),
                           ref_table_buff,
                           TL_WRITE);
    mrn_open_mutex_lock(table_share);
#  ifdef MRN_OPEN_TABLE_DEF_USE_TABLE_DEFINITION
    TABLE_SHARE* tmp_ref_table_share =
      mrn_create_tmp_table_share(&table_list, ref_path, NULL, &error);
#  else
    TABLE_SHARE* tmp_ref_table_share =
      mrn_create_tmp_table_share(&table_list, ref_path, &error);
#  endif
    mrn_open_mutex_unlock(table_share);
    if (!tmp_ref_table_share) {
      DBUG_RETURN(error);
    }
    uint ref_pkey_nr = tmp_ref_table_share->primary_key;
    KEY* ref_key_info = &tmp_ref_table_share->key_info[ref_pkey_nr];
    Field* ref_field = &ref_key_info->key_part->field[0];
    mrn_thd_lex_string* ref_col_name = FIELD_NAME_TO_LEX_STRING(thd, ref_field);
    f_key_info.referenced_fields.push_back(ref_col_name);
    mrn_open_mutex_lock(table_share);
    mrn_free_tmp_table_share(tmp_ref_table_share);
    mrn_open_mutex_unlock(table_share);
    FOREIGN_KEY_INFO* p_f_key_info =
      (FOREIGN_KEY_INFO*)thd_memdup(thd, &f_key_info, sizeof(FOREIGN_KEY_INFO));
    if (!p_f_key_info) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    f_key_list->push_back(p_f_key_info);
  }
  DBUG_RETURN(0);
}

int ha_mroonga::get_foreign_key_list(THD* thd,
                                     List<FOREIGN_KEY_INFO>* f_key_list)
{
  MRN_DBUG_ENTER_METHOD();
  int res;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_get_foreign_key_list(thd, f_key_list);
  } else {
#  endif
    res = storage_get_foreign_key_list(thd, f_key_list);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(res);
}

#  ifdef MRN_ENABLE_WRAPPER_MODE
int ha_mroonga::wrapper_get_parent_foreign_key_list(
  THD* thd, List<FOREIGN_KEY_INFO>* f_key_list)
{
  MRN_DBUG_ENTER_METHOD();
  int res;
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  res = wrap_handler->get_parent_foreign_key_list(thd, f_key_list);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

int ha_mroonga::storage_get_parent_foreign_key_list(
  THD* thd, List<FOREIGN_KEY_INFO>* f_key_list)
{
  MRN_DBUG_ENTER_METHOD();
  int res = handler::get_parent_foreign_key_list(thd, f_key_list);
  DBUG_RETURN(res);
}

int ha_mroonga::get_parent_foreign_key_list(THD* thd,
                                            List<FOREIGN_KEY_INFO>* f_key_list)
{
  MRN_DBUG_ENTER_METHOD();
  int res;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_get_parent_foreign_key_list(thd, f_key_list);
  } else {
#  endif
    res = storage_get_parent_foreign_key_list(thd, f_key_list);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(res);
}

#  ifdef MRN_ENABLE_WRAPPER_MODE
mrn_handler_referenced_by_foreign_key_bool
ha_mroonga::wrapper_referenced_by_foreign_key()
  MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  auto res = wrap_handler->referenced_by_foreign_key();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_RETURN(res);
}
#  endif

mrn_handler_referenced_by_foreign_key_bool
ha_mroonga::storage_referenced_by_foreign_key()
  MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT
{
  MRN_DBUG_ENTER_METHOD();
  auto res = handler::referenced_by_foreign_key();
  DBUG_RETURN(res);
}

mrn_handler_referenced_by_foreign_key_bool
ha_mroonga::referenced_by_foreign_key()
  MRN_HANDLER_REFERENCED_BY_FOREIGN_KEY_CONST_NOEXCEPT
{
  MRN_DBUG_ENTER_METHOD();
  mrn_handler_referenced_by_foreign_key_bool res;
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    res = wrapper_referenced_by_foreign_key();
  } else {
#  endif
    res = storage_referenced_by_foreign_key();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_RETURN(res);
}

#  ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_free_foreign_key_create_info(char* str)
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->free_foreign_key_create_info(str);
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#  endif

void ha_mroonga::storage_free_foreign_key_create_info(char* str)
{
  MRN_DBUG_ENTER_METHOD();
  my_free(str);
  DBUG_VOID_RETURN;
}

void ha_mroonga::free_foreign_key_create_info(char* str)
{
  MRN_DBUG_ENTER_METHOD();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_free_foreign_key_create_info(str);
  } else {
#  endif
    storage_free_foreign_key_create_info(str);
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_VOID_RETURN;
}
#endif

#ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_init_table_handle_for_HANDLER()
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->init_table_handle_for_HANDLER();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#endif

void ha_mroonga::storage_init_table_handle_for_HANDLER()
{
  MRN_DBUG_ENTER_METHOD();
  handler::init_table_handle_for_HANDLER();
  DBUG_VOID_RETURN;
}

void ha_mroonga::init_table_handle_for_HANDLER()
{
  MRN_DBUG_ENTER_METHOD();
#ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_init_table_handle_for_HANDLER();
  } else {
#endif
    storage_init_table_handle_for_HANDLER();
#ifdef MRN_ENABLE_WRAPPER_MODE
  }
#endif
  DBUG_VOID_RETURN;
}

#ifdef MRN_HANDLER_NEED_OVERRIDE_UNBIND_PSI
#  ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_unbind_psi()
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->unbind_psi();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#  endif

void ha_mroonga::storage_unbind_psi()
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_VOID_RETURN;
}

void ha_mroonga::unbind_psi()
{
  MRN_DBUG_ENTER_METHOD();
  handler::unbind_psi();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_unbind_psi();
  } else {
#  endif
    storage_unbind_psi();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_VOID_RETURN;
}
#endif

#ifdef MRN_HANDLER_NEED_OVERRIDE_REBIND_PSI
#  ifdef MRN_ENABLE_WRAPPER_MODE
void ha_mroonga::wrapper_rebind_psi()
{
  MRN_DBUG_ENTER_METHOD();
  MRN_SET_WRAP_SHARE_KEY(share, table->s);
  MRN_SET_WRAP_TABLE_KEY(this, table);
  wrap_handler->rebind_psi();
  MRN_SET_BASE_SHARE_KEY(share, table->s);
  MRN_SET_BASE_TABLE_KEY(this, table);
  DBUG_VOID_RETURN;
}
#  endif

void ha_mroonga::storage_rebind_psi()
{
  MRN_DBUG_ENTER_METHOD();
  DBUG_VOID_RETURN;
}

void ha_mroonga::rebind_psi()
{
  MRN_DBUG_ENTER_METHOD();
  handler::rebind_psi();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  if (share->wrapper_mode) {
    wrapper_rebind_psi();
  } else {
#  endif
    storage_rebind_psi();
#  ifdef MRN_ENABLE_WRAPPER_MODE
  }
#  endif
  DBUG_VOID_RETURN;
}
#endif

namespace mrn {
  namespace variables {
    ulonglong get_boolean_mode_syntax_flags(THD* thd)
    {
      ulonglong flags = BOOLEAN_MODE_SYNTAX_FLAG_DEFAULT;
      flags = THDVAR(thd, boolean_mode_syntax_flags);
      return flags;
    }

    ActionOnError get_action_on_fulltext_query_error(THD* thd)
    {
      ulong action = THDVAR(thd, action_on_fulltext_query_error);
      return static_cast<ActionOnError>(action);
    }
  } // namespace variables
} // namespace mrn
