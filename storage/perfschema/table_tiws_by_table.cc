/* Copyright (c) 2010, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file storage/perfschema/table_tiws_by_table.cc
  Table TABLE_IO_WAITS_SUMMARY_BY_TABLE (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_tiws_by_table.h"
#include "pfs_global.h"
#include "pfs_visitor.h"
#include "pfs_buffer_container.h"
#include "field.h"

THR_LOCK table_tiws_by_table::m_table_lock;

PFS_engine_table_share_state
table_tiws_by_table::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_tiws_by_table::m_share=
{
  { C_STRING_WITH_LEN("table_io_waits_summary_by_table") },
  &pfs_truncatable_acl,
  table_tiws_by_table::create,
  NULL, /* write_row */
  table_tiws_by_table::delete_all_rows,
  table_tiws_by_table::get_row_count,
  sizeof(PFS_simple_index),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE table_io_waits_summary_by_table("
                      "OBJECT_TYPE VARCHAR(64) comment 'Since this table records waits by table, always set to TABLE.',"
                      "OBJECT_SCHEMA VARCHAR(64) comment 'Schema name.',"
                      "OBJECT_NAME VARCHAR(64) comment 'Table name.',"
                      "COUNT_STAR BIGINT unsigned not null comment 'Number of summarized events and the sum of the x_READ and x_WRITE columns.',"
                      "SUM_TIMER_WAIT BIGINT unsigned not null comment 'Total wait time of the summarized events that are timed.',"
                      "MIN_TIMER_WAIT BIGINT unsigned not null comment 'Minimum wait time of the summarized events that are timed.',"
                      "AVG_TIMER_WAIT BIGINT unsigned not null comment 'Average wait time of the summarized events that are timed.',"
                      "MAX_TIMER_WAIT BIGINT unsigned not null comment 'Maximum wait time of the summarized events that are timed.',"
                      "COUNT_READ BIGINT unsigned not null comment 'Number of all read operations, and the sum of the equivalent x_FETCH columns.',"
                      "SUM_TIMER_READ BIGINT unsigned not null comment 'Total wait time of all read operations that are timed.',"
                      "MIN_TIMER_READ BIGINT unsigned not null comment 'Minimum wait time of all read operations that are timed.',"
                      "AVG_TIMER_READ BIGINT unsigned not null comment 'Average wait time of all read operations that are timed.',"
                      "MAX_TIMER_READ BIGINT unsigned not null comment 'Maximum wait time of all read operations that are timed.',"
                      "COUNT_WRITE BIGINT unsigned not null comment 'Number of all write operations, and the sum of the equivalent x_INSERT, x_UPDATE and x_DELETE columns.',"
                      "SUM_TIMER_WRITE BIGINT unsigned not null comment 'Total wait time of all write operations that are timed.',"
                      "MIN_TIMER_WRITE BIGINT unsigned not null comment 'Minimum wait time of all write operations that are timed.',"
                      "AVG_TIMER_WRITE BIGINT unsigned not null comment 'Average wait time of all write operations that are timed.',"
                      "MAX_TIMER_WRITE BIGINT unsigned not null comment 'Maximum wait time of all write operations that are timed.',"
                      "COUNT_FETCH BIGINT unsigned not null comment 'Number of all fetch operations.',"
                      "SUM_TIMER_FETCH BIGINT unsigned not null comment 'Total wait time of all fetch operations that are timed.',"
                      "MIN_TIMER_FETCH BIGINT unsigned not null comment 'Minimum wait time of all fetch operations that are timed.',"
                      "AVG_TIMER_FETCH BIGINT unsigned not null comment 'Average wait time of all fetch operations that are timed.',"
                      "MAX_TIMER_FETCH BIGINT unsigned not null comment 'Maximum wait time of all fetch operations that are timed.',"
                      "COUNT_INSERT BIGINT unsigned not null comment 'Number of all insert operations.',"
                      "SUM_TIMER_INSERT BIGINT unsigned not null comment 'Total wait time of all insert operations that are timed.',"
                      "MIN_TIMER_INSERT BIGINT unsigned not null comment 'Minimum wait time of all insert operations that are timed.',"
                      "AVG_TIMER_INSERT BIGINT unsigned not null comment 'Average wait time of all insert operations that are timed.',"
                      "MAX_TIMER_INSERT BIGINT unsigned not null comment 'Maximum wait time of all insert operations that are timed.',"
                      "COUNT_UPDATE BIGINT unsigned not null comment 'Number of all update operations.',"
                      "SUM_TIMER_UPDATE BIGINT unsigned not null comment 'Total wait time of all update operations that are timed.',"
                      "MIN_TIMER_UPDATE BIGINT unsigned not null comment 'Minimum wait time of all update operations that are timed.',"
                      "AVG_TIMER_UPDATE BIGINT unsigned not null comment 'Average wait time of all update operations that are timed.',"
                      "MAX_TIMER_UPDATE BIGINT unsigned not null comment 'Maximum wait time of all update operations that are timed.',"
                      "COUNT_DELETE BIGINT unsigned not null comment 'Number of all delete operations.',"
                      "SUM_TIMER_DELETE BIGINT unsigned not null comment 'Total wait time of all delete operations that are timed.',"
                      "MIN_TIMER_DELETE BIGINT unsigned not null comment 'Minimum wait time of all delete operations that are timed.',"
                      "AVG_TIMER_DELETE BIGINT unsigned not null comment 'Average wait time of all delete operations that are timed.',"
                      "MAX_TIMER_DELETE BIGINT unsigned not null comment 'Maximum wait time of all delete operations that are timed.')") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table*
table_tiws_by_table::create(void)
{
  return new table_tiws_by_table();
}

int
table_tiws_by_table::delete_all_rows(void)
{
  reset_table_io_waits_by_table_handle();
  reset_table_io_waits_by_table();
  return 0;
}

ha_rows
table_tiws_by_table::get_row_count(void)
{
  return global_table_share_container.get_row_count();
}

table_tiws_by_table::table_tiws_by_table()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{
  m_normalizer= time_normalizer::get_wait();
}

void table_tiws_by_table::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_tiws_by_table::rnd_init(bool scan)
{
  return 0;
}

int table_tiws_by_table::rnd_next(void)
{
  PFS_table_share *pfs;

  m_pos.set_at(&m_next_pos);
  PFS_table_share_iterator it= global_table_share_container.iterate(m_pos.m_index);
  do
  {
    pfs= it.scan_next(& m_pos.m_index);
    if (pfs != NULL)
    {
      if (pfs->m_enabled)
      {
        make_row(pfs);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  } while (pfs != NULL);

  return HA_ERR_END_OF_FILE;
}

int
table_tiws_by_table::rnd_pos(const void *pos)
{
  PFS_table_share *pfs;

  set_position(pos);

  pfs= global_table_share_container.get(m_pos.m_index);
  if (pfs != NULL)
  {
    if (pfs->m_enabled)
    {
      make_row(pfs);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}

void table_tiws_by_table::make_row(PFS_table_share *share)
{
  pfs_optimistic_state lock;

  m_row_exists= false;

  share->m_lock.begin_optimistic_lock(&lock);

  if (m_row.m_object.make_row(share))
    return;

  PFS_table_io_stat_visitor visitor;
  PFS_object_iterator::visit_tables(share, & visitor);

  if (! share->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
  m_row.m_stat.set(m_normalizer, &visitor.m_stat);
}

int table_tiws_by_table::read_row_values(TABLE *table,
                                        unsigned char *buf,
                                        Field **fields,
                                        bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* OBJECT_TYPE */
      case 1: /* SCHEMA_NAME */
      case 2: /* OBJECT_NAME */
        m_row.m_object.set_field(f->field_index, f);
        break;
      case 3: /* COUNT_STAR */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_count);
        break;
      case 4: /* SUM */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_sum);
        break;
      case 5: /* MIN */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_min);
        break;
      case 6: /* AVG */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_avg);
        break;
      case 7: /* MAX */
        set_field_ulonglong(f, m_row.m_stat.m_all.m_max);
        break;
      case 8: /* COUNT_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_count);
        break;
      case 9: /* SUM_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_sum);
        break;
      case 10: /* MIN_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_min);
        break;
      case 11: /* AVG_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_avg);
        break;
      case 12: /* MAX_READ */
        set_field_ulonglong(f, m_row.m_stat.m_all_read.m_max);
        break;
      case 13: /* COUNT_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_count);
        break;
      case 14: /* SUM_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_sum);
        break;
      case 15: /* MIN_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_min);
        break;
      case 16: /* AVG_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_avg);
        break;
      case 17: /* MAX_WRITE */
        set_field_ulonglong(f, m_row.m_stat.m_all_write.m_max);
        break;
      case 18: /* COUNT_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_count);
        break;
      case 19: /* SUM_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_sum);
        break;
      case 20: /* MIN_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_min);
        break;
      case 21: /* AVG_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_avg);
        break;
      case 22: /* MAX_FETCH */
        set_field_ulonglong(f, m_row.m_stat.m_fetch.m_max);
        break;
      case 23: /* COUNT_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_count);
        break;
      case 24: /* SUM_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_sum);
        break;
      case 25: /* MIN_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_min);
        break;
      case 26: /* AVG_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_avg);
        break;
      case 27: /* MAX_INSERT */
        set_field_ulonglong(f, m_row.m_stat.m_insert.m_max);
        break;
      case 28: /* COUNT_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_count);
        break;
      case 29: /* SUM_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_sum);
        break;
      case 30: /* MIN_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_min);
        break;
      case 31: /* AVG_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_avg);
        break;
      case 32: /* MAX_UPDATE */
        set_field_ulonglong(f, m_row.m_stat.m_update.m_max);
        break;
      case 33: /* COUNT_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_count);
        break;
      case 34: /* SUM_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_sum);
        break;
      case 35: /* MIN_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_min);
        break;
      case 36: /* AVG_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_avg);
        break;
      case 37: /* MAX_DELETE */
        set_field_ulonglong(f, m_row.m_stat.m_delete.m_max);
        break;
      default:
        assert(false);
      }
    }
  }

  return 0;
}

